// MFJ-993B Remote Control — strict original-style row capture, 2026-09-14
// Classic dual-core ESP32 only. Hardware pins and HTTP uploader are preserved.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Update.h>
#include "soc/gpio_reg.h"

// ============================================================================
// ПИНЫ — ПРОВЕРЕННАЯ РАСКЛАДКА
// ============================================================================

const uint8_t BTN_PINS[9] = {
    14, 26, 27, 33, 13, 2, 5, 21, 32
};

const int PIN_E   = 17;
const int PIN_RS  = 4;
const int PIN_DB4 = 25;
const int PIN_DB5 = 18;
const int PIN_DB6 = 19;
const int PIN_DB7 = 23;

const uint32_t MASK_E   = 1UL << PIN_E;
const uint32_t MASK_RS  = 1UL << PIN_RS;
const uint32_t MASK_DB4 = 1UL << PIN_DB4;
const uint32_t MASK_DB5 = 1UL << PIN_DB5;
const uint32_t MASK_DB6 = 1UL << PIN_DB6;
const uint32_t MASK_DB7 = 1UL << PIN_DB7;

const uint32_t MASK_LCD_BUS =
    MASK_RS  |
    MASK_DB4 |
    MASK_DB5 |
    MASK_DB6 |
    MASK_DB7;

const uint32_t SAMPLE_DELAY = 110;
const uint32_t NIBBLE_TIMEOUT_US = 5000;
const uint32_t NIBBLE_TIMEOUT_CYCLES = 240 * NIBBLE_TIMEOUT_US;
const uint32_t ENABLE_STUCK_CYCLES = 240 * 20;

// Биты 1, 2, 4, 5, 6 и 7 — кнопки без фиксации.
const uint16_t MOMENTARY_BUTTON_MASK =
    (1U << 1) |
    (1U << 2) |
    (1U << 4) |
    (1U << 5) |
    (1U << 6) |
    (1U << 7);

const uint16_t INITIAL_BUTTON_MASK =
    (1U << 3) | // AUTO
    (1U << 8);  // POWER ON

// ============================================================================
// WEB / WIFI / ОБНОВЛЕНИЕ ПРОШИВКИ
// ============================================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

volatile bool webUpdateInProgress = false;
volatile bool webUpdateFailed = false;
volatile bool webUpdateSucceeded = false;
volatile bool webUpdateRestartPending = false;
volatile uint32_t webUpdateRestartAtMs = 0;
volatile uint32_t webUpdateLastActivityMs = 0;

volatile uint16_t buttonMask = INITIAL_BUTTON_MASK;


// ============================================================================
// LCD MODEL — no Arduino dependencies; the same code is exercised by host tests.
// A quiet bus does NOT clear the address, DDRAM or CGRAM.
// ============================================================================

class LcdModel {
public:
    enum Space : uint8_t { Unknown, Ddram, Cgram };

    explicit LcdModel(bool fixedRows = false) : fixedRows_(fixedRows) {}

    void reset() {
        memset(ddram_, ' ', sizeof(ddram_));
        memset(cgram_, 0, sizeof(cgram_));
        address_ = shift_ = 0;
        space_ = Unknown;
        increment_ = displayOn_ = true;
        entryShift_ = false;
        ++revision_;
        ++clearGeneration_;
    }

    void loseAddress() { space_ = Unknown; }

    void write(uint8_t value, bool data) {
        if (fixedRows_) {
            writeFixed(value,data);
            return;
        }

        if (!data) {
            commandGeneric(value);
            return;
        }

        if (space_ == Cgram) {
            writeCgram(value);
        }
        else if (space_ == Ddram) {
            const int index = ddramIndex(address_);
            if (index >= 0 && ddram_[index] != value) {
                ddram_[index] = value;
                ++revision_;
            }
            advance(increment_);
            if (entryShift_) shiftDisplay(increment_);
        }
    }

    void snapshot(uint8_t *screen, uint8_t *cgram) const {
        for (unsigned row=0;row<2;row++) {
            for (unsigned column=0;column<16;column++) {
                const unsigned offset = fixedRows_ ? 0 : shift_;
                screen[row*16+column] = displayOn_
                    ? ddram_[row*40+(offset+column)%40]
                    : ' ';
            }
        }
        memcpy(cgram,cgram_,sizeof(cgram_));
    }

    uint8_t revision() const { return static_cast<uint8_t>(revision_); }
    uint32_t clearGeneration() const { return clearGeneration_; }

private:
    uint8_t ddram_[80] = {};
    uint8_t cgram_[64] = {};
    uint8_t address_ = 0;
    uint8_t shift_ = 0;
    Space space_ = Unknown;
    bool increment_ = true;
    bool entryShift_ = false;
    bool displayOn_ = true;
    uint32_t revision_ = 0;
    uint32_t clearGeneration_ = 0;
    bool fixedRows_ = false;

    static int ddramIndex(uint8_t address) {
        if (address <= 0x27) return address;
        if (address >= 0x40 && address <= 0x67)
            return 40+address-0x40;
        return -1;
    }

    void clearDisplay() {
        memset(ddram_,' ',sizeof(ddram_));
        address_ = shift_ = 0;
        increment_ = true;
        entryShift_ = false;
        displayOn_ = true;
        space_ = Ddram;
        ++revision_;
        ++clearGeneration_;
    }

    void selectFixedAddress(uint8_t address) {
        address_ = address;
        shift_ = 0;
        increment_ = true;
        entryShift_ = false;
        displayOn_ = true;
        space_ = Ddram;
    }

    void writeCgram(uint8_t value) {
        // Captured PIC writes use only five pixel bits. ASCII here means that
        // a DDRAM-address command was missed; do not turn text into a glyph.
        if (fixedRows_ && (value & 0xE0)) {
            space_ = Unknown;
            return;
        }

        const uint8_t pixels = value & 0x1F;
        if (cgram_[address_ & 0x3F] != pixels) {
            cgram_[address_ & 0x3F] = pixels;
            ++revision_;
        }
        address_ = static_cast<uint8_t>((address_+1)&0x3F);
    }

    void writeFixed(uint8_t value, bool data) {
        if (!data) {
            commandFixed(value);
            return;
        }

        if (space_ == Cgram) {
            writeCgram(value);
            return;
        }

        if (space_ != Ddram) return;

        const bool firstRow = address_ <= 0x0F;
        const bool secondRow = address_ >= 0x40 && address_ <= 0x4F;
        if (!firstRow && !secondRow) {
            space_ = Unknown;
            return;
        }

        const int index = ddramIndex(address_);
        if (ddram_[index] != value) {
            ddram_[index] = value;
            ++revision_;
        }

        // A line can never spill into hidden DDRAM or into the other line.
        if (address_ == 0x0F || address_ == 0x4F)
            space_ = Unknown;
        else
            ++address_;
    }

    void commandFixed(uint8_t value) {
        if (value == 0x01) {
            clearDisplay();
        }
        else if (value == 0x02 || value == 0x03) {
            selectFixedAddress(0x00);
        }
        else if (value >= 0x80 && value <= 0x8F) {
            // The controller often updates only a changed fragment of row 1.
            selectFixedAddress(value & 0x0F);
        }
        else if (value >= 0xC0 && value <= 0xCF) {
            // The controller often updates only a changed fragment of row 2.
            selectFixedAddress(static_cast<uint8_t>(0x40 | (value & 0x0F)));
        }
        else if ((value & 0xC0) == 0x40) {
            address_ = value & 0x3F;
            increment_ = true;
            space_ = Cgram;
        }
        else if (value & 0x80) {
            // Ignore hidden or invalid DDRAM positions until a visible address.
            space_ = Unknown;
        }
        // Other LCD control commands cannot move the two-row mirror.
    }

    void advance(bool forward) {
        if (forward) {
            address_ = address_ == 0x27 ? 0x40
                     : address_ == 0x67 ? 0x00
                     : static_cast<uint8_t>((address_+1)&0x7F);
        }
        else {
            address_ = address_ == 0x00 ? 0x67
                     : address_ == 0x40 ? 0x27
                     : static_cast<uint8_t>((address_-1)&0x7F);
        }
    }

