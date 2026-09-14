// MFJ-993B Remote Control — terminal-proven LCD capture with a web mirror
// Classic dual-core ESP32. The LCD bus is observed only; R/W remains grounded.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Update.h>
#include "soc/gpio_reg.h"

// ============================================================================
// HARDWARE
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
    MASK_RS | MASK_DB4 | MASK_DB5 | MASK_DB6 | MASK_DB7;

// This is the sample point proven by the terminal capture.
const uint32_t SAMPLE_DELAY = 110;
const uint32_t NIBBLE_TIMEOUT_US = 5000;
const uint32_t DISPLAY_IDLE_US = 12000;
const uint32_t CGRAM_BLOCK_TIMEOUT_US = 10000;

const uint16_t MOMENTARY_BUTTON_MASK =
    (1U << 1) | (1U << 2) | (1U << 4) |
    (1U << 5) | (1U << 6) | (1U << 7);

const uint16_t INITIAL_BUTTON_MASK =
    (1U << 3) |  // AUTO
    (1U << 8);   // POWER ON

// ============================================================================
// NETWORK AND UPDATE STATE
// ============================================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

volatile uint16_t buttonMask = INITIAL_BUTTON_MASK;
volatile bool webUpdateInProgress = false;
volatile bool webUpdateFailed = false;
volatile bool webUpdateSucceeded = false;
volatile bool webUpdateRestartPending = false;
volatile uint32_t webUpdateRestartAtMs = 0;
volatile uint32_t webUpdateLastActivityMs = 0;
char webUpdateError[128] = "";

// ============================================================================
// LCD STATE — THE SAME MODEL AS THE TERMINAL TEST
// ============================================================================

enum LcdAddressSpace : uint8_t {
    LCD_SPACE_NONE = 0,
    LCD_SPACE_DDRAM,
    LCD_SPACE_CGRAM
};

uint8_t lcdScreen[32];
uint8_t publishedScreen[32];

LcdAddressSpace lcdAddressSpace = LCD_SPACE_NONE;
uint8_t lcdAddress = 0;
uint8_t lcdCgramAddress = 0;
bool entryIncrement = true;

uint8_t nibbleStage = 0;
uint8_t firstNibble = 0;
bool lastRs = false;
uint32_t lastBusTime = 0;
bool cgramCandidateActive = false;
uint32_t cgramCandidateLastUs = 0;

volatile uint32_t lcdRevision = 0;
volatile uint32_t publishedRevision = 0;
volatile uint32_t lastLcdChangeUs = 0;
volatile bool captureResetRequested = false;

portMUX_TYPE lcdMux = portMUX_INITIALIZER_UNLOCKED;

void clearLcdBuffers()
{
    portENTER_CRITICAL(&lcdMux);
    memset(lcdScreen, ' ', sizeof(lcdScreen));
    memset(publishedScreen, ' ', sizeof(publishedScreen));
    ++lcdRevision;
    publishedRevision = lcdRevision;
    lastLcdChangeUs = micros();
    portEXIT_CRITICAL(&lcdMux);
}

void resetDecoder()
{
    nibbleStage = 0;
    firstNibble = 0;
    lastRs = false;
    lastBusTime = 0;
    lcdAddressSpace = LCD_SPACE_NONE;
    lcdAddress = 0;
    lcdCgramAddress = 0;
    entryIncrement = true;
    cgramCandidateActive = false;
    cgramCandidateLastUs = 0;
}

void requestCaptureReset(bool clearScreen)
{
    if (clearScreen) clearLcdBuffers();
    __atomic_store_n(&captureResetRequested, true, __ATOMIC_RELEASE);
}

static inline uint8_t readNibble(uint32_t reg)
{
    uint8_t nibble = 0;
    if (reg & MASK_DB4) nibble |= 0x01;
    if (reg & MASK_DB5) nibble |= 0x02;
    if (reg & MASK_DB6) nibble |= 0x04;
    if (reg & MASK_DB7) nibble |= 0x08;
    return nibble;
}

