# MFJ-993B Companion v1.2.2

Native Tkinter client for Windows x64. It uses the firmware WebSocket endpoint directly and does not embed a browser. Version 1.2.2 shows the firmware actually running on the remote ESP32 after every connection and OTA restart.

## Run from source

Install 64-bit Python 3 with Tkinter, then run:

```bat
py -3 MFJ993B_Companion.py
```

No third-party Python packages are required.

## Wire protocol

- ESP32 to client: 98-byte binary packet `0xFD + 32 DDRAM + 64 CGRAM + revision`.
- Client to ESP32: `Bxxxxxxxxx` for the nine button states.
- Safe LCD capture reset: text command `R`.
- Reset acknowledgement: text message `LCD_RESET_OK`.
- Firmware identity after connection: `FW:<version>`.

`RESET LCD` does **not** toggle POWER or any other tuner input. It clears the client's local formatter state and asks core 1 of the ESP32 to reset only the capture decoder. The firmware preserves CGRAM and waits for a clean RS command/data boundary before accepting more nibbles.

A reset cannot reconstruct bytes that the PIC is not currently transmitting. After acknowledgement, wait for the next normal physical LCD redraw or change screens using the tuner controls.

## Tests

```bat
py -3 -m unittest discover -s tests -v
```

The tests cover the 98-byte LCD frame, button-mask ordering, shifted main-screen recovery, raw MODE pages, and the dedicated reset command without any POWER-mask change.