    void shiftDisplay(bool left) {
        shift_ = static_cast<uint8_t>((shift_+(left ? 1 : 39))%40);
        ++revision_;
    }

    void commandGeneric(uint8_t value) {
        if (value & 0x80) {
            address_ = value & 0x7F;
            space_ = Ddram;
        }
        else if (value & 0x40) {
            address_ = value & 0x3F;
            space_ = Cgram;
        }
        else if (value & 0x20) {
            // Function Set does not change stored data or the address.
        }
        else if (value & 0x10) {
            if (value & 0x08)
                shiftDisplay((value & 0x04) == 0);
            else if (space_ == Cgram)
                address_ = static_cast<uint8_t>(
                    (address_+((value & 0x04) ? 1 : 63))&0x3F);
            else
                advance((value & 0x04) != 0);
        }
        else if (value & 0x08) {
            displayOn_ = (value & 0x04) != 0;
            ++revision_;
        }
        else if (value & 0x04) {
            increment_ = (value & 0x02) != 0;
            entryShift_ = (value & 0x01) != 0;
        }
        else if (value & 0x02) {
            address_ = shift_ = 0;
            space_ = Ddram;
            ++revision_;
        }
        else if (value & 0x01) {
            clearDisplay();
        }
    }
};

// Flags carried with a nibble, not inferred from time spent in a network task.
static constexpr uint8_t SAMPLE_RS   = 0x10;
static constexpr uint8_t SAMPLE_GAP  = 0x20;
static constexpr uint8_t SAMPLE_LOSS = 0x40;

class NibbleDecoder {
public:
    void reset() {
        partial_ = seen_ = quarantine_ = startup_ = started_ = false;
        lastRs_ = false;
        first_ = 0;
    }

    void accept(uint8_t sample, LcdModel &lcd) {
        const bool rs = (sample & SAMPLE_RS) != 0;
        const bool gap = (sample & SAMPLE_GAP) != 0;
        const bool boundary = gap || (seen_ && rs != lastRs_);
        const uint8_t nibble = sample & 0x0F;

        if (sample & SAMPLE_LOSS) {
            // Do not guess a new byte phase after an actually lost nibble.
            lcd.loseAddress();
            partial_ = false;
            quarantine_ = !boundary;
        }
        if (boundary) {
            // A truncated command must not leave its following text writing
            // into the preceding row or a previously selected CGRAM slot.
            if (partial_ && !lastRs_) lcd.loseAddress();
            partial_ = false;
            quarantine_ = false;
        }
        seen_ = true;
        lastRs_ = rs;
        if (quarantine_) return;

        // The reset preamble sends single 0x3 nibbles in 8-bit mode, then
        // one 0x2 nibble to select 4-bit mode. Do not pair those as text.
        if (!rs && !partial_ && nibble == 3 && !started_) {
            startup_ = true;
            return;
        }
        if (startup_) {
            if (!rs && nibble == 3) return;
            startup_ = false;
            if (!rs && nibble == 2) { started_ = true; return; }
        }

        if (!partial_) {
            first_ = nibble;
            partial_ = true;
        } else {
            partial_ = false;
            started_ = true;
            lcd.write(static_cast<uint8_t>((first_ << 4) | nibble), rs);
        }
    }

private:
    bool partial_ = false, seen_ = false, lastRs_ = false;
    bool quarantine_ = false, startup_ = false, started_ = false;
    uint8_t first_ = 0;
};

// ============================================================================
// ACQUISITION — single producer on core 1, single consumer on core 0.
// This is a raw bus buffer, NOT a queue of old frames sent to the browser.
// ============================================================================

struct CaptureSample {
    uint32_t cycles;
    uint32_t payload;  // low byte: nibble/flags; upper 24 bits: reset epoch
};

class CaptureRing {
public:
    static constexpr uint32_t Capacity = 4096;

    bool push(const CaptureSample &sample) {
        const uint32_t head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        if (head - tail >= Capacity) return false;
        samples_[head & (Capacity - 1)] = sample;
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
        return true;
    }

    bool pop(CaptureSample &sample) {
        const uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const uint32_t head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        if (tail == head) return false;
        sample = samples_[tail & (Capacity - 1)];
        __atomic_store_n(&tail_, tail + 1, __ATOMIC_RELEASE);
        return true;
    }

private:
    CaptureSample samples_[Capacity] = {};
    alignas(4) uint32_t head_ = 0, tail_ = 0;
};

static_assert((CaptureRing::Capacity & (CaptureRing::Capacity - 1)) == 0,
              "Capture ring size must be a power of two");

CaptureRing captureRing;
LcdModel lcd(true);  // Strict 0x80 / 0xC0 / 0x01 mirror.
NibbleDecoder decoder;
portMUX_TYPE lcdMux = portMUX_INITIALIZER_UNLOCKED;
alignas(4) uint32_t captureEpoch = 1;
uint32_t droppedNibbles = 0;  // Written only by the producer; diagnostic only.

void resetCapturedLcd(bool clearPixels) {
    portENTER_CRITICAL(&lcdMux);
    if (clearPixels) lcd.reset();
    else lcd.loseAddress();
    decoder.reset();
    // Queued samples from before a power-on/reset can never enter the new model.
    const uint32_t next =
        (__atomic_load_n(&captureEpoch, __ATOMIC_RELAXED) + 1) & 0x00FFFFFF;
    __atomic_store_n(&captureEpoch, next, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&lcdMux);
}

void drainCapturedLcd() {
    CaptureSample sample;
    // Bounded work keeps housekeeping alive even during a long LCD burst.
    for (unsigned count = 0; count < 2048 && captureRing.pop(sample); ++count) {
        portENTER_CRITICAL(&lcdMux);
        const uint32_t epoch = __atomic_load_n(&captureEpoch, __ATOMIC_ACQUIRE);
        if ((sample.payload >> 8) == epoch)
            decoder.accept(static_cast<uint8_t>(sample.payload), lcd);
        portEXIT_CRITICAL(&lcdMux);
    }
}

// ============================================================================
// ОСНОВНАЯ WEB-СТРАНИЦА
// ============================================================================

const char indexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>MFJ-993B Remote</title>
<style>
  * { box-sizing: border-box; }
  body {
    background: #111;
    color: #4e4;
    font-family: monospace;
    display: flex;
    flex-direction: column;
    align-items: center;
    margin: 0;
    padding: 10px;
    user-select: none;
  }
  #header {
    width: 350px;
    display: flex;
    justify-content: space-between;
    color: #777;
    font-size: 11px;
    margin-bottom: 5px;
  }
  #status.on { color: #0c0; font-weight: bold; }
  #status.off { color: #c00; font-weight: bold; }
  #lcd {
    background: #020;
    padding: 10px;
    border: 4px solid #333;
    border-radius: 8px;
    box-shadow: 0 0 15px rgba(0,255,0,.15);
    margin-bottom: 12px;
  }
  .row {
    display: flex;
    gap: 2px;
    height: 30px;
    margin-bottom: 3px;
  }
  .cell {
    width: 18px;
    height: 28px;
    background: rgba(0,255,0,.03);
    border: 1px solid rgba(0,255,0,.06);
    display: flex;
    align-items: center;
    justify-content: center;
    color: #4e4;
    font-size: 20px;
    font-weight: bold;
    text-shadow: 0 0 3px #0f0;
  }
  .lcd-glyph {
    display: none;
    width: 15px;
    height: 24px;
    opacity: .94;
    filter: drop-shadow(0 0 1px #0f0);
  }
  .cell.special .lcd-char { display: none; }
  .cell.special .lcd-glyph { display: block; }
  .grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 8px;
    width: 350px;
  }
  button {
    height: 45px;
    background: #222;
    color: #bbb;
    border: 1px solid #444;
    border-radius: 5px;
    font-size: 11px;
    font-weight: bold;
    touch-action: none;
    -webkit-touch-callout: none;
  }
  button.active {
    background: #004488 !important;
    color: #fff;
    border-color: #078cff !important;
  }
  #b8.active {
    background: #880000 !important;
    border-color: #ff3030 !important;
  }
  .macro {
    background: #223;
    border-color: #446;
    color: #aab;
  }
  #extraCombinations {
    grid-column: 1 / -1;
    width: 100%;
    border: 1px solid #333;
    border-radius: 5px;
    background: #181818;
    color: #999;
  }
  #extraCombinations summary {
    padding: 13px 10px;
    cursor: pointer;
    text-align: center;
    font-weight: bold;
    color: #aaa;
  }
  .extra-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 8px;
    padding: 0 8px 8px;
  }
  .extra-title,
  .extra-note {
    grid-column: 1 / -1;
    text-align: center;
  }
  .extra-title {
    margin-top: 5px;
    color: #6b9;
    font-size: 11px;
    font-weight: bold;
  }
  .extra-note {
    color: #866;
    font-size: 9px;
    line-height: 1.35;
  }
  .danger {
    background: #321 !important;
    border-color: #743 !important;
    color: #d99 !important;
  }
  #modal {
    display: none;
    position: fixed;
    top: 50%;
    left: 50%;
    transform: translate(-50%,-50%);
    background: #222;
    border: 2px solid #f00;
    border-radius: 10px;
    padding: 20px;
    z-index: 10;
    text-align: center;
    box-shadow: 0 0 30px #000;
  }
  #modal button {
    width: 80px;
    margin: 12px 6px 0;
  }
  #updateButton {
    margin-top: 22px;
    color: #555;
    font-size: 9px;
    cursor: pointer;
    text-decoration: underline;
  }