static inline int addressToPosition(uint8_t address)
{
    if (address <= 0x0F) return address;
    if (address >= 0x40 && address <= 0x4F)
        return 16 + address - 0x40;
    return -1;
}

static inline bool isVisibleAddressCommand(uint8_t command)
{
    return
        (command >= 0x80 && command <= 0x8F) ||
        (command >= 0xC0 && command <= 0xCF);
}

static inline void advanceDdramAddress()
{
    if (entryIncrement)
        lcdAddress = static_cast<uint8_t>((lcdAddress + 1) & 0x7F);
    else
        lcdAddress = static_cast<uint8_t>((lcdAddress - 1) & 0x7F);
}

void processCommand(uint8_t command, uint32_t now)
{
    if (command == 0x01) {
        portENTER_CRITICAL(&lcdMux);
        memset(lcdScreen, ' ', sizeof(lcdScreen));
        ++lcdRevision;
        lastLcdChangeUs = now;
        portEXIT_CRITICAL(&lcdMux);

        lcdAddress = 0;
        lcdAddressSpace = LCD_SPACE_DDRAM;
        cgramCandidateActive = false;
        return;
    }

    if (command == 0x02 || command == 0x03) {
        lcdAddress = 0;
        lcdAddressSpace = LCD_SPACE_DDRAM;
        cgramCandidateActive = false;
        return;
    }

    if ((command & 0xFC) == 0x04) {
        entryIncrement = (command & 0x02) != 0;
        cgramCandidateActive = false;
        return;
    }

    if ((command & 0xC0) == 0x40) {
        lcdCgramAddress = command & 0x3F;
        cgramCandidateLastUs = now;
        cgramCandidateActive = true;
        lcdAddressSpace = LCD_SPACE_CGRAM;
        return;
    }

    if (isVisibleAddressCommand(command)) {
        lcdAddress = command & 0x7F;
        lcdAddressSpace = LCD_SPACE_DDRAM;
        cgramCandidateActive = false;
        return;
    }

    // A hidden DDRAM address must never redirect following data into a
    // previously visible cell.
    if (command & 0x80) {
        lcdAddressSpace = LCD_SPACE_NONE;
        cgramCandidateActive = false;
    }
}

void processCgramData(uint8_t value, uint32_t now)
{
    if (!cgramCandidateActive ||
        static_cast<uint32_t>(now - cgramCandidateLastUs) >
            CGRAM_BLOCK_TIMEOUT_US ||
        (value & 0xE0) != 0) {
        cgramCandidateActive = false;
        lcdAddressSpace = LCD_SPACE_NONE;
        return;
    }

    cgramCandidateLastUs = now;

    if (entryIncrement)
        lcdCgramAddress =
            static_cast<uint8_t>((lcdCgramAddress + 1) & 0x3F);
    else
        lcdCgramAddress =
            static_cast<uint8_t>((lcdCgramAddress - 1) & 0x3F);
}

void processDdramData(uint8_t value, uint32_t now)
{
    const int position = addressToPosition(lcdAddress);

    if (position < 0 || position >= 32) {
        lcdAddressSpace = LCD_SPACE_NONE;
        return;
    }

    portENTER_CRITICAL(&lcdMux);
    if (lcdScreen[position] != value) {
        lcdScreen[position] = value;
        ++lcdRevision;
        lastLcdChangeUs = now;
    }
    portEXIT_CRITICAL(&lcdMux);

    advanceDdramAddress();

    // A 16-character row cannot spill into hidden DDRAM or into the other row.
    if (addressToPosition(lcdAddress) < 0)
        lcdAddressSpace = LCD_SPACE_NONE;
}

void processData(uint8_t value, uint32_t now)
{
    if (lcdAddressSpace == LCD_SPACE_CGRAM)
        processCgramData(value, now);
    else if (lcdAddressSpace == LCD_SPACE_DDRAM)
        processDdramData(value, now);
}

