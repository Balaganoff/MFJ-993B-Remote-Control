# Changelog

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