</style>
</head>
<body>
  <div id="header">
    <span>MFJ-993B Remote Control</span>
    <span id="status" class="off">●</span>
  </div>

  <div id="lcd">
    <div id="row1" class="row"></div>
    <div id="row2" class="row"></div>
  </div>

  <div id="controls" class="grid">
    <button id="b0" onclick="toggleButton(0)">ANT1</button>
    <button id="b1">C-UP</button>
    <button id="b2">L-UP</button>
    <button id="b3" onclick="toggleButton(3)">AUTO</button>

    <button id="b4">MODE</button>
    <button id="b5">C-DN</button>
    <button id="b6">L-DN</button>
    <button id="b7">TUNE</button>

    <button id="b8" style="grid-column:span 4" onclick="checkPower()">POWER OFF</button>

    <details id="extraCombinations">
      <summary>Доп. комбинации</summary>
      <div class="extra-grid">
        <div class="extra-title">Во время обычной работы</div>

        <button class="macro" data-pins="1,5">CAP INPUT/OUTPUT</button>
        <button class="macro" data-pins="2,6">SWR BEEP</button>
        <button class="macro" data-pins="1,2">C + L UP</button>
        <button class="macro" data-pins="5,6">BYPASS</button>

        <button class="macro" data-pins="7,1">TARGET SWR</button>
        <button class="macro" data-pins="7,2">AUTO TUNE SWR</button>
        <button class="macro" data-pins="7,5">MEMORY A-D</button>
        <button class="macro" data-pins="7,6">METER RANGE</button>

        <button class="macro" data-pins="7,1,2">POWER 300/150</button>
        <button class="macro danger" data-pins="7,5,6"
                data-confirm="Перезаписать текущей настройкой выбранную ячейку памяти тюнера?">SAVE CURRENT</button>

        <button id="lcLimitButton" class="macro danger" style="grid-column:1/-1">LC LIMIT (SETUP)</button>
        <div class="extra-note">LC LIMIT запускается только из уже открытого Setup Mode.</div>

        <div class="extra-title">Операции при включении</div>
        <div class="extra-note">ESP выключит тюнер, зажмёт сочетание и снова включит. Радиоинтерфейс должен быть отключён, если трансивер выключен.</div>

        <button class="macro danger" data-poweron-pins="1"
                data-confirm="Выключить и включить тюнер для показа версии прошивки?">FIRMWARE VERSION</button>
        <button class="macro danger" data-poweron-pins="2"
                data-confirm="Запустить SELF TEST? Он длится около 30 секунд и сбрасывает настройки к заводским.">SELF TEST</button>
        <button class="macro danger" data-poweron-pins="5"
                data-confirm="Запустить тест реле? Перед продолжением проверьте условия из руководства.">RELAY TEST</button>
        <button class="macro danger" data-poweron-pins="6"
                data-confirm="Запустить тест схемы выключения питания?">POWER-DOWN TEST</button>

        <button class="macro danger" data-poweron-pins="1,5"
                data-confirm="Запустить калибровку ваттметра? Не продолжайте без измерительного оборудования и процедуры из руководства.">WATTMETER CAL</button>
        <button class="macro danger" data-poweron-pins="2,6"
                data-confirm="Включить тестовый звук для настройки громкости?">AUDIO / VOLUME</button>
        <button class="macro danger" data-poweron-pins="1,2"
                data-confirm="Запустить калибровку КСВ-моста? Не продолжайте без измерительного оборудования и процедуры из руководства.">SWR BRIDGE CAL</button>
        <button class="macro danger" data-poweron-pins="5,6"
                data-confirm="Запустить калибровку частотомера? Не продолжайте без измерительного оборудования и процедуры из руководства.">FREQ COUNTER CAL</button>

        <button class="macro danger" data-poweron-pins="7,5,0"
                data-confirm="Удалить память выбранной антенны? После появления DELETE ANTENNA нажмите C-UP = YES или L-UP = NO.">DELETE ANT MEMORY</button>
        <button class="macro danger" data-poweron-pins="7,6"
                data-confirm="Вернуть заводские настройки? Память антенн при этом не стирается.">FACTORY DEFAULTS</button>
        <button class="macro danger" style="grid-column:1/-1" data-poweron-pins="7,5,6"
                data-confirm="TOTAL RESET удалит память ОБЕИХ антенн и вернёт заводские настройки. После появления TOTAL RESET нажмите C-UP = YES или L-UP = NO. Продолжить?">TOTAL RESET</button>
      </div>
    </details>
  </div>

  <div id="modal">
    <div>ВЫКЛЮЧИТЬ?</div>
    <button onclick="confirmPower(true)" style="background:#800;color:#fff">ДА</button>
    <button onclick="confirmPower(false)">НЕТ</button>
  </div>

  <div id="updateButton" onclick="location.href='/update'">Firmware Update (.bin)</div>

