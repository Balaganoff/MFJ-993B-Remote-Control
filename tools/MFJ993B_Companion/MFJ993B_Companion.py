#!/usr/bin/env python3
"""MFJ-993B native companion for the existing ESP32 firmware.

No firmware changes and no third-party Python packages are required.
The program speaks the same WebSocket protocol as the browser UI:

    ESP -> client: 0xFD + 32 DDRAM bytes + 64 CGRAM bytes + revision
    client -> ESP:  "B" + nine ASCII 0/1 button states
"""

from __future__ import annotations

import base64
import hashlib
import http.client
import json
import os
import queue
import secrets
import socket
import ssl
import struct
import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from urllib.parse import urlsplit


APP_NAME = "MFJ-993B Companion"
APP_VERSION = "1.2.0"
DEFAULT_HOST = "192.168.2.124"

MIN_UI_SCALE = 0.60
MAX_UI_SCALE = 1.60
UI_SCALE_STEP = 0.10

PACKET_LCD = 0xFD
LCD_PACKET_LENGTH = 98
SPACE = 0x20
LCD_RESET_COMMAND = "R"
LCD_RESET_ACK = "LCD_RESET_OK"
LCD_RESET_TIMEOUT_MS = 1500

MOMENTARY_INDICES = (1, 2, 4, 5, 6, 7)
MOMENTARY_MASK = sum(1 << index for index in MOMENTARY_INDICES)
INITIAL_BUTTON_MASK = (1 << 3) | (1 << 8)

BG = "#111111"
PANEL = "#181818"
BUTTON_BG = "#242424"
BUTTON_FG = "#bdbdbd"
BUTTON_BORDER = "#484848"
ACTIVE_BG = "#004b89"
ACTIVE_BORDER = "#168fe2"
POWER_ACTIVE_BG = "#880000"
POWER_ACTIVE_BORDER = "#ff3030"
DANGER_BG = "#321f1f"
DANGER_FG = "#d9a0a0"
LCD_CASE = "#303330"
LCD_BG = "#001b08"
LCD_CELL = "#00270b"
LCD_BORDER = "#074316"
LCD_PIXEL = "#4cee5b"


@dataclass(frozen=True)
class LcdSnapshot:
    screen: bytes
    cgram: bytes
    revision: int

    @classmethod
    def parse(cls, packet: bytes) -> "LcdSnapshot | None":
        if len(packet) != LCD_PACKET_LENGTH or packet[0] != PACKET_LCD:
            return None
        return cls(packet[1:33], packet[33:97], packet[97])


class MeterNormalizer:
    """Browser-compatible meter formatter with resilient anchor recovery."""

    _CANONICAL_ANCHORS = {
        b"MHz": 6,
        b"FWD": 16,
        b"REF": 25,
    }

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.frequency = " 0.000"
        self.big_slots = [5, 7, 6]
        self.small_swr = "1,2"
        self.forward = "0.0"
        self.reflected = "0.0"

    @staticmethod
    def _is_value_byte(value: int) -> bool:
        return 0x30 <= value <= 0x39 or value in (0x2E, 0x2C, 0x20)

    @classmethod
    def _extract(cls, data: bytes, start: int, length: int, *, circular: bool = False) -> str | None:
        if not data or length <= 0:
            return None

        values: list[int] = []
        for offset in range(length):
            index = start + offset
            if circular:
                index %= len(data)
            elif index < 0 or index >= len(data):
                return None

            value = data[index]
            if not cls._is_value_byte(value):
                return None
            values.append(value)

        if not any(0x30 <= value <= 0x39 for value in values):
            return None
        return bytes(values).decode("ascii")

    @staticmethod
    def _find_circular(row: bytes, needle: bytes) -> int:
        if not row or not needle or len(needle) > len(row):
            return -1
        position = (row + row[: len(needle) - 1]).find(needle)
        return position if 0 <= position < len(row) else -1

    @staticmethod
    def _circular_byte(row: bytes, index: int) -> int:
        return row[index % len(row)]

    @staticmethod
    def _mismatch_count(data: bytes, start: int, needle: bytes) -> int:
        return sum(
            data[(start + offset) % len(data)] != expected
            for offset, expected in enumerate(needle)
        )

    def _locate_anchors(self, screen: bytes) -> tuple[dict[bytes, int], list[str]]:
        """Locate labels after any circular displacement of the 32-byte image.

        Exact labels are always preferred.  If at least one exact label fixes a
        likely screen rotation, a second label with one damaged byte may still
        be used.  This is deliberately conservative so an ordinary menu is not
        mistaken for the meter screen.
        """

        positions: dict[bytes, int] = {}
        evidence: list[str] = []

        for label in self._CANONICAL_ANCHORS:
            position = self._find_circular(screen, label)
            if position >= 0:
                positions[label] = position
                evidence.append(f"{label.decode()}@{position}")

        if not positions:
            return positions, evidence

        # Derive candidate whole-screen rotations from exact anchors.  A
        # missing label is accepted only at a predicted position and only when
        # exactly one of its three bytes was damaged.
        rotations = [
            (position - self._CANONICAL_ANCHORS[label]) % len(screen)
            for label, position in positions.items()
        ]
        for label, canonical_position in self._CANONICAL_ANCHORS.items():
            if label in positions:
                continue
            for rotation in rotations:
                predicted = (canonical_position + rotation) % len(screen)
                if self._mismatch_count(screen, predicted, label) == 1:
                    positions[label] = predicted
                    evidence.append(f"{label.decode()}~@{predicted}")
                    break

        return positions, evidence

    def normalize(self, snapshot: LcdSnapshot) -> tuple[bool, LcdSnapshot, str]:
        """Return (is_main_meter, snapshot_to_draw, diagnostic_evidence)."""

        screen = snapshot.screen
        anchors, evidence_parts = self._locate_anchors(screen)
        mhz_pos = anchors.get(b"MHz", -1)
        fwd_pos = anchors.get(b"FWD", -1)
        ref_pos = anchors.get(b"REF", -1)

        frequency = None
        slot_values: list[int] = []
        small_swr = None
        if mhz_pos >= 0:
            frequency = self._extract(screen, mhz_pos - 6, 6, circular=True)
            if frequency and ("." in frequency or "," in frequency):
                self.frequency = frequency.rjust(6)[-6:]

            for slot_index in range(3):
                value = self._circular_byte(screen, mhz_pos + 3 + slot_index)
                slot_values.append(value)
                if value <= 7:
                    self.big_slots[slot_index] = value

            small_swr = self._extract(screen, mhz_pos + 7, 3, circular=True)
            if small_swr and len(small_swr) == 3 and ("." in small_swr or "," in small_swr):
                self.small_swr = small_swr

        forward_value = None
        if fwd_pos >= 0:
            value_start = fwd_pos + 3
            if self._circular_byte(screen, value_start) == ord("="):
                value_start += 1
            forward_value = self._extract(screen, value_start, 3, circular=True)
            if forward_value and len(forward_value) == 3:
                self.forward = forward_value

        reflected_value = None
        if ref_pos >= 0:
            value_start = ref_pos + 3
            if self._circular_byte(screen, value_start) == ord("="):
                value_start += 1
            reflected_value = self._extract(screen, value_start, 3, circular=True)
            if reflected_value and len(reflected_value) == 3:
                self.reflected = reflected_value

        # Do not classify a MODE screen by one stale label left in DDRAM.
        # The meter screen has a much stronger signature: frequency + MHz,
        # three CGRAM digit slots, small SWR, and the FWD/REF pair nine cells
        # apart.  A one-byte damaged label can still be reconstructed above.
        frequency_ok = bool(frequency and ("." in frequency or "," in frequency))
        slots_ok = len(slot_values) == 3 and all(value <= 7 for value in slot_values)
        small_swr_ok = bool(small_swr and ("." in small_swr or "," in small_swr))
        power_pair_ok = (
            fwd_pos >= 0
            and ref_pos >= 0
            and (ref_pos - fwd_pos) % len(screen) == 9
            and bool(forward_value or reflected_value)
        )
        is_main = frequency_ok and slots_ok and small_swr_ok and power_pair_ok

        if not is_main:
            return False, snapshot, ",".join(evidence_parts)

        output = bytearray(b" " * 32)
        self._write_ascii(output, 0, self.frequency)
        self._write_ascii(output, 6, "MHz")
        output[9:12] = bytes(self.big_slots)
        output[12] = SPACE
        self._write_ascii(output, 13, self.small_swr)

        self._write_ascii(output, 16, "FWD=")
        self._write_ascii(output, 20, self.forward)
        output[23] = SPACE
        output[24] = SPACE
        self._write_ascii(output, 25, "REF=")
        self._write_ascii(output, 29, self.reflected)

        return (
            True,
            LcdSnapshot(bytes(output), snapshot.cgram, snapshot.revision),
            ",".join(evidence_parts),
        )

    @staticmethod
    def _write_ascii(target: bytearray, offset: int, text: str) -> None:
        encoded = text.encode("ascii", errors="replace")
        end = min(len(target), offset + len(encoded))
        target[offset:end] = encoded[: end - offset]


