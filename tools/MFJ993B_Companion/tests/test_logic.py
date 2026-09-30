from __future__ import annotations

import socket
import sys
import unittest
from pathlib import Path
from queue import Queue


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from MFJ993B_Companion import (  # noqa: E402
    CompanionApp,
    INITIAL_BUTTON_MASK,
    LCD_PACKET_LENGTH,
    LCD_RESET_ACK,
    LCD_RESET_COMMAND,
    LCD_RESET_TIMEOUT_MS,
    LcdSnapshot,
    MeterNormalizer,
    WebSocketWorker,
    WsEndpoint,
)


def meter_snapshot(revision: int = 7) -> LcdSnapshot:
    row1 = b"14.200MHz" + bytes((5, 7, 6)) + b" " + b"1,2"
    row2 = b"FWD=1.2  REF=0.1"
    assert len(row1) == 16
    assert len(row2) == 16
    return LcdSnapshot(row1 + row2, bytes(64), revision)


class SnapshotTests(unittest.TestCase):
    def test_exact_wire_packet(self) -> None:
        source = meter_snapshot(0xA5)
        packet = bytes((0xFD,)) + source.screen + source.cgram + bytes((source.revision,))
        self.assertEqual(len(packet), LCD_PACKET_LENGTH)
        self.assertEqual(LcdSnapshot.parse(packet), source)
        self.assertIsNone(LcdSnapshot.parse(packet[:-1]))
        self.assertIsNone(LcdSnapshot.parse(b"\x00" + packet[1:]))


class MeterNormalizerTests(unittest.TestCase):
    def test_canonical_meter_is_pinned(self) -> None:
        is_main, output, evidence = MeterNormalizer().normalize(meter_snapshot())
        self.assertTrue(is_main)
        self.assertEqual(output.screen, meter_snapshot().screen)
        self.assertIn("MHz@6", evidence)
        self.assertIn("FWD@16", evidence)
        self.assertIn("REF@25", evidence)

    def test_whole_screen_rotation_is_recovered(self) -> None:
        source = meter_snapshot()
        shift = 11
        rotated = source.screen[shift:] + source.screen[:shift]
        is_main, output, _ = MeterNormalizer().normalize(
            LcdSnapshot(rotated, source.cgram, source.revision)
        )
        self.assertTrue(is_main)
        self.assertEqual(output.screen, source.screen)

    def test_anchor_can_cross_physical_row_boundary(self) -> None:
        source = meter_snapshot()
        # FWD begins at byte 30 after this rotation and therefore wraps across
        # the 31 -> 0 boundary.  A per-row search would lose it.
        shift = 18
        rotated = source.screen[shift:] + source.screen[:shift]
        is_main, output, evidence = MeterNormalizer().normalize(
            LcdSnapshot(rotated, source.cgram, source.revision)
        )
        self.assertTrue(is_main)
        self.assertEqual(output.screen, source.screen)
        self.assertIn("FWD@30", evidence)

    def test_one_damaged_anchor_byte_uses_learned_rotation(self) -> None:
        source = meter_snapshot()
        damaged = bytearray(source.screen)
        damaged[16] = ord("X")
        normalizer = MeterNormalizer()
        is_main, output, evidence = normalizer.normalize(
            LcdSnapshot(bytes(damaged), source.cgram, source.revision)
        )
        self.assertTrue(is_main)
        self.assertEqual(output.screen[20:23], b"1.2")
        self.assertIn("FWD~@16", evidence)

    def test_non_meter_screen_is_not_reformatted(self) -> None:
        source = LcdSnapshot(b"SETUP MODE      " + b"CAP LIMIT       ", bytes(64), 4)
        is_main, output, _ = MeterNormalizer().normalize(source)
        self.assertFalse(is_main)
        self.assertIs(output, source)

    def test_mode_screen_with_stale_power_labels_stays_raw(self) -> None:
        # A MODE page can inherit valid-looking FWD/REF text from DDRAM.  It
        # must not be collapsed back to the formatted main meter unless the
        # complete meter structure (including CGRAM digits) is present.
        row1 = b"14.200MHz POWER "
        row2 = b"FWD=1.2  REF=0.1"
        self.assertEqual(len(row1), 16)
        source = LcdSnapshot(row1 + row2, bytes(64), 8)
        is_main, output, _ = MeterNormalizer().normalize(source)
        self.assertFalse(is_main)
        self.assertIs(output, source)