<script>
  const PACKET_LCD = 0xFD;

  let socket = null;
  let reconnectTimer = null;
  let lcdRequestTimer = null;
  let lcdRequestWatchdog = null;
  let lcdRequestPending = false;
  let lcdRefreshAfterRelease = false;
  let lastSentMomentaryHeld = false;
  let buttonMask = (1 << 3) | (1 << 8);
  let lastCellSignatures = Array(32).fill('');
  let specialSequenceRunning = false;
  let activeLayout = null;
  let lastClearGeneration = null;
  const meterFields = new Map();
  const INVALID_FIELD_HOLD_MS = 120;

  const SPACE = 0x20;
  const MOMENTARY_MASK =
    (1 << 1) | (1 << 2) | (1 << 4) |
    (1 << 5) | (1 << 6) | (1 << 7);
  const LCD_POLL_IDLE_MS = 20;
  const LCD_POLL_HELD_MS = 40;
  const blankCgram = new Uint8Array(64);
  const meterScreen = new Uint8Array(32);
  meterScreen.fill(SPACE);


  // One dot renderer for ROM text and CGRAM. No DOM/font mismatch.
  // Original 5x7 patterns with an eighth blank row; not a dump of chip CGROM.
  const FONT_HEX = {
    ' ':'00000000000000', '!':'04040404040004', '"':'0A0A0A00000000',
    '#':'0A0A1F0A1F0A0A', '$':'040F140E051E04', '%':'18190204081303',
    '&':'0C12140A15120D', "'":'04040800000000', '(':'02040808080402',
    ')':'08040202020408', '*':'000A041F040A00', '+':'0004041F040400',
    ',':'00000000000408', '-':'0000001F000000', '.':'00000000000004',
    '/':'01010204081010', '0':'0E11131519110E', '1':'040C040404040E',
    '2':'0E11010204081F', '3':'1F01020601110E', '4':'02060A121F0202',
    '5':'1F101E0101110E', '6':'0608101E11110E', '7':'1F010204080808',
    '8':'0E11110E11110E', '9':'0E11110F01020C', ':':'00040400040400',
    ';':'00040400040408', '<':'02040810080402', '=':'00001F001F0000',
    '>':'08040201020408', '?':'0E110102040004', '@':'0E11171517100E',
    'A':'0E1111111F1111', 'B':'1E11111E11111E', 'C':'0E11101010110E',
    'D':'1E11111111111E', 'E':'1F10101E10101F', 'F':'1F10101E101010',
    'G':'0E11101711110F', 'H':'1111111F111111', 'I':'0E04040404040E',
    'J':'0702020202120C', 'K':'11121418141211', 'L':'1010101010101F',
    'M':'111B1515111111', 'N':'11191513111111', 'O':'0E11111111110E',
    'P':'1E11111E101010', 'Q':'0E11111115120D', 'R':'1E11111E141211',
    'S':'0F10100E01011E', 'T':'1F040404040404', 'U':'1111111111110E',
    'V':'11111111110A04', 'W':'11111115151B11', 'X':'11110A040A1111',
    'Y':'11110A04040404', 'Z':'1F01020408101F', '[':'0E08080808080E',
    '\\':'10100804020101', ']':'0E02020202020E', '^':'040A1100000000',
    '_':'0000000000001F', '`':'08040200000000', 'a':'00000E010F110F',
    'b':'1010161911111E', 'c':'00000E1110110E', 'd':'01010D1311110F',
    'e':'00000E111F100E', 'f':'0609091C080808', 'g':'00000F11110F01',
    'h':'10101619111111', 'i':'04000C0404040E', 'j':'0200060202120C',
    'k':'10101214181412', 'l':'0C04040404040E', 'm':'00001A15151515',
    'n':'00001619111111', 'o':'00000E1111110E', 'p':'00001E11111E10',
    'q':'00000F11110F01', 'r':'00001619101010', 's':'00000F100E011E',
    't':'08081C08080906', 'u':'0000111111130D', 'v':'00001111110A04',
    'w':'0000111115150A', 'x':'0000110A040A11', 'y':'00001111110F01',
    'z':'00001F0204081F', '{':'02040408040402', '|':'04040404040404',
    '}':'08040402040408', '~':'00000815020000'
  };
  const FONT_ROWS = new Map(Object.entries(FONT_HEX).map(([character, hex]) => {
    const pixels = new Uint8Array(8);
    for (let row = 0; row < 7; row++) pixels[row] = parseInt(hex.slice(row*2, row*2+2), 16);
    return [character.charCodeAt(0), pixels];
  }));
  FONT_ROWS.set(0xE4, new Uint8Array([0,0,17,17,19,29,16,16])); // micro sign
  FONT_ROWS.set(0xDF, new Uint8Array([6,9,9,6,0,0,0,0]));       // degree
  FONT_ROWS.set(0x7E, new Uint8Array([0,4,2,31,2,4,0,0]));
  FONT_ROWS.set(0x7F, new Uint8Array([0,4,8,31,8,4,0,0]));
  const fullGlyph = new Uint8Array(8).fill(31);
  const emptyGlyph = new Uint8Array(8);
  const contexts = [];

  const rows = [document.getElementById('row1'), document.getElementById('row2')];
  for (let i = 0; i < 32; i++) {
    const cell = document.createElement('div');
    const canvas = document.createElement('canvas');
    cell.className = 'cell special';
    canvas.className = 'lcd-glyph';
    canvas.width = 30;
    canvas.height = 48;
    cell.appendChild(canvas);
    rows[i < 16 ? 0 : 1].appendChild(cell);
    contexts.push(canvas.getContext('2d'));
  }

  function blankScreen() { return new Uint8Array(32).fill(SPACE); }
  function asciiScreen(row1, row2) {
    return Uint8Array.from(
      row1.padEnd(16,' ').slice(0,16) + row2.padEnd(16,' ').slice(0,16),
      character => character.charCodeAt(0)
    );
  }
  const powerOffScreen = asciiScreen('   POWER OFF', '');
  const fixMeterLayout = !new URLSearchParams(location.search).has('raw');

  function renderCell(index, value, cgram, fullBlocks = false) {
    const pixels = value === 0xFF || (fullBlocks && (value & 0xF7) === 0)
      ? fullGlyph
      : value < 16 ? cgram.subarray((value & 7)*8, (value & 7)*8+8)
      : FONT_ROWS.get(value) || emptyGlyph;
    const signature = Array.from(pixels, row => row & 31).join(',');
    if (lastCellSignatures[index] === signature) return;
    lastCellSignatures[index] = signature;
    const context = contexts[index];
    context.clearRect(0,0,30,48);
    context.fillStyle = '#4e4';
    context.shadowColor = '#0f0';
    context.shadowBlur = 1.4;
    for (let row = 0; row < 8; row++)
      for (let col = 0; col < 5; col++)
        if (pixels[row] & (1 << (4-col)))
          context.fillRect(col*6+0.8, row*6+0.8, 4.4, 4.4);
  }

  function drawScreen(screen, cgram = blankCgram, fullBlocks = false) {
    for (let i = 0; i < 32; i++) renderCell(i, screen[i], cgram, fullBlocks);
  }

  function writeAscii(screen, offset, text) {
    for (let i = 0; i < text.length; i++) screen[offset+i] = text.charCodeAt(i);
  }
  function bytesToAscii(screen, start, end) {
    return Array.from(screen.subarray(start,end),
      value => value >= 32 && value <= 126 ? String.fromCharCode(value) : '\x01').join('');
  }
  function findAscii(screen, start, end, text) {
    const index = bytesToAscii(screen,start,end).indexOf(text);
    return index < 0 ? -1 : start+index;
  }
  function numericTokens(screen, start, end) {
    const expression = /[0-9][.,\/][0-9]|[0-9]{1,3}/g;
    const result = [];
    let match;
    while ((match = expression.exec(bytesToAscii(screen,start,end))) !== null)
      result.push({value:match[0], offset:start+match.index});
    return result;
  }
  function extractFrequency(screen) {
    const anchor = findAscii(screen,0,16,'MHz');
    if (anchor < 0) return null;
    const match = bytesToAscii(screen,Math.max(0,anchor-6),anchor)
      .match(/[ 0-9]{1,2}[.,][0-9]{3}$/);
    return match ? match[0].padStart(6,' ').slice(-6) : null;
  }
  function extractValueAfter(screen, anchor) {
    if (anchor < 0 || anchor+7 > screen.length) return null;
    const value = bytesToAscii(screen,anchor+4,anchor+7);
    return /^(?:[0-9][.,\/][0-9]|[ 0-9]{3})$/.test(value) && /[0-9]/.test(value)
      ? value : null;
  }
  function countBarCells(screen) {
    let count = 0;
    for (let i = 16; i < 29; i++) if (screen[i] < 16 || screen[i] === 0x3D) count++;
    return count;
  }

  function resetMeterState() {
    activeLayout = null;
    meterFields.clear();
    meterScreen.fill(SPACE);
  }

  function findMainAnchor(screen,start,end,label) {
    const exact = findAscii(screen,start,end,label);
    if (exact >= 0) return exact;

    // Allow one damaged byte, but reject an ambiguous match.
    let found = -1;
    for (let offset=start;offset+label.length<=end;offset++) {
      let equal = 0;
      for (let i=0;i<label.length;i++)
        if (screen[offset+i] === label.charCodeAt(i)) equal++;
      if (equal === label.length-1) {
        if (found >= 0) return -1;
        found = offset;
      }
    }
    return found;
  }

  function mainAnchors(screen) {
    return {
      mhz: findMainAnchor(screen,3,12,'MHz'),
      fwd: findMainAnchor(screen,16,25,'FWD='),
      ref: findMainAnchor(screen,23,32,'REF=')
    };
  }

  function mainFrequency(screen,anchor) {
    const row = bytesToAscii(screen,0,12);
    if (anchor >= 0) {
      const before = bytesToAscii(screen,Math.max(0,anchor-6),anchor);
      const exact = before.match(/[ 0-9]{1,2}[.,][0-9]{3}$/);
      if (exact) return exact[0].padStart(6,' ').slice(-6);
    }
    const fallback = row.match(/[ 0-9]{1,2}[.,][0-9]{3}/);
    return fallback ? fallback[0].padStart(6,' ').slice(-6) : null;
  }

  function isExplicitOtherScreen(screen) {
    let microhenry = false;
    for (let i=0;i<15;i++)
      if (screen[i] === 0xE4 && screen[i+1] === 0x48) microhenry = true;
    if (microhenry || findAscii(screen,16,32,'pF') >= 0) return true;

    const text = bytesToAscii(screen,0,32).replace(/\x01/g,' ');
    return /SETUP|TARGET|AUTO TUNE|MEMORY|METER ?RANGE|POWER LEVEL|LC LIMIT|CAP |BYPASS|BEEP|STICKY|SEMI|SAVE CURRENT|VERSION|SELF TEST|RELAY TEST|WATTMETER|AUDIO|BRIDGE|COUNTER|DELETE|DEFAULT|TOTAL RESET/.test(text);
  }

  function classifyScreen(screen) {
    const anchors = mainAnchors(screen);
    const frequency = mainFrequency(screen,anchors.mhz);
    const barCells = countBarCells(screen);

    if (barCells >= 4 &&
        (anchors.mhz >= 0 || activeLayout === 'bar' ||
         (frequency && anchors.fwd < 0 && anchors.ref < 0)))
      return 'bar';

    if (isExplicitOtherScreen(screen)) return null;

    const anchorCount = Object.values(anchors)
      .filter(offset => offset >= 0).length;
    let indicators = 0;
    [5,7,6].forEach((slot,index) => {
      const value = screen[9+index];
      if (value < 16 && (value & 7) === slot) indicators++;
    });

    if (anchorCount >= 2 ||
        (frequency && indicators >= 2) ||
        (frequency && (anchors.fwd >= 0 || anchors.ref >= 0)))
      return 'meter';

    // Once positively recognized, the main layout survives damaged anchors.
    // A real 0x01 or an explicit other-screen heading releases this latch.
    if (activeLayout === 'meter') return 'meter';
    return null;
  }

  function setMeterField(offset,length,value) {
    const now = Date.now();
    if (value != null) meterFields.set(offset,{value:value,at:now});
    const cached = meterFields.get(offset);
    const visible = value != null ? value :
      cached && now-cached.at <= INVALID_FIELD_HOLD_MS ? cached.value : null;
    if (visible != null)
      writeAscii(meterScreen,offset,visible.padStart(length,' ').slice(-length));
  }

  function validNumberAt(screen,offset) {
    const value = bytesToAscii(screen,offset,offset+3);
    return /^(?:[0-9][.,\/][0-9]|[ 0-9]{3})$/.test(value) && /[0-9]/.test(value)
      ? value : null;
  }

  function antennaGlyph(cgram) {
    const result = new Uint8Array(cgram);
    // Keep live IntelliTune row; the antenna numeral comes from ANT state.
    result[41] = result[47] = 0;
    result.set((buttonMask & 1) ? [7,1,7,4,7] : [2,6,2,2,7],42);
    return result;
  }

  function drawMeterSnapshot(screen,cgram) {
    const anchors = mainAnchors(screen);
    meterScreen.fill(SPACE);

    setMeterField(0,6,mainFrequency(screen,anchors.mhz));
    writeAscii(meterScreen,6,'MHz');
    meterScreen.set([5,7,6],9);
    setMeterField(13,3,validNumberAt(screen,13));

    writeAscii(meterScreen,16,'FWD=');
    setMeterField(20,3,
      extractValueAfter(screen,anchors.fwd) || validNumberAt(screen,20));

    writeAscii(meterScreen,25,'REF=');
    setMeterField(29,3,
      extractValueAfter(screen,anchors.ref) || validNumberAt(screen,29));

    drawScreen(meterScreen,antennaGlyph(cgram));
  }

  function drawSnapshot(data) {
    if (data.length === 102) {
      const clear = new DataView(data.buffer,data.byteOffset,data.byteLength)
        .getUint32(98,true);
      if (clear !== lastClearGeneration) {
        resetMeterState();
        lastClearGeneration = clear;
      }
    }

    if ((buttonMask & (1 << 8)) === 0) {
      resetMeterState();
      drawScreen(powerOffScreen);
      return;
    }

    const screen = data.subarray(1,33);
    const cgram = data.subarray(33,97);

    if (screen.every(value => value === SPACE)) {
      resetMeterState();
      drawScreen(screen,cgram);
      return;
    }

    const kind = classifyScreen(screen);
    if (kind === 'meter' && fixMeterLayout) {
      if (activeLayout !== 'meter') meterFields.clear();
      activeLayout = 'meter';
      drawMeterSnapshot(screen,cgram);
      return;
    }

    if (activeLayout === 'meter') meterFields.clear();
    activeLayout = kind;
    drawScreen(screen,cgram,kind === 'bar' && fixMeterLayout);
  }

  function requestLcdSnapshot() {
    lcdRequestTimer = null;
    if (lcdRequestPending || !socket || socket.readyState !== WebSocket.OPEN) return;
    lcdRequestPending = true;
    const connection = socket;
    connection.send('L');
    clearTimeout(lcdRequestWatchdog);
    // A timeout closes this stream. Never add duplicate requests to it.
    lcdRequestWatchdog = setTimeout(() => {
      if (socket === connection && lcdRequestPending) connection.close();
    },2000);
  }

  function scheduleLcdSnapshotRequest() {
    clearTimeout(lcdRequestTimer);
    lcdRequestTimer = setTimeout(requestLcdSnapshot,
      buttonMask & MOMENTARY_MASK ? LCD_POLL_HELD_MS : LCD_POLL_IDLE_MS);
  }

  function connect() {
    if (socket && (socket.readyState === WebSocket.OPEN ||
                   socket.readyState === WebSocket.CONNECTING)) return;
    const connection = new WebSocket(
      (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    socket = connection;
    connection.binaryType = 'arraybuffer';
    connection.onopen = () => {
      if (socket !== connection) return;
      document.getElementById('status').className = 'on';
      // The ESP sends S + its actual output mask first. Reloading the page
      // must not overwrite ANT/AUTO/POWER with browser defaults.
    };
    connection.onclose = () => {
      if (socket !== connection) return;
      document.getElementById('status').className = 'off';
      clearTimeout(lcdRequestTimer);
      clearTimeout(lcdRequestWatchdog);
      lcdRequestPending = lcdRefreshAfterRelease = false;
      releaseMomentaryButtons();
      if (!reconnectTimer) reconnectTimer = setTimeout(() => {
        reconnectTimer = null; connect();
      },1500);
    };
    connection.onerror = () => {
      if (socket === connection) document.getElementById('status').className = 'off';
    };
    connection.onmessage = event => {
      if (socket !== connection) return;
      if (typeof event.data === 'string') {
        if (/^S[01]{9}$/.test(event.data)) {
          buttonMask = 0;
          for (let i = 0; i < 9; i++) {
            if (event.data[i+1] === '1') buttonMask |= 1 << i;
            updateButtonVisual(i);
          }
          lastSentMomentaryHeld = !!(buttonMask & MOMENTARY_MASK);
          requestLcdSnapshot();
        }
        return;
      }
      const data = new Uint8Array(event.data);
      if ((data.length !== 98 && data.length !== 102) || data[0] !== PACKET_LCD) return;
      clearTimeout(lcdRequestWatchdog);
      lcdRequestPending = false;
      drawSnapshot(data);
      if (lcdRefreshAfterRelease) {
        lcdRefreshAfterRelease = false;
        requestLcdSnapshot();
      } else scheduleLcdSnapshotRequest();
    };
  }

  function sendButtons() {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    let message = 'B';
    for (let i = 0; i < 9; i++) message += buttonMask & (1 << i) ? '1' : '0';
    socket.send(message);  // Always before any LCD scheduling.
    const held = !!(buttonMask & MOMENTARY_MASK);
    if (lastSentMomentaryHeld && !held) {
      lcdRefreshAfterRelease = lcdRequestPending;
      clearTimeout(lcdRequestTimer);
      if (!lcdRequestPending) requestLcdSnapshot();
    } else if (held) scheduleLcdSnapshotRequest();
    lastSentMomentaryHeld = held;
  }

  function updateButtonVisual(index) {
    const button = document.getElementById('b' + index);
    const active = (buttonMask & (1 << index)) !== 0;

    if (!button) return;

    button.classList.toggle('active', active);

    if (index === 0) button.textContent = active ? 'ANT2' : 'ANT1';
    if (index === 3) button.textContent = active ? 'AUTO' : 'MANUAL';
    if (index === 8) button.textContent = active ? 'POWER ON' : 'POWER OFF';
  }

  function setButton(index, active, sendNow = true) {
    if (active) {
      buttonMask |= 1 << index;
    }
    else {
      buttonMask &= ~(1 << index);
    }

    updateButtonVisual(index);

    if (index === 8) {
      lastCellSignatures.fill('');
      resetMeterState();
      lastClearGeneration = null;

      if (active) {
        drawScreen(blankScreen(), blankCgram);
      }
      else {
        drawScreen(powerOffScreen, blankCgram);
      }
    }

    if (sendNow) sendButtons();
  }

  function toggleButton(index) {
    setButton(index, (buttonMask & (1 << index)) === 0);
  }

  function bindMomentary(index) {
    const button = document.getElementById('b'+index);
    button.addEventListener('pointerdown', event => {
      event.preventDefault();
      if (buttonMask & (1 << index)) return;
      if (button.setPointerCapture) button.setPointerCapture(event.pointerId);
      setButton(index,true);
    });
    const release = event => {
      if (!(buttonMask & (1 << index))) return;
      if (event) event.preventDefault();
      setButton(index,false);
    };
    ['pointerup','pointercancel','lostpointercapture'].forEach(type =>
      button.addEventListener(type,release));
  }

  [1, 2, 4, 5, 6, 7].forEach(bindMomentary);

  document.querySelectorAll('[data-pins]').forEach(button => {
    const pins = button.dataset.pins.split(',').map(Number);
    let active = false;

    const release = event => {
      if (!active) return;
      if (event) event.preventDefault();

      active = false;
      button.classList.remove('active');

      pins.forEach(index => setButton(index, false, false));
      sendButtons();

      window.removeEventListener('mouseup', release);
      window.removeEventListener('touchend', release);
      window.removeEventListener('touchcancel', release);
    };

    const press = event => {
      event.preventDefault();
      if (active) return;

      if (
        button.dataset.confirm &&
        !confirm(button.dataset.confirm)
      ) {
        return;
      }

      active = true;
      button.classList.add('active');

      pins.forEach(index => setButton(index, true, false));
      sendButtons();

      window.addEventListener('mouseup', release);
      window.addEventListener('touchend', release, {passive:false});
      window.addEventListener('touchcancel', release, {passive:false});
    };

    button.addEventListener('mousedown', press);
    button.addEventListener('touchstart', press, {passive:false});
  });

  const waitMs = milliseconds =>
    new Promise(resolve => setTimeout(resolve, milliseconds));

  async function runLcLimitSequence(button) {
    if (specialSequenceRunning) return;

    if (!confirm(
      'LC LIMIT является защитой тюнера. Выполнить сочетание MODE, затем C-UP + L-UP?'
    )) {
      return;
    }

    specialSequenceRunning = true;
    button.classList.add('active');
    setButton(4, true, false);
    sendButtons();
    await waitMs(250);

    setButton(1, true, false);
    setButton(2, true, false);
    sendButtons();
    await waitMs(700);

    setButton(1, false, false);
    setButton(2, false, false);
    setButton(4, false, false);
    sendButtons();

    button.classList.remove('active');
    specialSequenceRunning = false;
  }

  async function runPowerOnSequence(button) {
    if (specialSequenceRunning) return;

    if (
      button.dataset.confirm &&
      !confirm(button.dataset.confirm)
    ) {
      return;
    }

    const pins = button.dataset.poweronPins
      .split(',')
      .map(Number);

    specialSequenceRunning = true;
    button.classList.add('active');
    releaseMomentaryButtons();

    // Руководство запрещает быстрое переключение питания: даём тюнеру
    // полностью выключиться перед зажимом требуемых кнопок.
    setButton(8, false, false);
    sendButtons();
    await waitMs(2200);

    pins.forEach(index => setButton(index, true, false));
    sendButtons();
    await waitMs(250);

    setButton(8, true, false);
    sendButtons();
    await waitMs(1800);

    pins.forEach(index => setButton(index, false, false));
    sendButtons();

    button.classList.remove('active');
    specialSequenceRunning = false;
  }

  document.getElementById('lcLimitButton').addEventListener(
    'click',
    event => {
      event.preventDefault();
      runLcLimitSequence(event.currentTarget);
    }
  );

  document.querySelectorAll('[data-poweron-pins]').forEach(button => {
    button.addEventListener('click', event => {
      event.preventDefault();
      runPowerOnSequence(event.currentTarget);
    });
  });

  function releaseMomentaryButtons() {
    [1, 2, 4, 5, 6, 7].forEach(index => {
      setButton(index, false, false);
    });

    sendButtons();
  }

  window.addEventListener('blur', releaseMomentaryButtons);
  window.addEventListener('pagehide', releaseMomentaryButtons);

  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      releaseMomentaryButtons();
    }
  });

  function checkPower() {
    if (buttonMask & (1 << 8)) {
      document.getElementById('modal').style.display = 'block';
    }
    else {
      toggleButton(8);
    }
  }

  function confirmPower(confirmed) {
    document.getElementById('modal').style.display = 'none';

    if (confirmed) {
      toggleButton(8);
    }
  }

  [0, 3, 8].forEach(updateButtonVisual);

  connect();
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// СТРАНИЦА НАСТРОЙКИ WIFI
// ============================================================================