static inline void processCapturedNibble(
    uint8_t nibble,
    bool currentRs,
    uint32_t now
) {
    if (lastBusTime != 0 &&
        static_cast<uint32_t>(now - lastBusTime) > NIBBLE_TIMEOUT_US)
        nibbleStage = 0;

    if (currentRs != lastRs)
        nibbleStage = 0;

    lastRs = currentRs;
    lastBusTime = now;

    if (nibbleStage == 0) {
        firstNibble = nibble;
        nibbleStage = 1;
        return;
    }

    const uint8_t value =
        static_cast<uint8_t>((firstNibble << 4) | nibble);
    nibbleStage = 0;

    if (currentRs) processData(value, now);
    else processCommand(value, now);
}

void copyStableScreen(uint8_t *destination, uint32_t &revision)
{
    const uint32_t now = micros();

    portENTER_CRITICAL(&lcdMux);

    if (lcdRevision != publishedRevision &&
        static_cast<uint32_t>(now - lastLcdChangeUs) >= DISPLAY_IDLE_US) {
        memcpy(publishedScreen, lcdScreen, sizeof(publishedScreen));
        publishedRevision = lcdRevision;
    }

    memcpy(destination, publishedScreen, sizeof(publishedScreen));
    revision = publishedRevision;

    portEXIT_CRITICAL(&lcdMux);
}