@dataclass(frozen=True)
class WsEndpoint:
    scheme: str
    host: str
    port: int
    path: str

    @classmethod
    def parse(cls, value: str) -> "WsEndpoint":
        value = value.strip()
        if not value:
            raise ValueError("Укажите IP-адрес ESP32")
        if "://" not in value:
            value = f"ws://{value}/ws"

        parsed = urlsplit(value)
        if parsed.scheme not in ("ws", "wss"):
            raise ValueError("Поддерживаются адреса IP, ws:// и wss://")
        if not parsed.hostname:
            raise ValueError("Некорректный адрес ESP32")

        port = parsed.port or (443 if parsed.scheme == "wss" else 80)
        path = parsed.path or "/ws"
        if parsed.query:
            path += "?" + parsed.query
        return cls(parsed.scheme, parsed.hostname, port, path)

    @property
    def display_host(self) -> str:
        default_port = 443 if self.scheme == "wss" else 80
        return self.host if self.port == default_port else f"{self.host}:{self.port}"


class WebSocketWorker(threading.Thread):
    """Small RFC 6455 client for the ESP32; uses only Python stdlib."""

    def __init__(self, worker_id: int, endpoint: WsEndpoint, events: queue.Queue) -> None:
        super().__init__(name=f"MFJ993B-WebSocket-{worker_id}", daemon=True)
        self.worker_id = worker_id
        self.endpoint = endpoint
        self.events = events
        self._stop_event = threading.Event()
        self._send_lock = threading.Lock()
        self._socket_lock = threading.Lock()
        self._socket: socket.socket | None = None
        self._rx_buffer = bytearray()

    def stop(self) -> None:
        self._stop_event.set()
        with self._socket_lock:
            sock = self._socket
            self._socket = None
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

    def send_button_mask(self, mask: int) -> bool:
        message = "B" + "".join("1" if mask & (1 << index) else "0" for index in range(9))
        return self.send_text(message)

    def send_text(self, text: str) -> bool:
        return self._send_frame(0x1, text.encode("ascii"))

    def run(self) -> None:
        while not self._stop_event.is_set():
            self._post("status", f"Подключение к {self.endpoint.display_host}…")
            try:
                self._connect_and_receive()
            except Exception as exc:  # noqa: BLE001 - worker must reconnect after all I/O failures
                if not self._stop_event.is_set():
                    text = str(exc).replace("\r", " ").replace("\n", " ").strip()
                    self._post("status", f"Связь потеряна: {text[:110]}")
            finally:
                self._close_current_socket()
                self._post("connected", False)

            if self._stop_event.wait(2.0):
                break
            self._post("status", "Повторное подключение…")

    def _connect_and_receive(self) -> None:
        raw_socket = socket.create_connection((self.endpoint.host, self.endpoint.port), timeout=5.0)
        raw_socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        if self.endpoint.scheme == "wss":
            context = ssl.create_default_context()
            sock = context.wrap_socket(raw_socket, server_hostname=self.endpoint.host)
        else:
            sock = raw_socket

        sock.settimeout(1.0)
        with self._socket_lock:
            if self._stop_event.is_set():
                sock.close()
                return
            self._socket = sock

        self._perform_handshake(sock)
        self._post("connected", True)
        self._post("status", f"Подключено: {self.endpoint.display_host}")

        fragmented_opcode: int | None = None
        fragments = bytearray()

        while not self._stop_event.is_set():
            try:
                fin, opcode, payload = self._receive_frame()
            except socket.timeout:
                continue

            if opcode == 0x8:
                self._send_frame(0x8, payload[:125])
                return
            if opcode == 0x9:
                self._send_frame(0xA, payload[:125])
                continue
            if opcode == 0xA:
                continue

            if opcode in (0x1, 0x2):
                if fin:
                    self._dispatch_message(opcode, payload)
                else:
                    fragmented_opcode = opcode
                    fragments[:] = payload
                continue

            if opcode == 0x0 and fragmented_opcode is not None:
                fragments.extend(payload)
                if fin:
                    self._dispatch_message(fragmented_opcode, bytes(fragments))
                    fragmented_opcode = None
                    fragments.clear()

    def _perform_handshake(self, sock: socket.socket) -> None:
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        host_header = self.endpoint.display_host
        request = (
            f"GET {self.endpoint.path} HTTP/1.1\r\n"
            f"Host: {host_header}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"User-Agent: {APP_NAME}/{APP_VERSION}\r\n"
            "\r\n"
        ).encode("ascii")
        sock.sendall(request)

        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("ESP32 закрыл соединение при WebSocket handshake")
            response.extend(chunk)
            if len(response) > 16384:
                raise ConnectionError("Слишком большой ответ WebSocket handshake")

        header_bytes, leftover = bytes(response).split(b"\r\n\r\n", 1)
        self._rx_buffer[:] = leftover
        lines = header_bytes.decode("iso-8859-1").split("\r\n")
        if not lines or " 101 " not in f" {lines[0]} ":
            raise ConnectionError(f"WebSocket handshake: {lines[0] if lines else 'нет ответа'}")

        headers: dict[str, str] = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()

        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
        ).decode("ascii")
        if headers.get("sec-websocket-accept") != expected:
            raise ConnectionError("ESP32 вернул неверный WebSocket handshake")

    def _receive_frame(self) -> tuple[bool, int, bytes]:
        first, second = self._recv_exact(2)
        fin = bool(first & 0x80)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F

        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]

        if length > 1024 * 1024:
            raise ConnectionError("Слишком большой WebSocket пакет")

        mask = self._recv_exact(4) if masked else b""
        payload = self._recv_exact(length)
        if masked:
            payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        return fin, opcode, payload

    def _recv_exact(self, count: int) -> bytes:
        while len(self._rx_buffer) < count:
            with self._socket_lock:
                sock = self._socket
            if sock is None:
                raise ConnectionError("WebSocket отключён")
            chunk = sock.recv(max(4096, count - len(self._rx_buffer)))
            if not chunk:
                raise ConnectionError("ESP32 закрыл WebSocket")
            self._rx_buffer.extend(chunk)

        result = bytes(self._rx_buffer[:count])
        del self._rx_buffer[:count]
        return result

    def _send_frame(self, opcode: int, payload: bytes) -> bool:
        if len(payload) > 0xFFFFFFFF:
            return False

        mask = os.urandom(4)
        first = 0x80 | (opcode & 0x0F)
        length = len(payload)
        if length < 126:
            header = bytes((first, 0x80 | length))
        elif length <= 0xFFFF:
            header = bytes((first, 0x80 | 126)) + struct.pack("!H", length)
        else:
            header = bytes((first, 0x80 | 127)) + struct.pack("!Q", length)
        masked_payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))

        with self._send_lock:
            with self._socket_lock:
                sock = self._socket
            if sock is None:
                return False
            try:
                sock.sendall(header + mask + masked_payload)
                return True
            except OSError:
                return False

    def _dispatch_message(self, opcode: int, payload: bytes) -> None:
        if opcode == 0x2:
            snapshot = LcdSnapshot.parse(payload)
            if snapshot is not None:
                self._post("snapshot", snapshot)
        elif opcode == 0x1:
            self._post("text", payload.decode("utf-8", errors="replace"))

    def _post(self, event_type: str, payload: object) -> None:
        self.events.put((self.worker_id, event_type, payload))

    def _close_current_socket(self) -> None:
        with self._socket_lock:
            sock = self._socket
            self._socket = None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        self._rx_buffer.clear()