const char configHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MFJ-993B WiFi</title>
<style>
body{background:#111;color:#0f4;font-family:sans-serif;text-align:center;padding:25px}
input{width:260px;margin:8px;padding:12px;background:#222;color:#fff;border:1px solid #555}
button{margin:8px;padding:12px 25px;background:#064;color:#fff;border:0;border-radius:5px}
</style>
</head>
<body>
<h2>MFJ-993B WiFi Config</h2>
<form action="/save" method="POST">
  <input name="s" placeholder="WiFi Name (SSID)" required><br>
  <input name="p" type="password" placeholder="Password"><br>
  <button type="submit">SAVE AND REBOOT</button>
</form>
</body>
</html>
)rawliteral";

// ============================================================================
// СТРАНИЦА ОБНОВЛЕНИЯ ЧЕРЕЗ HTTP
// ============================================================================

const char updateHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MFJ-993B Firmware Update</title>
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0;
    min-height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    background: #111;
    color: #bbb;
    font-family: sans-serif;
    padding: 18px;
  }
  .panel {
    width: min(430px, 100%);
    background: #191919;
    border: 1px solid #3a3a3a;
    border-radius: 10px;
    padding: 22px;
    box-shadow: 0 0 24px #000;
  }
  h2 { margin: 0 0 15px; color: #5d6; font-size: 20px; }
  p { color: #999; font-size: 13px; line-height: 1.45; }
  input {
    display: block;
    width: 100%;
    margin: 18px 0 12px;
    padding: 11px;
    color: #ddd;
    background: #222;
    border: 1px solid #555;
    border-radius: 5px;
  }
  button {
    width: 100%;
    padding: 13px;
    color: #fff;
    background: #075f31;
    border: 1px solid #168b4d;
    border-radius: 5px;
    font-weight: bold;
    cursor: pointer;
  }
  button:disabled { opacity: .45; cursor: default; }
  .bar {
    height: 16px;
    margin-top: 16px;
    overflow: hidden;
    border: 1px solid #444;
    border-radius: 8px;
    background: #222;
  }
  #progress {
    width: 0;
    height: 100%;
    background: #2b5;
    transition: width .12s linear;
  }
  #status {
    min-height: 36px;
    margin-top: 12px;
    color: #aaa;
    font-family: monospace;
    font-size: 12px;
    white-space: pre-wrap;
  }
  a { color: #699; font-size: 12px; }
  .warning { color: #d98; }
</style>
</head>
<body>
  <div class="panel">
    <h2>Обновление прошивки</h2>
    <p>Выберите основной файл <b>*.ino.bin</b>. Не выбирайте bootloader, partitions или merged-файл.</p>
    <p class="warning">Не отключайте питание и не закрывайте страницу во время передачи.</p>

    <input id="firmware" type="file" accept=".bin,application/octet-stream">
    <button id="upload" disabled>ЗАГРУЗИТЬ И ПЕРЕЗАГРУЗИТЬ</button>

    <div class="bar"><div id="progress"></div></div>
    <div id="status">Файл не выбран</div>
    <a href="/">Вернуться к управлению</a>
  </div>

<script>
  const firmware = document.getElementById('firmware');
  const upload = document.getElementById('upload');
  const progress = document.getElementById('progress');
  const status = document.getElementById('status');

  function setReadyState() {
    const file = firmware.files[0];
    const valid = file && file.name.toLowerCase().endsWith('.ino.bin');

    upload.disabled = !valid;
    status.textContent = valid
      ? file.name + ' — ' + Math.ceil(file.size / 1024) + ' KiB'
      : (file ? 'Нужен файл с окончанием .ino.bin' : 'Файл не выбран');
  }

  firmware.addEventListener('change', setReadyState);

  upload.addEventListener('click', () => {
    const file = firmware.files[0];

    if (!file || !file.name.toLowerCase().endsWith('.ino.bin')) return;
    if (!confirm('Начать обновление прошивки?')) return;

    upload.disabled = true;
    firmware.disabled = true;
    progress.style.width = '0%';
    status.textContent = 'Подготовка...';

    const body = new FormData();
    body.append('firmware', file, file.name);

    const request = new XMLHttpRequest();
    request.open('POST', '/update');
    request.timeout = 300000;

    request.upload.onprogress = event => {
      if (!event.lengthComputable) return;

      const percent = Math.round(event.loaded * 100 / event.total);
      progress.style.width = percent + '%';
      status.textContent = 'Передача: ' + percent + '%';
    };

    request.onload = () => {
      if (request.status === 200) {
        progress.style.width = '100%';
        status.textContent = 'Готово. ESP перезагружается...';
        setTimeout(() => { location.href = '/'; }, 8000);
        return;
      }

      status.textContent = 'Ошибка: ' + (request.responseText || request.status);
      upload.disabled = false;
      firmware.disabled = false;
    };

    request.onerror = () => {
      status.textContent = 'Соединение прервано. Подождите 10 секунд и проверьте основную страницу.';
      setTimeout(() => { location.href = '/'; }, 10000);
    };

    request.ontimeout = () => {
      status.textContent = 'Превышено время ожидания. Проверьте доступность ESP.';
      upload.disabled = false;
      firmware.disabled = false;
    };

    request.send(body);
  });
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// КНОПКИ
// ============================================================================

void applyButtonMask(uint16_t newMask)
{
    newMask &= 0x01FF;

    bool powerWasOn =
        (buttonMask & (1U << 8)) != 0;

    bool powerWillBeOn =
        (newMask & (1U << 8)) != 0;

    if (!powerWasOn && powerWillBeOn) {
        resetCapturedLcd(true);
    }

    for (uint8_t i = 0; i < 9; i++) {
        bool logicalState =
            (newMask & (1U << i)) != 0;

        // POWER (GPIO32) подключён с обратной полярностью.
        // В протоколе и интерфейсе 1 означает POWER ON,
        // а на физический выход подаётся обратный уровень.
        bool outputLevel =
            (i == 8) ? !logicalState : logicalState;

        digitalWrite(
            BTN_PINS[i],
            outputLevel ? HIGH : LOW
        );
    }

    buttonMask = newMask;
}

void releaseMomentaryButtons()
{
    uint16_t newMask =
        buttonMask & ~MOMENTARY_BUTTON_MASK;

    applyButtonMask(newMask);
}

// ============================================================================
// WEBSOCKET
// ============================================================================

void sendLcdSnapshot(AsyncWebSocketClient *client)
{
    if (!client || !client->canSend()) return;
    uint8_t packet[102];
    packet[0] = 0xFD;
    // This lock protects the model on core 0 only. The acquisition producer
    // never takes it, even while all 96 bytes are copied here.
    portENTER_CRITICAL(&lcdMux);
    lcd.snapshot(&packet[1], &packet[33]);
    packet[97] = lcd.revision();
    const uint32_t clear = lcd.clearGeneration();
    for (unsigned i = 0; i < 4; ++i) packet[98 + i] = (clear >> (8 * i)) & 0xFF;
    portEXIT_CRITICAL(&lcdMux);
    client->binary(packet, sizeof(packet));
}

void sendButtonState(AsyncWebSocketClient *client)
{
    char state[11] = "S000000000";
    const uint16_t mask = buttonMask;
    for (unsigned i = 0; i < 9; ++i) state[i + 1] = mask & (1U << i) ? '1' : '0';
    client->text(state, 10);
}

void handleWebSocketEvent(
    AsyncWebSocket *serverSocket,
    AsyncWebSocketClient *client,
    AwsEventType type,
    void *arg,
    uint8_t *data,
    size_t length
) {
    if (type == WS_EVT_CONNECT) {
        sendButtonState(client);
        return;
    }

    if (type == WS_EVT_DISCONNECT) {
        releaseMomentaryButtons();
        return;
    }

    if (type != WS_EVT_DATA) {
        return;
    }

    AwsFrameInfo *info =
        static_cast<AwsFrameInfo *>(arg);

    if (
        info == nullptr ||
        !info->final ||
        info->index != 0 ||
        info->len != length ||
        info->opcode != WS_TEXT
    ) {
        return;
    }

    if (
        length == 1 &&
        data[0] == 'L' &&
        !webUpdateInProgress
    ) {
        sendLcdSnapshot(client);
        return;
    }

    if (
        length == 10 &&
        data[0] == 'B' &&
        !webUpdateInProgress
    ) {
        uint16_t newMask = 0;

        for (uint8_t i = 0; i < 9; i++) {
            if (data[i + 1] == '1') {
                newMask |= 1U << i;
            }
            else if (data[i + 1] != '0') {
                return;
            }
        }

        applyButtonMask(newMask);
    }
}

// ============================================================================
// WEB-ЗАДАЧА — ЯДРО 0
// ============================================================================

void TaskWeb(void *parameter)
{
    uint32_t lastCleanupMs = 0;

    for (;;) {
        drainCapturedLcd();
        uint32_t now = millis();

        if (
            webUpdateRestartPending &&
            (int32_t)(now - webUpdateRestartAtMs) >= 0
        ) {
            ESP.restart();
        }

        if (
            webUpdateInProgress &&
            (uint32_t)(now - webUpdateLastActivityMs) >= 60000
        ) {
            Update.abort();
            webUpdateFailed = true;
            webUpdateInProgress = false;
            Serial.println("HTTP update aborted: timeout");
        }

        if ((uint32_t)(now - lastCleanupMs) >= 5000) {
            lastCleanupMs = now;
            ws.cleanupClients();
        }

        vTaskDelay(1);
    }
}

// ============================================================================
// SETUP
// ============================================================================

void TaskNetworkSetup(void *parameter)
{
    prefs.begin("wifi", false);

    String savedSsid = prefs.getString("s", "");
    String savedPassword = prefs.getString("p", "");

    if (savedSsid.length() > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    }

    int attempts = 0;

    while (
        WiFi.status() != WL_CONNECTED &&
        attempts < 20
    ) {
        delay(500);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    }
    else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("MFJ993b-CONFIG", "12345678");

        Serial.print("Config AP: ");
        Serial.println(WiFi.softAPIP());
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        const char *page =
            (WiFi.status() == WL_CONNECTED)
                ? indexHtml
                : configHtml;

        AsyncWebServerResponse *response =
            request->beginResponse_P(
                200,
                "text/html; charset=utf-8",
                page
            );

        response->addHeader(
            "Cache-Control",
            "no-store, no-cache, must-revalidate, max-age=0"
        );
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");

        request->send(response);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (
            !request->hasParam("s", true) ||
            !request->hasParam("p", true)
        ) {
            request->send(
                400,
                "text/plain",
                "SSID or password missing"
            );
            return;
        }

        String ssid =
            request->getParam("s", true)->value();

        String password =
            request->getParam("p", true)->value();

        prefs.putString("s", ssid);
        prefs.putString("p", password);

        request->send(
            200,
            "text/plain",
            "SAVED. RESTARTING..."
        );

        webUpdateRestartAtMs = millis() + 500;
        webUpdateRestartPending = true;
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(
                200,
                "text/html; charset=utf-8",
                updateHtml
            );

        response->addHeader(
            "Cache-Control",
            "no-store, no-cache, must-revalidate, max-age=0"
        );
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");

        request->send(response);
    });

    server.on(
        "/update",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {
            bool success =
                webUpdateSucceeded &&
                !webUpdateFailed &&
                !Update.hasError();

            AsyncWebServerResponse *response =
                request->beginResponse(
                    success ? 200 : 500,
                    "text/plain; charset=utf-8",
                    success
                        ? "OK. RESTARTING..."
                        : "UPDATE FAILED"
                );

            response->addHeader("Connection", "close");
            request->send(response);

            webUpdateInProgress = false;

            if (success) {
                webUpdateRestartAtMs = millis() + 2000;
                webUpdateRestartPending = true;
            }
        },
        [](AsyncWebServerRequest *request,
           const String& filename,
           size_t index,
           uint8_t *data,
           size_t length,
           bool final) {
            if (index == 0) {
                webUpdateFailed = false;
                webUpdateSucceeded = false;
                webUpdateRestartPending = false;
                webUpdateLastActivityMs = millis();
                webUpdateInProgress = true;

                releaseMomentaryButtons();

                resetCapturedLcd(false);

                Serial.printf(
                    "HTTP update: %s\n",
                    filename.c_str()
                );

                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    webUpdateFailed = true;
                    Update.printError(Serial);
                }
            }

            if (
                !webUpdateFailed &&
                length > 0 &&
                Update.write(data, length) != length
            ) {
                webUpdateFailed = true;
                Update.printError(Serial);
            }

            if (length > 0) {
                webUpdateLastActivityMs = millis();
            }

            if (final) {
                if (!webUpdateFailed) {
                    if (Update.end(true)) {
                        webUpdateSucceeded = true;

                        Serial.printf(
                            "HTTP update complete: %u bytes\n",
                            (unsigned int)(index + length)
                        );
                    }
                    else {
                        webUpdateFailed = true;
                        Update.printError(Serial);
                    }
                }
                else {
                    Update.end();
                }
            }
        }
    );

    ws.onEvent(handleWebSocketEvent);
    server.addHandler(&ws);
    server.begin();

    Serial.println("LCD capture: SAMPLE_DELAY=110, capture=s1");
    Serial.println("LCD mirror: raw SPSC capture, live model, no frame-matching gates");
    Serial.println("Nibble sync: reset incomplete byte only");
    vTaskDelete(nullptr);
}