// ============================================================================
// WEB INTERFACE
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
  const MOMENTARY_MASK =
    (1 << 1) | (1 << 2) | (1 << 4) |
    (1 << 5) | (1 << 6) | (1 << 7);
  const POLL_IDLE_MS = 20;
  const POLL_HELD_MS = 50;

  let socket = null;
  let reconnectTimer = null;
  let pollTimer = null;
  let requestWatchdog = null;
  let requestPending = false;
  let buttonMask = (1 << 3) | (1 << 8);
  let specialSequenceRunning = false;
  const lastScreen = new Uint8Array(32).fill(0x20);
  const lastCharacters = Array(32).fill('');

  const rows = [
    document.getElementById('row1'),
    document.getElementById('row2')
  ];

  for (let i = 0; i < 32; i++) {
    const cell = document.createElement('div');
    cell.className = 'cell';
    rows[i < 16 ? 0 : 1].appendChild(cell);
  }

  function characterFor(value) {
    if (value <= 0x07) return '\u00a0';
    if (value === 0xE4) return '\u00b5';
    if (value >= 0x20 && value <= 0x7E)
      return String.fromCharCode(value);
    return '\u00a0';
  }

  function renderBytes(bytes) {
    for (let i = 0; i < 32; i++) lastScreen[i] = bytes[i];

    if (!(buttonMask & (1 << 8))) {
      const off = new Uint8Array(32).fill(0x20);
      const text = 'POWER OFF';
      for (let i = 0; i < text.length; i++)
        off[3 + i] = text.charCodeAt(i);
      bytes = off;
    }

    for (let i = 0; i < 32; i++) {
      const character = characterFor(bytes[i]);
      if (lastCharacters[i] === character) continue;
      rows[i < 16 ? 0 : 1].children[i % 16].textContent = character;
      lastCharacters[i] = character;
    }
  }

  function requestLcd() {
    pollTimer = null;
    if (requestPending || !socket || socket.readyState !== WebSocket.OPEN)
      return;

    requestPending = true;
    const connection = socket;
    connection.send('L');

    clearTimeout(requestWatchdog);
    requestWatchdog = setTimeout(() => {
      if (socket === connection && requestPending) connection.close();
    }, 2000);
  }

  function scheduleLcd(immediate) {
    clearTimeout(pollTimer);
    if (immediate && !requestPending) {
      requestLcd();
      return;
    }

    pollTimer = setTimeout(
      requestLcd,
      buttonMask & MOMENTARY_MASK ? POLL_HELD_MS : POLL_IDLE_MS
    );
  }

  function connect() {
    if (socket &&
        (socket.readyState === WebSocket.OPEN ||
         socket.readyState === WebSocket.CONNECTING))
      return;

    const connection = new WebSocket(
      (location.protocol === 'https:' ? 'wss://' : 'ws://') +
      location.host + '/ws'
    );

    socket = connection;
    connection.binaryType = 'arraybuffer';

    connection.onopen = () => {
      if (socket !== connection) return;
      document.getElementById('status').className = 'on';
    };

    connection.onclose = () => {
      if (socket !== connection) return;
      document.getElementById('status').className = 'off';
      clearTimeout(pollTimer);
      clearTimeout(requestWatchdog);
      requestPending = false;
      releaseMomentaryButtons();

      if (!reconnectTimer) {
        reconnectTimer = setTimeout(() => {
          reconnectTimer = null;
          connect();
        }, 1500);
      }
    };

    connection.onerror = () => {
      if (socket === connection)
        document.getElementById('status').className = 'off';
    };

    connection.onmessage = event => {
      if (socket !== connection) return;

      if (typeof event.data === 'string') {
        if (/^S[01]{9}$/.test(event.data)) {
          buttonMask = 0;
          for (let i = 0; i < 9; i++) {
            if (event.data[i + 1] === '1') buttonMask |= 1 << i;
            updateButtonVisual(i);
          }
          renderBytes(lastScreen);
          scheduleLcd(true);
        }
        return;
      }

      const data = new Uint8Array(event.data);
      if (data.length !== 37 || data[0] !== PACKET_LCD) return;

      clearTimeout(requestWatchdog);
      requestPending = false;
      renderBytes(data.subarray(1, 33));
      scheduleLcd(false);
    };
  }

  function sendButtons() {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;

    let message = 'B';
    for (let i = 0; i < 9; i++)
      message += buttonMask & (1 << i) ? '1' : '0';

    socket.send(message);
    scheduleLcd(true);
  }

  function updateButtonVisual(index) {
    const button = document.getElementById('b' + index);
    if (!button) return;

    const active = !!(buttonMask & (1 << index));
    button.classList.toggle('active', active);

    if (index === 0) button.textContent = active ? 'ANT2' : 'ANT1';
    if (index === 3) button.textContent = active ? 'AUTO' : 'MANUAL';
    if (index === 8) button.textContent = active ? 'POWER ON' : 'POWER OFF';
  }

  function setButton(index, active, sendNow = true) {
    if (active) buttonMask |= 1 << index;
    else buttonMask &= ~(1 << index);

    updateButtonVisual(index);
    if (index === 8) {
      if (active) lastScreen.fill(0x20);
      renderBytes(lastScreen);
    }
    if (sendNow) sendButtons();
  }

  function toggleButton(index) {
    setButton(index, !(buttonMask & (1 << index)));
  }

  function bindMomentary(index) {
    const button = document.getElementById('b' + index);
    let pressed = false;

    button.addEventListener('pointerdown', event => {
      event.preventDefault();
      if (pressed) return;
      pressed = true;
      if (button.setPointerCapture)
        button.setPointerCapture(event.pointerId);
      setButton(index, true);
    });

    const release = event => {
      if (!pressed) return;
      pressed = false;
      if (event) event.preventDefault();
      setButton(index, false);
    };

    button.addEventListener('pointerup', release);
    button.addEventListener('pointercancel', release);
    button.addEventListener('lostpointercapture', release);
  }

  [1, 2, 4, 5, 6, 7].forEach(bindMomentary);

  document.querySelectorAll('[data-pins]').forEach(button => {
    const pins = button.dataset.pins.split(',').map(Number);
    let pressed = false;

    button.addEventListener('pointerdown', event => {
      event.preventDefault();
      if (pressed) return;
      if (button.dataset.confirm && !confirm(button.dataset.confirm)) return;

      pressed = true;
      button.classList.add('active');
      if (button.setPointerCapture)
        button.setPointerCapture(event.pointerId);
      pins.forEach(index => setButton(index, true, false));
      sendButtons();
    });

    const release = event => {
      if (!pressed) return;
      pressed = false;
      if (event) event.preventDefault();
      button.classList.remove('active');
      pins.forEach(index => setButton(index, false, false));
      sendButtons();
    };

    button.addEventListener('pointerup', release);
    button.addEventListener('pointercancel', release);
    button.addEventListener('lostpointercapture', release);
  });

  const waitMs = milliseconds =>
    new Promise(resolve => setTimeout(resolve, milliseconds));

  async function runLcLimitSequence(button) {
    if (specialSequenceRunning) return;
    if (!confirm(
      'LC LIMIT является защитой тюнера. Выполнить сочетание MODE, затем C-UP + L-UP?'
    )) return;

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
    if (button.dataset.confirm && !confirm(button.dataset.confirm)) return;

    const pins = button.dataset.poweronPins.split(',').map(Number);
    specialSequenceRunning = true;
    button.classList.add('active');
    releaseMomentaryButtons();

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

  document.getElementById('lcLimitButton').addEventListener('click', event => {
    event.preventDefault();
    runLcLimitSequence(event.currentTarget);
  });

  document.querySelectorAll('[data-poweron-pins]').forEach(button => {
    button.addEventListener('click', event => {
      event.preventDefault();
      runPowerOnSequence(event.currentTarget);
    });
  });

  function releaseMomentaryButtons() {
    let changed = false;

    [1, 2, 4, 5, 6, 7].forEach(index => {
      if (buttonMask & (1 << index)) changed = true;
      setButton(index, false, false);
    });

    if (changed) sendButtons();
  }

  window.addEventListener('blur', releaseMomentaryButtons);
  window.addEventListener('pagehide', releaseMomentaryButtons);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) releaseMomentaryButtons();
  });

  function checkPower() {
    if (buttonMask & (1 << 8))
      document.getElementById('modal').style.display = 'block';
    else
      toggleButton(8);
  }

  function confirmPower(confirmed) {
    document.getElementById('modal').style.display = 'none';
    if (confirmed) toggleButton(8);
  }

  [0, 3, 8].forEach(updateButtonVisual);
  renderBytes(lastScreen);
  connect();
