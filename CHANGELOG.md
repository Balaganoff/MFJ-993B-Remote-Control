# Changelog

## 2026-10-01

- Version 1.2.2 restores the measured capture path: the first delayed sample `s1` drives the decoder and `s2` is diagnostic only.
- An `s1`/`s2` disagreement no longer deletes a nibble, clears the LCD address state or locks the decoder until an RS transition.
- Timeout, RS-change and manual reset recovery now discard only an incomplete byte; manual reset still never changes POWER or button GPIOs.
- `/status` now exposes byte, address-command, accepted/rejected-data, timeout, RS-reset and live decoder-state counters.
- Added regression coverage for the proven first-sample path and non-destructive capture recovery.

## 2026-09-30

- Version 1.2.1: accept the measured short `E` pulse while still requiring both immediate RS/DB4-DB7 reads to agree; this fixes the blank browser LCD introduced by the over-strict `E` gate.
- Added `GET /status` and a WebSocket `FW:<version>` greeting for remote post-OTA verification.
- Reject LCD samples unless both immediate RS/DB4-DB7 reads agree.
- Recover nibble phase only at an unambiguous RS command/data boundary after a rejected pulse.
- Added WebSocket command `R` for a capture-only reset and `LCD_RESET_OK` acknowledgement.
- The capture reset never changes button GPIOs or tuner power and preserves CGRAM during a manual reset.
- Added host regression coverage for reset, CGRAM preservation, power-mask preservation and RS-boundary recovery.

## 2026-09-11

- Replaced Arduino IDE network-port update mode with browser upload at `/update`.
- Added progress reporting, application-image validation and automatic restart after HTTP upload.
- Fixed blank virtual LCD caused by treating continuous LCD-bus activity as a permanently busy display.
- Snapshot readiness now follows real DDRAM changes; repeated data and CGRAM animation do not block frames.
- Restored the proven nibble synchronizer that discards only an incomplete byte without rolling back accepted screen data.
- Added fixed-cell normalization for the main frequency/SWR/FWD/REF screen.
- Main-screen values are validated and updated independently, retaining the last valid field when a captured byte is damaged.
- Main-screen custom indicators are committed only after two identical CGRAM snapshots.
- Changed normal browser polling from 25 ms to 20 ms; held-button polling remains 100 ms.
- Updated README, protocol, wiring, controls and firmware-update documentation.
- GitHub Actions now publishes a correctly named `.ino.bin` browser-update artifact after a successful build.
- Added interface screenshots for the main meter, manual L/C screen, button combinations, firmware update and Wi-Fi recovery setup.
- Added the measured PIC-to-LCD bus behavior, 74LVC244A input stage, PC817 button interfaces and power-relay wiring diagram.
- Added a dedicated Wi-Fi fallback and recovery guide matching the actual configuration page.
- Added a scoped MFJ-998 adaptation note: the LCD-capture approach is reusable, but wiring, controls, timing and firmware require model-specific verification and changes.

## 2026-09-09

- Added the standalone `LCD1602_CGRAM_Terminal_110` diagnostic sketch.
- Added the initial project documentation, PlatformIO environment and GitHub Actions build.