void setup()
{
    Serial.begin(460800);
    setCpuFrequencyMhz(240);

    resetCapturedLcd(true);

    for (uint8_t i = 0; i < 9; i++) {
        pinMode(BTN_PINS[i], OUTPUT);
    }

    applyButtonMask(INITIAL_BUTTON_MASK);

    pinMode(PIN_E, INPUT_PULLDOWN);
    pinMode(PIN_RS, INPUT);
    pinMode(PIN_DB4, INPUT);
    pinMode(PIN_DB5, INPUT);
    pinMode(PIN_DB6, INPUT);
    pinMode(PIN_DB7, INPUT);

    // Capture starts as soon as setup returns, before WiFi waits for a link.
    // Decode and network initialization run independently on core 0.
    const BaseType_t modelTaskCreated = xTaskCreatePinnedToCore(
        TaskWeb, "LCD model", 6144, nullptr, 2, nullptr, 0);
    const BaseType_t networkTaskCreated = xTaskCreatePinnedToCore(
        TaskNetworkSetup, "Network startup", 8192, nullptr, 1, nullptr, 0);
    configASSERT(modelTaskCreated == pdPASS && networkTaskCreated == pdPASS);
}

// ============================================================================
// ЗАХВАТ LCD — ЯДРО 1
// ============================================================================