</script>
</body>
</html>
)rawliteral";

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
// BUTTON OUTPUTS
// ============================================================================

void applyButtonMask(uint16_t newMask)
{
    newMask &= 0x01FF;

    const bool powerWasOn = (buttonMask & (1U << 8)) != 0;
    const bool powerWillBeOn = (newMask & (1U << 8)) != 0;

    if (!powerWasOn && powerWillBeOn)
        requestCaptureReset(true);

    for (uint8_t i = 0; i < 9; i++) {
        const bool logicalState = (newMask & (1U << i)) != 0;
        const bool outputLevel = (i == 8) ? !logicalState : logicalState;
        digitalWrite(BTN_PINS[i], outputLevel ? HIGH : LOW);
    }

    buttonMask = newMask;
}

void releaseMomentaryButtons()
{
    applyButtonMask(buttonMask & ~MOMENTARY_BUTTON_MASK);
}

// ============================================================================
// WEBSOCKET
// ============================================================================

void sendLcdSnapshot(AsyncWebSocketClient *client)
{
    if (!client || !client->canSend()) return;

    uint8_t packet[37];
    uint32_t revision = 0;
    packet[0] = 0xFD;
    copyStableScreen(&packet[1], revision);

    packet[33] = revision & 0xFF;
    packet[34] = (revision >> 8) & 0xFF;
    packet[35] = (revision >> 16) & 0xFF;
    packet[36] = (revision >> 24) & 0xFF;

    client->binary(packet, sizeof(packet));
}