class ProtocolTests(unittest.TestCase):
    def test_endpoint_default(self) -> None:
        endpoint = WsEndpoint.parse("192.168.2.124")
        self.assertEqual((endpoint.scheme, endpoint.host, endpoint.port, endpoint.path),
                         ("ws", "192.168.2.124", 80, "/ws"))

    def test_button_frame_matches_firmware_protocol(self) -> None:
        sender, receiver = socket.socketpair()
        self.addCleanup(sender.close)
        self.addCleanup(receiver.close)

        worker = WebSocketWorker(1, WsEndpoint.parse("127.0.0.1"), Queue())
        worker._socket = sender
        self.assertTrue(worker.send_button_mask(INITIAL_BUTTON_MASK))

        header = receiver.recv(2)
        self.assertEqual(header[0], 0x81)
        self.assertTrue(header[1] & 0x80)
        length = header[1] & 0x7F
        mask = receiver.recv(4)
        payload = receiver.recv(length)
        decoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        self.assertEqual(decoded, b"B000100001")


class ResetCaptureTests(unittest.TestCase):
    def test_reset_uses_dedicated_command_without_changing_power(self) -> None:
        class FakeRoot:
            def __init__(self) -> None:
                self.after_calls: list[tuple[int, object]] = []
                self.cancelled: list[str] = []

            def after(self, delay: int, callback):
                self.after_calls.append((delay, callback))
                return "reset-job"

            def after_cancel(self, job: str) -> None:
                self.cancelled.append(job)

        class FakeWorker:
            def __init__(self) -> None:
                self.messages: list[str] = []

            def send_text(self, message: str) -> bool:
                self.messages.append(message)
                return True

        class FakeNormalizer:
            def __init__(self) -> None:
                self.reset_count = 0

            def reset(self) -> None:
                self.reset_count += 1

        class FakeLcd:
            def __init__(self) -> None:
                self.text: tuple[str, str] | None = None

            def show_text(self, row1: str, row2: str) -> None:
                self.text = (row1, row2)

        class FakeVar:
            def __init__(self) -> None:
                self.value = ""

            def set(self, value: str) -> None:
                self.value = value

        app = CompanionApp.__new__(CompanionApp)
        app.connected = True
        app.worker = FakeWorker()
        app.root = FakeRoot()
        app.config_window = None
        app.pending_raw_job = None
        app.pending_raw_snapshot = None
        app.normalizer = FakeNormalizer()
        app.display_mode = "main"
        app.snapshot_serial = 0
        app.lcd = FakeLcd()
        app.status_var = FakeVar()
        app.reset_ack_job = None
        app.reset_waiting = False
        original_mask = INITIAL_BUTTON_MASK | 1
        app.button_mask = original_mask

        app.reset_lcd_capture()

        self.assertEqual(app.worker.messages, [LCD_RESET_COMMAND])
        self.assertEqual(app.button_mask, original_mask)
        self.assertEqual(app.lcd.text, ("", ""))
        self.assertTrue(app.reset_waiting)
        self.assertEqual(app.root.after_calls[0][0], LCD_RESET_TIMEOUT_MS)

        app._handle_text_message(LCD_RESET_ACK)
        self.assertFalse(app.reset_waiting)
        self.assertEqual(app.root.cancelled, ["reset-job"])


class ModeTransitionTests(unittest.TestCase):
    def test_live_mode_updates_do_not_restart_transition_timer(self) -> None:
        class FakeRoot:
            def __init__(self) -> None:
                self.callbacks: list[object] = []

            def after(self, _delay: int, callback):
                self.callbacks.append(callback)
                return "job-1"

            def after_cancel(self, _job: str) -> None:
                self.callbacks.clear()

        class FakeLcd:
            def __init__(self) -> None:
                self.frames: list[LcdSnapshot] = []

            def draw_snapshot(self, frame: LcdSnapshot) -> None:
                self.frames.append(frame)

        class RawNormalizer:
            @staticmethod
            def normalize(frame: LcdSnapshot):
                return False, frame, ""

        first = LcdSnapshot(b"MODE PAGE 1     " + b"VALUE=1         ", bytes(64), 1)
        second = LcdSnapshot(b"MODE PAGE 1     " + b"VALUE=2         ", bytes(64), 2)
        app = CompanionApp.__new__(CompanionApp)
        app.button_mask = INITIAL_BUTTON_MASK
        app.snapshot_serial = 0
        app.normalizer = RawNormalizer()
        app.display_mode = "main"
        app.pending_raw_job = None
        app.pending_raw_snapshot = None
        app.root = FakeRoot()
        app.lcd = FakeLcd()

        app._handle_snapshot(first)
        app._handle_snapshot(second)

        self.assertEqual(len(app.root.callbacks), 1)
        self.assertIs(app.pending_raw_snapshot, second)
        app.root.callbacks[0]()
        self.assertEqual(app.display_mode, "raw")
        self.assertEqual(app.lcd.frames, [second])


if __name__ == "__main__":
    unittest.main()