static inline uint8_t readNibble(uint32_t reg)
{
    return ((reg & MASK_DB4) ? 1 : 0) |
           ((reg & MASK_DB5) ? 2 : 0) |
           ((reg & MASK_DB6) ? 4 : 0) |
           ((reg & MASK_DB7) ? 8 : 0);
}

void loop()
{
    static uint32_t lastPulseCycles = 0;
    static bool gapPending = true, lossPending = false, stuckEnable = false;

    if (webUpdateInProgress || webUpdateRestartPending) {
        gapPending = true;
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    const uint32_t bus = REG_READ(GPIO_IN_REG);
    if (!(bus & MASK_E)) {
        stuckEnable = false;
        if (!gapPending &&
            static_cast<uint32_t>(xthal_get_ccount() - lastPulseCycles) >
                NIBBLE_TIMEOUT_CYCLES)
            gapPending = true;
        return;
    }
    if (stuckEnable) return;

    const uint32_t start = xthal_get_ccount();
    while (static_cast<uint32_t>(xthal_get_ccount() - start) < SAMPLE_DELAY) {
        // Preserve the tested sample point. No queue, decoding or lock here.
    }
    const uint32_t sampledBus = REG_READ(GPIO_IN_REG);  // s1, never s1 & s2

    while (REG_READ(GPIO_IN_REG) & MASK_E) {
        if (static_cast<uint32_t>(xthal_get_ccount() - start) > ENABLE_STUCK_CYCLES) {
            stuckEnable = lossPending = true;
            return;  // A stuck E cannot freeze HTTP or fabricate repeated bytes.
        }
    }

    // Also detect a gap if this core was preempted for the whole idle period.
    // This runs after sampling, so the tested 110-cycle point is unchanged.
    if (lastPulseCycles != 0 &&
        static_cast<uint32_t>(start - lastPulseCycles) > NIBBLE_TIMEOUT_CYCLES)
        gapPending = true;

    uint8_t flags = readNibble(sampledBus);
    if (sampledBus & MASK_RS) flags |= SAMPLE_RS;
    if (gapPending) flags |= SAMPLE_GAP;
    if (lossPending) flags |= SAMPLE_LOSS;

    const uint32_t epoch = __atomic_load_n(&captureEpoch, __ATOMIC_ACQUIRE);
    const CaptureSample sample = {start, (epoch << 8) | flags};
    if (captureRing.push(sample)) lossPending = false;
    else { lossPending = true; ++droppedNibbles; }

    lastPulseCycles = start;
    gapPending = false;
}
