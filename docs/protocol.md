# LCD capture, browser rendering and network protocol

This document describes the current firmware, including the fixed main meter layout and browser-based firmware upload.

## LCD capture

The target is a classic dual-core ESP32 running at a fixed 240 MHz CPU frequency.

1. Core 1 watches `GPIO_IN_REG` for LCD `E` high.
2. After `SAMPLE_DELAY = 110` CPU cycles, RS and DB4-DB7 are read from one GPIO register sample.
3. The loop waits for `E` low so one enable pulse is handled once.
4. Two nibbles with the same RS state are combined into one byte.
5. A gap longer than 5000 µs or an RS change discards only an incomplete nibble pair. Previously accepted LCD state and the selected address space are preserved.
6. Commands handle clear, home, entry direction, visible DDRAM addresses and CGRAM addresses.
7. The visible DDRAM ranges are `0x00-0x0F` and `0x40-0x4F`.
8. CGRAM stores eight custom 5x8 glyphs: 64 rows in total. A row update preserves the other seven rows exactly as the LCD controller does.

The second immediate GPIO sample is diagnostic only. A difference increments an internal counter but does not replace the first sample or roll back an entire run.

The 110-cycle point depends on the exact ESP32 model, fixed CPU clock, wiring, level shifter and tuner board. Changing any of them may require a new terminal capture test.

## Snapshot readiness

The firmware tracks the last real DDRAM content change. A web snapshot waits 12 ms after that change so a slow character-by-character text update is not sent halfway through.

CGRAM activity does not extend this delay. This is important on the working meter screen because the tuner may rewrite custom-character rows while frequency, SWR, `FWD` and `REF` are already ready.

Repeated writes of the same DDRAM byte do not restart the delay.

## Browser rendering

- Printable bytes are rendered as text.
- `0xE4` is rendered as `µ`.
- CGRAM character codes `0x00-0x07` are drawn as 5x8 dot matrices.
- Code `0x00` is intentionally drawn as a fully filled character cell for the solid tuning bar seen on this tuner.
- Tuning-bar, L/C, Setup and service screens are displayed from their captured 32-byte layout.

### Fixed main meter screen

When the browser recognizes the screen containing `MHz`, `FWD=` and `REF=`, it uses these fixed cells:

| Row | Cells (1-based) | Content |
|---:|---:|---|
| 1 | 1-6 | Frequency |
| 1 | 7-9 | `MHz` |
| 1 | 10-12 | Three CGRAM indicators |
| 1 | 13 | Space |
| 1 | 14-16 | SWR |
| 2 | 1-4 | `FWD=` |
| 2 | 5-7 | Forward value |
| 2 | 8-9 | Spaces |
| 2 | 10-13 | `REF=` |
| 2 | 14-16 | Reflected value |

Each numeric field is extracted separately. A field is updated only when it has a valid meter format; otherwise the last valid field remains visible. Consequently, one damaged byte cannot shift the complete virtual screen.

The three custom indicators use a separate CGRAM cache. A new glyph set is accepted only after two identical snapshots, which suppresses partial row-by-row rewrites. CGRAM remains fully live on all other screens.

## Polling and flow control

- Normal request delay: 20 ms.
- While MODE, TUNE, C-UP, C-DN, L-UP or L-DN is held: 100 ms.
- At most one LCD request is in flight.
- The next request is scheduled only after a response or a 500 ms watchdog timeout.
- After a momentary control is released, a response requested before release is discarded and a fresh snapshot is requested.

This provides current values without accumulating obsolete WebSocket frames on slow Wi-Fi or through a VPN.

## WebSocket endpoint

Endpoint: `/ws`

### Browser to ESP32

| Message | Format | Meaning |
|---|---|---|
| LCD request | ASCII `L` | Request one current DDRAM+CGRAM snapshot |
| Buttons | ASCII `Bxxxxxxxxx` | Nine `0`/`1` states in `BTN_PINS[]` order |

Button order:

```text
0 ANT, 1 C-UP, 2 L-UP, 3 AUTO, 4 MODE,
5 C-DN, 6 L-DN, 7 TUNE, 8 POWER
```

### ESP32 to browser

#### LCD snapshot — 98 bytes

| Offset | Size | Content |
|---:|---:|---|
| 0 | 1 | Packet type `0xFD` |
| 1 | 32 | Visible DDRAM: first row, then second row |
| 33 | 64 | CGRAM: 8 glyphs × 8 rows |
| 97 | 1 | LCD revision counter modulo 256 |

#### Wait response — 1 byte

`0xFC` means DDRAM changed less than 12 ms ago. The browser clears the pending request and retries after 12 ms. It is flow control, not an LCD frame.

There is no WebSocket command for firmware-update mode in the current version.

## HTTP endpoints

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | Main remote-control page, or Wi-Fi form in configuration AP mode |
| `POST` | `/save` | Store Wi-Fi form fields `s` and `p`, then restart |
| `GET` | `/update` | Firmware upload page |
| `POST` | `/update` | Stream the selected application image to the ESP32 update partition |
| WebSocket | `/ws` | LCD snapshots and button states |

The update page accepts only a filename ending in `.ino.bin` in its browser UI. The server writes the application image with `Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)`, validates completion with `Update.end(true)` and restarts approximately two seconds after a successful response. An inactive upload is aborted after 60 seconds.

## Wi-Fi behavior

- Credentials are stored by `Preferences` in namespace `wifi`, keys `s` and `p`.
- Station mode tries to connect 20 times at 500 ms intervals.
- On failure, the ESP32 starts access point `MFJ993b-CONFIG` with password `12345678`.
- `POST /save` stores the submitted SSID/password and restarts.
- `WiFi.setSleep(false)` is used to reduce latency and missed activity.

## Security

The HTTP pages, WebSocket commands and firmware upload have no authentication or transport encryption. Keep the device on a trusted LAN or trusted VPN. Do not publish port 80 directly to the Internet.