void sendButtonState(AsyncWebSocketClient *client)
{
    if (!client || !client->canSend()) return;

    char state[11] = "S000000000";
    const uint16_t mask = buttonMask;

    for (uint8_t i = 0; i < 9; i++)
        state[i + 1] = mask & (1U << i) ? '1' : '0';

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

    if (type != WS_EVT_DATA) return;

    AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);

    if (!info || !info->final || info->index != 0 ||
        info->len != length || info->opcode != WS_TEXT)
        return;

    if (length == 1 && data[0] == 'L' && !webUpdateInProgress) {
        sendLcdSnapshot(client);
        return;
    }

    if (length != 10 || data[0] != 'B' || webUpdateInProgress)
        return;

    uint16_t newMask = 0;

    for (uint8_t i = 0; i < 9; i++) {
        if (data[i + 1] == '1')
            newMask |= 1U << i;
        else if (data[i + 1] != '0')
            return;
    }

    applyButtonMask(newMask);
}

// ============================================================================
// HTTP AND WIFI — CORE 0
// ============================================================================

void addNoCacheHeaders(AsyncWebServerResponse *response)
{
    response->addHeader(
        "Cache-Control",
        "no-store, no-cache, must-revalidate, max-age=0"
    );
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
}

void rememberUpdateError(const char *stage)
{
    snprintf(
        webUpdateError,
        sizeof(webUpdateError),
        "%s: %s",
        stage,
        Update.errorString()
    );
}

