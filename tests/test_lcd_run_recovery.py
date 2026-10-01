from __future__ import annotations

import hashlib
import re
import unittest
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = (
    ROOT
    / "firmware"
    / "MFJ993B_Remote_Control"
    / "MFJ993B_Remote_Control.ino"
)
MAIN_CAPTURE_LOOP_SHA256 = (
    "e2b5c624c76a2f828b1c87c3c17b5edc"
    "f13095fa96564f9cdd35ee585a1f6c76"
)


@dataclass
class DecoderModel:
    screen: bytearray = field(default_factory=lambda: bytearray(b" " * 32))
    address_space: str = "none"
    address: int = 0
    increment: bool = True
    stage: int = 0
    first_nibble: int = 0
    last_rs: bool = False
    last_time: int = 0
    checkpoint: bytes | None = None
    checkpoint_increment: bool = True
    rollbacks: int = 0

    def begin(self) -> None:
        self.checkpoint = bytes(self.screen)
        self.checkpoint_increment = self.increment

    def commit(self) -> None:
        self.checkpoint = None

    def rollback(self) -> None:
        if self.checkpoint is None:
            return
        self.screen[:] = self.checkpoint
        self.increment = self.checkpoint_increment
        self.checkpoint = None
        self.address_space = "none"
        self.rollbacks += 1

    @staticmethod
    def position(address: int) -> int | None:
        if 0x00 <= address <= 0x0F:
            return address
        if 0x40 <= address <= 0x4F:
            return 16 + address - 0x40
        return None

    def process_byte(self, value: int, rs: bool) -> None:
        if not rs:
            if value == 0x01:
                self.screen[:] = b" " * 32
                self.address = 0
                self.address_space = "ddram"
            elif value == 0x02:
                self.address = 0
                self.address_space = "ddram"
            elif value & 0xFC == 0x04:
                self.increment = bool(value & 0x02)
            elif 0x80 <= value <= 0x8F or 0xC0 <= value <= 0xCF:
                self.address = value & 0x7F
                self.address_space = "ddram"
            return

        if self.address_space != "ddram":
            return
        position = self.position(self.address)
        if position is None:
            self.address_space = "none"
            return
        self.screen[position] = value
        self.address = (self.address + (1 if self.increment else -1)) & 0x7F
        if self.position(self.address) is None:
            self.address_space = "none"

    def nibble(self, value: int, rs: bool, now: int) -> None:
        if self.last_time and now - self.last_time > 5000:
            self.rollback() if self.stage else self.commit()
            self.stage = 0

        if rs != self.last_rs:
            self.rollback() if self.stage else self.commit()
            self.stage = 0

        if self.checkpoint is None:
            self.begin()

        self.last_rs = rs
        self.last_time = now
        if self.stage == 0:
            self.first_nibble = value
            self.stage = 1
            return
        byte = self.first_nibble << 4 | value
        self.stage = 0
        self.process_byte(byte, rs)


class DecoderRecoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.decoder = DecoderModel()
        self.now = 0

    def nibble(self, value: int, rs: bool) -> None:
        self.now += 50
        self.decoder.nibble(value, rs, self.now)

    def byte(self, value: int, rs: bool) -> None:
        self.nibble(value >> 4, rs)
        self.nibble(value & 0x0F, rs)

    def test_normal_capture_is_unchanged(self) -> None:
        self.byte(0x80, False)
        for value in b"NORMAL CAPTURE  ":
            self.byte(value, True)
        self.byte(0xC0, False)
        for value in b"SECOND ROW OK   ":
            self.byte(value, True)

        self.assertEqual(self.decoder.screen[:16], b"NORMAL CAPTURE  ")
        self.assertEqual(self.decoder.screen[16:], b"SECOND ROW OK   ")
        self.assertEqual(self.decoder.rollbacks, 0)

    def test_lost_nibble_rolls_back_bad_run_then_recovers(self) -> None:
        original = b"LAST GOOD ROW   "
        self.decoder.screen[:16] = original

        self.byte(0x80, False)
        self.byte(ord("A"), True)
        self.byte(ord("B"), True)

        # Intended C,D nibbles: 4,3,4,4.  The low nibble 3 is lost.
        self.nibble(0x4, True)
        self.nibble(0x4, True)
        self.nibble(0x4, True)

        # RS transition proves that the preceding DATA run was odd.
        self.byte(0xC0, False)
        self.byte(ord("O"), True)
        self.byte(ord("K"), True)

        self.assertEqual(self.decoder.screen[:16], original)
        self.assertEqual(self.decoder.screen[16:18], b"OK")
        self.assertEqual(self.decoder.rollbacks, 1)


class FirmwareInvariantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = FIRMWARE.read_text(encoding="utf-8")

    def test_proven_main_capture_loop_is_byte_for_byte_unchanged(self) -> None:
        capture_loop = self.source[self.source.index("void loop()"):].rstrip()
        digest = hashlib.sha256(capture_loop.encode()).hexdigest()
        self.assertEqual(digest, MAIN_CAPTURE_LOOP_SHA256)

    def test_sample_mismatch_remains_diagnostic_only(self) -> None:
        self.assertIn("const uint32_t SAMPLE_DELAY = 90;", self.source)
        self.assertIn("uint32_t reg = s1;", self.source)
        diagnostic = re.search(
            r"if \(sampleUnstable\) \{(?P<body>.*?)\}\s*bool currentRs",
            self.source,
            re.S,
        )
        self.assertIsNotNone(diagnostic)
        self.assertIn("sampleDifferenceCounter++;", diagnostic.group("body"))
        self.assertNotIn("return", diagnostic.group("body"))

    def test_recover_command_cannot_touch_power_or_buttons(self) -> None:
        start = self.source.index("data[0] == 'R'")
        block = self.source[start : start + 650]
        self.assertIn("lcdRecoveryRequested", block)
        self.assertNotIn("applyButtonMask", block)
        self.assertNotIn("digitalWrite", block)
        self.assertNotIn("BTN_PINS", block)


if __name__ == "__main__":
    unittest.main(verbosity=2)