class LcdCanvas(tk.Canvas):
    COLUMNS = 16
    ROWS = 2
    BASE_CELL_W = 28.0
    BASE_CELL_H = 34.0
    BASE_GAP_X = 2.0
    BASE_GAP_Y = 4.0
    BASE_PAD = 10.0

    def __init__(self, master: tk.Misc, ui_scale: float = 1.0) -> None:
        self.ui_scale = self._clamp_scale(ui_scale)
        self._update_metrics()
        super().__init__(
            master,
            width=self.canvas_width,
            height=self.canvas_height,
            bg=LCD_CASE,
            highlightthickness=0,
            bd=0,
        )
        self._font = tkfont.Font(
            family="Consolas",
            size=max(7, round(18 * self.ui_scale)),
            weight="bold",
        )
        self._signatures: list[object | None] = [None] * 32
        self._last_snapshot = LcdSnapshot(b" " * 32, bytes(64), 0)
        self._draw_static_cells()

    @staticmethod
    def _clamp_scale(value: float) -> float:
        return max(MIN_UI_SCALE, min(MAX_UI_SCALE, value))

    def _update_metrics(self) -> None:
        self.cell_w = self.BASE_CELL_W * self.ui_scale
        self.cell_h = self.BASE_CELL_H * self.ui_scale
        self.gap_x = self.BASE_GAP_X * self.ui_scale
        self.gap_y = self.BASE_GAP_Y * self.ui_scale
        self.pad = self.BASE_PAD * self.ui_scale
        self.canvas_width = round(
            self.pad * 2 + self.COLUMNS * self.cell_w + (self.COLUMNS - 1) * self.gap_x
        )
        self.canvas_height = round(self.pad * 2 + self.ROWS * self.cell_h + self.gap_y)

    def set_scale(self, ui_scale: float) -> None:
        new_scale = self._clamp_scale(ui_scale)
        if abs(new_scale - self.ui_scale) < 0.001:
            return
        self.ui_scale = new_scale
        self._update_metrics()
        self.configure(width=self.canvas_width, height=self.canvas_height)
        self._font.configure(size=max(7, round(18 * self.ui_scale)))
        self.delete("all")
        self._signatures = [None] * 32
        self._draw_static_cells()
        self.draw_snapshot(self._last_snapshot, force=True)

    def _draw_static_cells(self) -> None:
        edge = max(2.0, 5.0 * self.ui_scale)
        self.create_rectangle(
            edge,
            edge,
            self.canvas_width - edge,
            self.canvas_height - edge,
            fill=LCD_BG,
            outline="#16381f",
            width=1,
        )
        for index in range(32):
            x0, y0, x1, y1 = self._cell_rect(index)
            self.create_rectangle(x0, y0, x1, y1, fill=LCD_CELL, outline=LCD_BORDER, width=1)

    def show_text(self, row1: str, row2: str = "") -> None:
        screen = bytearray(b" " * 32)
        for offset, text in ((0, row1), (16, row2)):
            encoded = text.encode("ascii", errors="replace")[:16]
            screen[offset : offset + len(encoded)] = encoded
        self.draw_snapshot(LcdSnapshot(bytes(screen), bytes(64), 0), force=True)

    def draw_snapshot(self, snapshot: LcdSnapshot, *, force: bool = False) -> None:
        self._last_snapshot = snapshot
        for index, value in enumerate(snapshot.screen):
            signature = self._signature(value, snapshot.cgram)
            if not force and signature == self._signatures[index]:
                continue
            self._signatures[index] = signature
            tag = f"lcd-content-{index}"
            self.delete(tag)
            self._draw_cell_content(index, value, snapshot.cgram, tag)

    @staticmethod
    def _signature(value: int, cgram: bytes) -> object:
        if value != 0xE4 and value >= 0x80:
            return ("full",)
        if value != 0xE4 and value <= 0x07:
            offset = value * 8
            return ("glyph", value, cgram[offset : offset + 8])
        return ("text", value)

    def _draw_cell_content(self, index: int, value: int, cgram: bytes, tag: str) -> None:
        x0, y0, x1, y1 = self._cell_rect(index)
        if value != 0xE4 and value >= 0x80:
            self._draw_matrix(x0, y0, [0x1F] * 8, tag)
            return
        if value != 0xE4 and value <= 0x07:
            offset = value * 8
            self._draw_matrix(x0, y0, [row & 0x1F for row in cgram[offset : offset + 8]], tag)
            return

        if value == 0xE4:
            character = "µ"
        elif 32 <= value <= 126:
            character = chr(value)
        else:
            character = " "
        if character != " ":
            self.create_text(
                (x0 + x1) / 2,
                (y0 + y1) / 2,
                text=character,
                font=self._font,
                fill=LCD_PIXEL,
                tags=(tag,),
            )

    def _draw_matrix(self, x0: float, y0: float, rows: list[int], tag: str) -> None:
        pixel_w = 3.4 * self.ui_scale
        pixel_h = 2.8 * self.ui_scale
        gap_x = 1.3 * self.ui_scale
        gap_y = 1.0 * self.ui_scale
        matrix_w = pixel_w * 5 + gap_x * 4
        matrix_h = pixel_h * 8 + gap_y * 7
        start_x = x0 + (self.cell_w - matrix_w) / 2
        start_y = y0 + (self.cell_h - matrix_h) / 2

        for row_index, bits in enumerate(rows[:8]):
            for column in range(5):
                if not bits & (1 << (4 - column)):
                    continue
                px = start_x + column * (pixel_w + gap_x)
                py = start_y + row_index * (pixel_h + gap_y)
                self.create_rectangle(
                    px,
                    py,
                    px + pixel_w,
                    py + pixel_h,
                    fill=LCD_PIXEL,
                    outline=LCD_PIXEL,
                    tags=(tag,),
                )

    def _cell_rect(self, index: int) -> tuple[float, float, float, float]:
        row, column = divmod(index, 16)
        x0 = self.pad + column * (self.cell_w + self.gap_x)
        y0 = self.pad + row * (self.cell_h + self.gap_y)
        return x0, y0, x0 + self.cell_w, y0 + self.cell_h


class CompanionApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("MFJ-993B Remote Control")
        self.root.configure(bg=BG)
        self.root.resizable(False, False)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        settings = self._load_settings()
        self.ui_scale = self._clamp_ui_scale(settings.get("scale", 1.0))

        self.events: queue.Queue = queue.Queue()
        self.worker: WebSocketWorker | None = None
        self.worker_id = 0
        self.connected = False
        self.button_mask = INITIAL_BUTTON_MASK
        self.normalizer = MeterNormalizer()
        self.display_mode = "raw"
        self.pending_raw_job: str | None = None
        self.pending_raw_snapshot: LcdSnapshot | None = None
        self.snapshot_serial = 0
        self.reset_ack_job: str | None = None
        self.reset_waiting = False
        self.special_sequence_running = False
        self.active_momentary: set[int] = set()
        self.active_macro_pins: set[int] = set()
        self.config_window: tk.Toplevel | None = None
        self.reconnect_button: tk.Button | None = None

        self.button_widgets: dict[int, tk.Button] = {}
        self.scalable_buttons: list[tk.Button] = []
        self.status_var = tk.StringVar(value="Не подключено")
        self.scale_var = tk.StringVar(value=self._scale_text())
        self.host_var = tk.StringVar(value=str(settings.get("host") or DEFAULT_HOST))

        self._build_ui()
        self._update_all_button_visuals()

        self.root.bind_all("<ButtonRelease-1>", self._global_mouse_release, add="+")
        self.root.bind_all("<FocusOut>", self._focus_out, add="+")
        self.root.after(15, self._drain_events)
        self.root.after(80, self.connect)

    def _build_ui(self) -> None:
        outer = tk.Frame(self.root, bg=BG, padx=8, pady=8)
        outer.pack(fill="both", expand=True)

        self.lcd = LcdCanvas(outer, self.ui_scale)
        self.lcd.pack(pady=(0, 5))

        controls = tk.Frame(outer, bg=BG)
        controls.pack(fill="x")

        for column in range(5):
            controls.grid_columnconfigure(column, weight=1, uniform="main")

        main_layout = (
            (0, "ANT1", 0, 0),
            (1, "C-UP", 0, 1),
            (2, "L-UP", 0, 2),
            (3, "AUTO", 0, 3),
            (4, "MODE", 1, 0),
            (5, "C-DN", 1, 1),
            (6, "L-DN", 1, 2),
            (7, "TUNE", 1, 3),
        )

        for index, label, row, column in main_layout:
            button = self._make_button(controls, label, width=9, height=1)
            button.grid(row=row, column=column, padx=2, pady=2, sticky="nsew")
            self.button_widgets[index] = button
            if index in (0, 3):
                button.configure(command=lambda i=index: self.toggle_button(i))
            else:
                self._bind_momentary_button(button, (index,))

        reset = self._make_button(controls, "RESET LCD", self.reset_lcd_capture, width=9, height=1)
        reset.grid(row=0, column=4, padx=2, pady=2, sticky="nsew")

        power = self._make_button(controls, "POWER ON", self.toggle_power, width=9, height=1, danger=True)
        power.grid(row=1, column=4, padx=2, pady=2, sticky="nsew")
        self.button_widgets[8] = power

        self._make_button(
            outer,
            "CONFIG",
            self.show_config_window,
            width=14,
            height=1,
        ).pack(pady=(5, 0))

    def _make_button(
        self,
        parent: tk.Misc,
        text: str,
        command=None,
        *,
        width: int = 12,
        danger: bool = False,
        height: int = 2,
    ) -> tk.Button:
        button = tk.Button(
            parent,
            text=text,
            command=command,
            width=width,
            height=height,
            bg=DANGER_BG if danger else BUTTON_BG,
            fg=DANGER_FG if danger else BUTTON_FG,
            activebackground=ACTIVE_BG,
            activeforeground="white",
            relief="flat",
            bd=0,
            highlightthickness=1,
            highlightbackground=BUTTON_BORDER,
            font=("Segoe UI", self._scaled_font_size(8), "bold"),
            cursor="hand2",
            takefocus=False,
        )
        self.scalable_buttons.append(button)
        return button

    def _bind_momentary_button(self, button: tk.Button, pins: tuple[int, ...]) -> None:
        button.bind("<ButtonPress-1>", lambda event, p=pins: self._momentary_press(event, p))

    def _momentary_press(self, event: tk.Event, pins: tuple[int, ...]) -> str:
        for index in pins:
            self.active_momentary.add(index)
            self.set_button(index, True, send=False)
        self.send_buttons()
        return "break"

    def _global_mouse_release(self, _event: tk.Event | None = None) -> None:
        changed = False
        for index in tuple(self.active_momentary):
            self.active_momentary.discard(index)
            self.set_button(index, False, send=False)
            changed = True
        for index in tuple(self.active_macro_pins):
            self.active_macro_pins.discard(index)
            self.set_button(index, False, send=False)
            changed = True
        if changed:
            self.send_buttons()

    def _focus_out(self, event: tk.Event) -> None:
        self.root.after_idle(self._release_if_app_inactive)

    def _release_if_app_inactive(self) -> None:
        if self.root.focus_displayof() is None and not self.special_sequence_running:
            self._global_mouse_release()

    def connect(self) -> None:
        try:
            endpoint = WsEndpoint.parse(self.host_var.get())
        except ValueError as exc:
            messagebox.showwarning(APP_NAME, str(exc), parent=self.root)
            return

        self.host_var.set(self.host_var.get().strip())
        self._save_settings()
        if self.worker is not None:
            self.worker.stop()
        self.worker_id += 1
        self.worker = WebSocketWorker(self.worker_id, endpoint, self.events)
        self.worker.start()

    def send_buttons(self) -> None:
        worker = self.worker
        if worker is not None:
            worker.send_button_mask(self.button_mask)

    def set_button(self, index: int, active: bool, *, send: bool = True) -> None:
        if active:
            self.button_mask |= 1 << index
        else:
            self.button_mask &= ~(1 << index)
        self.button_mask &= 0x01FF
        self._update_button_visual(index)

        if index == 8:
            if active:
                self.lcd.show_text("", "")
            else:
                self.lcd.show_text("   POWER OFF", "")
        if send:
            self.send_buttons()

    def toggle_button(self, index: int) -> None:
        self.set_button(index, not bool(self.button_mask & (1 << index)))

    def toggle_power(self) -> None:
        is_on = bool(self.button_mask & (1 << 8))
        if is_on and not messagebox.askyesno("Питание тюнера", "Выключить MFJ-993B?", parent=self.root):
            return
        self.set_button(8, not is_on)

    def release_momentary(self, *, send: bool) -> None:
        self.button_mask &= ~MOMENTARY_MASK
        self.active_momentary.clear()
        self.active_macro_pins.clear()
        for index in MOMENTARY_INDICES:
            self._update_button_visual(index)
        if send:
            self.send_buttons()

    def _update_all_button_visuals(self) -> None:
        for index in range(9):
            self._update_button_visual(index)

    def _update_button_visual(self, index: int) -> None:
        button = self.button_widgets.get(index)
        if button is None:
            return
        active = bool(self.button_mask & (1 << index))
        if index == 0:
            button.configure(text="ANT2" if active else "ANT1")
        elif index == 3:
            button.configure(text="AUTO" if active else "MANUAL")
        elif index == 8:
            button.configure(text="POWER ON" if active else "POWER OFF")

        if active:
            button.configure(
                bg=POWER_ACTIVE_BG if index == 8 else ACTIVE_BG,
                fg="white",
                highlightbackground=POWER_ACTIVE_BORDER if index == 8 else ACTIVE_BORDER,
            )
        else:
            button.configure(
                bg=DANGER_BG if index == 8 else BUTTON_BG,
                fg=DANGER_FG if index == 8 else BUTTON_FG,
                highlightbackground=BUTTON_BORDER,
            )

    def _drain_events(self) -> None:
        try:
            while True:
                worker_id, event_type, payload = self.events.get_nowait()
                if event_type.startswith("ota_"):
                    self._handle_ota_event(event_type, payload)
                    continue
                if worker_id != self.worker_id:
                    continue
                if event_type == "connected":
                    self.connected = bool(payload)
                    if self.reconnect_button is not None and self.reconnect_button.winfo_exists():
                        self.reconnect_button.configure(
                            text="ПЕРЕПОДКЛЮЧИТЬ" if self.connected else "ПОДКЛЮЧИТЬ"
                        )
                    if self.connected:
                        self.send_buttons()
                    else:
                        self._cancel_reset_ack_wait()
                        self.release_momentary(send=False)
                elif event_type == "status":
                    self.status_var.set(str(payload))
                elif event_type == "snapshot":
                    self._handle_snapshot(payload)
                elif event_type == "text":
                    self._handle_text_message(str(payload))
        except queue.Empty:
            pass
        finally:
            if self.root.winfo_exists():
                self.root.after(15, self._drain_events)

    def _handle_snapshot(self, snapshot: LcdSnapshot) -> None:
        if not self.button_mask & (1 << 8):
            return

        self.snapshot_serial += 1
        is_main, processed, _evidence = self.normalizer.normalize(snapshot)

        if is_main:
            self._cancel_pending_raw()
            self.display_mode = "main"
            self.lcd.draw_snapshot(processed)
            return

        is_blank = all(value == SPACE for value in snapshot.screen)
        if self.display_mode == "main" and not is_blank:
            # Do not let one transient/corrupt push tear down the pinned meter.
            # Start one fixed 80 ms gate and keep replacing only the pending
            # frame.  Do not restart the timer for every live update: MODE
            # pages with changing values would otherwise never become visible.
            self.pending_raw_snapshot = snapshot
            if self.pending_raw_job is None:
                self.pending_raw_job = self.root.after(80, self._commit_pending_raw)
            return

        self._cancel_pending_raw()
        self.display_mode = "raw"
        self.lcd.draw_snapshot(snapshot)

    def _commit_pending_raw(self) -> None:
        self.pending_raw_job = None
        snapshot = self.pending_raw_snapshot
        self.pending_raw_snapshot = None
        if snapshot is None:
            return
        self.display_mode = "raw"
        self.lcd.draw_snapshot(snapshot)

    def _cancel_pending_raw(self) -> None:
        if self.pending_raw_job is not None:
            try:
                self.root.after_cancel(self.pending_raw_job)
            except tk.TclError:
                pass
            self.pending_raw_job = None
        self.pending_raw_snapshot = None

    def _clear_local_lcd_state(self) -> None:
        self._cancel_pending_raw()
        self.normalizer.reset()
        self.display_mode = "raw"
        self.snapshot_serial += 1
        self.lcd.show_text("", "")

    def reset_lcd_capture(self) -> None:
        if not self.connected or self.worker is None:
            messagebox.showwarning(
                "RESET LCD",
                "Нет соединения с ESP32. Сначала выполните подключение.",
                parent=self.config_window or self.root,
            )
            return

        if not self.worker.send_text(LCD_RESET_COMMAND):
            messagebox.showwarning(
                "RESET LCD",
                "Команда не отправлена: WebSocket уже отключён.",
                parent=self.config_window or self.root,
            )
            return

        self._cancel_reset_ack_wait()
        self._clear_local_lcd_state()
        self.reset_waiting = True
        self.status_var.set("Сброс автомата захвата LCD…")
        self.reset_ack_job = self.root.after(
            LCD_RESET_TIMEOUT_MS,
            self._reset_ack_timeout,
        )

    def _handle_text_message(self, message: str) -> None:
        if message.strip() != LCD_RESET_ACK:
            return

        self._cancel_reset_ack_wait()
        self.status_var.set("Захват LCD синхронизирован; ждём следующую перерисовку")

    def _reset_ack_timeout(self) -> None:
        self.reset_ack_job = None
        if not self.reset_waiting:
            return

        self.reset_waiting = False
        self.status_var.set("ESP32 не подтвердил RESET LCD")
        messagebox.showwarning(
            "RESET LCD",
            "ESP32 не подтвердил безопасный сброс захвата. "
            "Установите совместимую прошивку v1.2 или новее.",
            parent=self.config_window or self.root,
        )

    def _cancel_reset_ack_wait(self) -> None:
        if self.reset_ack_job is not None:
            try:
                self.root.after_cancel(self.reset_ack_job)
            except tk.TclError:
                pass
            self.reset_ack_job = None
        self.reset_waiting = False

    def show_config_window(self) -> None:
        if self.config_window is not None and self.config_window.winfo_exists():
            self.config_window.deiconify()
            self.config_window.lift()
            return

        window = tk.Toplevel(self.root)
        self.config_window = window
        window.title("MFJ-993B — CONFIG")
        window.configure(bg=BG)
        window.resizable(False, False)
        window.protocol("WM_DELETE_WINDOW", window.withdraw)

        style = ttk.Style(window)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("MFJ.TNotebook", background=BG, borderwidth=0)
        style.configure(
            "MFJ.TNotebook.Tab",
            background=BUTTON_BG,
            foreground=BUTTON_FG,
            borderwidth=0,
            padding=(14, 7),
            font=("Segoe UI", self._scaled_font_size(8), "bold"),
        )
        style.map(
            "MFJ.TNotebook.Tab",
            background=[("selected", ACTIVE_BG)],
            foreground=[("selected", "white")],
        )

        notebook = ttk.Notebook(window, style="MFJ.TNotebook")
        notebook.pack(padx=8, pady=8, fill="both", expand=True)

        settings_tab = tk.Frame(notebook, bg=BG, padx=12, pady=12)
        functions_tab = tk.Frame(notebook, bg=BG, padx=10, pady=9)
        notebook.add(settings_tab, text="НАСТРОЙКИ")
        notebook.add(functions_tab, text="ДОП. ФУНКЦИИ")

        self._build_settings_tab(settings_tab)
        self._build_functions_tab(functions_tab)

    def _build_settings_tab(self, container: tk.Frame) -> None:
        tk.Label(
            container,
            text="IP-АДРЕС ESP32",
            bg=BG,
            fg="#70b991",
            font=("Segoe UI", 9, "bold"),
        ).grid(row=0, column=0, columnspan=2, pady=(0, 5), sticky="w")

        host_entry = tk.Entry(
            container,
            textvariable=self.host_var,
            bg="#202020",
            fg="#eeeeee",
            insertbackground="white",
            relief="flat",
            width=32,
            font=("Consolas", 10),
        )
        host_entry.grid(row=1, column=0, columnspan=2, sticky="ew", ipady=6)
        host_entry.bind("<Return>", lambda _event: self.reconnect_from_config())

        self._make_button(
            container,
            "СОХРАНИТЬ IP",
            self.save_ip_setting,
            width=18,
            height=1,
        ).grid(row=2, column=0, padx=(0, 3), pady=(6, 3), sticky="ew")
        self.reconnect_button = self._make_button(
            container,
            "ПЕРЕПОДКЛЮЧИТЬ" if self.connected else "ПОДКЛЮЧИТЬ",
            self.reconnect_from_config,
            width=18,
            height=1,
        )
        self.reconnect_button.grid(row=2, column=1, padx=(3, 0), pady=(6, 3), sticky="ew")

        tk.Label(
            container,
            textvariable=self.status_var,
            bg=BG,
            fg="#888888",
            anchor="w",
            justify="left",
            wraplength=360,
            font=("Segoe UI", 8),
        ).grid(row=3, column=0, columnspan=2, sticky="ew", pady=(2, 12))

        tk.Label(
            container,
            text="МАСШТАБ LCD И КНОПОК",
            bg=BG,
            fg="#70b991",
            font=("Segoe UI", 9, "bold"),
        ).grid(row=4, column=0, columnspan=2, pady=(0, 5), sticky="w")

        scale_frame = tk.Frame(container, bg=BG)
        scale_frame.grid(row=5, column=0, columnspan=2, sticky="ew")
        scale_frame.grid_columnconfigure(0, weight=1)
        scale_frame.grid_columnconfigure(2, weight=1)
        self._make_button(
            scale_frame,
            "МАСШТАБ −",
            lambda: self.change_ui_scale(-UI_SCALE_STEP),
            width=15,
            height=1,
        ).grid(row=0, column=0, sticky="ew")
        tk.Label(
            scale_frame,
            textvariable=self.scale_var,
            bg=PANEL,
            fg="#eeeeee",
            width=7,
            pady=7,
            font=("Consolas", 10, "bold"),
        ).grid(row=0, column=1, padx=6)
        self._make_button(
            scale_frame,
            "МАСШТАБ +",
            lambda: self.change_ui_scale(UI_SCALE_STEP),
            width=15,
            height=1,
        ).grid(row=0, column=2, sticky="ew")

        service = tk.Frame(container, bg=BG)
        service.grid(row=6, column=0, columnspan=2, sticky="ew", pady=(14, 0))
        service.grid_columnconfigure(0, weight=1)
        service.grid_columnconfigure(1, weight=1)
        self._make_button(
            service,
            "СБРОС ЭКРАНА",
            self.reset_lcd_capture,
            width=18,
            height=1,
        ).grid(row=0, column=0, padx=(0, 3), sticky="ew")
        self._make_button(
            service,
            "ПРОШИВКА .INO.BIN",
            self.choose_firmware,
            width=18,
            height=1,
            danger=True,
        ).grid(row=0, column=1, padx=(3, 0), sticky="ew")

        tk.Label(
            container,
            text=f"MFJ-993B Companion {APP_VERSION}",
            bg=BG,
            fg="#555555",
            font=("Segoe UI", 8),
        ).grid(row=7, column=0, columnspan=2, pady=(14, 0))

        container.grid_columnconfigure(0, weight=1)
        container.grid_columnconfigure(1, weight=1)

    def _build_functions_tab(self, container: tk.Frame) -> None:
        tk.Label(
            container,
            text="Во время обычной работы",
            bg=BG,
            fg="#70b991",
            font=("Segoe UI", 9, "bold"),
        ).grid(row=0, column=0, columnspan=2, pady=(0, 4))

        normal_macros = (
            ("CAP INPUT/OUTPUT", (1, 5), False),
            ("SWR BEEP", (2, 6), False),
            ("C + L UP", (1, 2), False),
            ("BYPASS", (5, 6), False),
            ("TARGET SWR", (7, 1), False),
            ("AUTO TUNE SWR", (7, 2), False),
            ("MEMORY A-D", (7, 5), False),
            ("METER RANGE", (7, 6), False),
            ("POWER 300/150", (7, 1, 2), False),
            ("SAVE CURRENT", (7, 5, 6), True),
        )

        row = 1
        for item_index, (label, pins, danger) in enumerate(normal_macros):
            button = self._make_button(container, label, width=22, danger=danger)
            button.grid(row=row + item_index // 2, column=item_index % 2, padx=3, pady=3)
            self._bind_macro(button, pins, confirm_save=label == "SAVE CURRENT")
        row += (len(normal_macros) + 1) // 2

        self._make_button(container, "LC LIMIT (SETUP)", self.run_lc_limit, width=47, danger=True).grid(
            row=row, column=0, columnspan=2, padx=3, pady=(5, 2), sticky="ew"
        )
        row += 1
        tk.Label(
            container,
            text="LC LIMIT запускается только из уже открытого Setup Mode.",
            bg=BG,
            fg="#987777",
            font=("Segoe UI", 8),
        ).grid(row=row, column=0, columnspan=2, pady=(0, 7))
        row += 1

        tk.Label(
            container,
            text="Операции при включении",
            bg=BG,
            fg="#70b991",
            font=("Segoe UI", 9, "bold"),
        ).grid(row=row, column=0, columnspan=2, pady=(3, 4))
        row += 1

        power_operations = (
            ("FIRMWARE VERSION", (1,)),
            ("SELF TEST", (2,)),
            ("RELAY TEST", (5,)),
            ("POWER-DOWN TEST", (6,)),
            ("WATTMETER CAL", (1, 5)),
            ("AUDIO / VOLUME", (2, 6)),
            ("SWR BRIDGE CAL", (1, 2)),
            ("FREQ COUNTER CAL", (5, 6)),
            ("DELETE ANT MEMORY", (7, 5, 0)),
            ("FACTORY DEFAULTS", (7, 6)),
            ("TOTAL RESET", (7, 5, 6)),
        )
        for item_index, (label, pins) in enumerate(power_operations):
            button = self._make_button(
                container,
                label,
                lambda name=label, p=pins: self.run_power_sequence(name, p),
                width=22 if label != "TOTAL RESET" else 47,
                danger=True,
            )
            if label == "TOTAL RESET":
                button.grid(row=row + item_index // 2, column=0, columnspan=2, padx=3, pady=3, sticky="ew")
            else:
                button.grid(row=row + item_index // 2, column=item_index % 2, padx=3, pady=3)

    def save_ip_setting(self) -> None:
        try:
            WsEndpoint.parse(self.host_var.get())
        except ValueError as exc:
            messagebox.showwarning(APP_NAME, str(exc), parent=self.config_window or self.root)
            return
        self.host_var.set(self.host_var.get().strip())
        self._save_settings()
        self.status_var.set(f"IP сохранён: {self.host_var.get()}")

    def reconnect_from_config(self) -> None:
        self.save_ip_setting()
        try:
            WsEndpoint.parse(self.host_var.get())
        except ValueError:
            return
        self.connect()

    def change_ui_scale(self, delta: float) -> None:
        new_scale = self._clamp_ui_scale(round(self.ui_scale + delta, 2))
        if abs(new_scale - self.ui_scale) < 0.001:
            return
        self.ui_scale = new_scale
        self.scale_var.set(self._scale_text())
        self.lcd.set_scale(self.ui_scale)
        alive: list[tk.Button] = []
        for button in self.scalable_buttons:
            try:
                if button.winfo_exists():
                    button.configure(font=("Segoe UI", self._scaled_font_size(8), "bold"))
                    alive.append(button)
            except tk.TclError:
                pass
        self.scalable_buttons = alive
        self._save_settings()
        self.root.update_idletasks()

    def _bind_macro(self, button: tk.Button, pins: tuple[int, ...], *, confirm_save: bool) -> None:
        def press(event: tk.Event) -> str:
            if confirm_save and not messagebox.askyesno(
                "SAVE CURRENT",
                "Перезаписать текущей настройкой выбранную ячейку памяти тюнера?",
                parent=self.config_window or self.root,
            ):
                return "break"
            for pin in pins:
                self.active_macro_pins.add(pin)
                self.set_button(pin, True, send=False)
            self.send_buttons()
            return "break"

        button.bind("<ButtonPress-1>", press)

    def run_lc_limit(self) -> None:
        if self.special_sequence_running:
            return
        if not messagebox.askyesno(
            "LC LIMIT",
            "LC LIMIT является защитой тюнера. Выполнить MODE, затем C-UP + L-UP?",
            parent=self.config_window or self.root,
        ):
            return
        self.special_sequence_running = True
        self.set_button(4, True, send=False)
        self.send_buttons()

        def second_step() -> None:
            self.set_button(1, True, send=False)
            self.set_button(2, True, send=False)
            self.send_buttons()
            self.root.after(700, finish)

        def finish() -> None:
            for pin in (1, 2, 4):
                self.set_button(pin, False, send=False)
            self.send_buttons()
            self.special_sequence_running = False

        self.root.after(250, second_step)

    def run_power_sequence(self, name: str, pins: tuple[int, ...]) -> None:
        if self.special_sequence_running:
            return
        warning = {
            "FIRMWARE VERSION": "Выключить и включить тюнер для показа версии прошивки?",
            "SELF TEST": "Запустить SELF TEST? Он длится около 30 секунд и сбрасывает настройки к заводским.",
            "RELAY TEST": "Запустить тест реле? Перед продолжением проверьте условия из руководства.",
            "POWER-DOWN TEST": "Запустить тест схемы выключения питания?",
            "WATTMETER CAL": "Запустить калибровку ваттметра? Не продолжайте без измерительного оборудования.",
            "AUDIO / VOLUME": "Включить тестовый звук для настройки громкости?",
            "SWR BRIDGE CAL": "Запустить калибровку КСВ-моста? Не продолжайте без измерительного оборудования.",
            "FREQ COUNTER CAL": "Запустить калибровку частотомера? Не продолжайте без измерительного оборудования.",
            "DELETE ANT MEMORY": "Удалить память выбранной антенны? После DELETE ANTENNA: C-UP = YES, L-UP = NO.",
            "FACTORY DEFAULTS": "Вернуть заводские настройки? Память антенн при этом не стирается.",
            "TOTAL RESET": "TOTAL RESET удалит память ОБЕИХ антенн и вернёт заводские настройки.",
        }.get(name, "Выполнить выбранную операцию при включении?")

        if not messagebox.askyesno(name, warning, parent=self.config_window or self.root):
            return

        self.special_sequence_running = True
        self.release_momentary(send=False)
        self.set_button(8, False, send=False)
        self.send_buttons()

        def hold_keys() -> None:
            for pin in pins:
                self.set_button(pin, True, send=False)
            self.send_buttons()
            self.root.after(250, power_on)

        def power_on() -> None:
            self.set_button(8, True, send=False)
            self.send_buttons()
            self.root.after(1800, finish)

        def finish() -> None:
            for pin in pins:
                self.set_button(pin, False, send=False)
            self.send_buttons()
            self.special_sequence_running = False

        self.root.after(2200, hold_keys)

    def choose_firmware(self) -> None:
        parent = self.config_window or self.root
        file_name = filedialog.askopenfilename(
            parent=parent,
            title="Выберите основной файл прошивки",
            filetypes=(("ESP32 firmware", "*.ino.bin"), ("Binary files", "*.bin")),
        )
        if not file_name:
            return
        if not file_name.lower().endswith(".ino.bin"):
            messagebox.showwarning("Обновление", "Нужен основной файл с окончанием .ino.bin", parent=parent)
            return
        if not messagebox.askyesno(
            "Обновление прошивки",
            f"Передать {Path(file_name).name} на ESP32?\n\nНе отключайте питание до завершения.",
            parent=parent,
        ):
            return
        try:
            endpoint = WsEndpoint.parse(self.host_var.get())
        except ValueError as exc:
            messagebox.showwarning("Обновление", str(exc), parent=parent)
            return

        self.release_momentary(send=True)
        thread = threading.Thread(
            target=self._upload_firmware,
            args=(endpoint, Path(file_name)),
            name="MFJ993B-HTTP-Update",
            daemon=True,
        )
        thread.start()

    def _upload_firmware(self, endpoint: WsEndpoint, file_path: Path) -> None:
        boundary = "----MFJ993B" + secrets.token_hex(12)
        prefix = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="firmware"; filename="{file_path.name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode("utf-8")
        suffix = f"\r\n--{boundary}--\r\n".encode("ascii")
        file_size = file_path.stat().st_size
        content_length = len(prefix) + file_size + len(suffix)
        connection_class = http.client.HTTPSConnection if endpoint.scheme == "wss" else http.client.HTTPConnection
        connection = connection_class(endpoint.host, endpoint.port, timeout=300)

        try:
            connection.putrequest("POST", "/update")
            connection.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
            connection.putheader("Content-Length", str(content_length))
            connection.putheader("Connection", "close")
            connection.putheader("User-Agent", f"{APP_NAME}/{APP_VERSION}")
            connection.endheaders()
            connection.send(prefix)

            sent = 0
            with file_path.open("rb") as source:
                while True:
                    chunk = source.read(64 * 1024)
                    if not chunk:
                        break
                    connection.send(chunk)
                    sent += len(chunk)
                    percent = int(sent * 100 / max(1, file_size))
                    self.events.put((0, "ota_progress", percent))

            connection.send(suffix)
            response = connection.getresponse()
            body = response.read().decode("utf-8", errors="replace").strip()
            if response.status != 200:
                raise RuntimeError(body or f"HTTP {response.status} {response.reason}")
            self.events.put((0, "ota_done", body or "OK. ESP32 перезагружается."))
        except Exception as exc:  # noqa: BLE001 - show complete upload failure to user
            self.events.put((0, "ota_error", str(exc)))
        finally:
            connection.close()

    def _handle_ota_event(self, event_type: str, payload: object) -> None:
        if event_type == "ota_progress":
            self.status_var.set(f"Передача прошивки: {payload}%")
        elif event_type == "ota_done":
            self.status_var.set("Прошивка передана. ESP32 перезагружается…")
            messagebox.showinfo("Обновление завершено", str(payload), parent=self.config_window or self.root)
        elif event_type == "ota_error":
            self.status_var.set("Ошибка обновления")
            messagebox.showerror("Ошибка обновления", str(payload), parent=self.config_window or self.root)

    def close(self) -> None:
        self._cancel_reset_ack_wait()
        self._cancel_pending_raw()
        self.release_momentary(send=True)
        if self.worker is not None:
            time.sleep(0.03)
            self.worker.stop()
        self.root.destroy()

    @staticmethod
    def _settings_path() -> Path:
        base = os.environ.get("LOCALAPPDATA") or str(Path.home())
        return Path(base) / "MFJ993B Companion" / "settings.json"

    @staticmethod
    def _clamp_ui_scale(value: object) -> float:
        try:
            numeric = float(value)
        except (TypeError, ValueError):
            numeric = 1.0
        return max(MIN_UI_SCALE, min(MAX_UI_SCALE, round(numeric, 2)))

    def _scale_text(self) -> str:
        return f"{round(self.ui_scale * 100):d}%"

    def _scaled_font_size(self, base_size: int) -> int:
        return max(6, round(base_size * self.ui_scale))

    def _load_settings(self) -> dict[str, object]:
        defaults: dict[str, object] = {"host": DEFAULT_HOST, "scale": 1.0}
        try:
            data = json.loads(self._settings_path().read_text(encoding="utf-8"))
            if not isinstance(data, dict):
                return defaults
            return {
                "host": str(data.get("host") or DEFAULT_HOST),
                "scale": self._clamp_ui_scale(data.get("scale", 1.0)),
            }
        except (OSError, ValueError, TypeError):
            return defaults

    def _save_settings(self) -> None:
        try:
            path = self._settings_path()
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                json.dumps(
                    {
                        "host": self.host_var.get().strip() or DEFAULT_HOST,
                        "scale": self.ui_scale,
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
        except OSError:
            pass


def main() -> int:
    root = tk.Tk()
    CompanionApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