void TaskNetwork(void *parameter)
{
    prefs.begin("wifi", false);

    const String savedSsid = prefs.getString("s", "");
    const String savedPassword = prefs.getString("p", "");

    bool stationConnected = false;

    if (savedSsid.length() > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setHostname("mfj993b-remote");
        WiFi.begin(savedSsid.c_str(), savedPassword.c_str());

        for (uint8_t attempt = 0;
             attempt < 20 && WiFi.status() != WL_CONNECTED;
             attempt++)
            vTaskDelay(pdMS_TO_TICKS(500));

        stationConnected = WiFi.status() == WL_CONNECTED;
    }

    if (!stationConnected) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("MFJ993b-CONFIG", "12345678");
    }

    server.on("/", HTTP_GET, [stationConnected](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(
            200,
            "text/html; charset=utf-8",
            stationConnected ? indexHtml : configHtml
        );
        addNoCacheHeaders(response);
        request->send(response);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("s", true) ||
            !request->hasParam("p", true)) {
            request->send(400, "text/plain", "SSID or password missing");
            return;
        }

        prefs.putString("s", request->getParam("s", true)->value());
        prefs.putString("p", request->getParam("p", true)->value());
        request->send(200, "text/plain", "SAVED. RESTARTING...");

        webUpdateRestartAtMs = millis() + 500;
        webUpdateRestartPending = true;
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(
            200,
            "text/html; charset=utf-8",
            updateHtml
        );
        addNoCacheHeaders(response);
        request->send(response);
    });

    server.on(
        "/update",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {
            const bool success =
                webUpdateSucceeded &&
                !webUpdateFailed &&
                !Update.hasError();

            String message;

            if (success) {
                message = "OK. RESTARTING...";
            }
            else {
                message = "UPDATE FAILED: ";
                message += webUpdateError[0]
                    ? webUpdateError
                    : Update.errorString();
            }

            AsyncWebServerResponse *response = request->beginResponse(
                success ? 200 : 500,
                "text/plain; charset=utf-8",
                message
            );
            response->addHeader("Connection", "close");
            request->send(response);

            webUpdateInProgress = false;
            requestCaptureReset(false);

            if (success) {
                webUpdateRestartAtMs = millis() + 1500;
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
                webUpdateError[0] = '\0';

                releaseMomentaryButtons();
                requestCaptureReset(false);

                String lowerName = filename;
                lowerName.toLowerCase();

                if (!lowerName.endsWith(".ino.bin")) {
                    webUpdateFailed = true;
                    snprintf(
                        webUpdateError,
                        sizeof(webUpdateError),
                        "select the application *.ino.bin file"
                    );
                }
                else if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    webUpdateFailed = true;
                    rememberUpdateError("begin");
                }
            }

            if (length > 0)
                webUpdateLastActivityMs = millis();

            if (!webUpdateFailed && length > 0 &&
                Update.write(data, length) != length) {
                webUpdateFailed = true;
                rememberUpdateError("write");
            }

            if (final) {
                if (!webUpdateFailed) {
                    if (Update.end(true))
                        webUpdateSucceeded = true;
                    else {
                        webUpdateFailed = true;
                        rememberUpdateError("finish");
                    }
                }
                else {
                    Update.abort();
                }
            }
        }
    );

    ws.onEvent(handleWebSocketEvent);
    server.addHandler(&ws);
    server.begin();

    uint32_t lastCleanupMs = 0;

    for (;;) {
        const uint32_t now = millis();

        if (webUpdateRestartPending &&
            static_cast<int32_t>(now - webUpdateRestartAtMs) >= 0)
            ESP.restart();

        if (webUpdateInProgress &&
            static_cast<uint32_t>(now - webUpdateLastActivityMs) >= 300000) {
            Update.abort();
            webUpdateFailed = true;
            snprintf(
                webUpdateError,
                sizeof(webUpdateError),
                "timeout while receiving the file"
            );
            webUpdateInProgress = false;
            requestCaptureReset(false);
        }

        if (static_cast<uint32_t>(now - lastCleanupMs) >= 5000) {
            lastCleanupMs = now;
            ws.cleanupClients();
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// ============================================================================
// STARTUP
// ============================================================================

void setup()
{
    setCpuFrequencyMhz(240);

    memset(lcdScreen, ' ', sizeof(lcdScreen));
    memset(publishedScreen, ' ', sizeof(publishedScreen));
    lastLcdChangeUs = micros();
    resetDecoder();

    for (uint8_t i = 0; i < 9; i++) {
        const bool logicalState =
            (INITIAL_BUTTON_MASK & (1U << i)) != 0;
        const bool outputLevel =
            (i == 8) ? !logicalState : logicalState;

        digitalWrite(BTN_PINS[i], outputLevel ? HIGH : LOW);
        pinMode(BTN_PINS[i], OUTPUT);
    }

    buttonMask = INITIAL_BUTTON_MASK;

    pinMode(PIN_E, INPUT_PULLDOWN);
    pinMode(PIN_RS, INPUT);
    pinMode(PIN_DB4, INPUT);
    pinMode(PIN_DB5, INPUT);
    pinMode(PIN_DB6, INPUT);
    pinMode(PIN_DB7, INPUT);

    const BaseType_t networkTaskCreated = xTaskCreatePinnedToCore(
        TaskNetwork,
        "Network",
        8192,
        nullptr,
        1,
        nullptr,
        0
    );

    configASSERT(networkTaskCreated == pdPASS);
}

// ============================================================================
// LCD CAPTURE — CORE 1, COPIED FROM THE SUCCESSFUL TERMINAL METHOD
// ============================================================================

void loop()
{
    if (__atomic_exchange_n(
            &captureResetRequested,
            false,
            __ATOMIC_ACQ_REL))
        resetDecoder();

    if (webUpdateInProgress || webUpdateRestartPending) {
        vTaskDelay(pdMS_TO_TICKS(2));
        return;
    }

    if (!(REG_READ(GPIO_IN_REG) & MASK_E))
        return;

    const uint32_t start = xthal_get_ccount();

    while (static_cast<uint32_t>(
               xthal_get_ccount() - start) < SAMPLE_DELAY) {
        // Timing-critical: keep this loop empty.
    }

    const uint32_t sample1 = REG_READ(GPIO_IN_REG);
    const uint32_t sample2 = REG_READ(GPIO_IN_REG);
    (void)sample2;

    while (REG_READ(GPIO_IN_REG) & MASK_E) {
        // Process each E pulse exactly once, after the falling edge.
    }

    processCapturedNibble(
        readNibble(sample1),
        (sample1 & MASK_RS) != 0,
        micros()
    );
}
