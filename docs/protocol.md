# LCD capture and WebSocket protocol

## LCD capture

The current target is a classic ESP32 running at a fixed 240 MHz CPU frequency.

1. Core 1 watches `GPIO_IN_REG` for LCD E high.
2. After `SAMPLE_DELAY = 110` CPU cycles, RS and DB4-DB7 are read in one register sample.
3. The loop waits for E low so that one enable pulse is processed once.
4. Two nibbles with the same RS value are combined into one byte.
5. A pause longer than 5000 µs discards only an incomplete nibble pair.
6. Commands select visible DDRAM, CGRAM, clear/home, and entry direction.
7. DDRAM mirrors the two visible ranges `0x00-0x0F` and `0x40-0x4F`.
8. CGRAM stores eight custom 5x8 glyphs, 64 rows total. Partial row updates preserve the other rows exactly like the LCD controller.

The sampling delay is hardware- and CPU-frequency-sensitive. Changing the ESP32 target, CPU clock, wiring length, level shifter, or MFJ board may require retuning it.

## Browser rendering

- Ordinary printable bytes are rendered as text.
- `0xE4` is rendered as `µ`.
- CGRAM codes `0x00-0x07` are rendered on a 5x8 canvas.
- Slot 0 is intentionally drawn as a full character cell for the tuner's solid moving bar.
- A light browser-side anchor repair is applied only to recognized main screens (`MHz`, `FWD=`, `REF=`, `µH`, `pF`). Setup screens are shown without that normalization.
- Normal requests use a 25 ms delay; while any momentary control is held, they use 100 ms.

## WebSocket endpoint

Endpoint: `/ws`

### Browser to ESP32

| Message | Format | Meaning |
|---|---|---|
| LCD request | ASCII `L` | Request one current LCD/CGRAM snapshot |
| Buttons | ASCII `Bxxxxxxxxx` | Nine `0`/`1` bits in `BTN_PINS[]` order |
| OTA | ASCII `START_OTA` | Release momentary buttons and enter ArduinoOTA mode |

Button bit order:

```text
0 ANT, 1 C-UP, 2 L-UP, 3 AUTO, 4 MODE,
5 C-DN, 6 L-DN, 7 TUNE, 8 POWER
```

### ESP32 to browser

LCD snapshot is a 98-byte binary packet:

| Offset | Size | Content |
|---:|---:|---|
| 0 | 1 | `0xFD` packet type |
| 1 | 32 | Visible DDRAM: row 1 then row 2 |
| 33 | 64 | CGRAM: 8 characters × 8 rows |
| 97 | 1 | LCD revision counter, modulo 256 |

OTA notification is a one-byte binary packet containing `0xFE`.

## Flow control and stale-screen prevention

The browser keeps at most one `L` request pending. It schedules the next request only after receiving the previous packet. This prevents AsyncWebSocket from accumulating old LCD frames when the client or Wi-Fi link is slow.

Button packets are sent immediately and are not delayed by LCD polling. If a pre-release LCD response is still in flight when a momentary button is released, that response is discarded and a fresh frame is requested.

## Wi-Fi behavior

- Credentials are stored with `Preferences` in namespace `wifi`, keys `s` and `p`.
- Station mode gets 20 attempts at 500 ms each.
- On failure the ESP32 starts `MFJ993b-CONFIG` / `12345678`.
- `POST /save` stores form fields `s` and `p`, responds, then restarts.
- When connected as a station, `GET /` serves the remote-control page.

## OTA behavior

`START_OTA` is accepted only outside OTA mode. The ESP32:

1. sends packet `0xFE` to connected browsers;
2. releases momentary controls;
3. resets the nibble/address state;
4. starts ArduinoOTA on port 3232 with hostname `MFJ-Remote`;
5. services only ArduinoOTA until upload or restart.

There is currently no OTA password or web authentication. Use only on a trusted LAN.
