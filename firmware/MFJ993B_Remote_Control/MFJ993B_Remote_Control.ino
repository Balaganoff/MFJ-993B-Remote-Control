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
const uint32_t CGRAM_BLOCK_TIMEOUT_US = 10000;
const uint32_t DISPLAY_IDLE_US = 12000;

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
// КОПИЯ LCD: DDRAM + ДИНАМИЧЕСКАЯ CGRAM
// ============================================================================

enum LcdAddressSpace : uint8_t {
    LCD_SPACE_NONE = 0,
    LCD_SPACE_DDRAM,
    LCD_SPACE_CGRAM
};

uint8_t lcdScreen[32];
uint8_t lcdCgram[8][8];
uint8_t lcdCgramKnownRows[8];

volatile LcdAddressSpace lcdAddressSpace = LCD_SPACE_NONE;
volatile uint8_t lcdAddress = 0;
volatile uint8_t lcdCgramAddress = 0;
volatile bool entryIncrement = true;

portMUX_TYPE lcdMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint8_t lcdVersion = 0;
// Время последнего реального изменения текстового экрана (DDRAM).
// Динамическая CGRAM обновляется отдельно и не должна задерживать цифры.
volatile uint32_t lastDdramChangeUs = 0;

// Одна команда 0x40...0x7F ещё не доказывает запись CGRAM: на общей
// шине возможен ошибочно собранный байт. Принимаем пиксели только сразу
// после Set CGRAM Address и только в допустимом диапазоне 0x00...0x1F.
bool cgramCandidateActive = false;
uint32_t cgramCandidateLastUs = 0;

// ============================================================================
// СБОРКА БАЙТА
// ============================================================================

uint8_t stage = 0;
uint8_t firstNibble = 0;
bool lastRs = false;
uint32_t lastBusTime = 0;

// ============================================================================
// ВНУТРЕННИЕ СЧЁТЧИКИ ЗАХВАТА
// На web-страницу и в Serial эти значения не выводятся.
// ============================================================================

volatile uint32_t pulseCounter = 0;
volatile uint32_t byteCounter = 0;
volatile uint32_t addressCounter = 0;
volatile uint32_t ignoredCommandCounter = 0;
volatile uint32_t acceptedDataCounter = 0;
volatile uint32_t rejectedDataCounter = 0;
volatile uint32_t timeoutCounter = 0;
volatile uint32_t rsResetCounter = 0;
volatile uint32_t sampleDifferenceCounter = 0;

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
  const PACKET_LCD_WAIT = 0xFC;
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
  let displayedMainKind = null;
  let pendingRawSignature = '';
  let meterGlyphSignature = '';
  let pendingMeterGlyphSignature = '';
  let pendingMeterGlyphCount = 0;

  const SPACE = 0x20;
  const MOMENTARY_MASK =
    (1 << 1) | (1 << 2) | (1 << 4) |
    (1 << 5) | (1 << 6) | (1 << 7);
  const LCD_POLL_IDLE_MS = 20;
  const LCD_POLL_HELD_MS = 100;
  const LCD_BUSY_RETRY_MS = 12;
  const blankCgram = new Uint8Array(64);
  const meterScreen = new Uint8Array(32);
  const meterCgram = new Uint8Array(64);
  meterScreen.fill(SPACE);

  const rows = [
    document.getElementById('row1'),
    document.getElementById('row2')
  ];

  for (let i = 0; i < 32; i++) {
    const cell = document.createElement('div');
    const text = document.createElement('span');
    const glyph = document.createElement('canvas');

    cell.className = 'cell';
    text.className = 'lcd-char';
    text.textContent = '\u00A0';
    glyph.className = 'lcd-glyph';
    // Рисуем в двойном разрешении и уменьшаем средствами браузера.
    // Так точки CGRAM выглядят мягче и ближе к свечению настоящего LCD.
    glyph.width = 30;
    glyph.height = 48;

    cell.appendChild(text);
    cell.appendChild(glyph);
    rows[i < 16 ? 0 : 1].appendChild(cell);
  }

  function byteToCharacter(value) {
    if (value === 0xE4) return 'µ';
    if (value >= 32 && value <= 126) return String.fromCharCode(value);
    return ' ';
  }

  function blankScreen() {
    const screen = new Uint8Array(32);
    screen.fill(SPACE);
    return screen;
  }

  function asciiScreen(row1, row2) {
    const screen = blankScreen();
    const text = (row1.padEnd(16, ' ').slice(0, 16) +
                  row2.padEnd(16, ' ').slice(0, 16));

    for (let i = 0; i < 32; i++) {
      screen[i] = text.charCodeAt(i);
    }

    return screen;
  }

  const powerOffScreen = asciiScreen(
    '   POWER OFF    ',
    '                '
  );

  function renderCell(index, value, cgram) {
    const cell = rows[index < 16 ? 0 : 1].children[index % 16];

    if (value <= 0x07) {
      const offset = value * 8;
      const forceFullCell = value === 0x00;
      let signature = forceFullCell ? 'g0:full' : 'g' + value + ':';

      if (!forceFullCell) {
        for (let row = 0; row < 8; row++) {
          signature += String.fromCharCode(cgram[offset + row]);
        }
      }

      if (lastCellSignatures[index] === signature) return;

      const canvas = cell.querySelector('.lcd-glyph');
      const context = canvas.getContext('2d');

      context.clearRect(0, 0, 30, 48);
      context.fillStyle = '#4e4';
      context.shadowColor = '#0f0';
      context.shadowBlur = 2.2;

      for (let row = 0; row < 8; row++) {
        const pixels = forceFullCell
          ? 0x1F
          : cgram[offset + row] & 0x1F;

        for (let column = 0; column < 5; column++) {
          if (pixels & (1 << (4 - column))) {
            context.fillRect(
              column * 6 + 0.8,
              row * 6 + 0.8,
              4.4,
              4.4
            );
          }
        }
      }

      cell.classList.add('special');
      lastCellSignatures[index] = signature;
      return;
    }

    let character = byteToCharacter(value);
    if (character === ' ') character = '\u00A0';

    const signature = 't' + value;
    if (lastCellSignatures[index] === signature) return;

    cell.classList.remove('special');
    cell.querySelector('.lcd-char').textContent = character;
    lastCellSignatures[index] = signature;
  }

  function drawScreen(screen, cgram = blankCgram) {
    for (let i = 0; i < 32; i++) {
      renderCell(i, screen[i], cgram);
    }
  }

  function matchesAscii(screen, offset, text) {
    if (offset < 0 || offset + text.length > screen.length) return false;

    for (let i = 0; i < text.length; i++) {
      if (screen[offset + i] !== text.charCodeAt(i)) return false;
    }

    return true;
  }

  function findAscii(screen, start, end, text) {
    const last = Math.min(end, screen.length) - text.length;

    for (let offset = start; offset <= last; offset++) {
      if (matchesAscii(screen, offset, text)) return offset;
    }

    return -1;
  }

  function writeAscii(screen, offset, text) {
    for (let i = 0; i < text.length; i++) {
      screen[offset + i] = text.charCodeAt(i);
    }
  }

  function bytesToAscii(screen, start, end) {
    let text = '';

    for (let i = start; i < end; i++) {
      const value = screen[i];
      text += value >= 0x20 && value <= 0x7E
        ? String.fromCharCode(value)
        : '\x01';
    }

    return text;
  }

  function setMeterField(offset, length, value) {
    if (!value) return;

    const normalized = value.padStart(length, ' ').slice(-length);

    for (let i = 0; i < length; i++) {
      meterScreen[offset + i] = normalized.charCodeAt(i);
    }
  }

  function numericTokens(screen, start, end) {
    const text = bytesToAscii(screen, start, end);
    const result = [];
    const expression = /[0-9][.,\/][0-9]|[0-9]{1,3}/g;
    let match;

    while ((match = expression.exec(text)) !== null) {
      result.push({
        value: match[0],
        offset: start + match.index
      });
    }

    return result;
  }

  function extractFrequency(screen) {
    const row = bytesToAscii(screen, 0, 16);
    const mhz = findAscii(screen, 0, 16, 'MHz');

    if (mhz >= 0) {
      const before = row.slice(Math.max(0, mhz - 6), mhz);
      const match = before.match(/[ 0-9]{1,2}[.,][0-9]{3}$/);
      if (match) return match[0].padStart(6, ' ').slice(-6);
    }

    const candidates = row.match(/[ 0-9]{1,2}[.,][0-9]{3}/g);
    if (!candidates || candidates.length === 0) return null;

    return candidates[0].padStart(6, ' ').slice(-6);
  }

  function extractValueAfter(screen, anchorOffset) {
    if (anchorOffset < 0 || anchorOffset + 7 > screen.length) return null;

    const value = bytesToAscii(
      screen,
      anchorOffset + 4,
      anchorOffset + 7
    );

    return /^(?:[0-9][.,\/][0-9]|[ 0-9]{3})$/.test(value) &&
      /[0-9]/.test(value)
        ? value
        : null;
  }

  function countBarCells(screen) {
    let count = 0;

    for (let i = 16; i < 29; i++) {
      if (screen[i] <= 0x07 || screen[i] === 0x3D) count++;
    }

    return count;
  }

  function detectMainKind(screen) {
    const mhz = findAscii(screen, 0, 16, 'MHz');
    const fwd = findAscii(screen, 16, 32, 'FWD=');
    const ref = findAscii(screen, 16, 32, 'REF=');
    const row2Values = numericTokens(screen, 16, 32);
    const barCells = countBarCells(screen);

    if (mhz >= 0 && barCells >= 4) return 'bar';

    if (
      (fwd >= 0 && ref >= 0) ||
      (
        mhz >= 0 &&
        (fwd >= 0 || ref >= 0 || row2Values.length >= 2)
      )
    ) {
      return 'meter';
    }

    // Один повреждённый якорь не должен выталкивать уже распознанный
    // основной экран из его фиксированной раскладки.
    if (
      displayedMainKind === 'meter' &&
      (mhz >= 0 || fwd >= 0 || ref >= 0)
    ) {
      return 'meter';
    }

    return null;
  }

  function getMeterGlyphSignature(cgram) {
    let signature = '';

    for (const slot of [5, 7, 6]) {
      const offset = slot * 8;
      for (let row = 0; row < 8; row++) {
        signature += String.fromCharCode(cgram[offset + row]);
      }
    }

    return signature;
  }

  function updateMeterGlyphs(cgram, entering) {
    const signature = getMeterGlyphSignature(cgram);

    if (entering || meterGlyphSignature === '') {
      meterCgram.set(cgram);
      meterGlyphSignature = signature;
      pendingMeterGlyphSignature = '';
      pendingMeterGlyphCount = 0;
      return;
    }

    if (signature === meterGlyphSignature) {
      pendingMeterGlyphSignature = '';
      pendingMeterGlyphCount = 0;
      return;
    }

    if (signature !== pendingMeterGlyphSignature) {
      pendingMeterGlyphSignature = signature;
      pendingMeterGlyphCount = 1;
      return;
    }

    pendingMeterGlyphCount++;

    // PIC обновляет CGRAM построчно. Принимаем новую картинку только после
    // двух одинаковых снимков, чтобы не рисовать недособранный значок.
    if (pendingMeterGlyphCount >= 2) {
      meterCgram.set(cgram);
      meterGlyphSignature = signature;
      pendingMeterGlyphSignature = '';
      pendingMeterGlyphCount = 0;
    }
  }

  function drawMeterSnapshot(screen, cgram, entering) {
    const frequency = extractFrequency(screen);
    const row1Values = numericTokens(screen, 0, 16);
    const row2Values = numericTokens(screen, 16, 32);
    const fwdOffset = findAscii(screen, 16, 32, 'FWD=');
    const refOffset = findAscii(screen, 16, 32, 'REF=');

    let swr = null;
    for (const token of row1Values) {
      if (token.offset >= 9) swr = token.value;
    }

    let fwd = extractValueAfter(screen, fwdOffset);
    let ref = extractValueAfter(screen, refOffset);

    // Если одна подпись поймана с ошибкой, значения всё равно обычно целы.
    // Первое число второй строки — FWD, последнее — REF.
    if (!fwd && row2Values.length >= 2) fwd = row2Values[0].value;
    if (!ref && row2Values.length >= 2) {
      ref = row2Values[row2Values.length - 1].value;
    }

    setMeterField(0, 6, frequency);
    writeAscii(meterScreen, 6, 'MHz');
    meterScreen[9] = 5;
    meterScreen[10] = 7;
    meterScreen[11] = 6;
    meterScreen[12] = SPACE;
    setMeterField(13, 3, swr);

    writeAscii(meterScreen, 16, 'FWD=');
    setMeterField(20, 3, fwd);
    meterScreen[23] = SPACE;
    meterScreen[24] = SPACE;
    writeAscii(meterScreen, 25, 'REF=');
    setMeterField(29, 3, ref);

    updateMeterGlyphs(cgram, entering);
    drawScreen(meterScreen, meterCgram);
  }

  function screenSignature(screen) {
    let signature = '';

    for (let i = 0; i < screen.length; i++) {
      signature += String.fromCharCode(screen[i]);
    }

    return signature;
  }

  function drawSnapshot(data) {
    if ((buttonMask & (1 << 8)) === 0) {
      drawScreen(powerOffScreen, blankCgram);
      return;
    }

    const screen = data.subarray(1, 33);
    const cgram = data.subarray(33, 97);
    const mainKind = detectMainKind(screen);

    if (mainKind === 'meter') {
      const entering = displayedMainKind !== 'meter';
      displayedMainKind = 'meter';
      pendingRawSignature = '';
      drawMeterSnapshot(screen, cgram, entering);
      return;
    }

    if (mainKind === 'bar') {
      displayedMainKind = 'bar';
      pendingRawSignature = '';
      drawScreen(screen, cgram);
      return;
    }

    // При выходе с основного экрана один раз подтверждаем новый текст.
    // Это не даёт промежуточной посимвольной записи стереть устойчивый кадр.
    if (displayedMainKind) {
      const signature = screenSignature(screen);

      if (signature !== pendingRawSignature) {
        pendingRawSignature = signature;
        return;
      }

      displayedMainKind = null;
      pendingRawSignature = '';
    }

    drawScreen(screen, cgram);
  }

  function requestLcdSnapshot() {
    lcdRequestTimer = null;

    if (
      lcdRequestPending ||
      !socket ||
      socket.readyState !== WebSocket.OPEN
    ) {
      return;
    }

    // Если предыдущий запрос завис во время отпускания кнопки, этот запрос
    // уже гарантированно сделан после отпускания и считается свежим.
    if ((buttonMask & MOMENTARY_MASK) === 0) {
      lcdRefreshAfterRelease = false;
    }

    lcdRequestPending = true;
    socket.send('L');

    clearTimeout(lcdRequestWatchdog);
    lcdRequestWatchdog = setTimeout(() => {
      lcdRequestPending = false;
      requestLcdSnapshot();
    }, 500);
  }

  function scheduleLcdSnapshotRequest() {
    clearTimeout(lcdRequestTimer);

    // Тюнер обновляет LCD неспешно. Во время удержания MODE/TUNE/C/L
    // опрашиваем реже, но не останавливаемся: именно при удержании могут
    // меняться значения или появиться Setup Mode.
    const delay = (buttonMask & MOMENTARY_MASK)
      ? LCD_POLL_HELD_MS
      : LCD_POLL_IDLE_MS;

    lcdRequestTimer = setTimeout(requestLcdSnapshot, delay);
  }

  function connect() {
    if (
      socket &&
      (socket.readyState === WebSocket.OPEN ||
       socket.readyState === WebSocket.CONNECTING)
    ) {
      return;
    }

    const protocol = location.protocol === 'https:' ? 'wss://' : 'ws://';

    socket = new WebSocket(protocol + location.host + '/ws');
    socket.binaryType = 'arraybuffer';

    socket.onopen = () => {
      document.getElementById('status').className = 'on';
      sendButtons();
      requestLcdSnapshot();
    };

    socket.onclose = () => {
      document.getElementById('status').className = 'off';
      clearTimeout(lcdRequestTimer);
      clearTimeout(lcdRequestWatchdog);
      lcdRequestPending = false;
      releaseMomentaryButtons();

      if (!reconnectTimer) {
        reconnectTimer = setTimeout(() => {
          reconnectTimer = null;
          connect();
        }, 2000);
      }
    };

    socket.onerror = () => {
      document.getElementById('status').className = 'off';
    };

    socket.onmessage = event => {
      if (typeof event.data === 'string') return;

      const data = new Uint8Array(event.data);

      if (data.length === 98 && data[0] === PACKET_LCD) {
        clearTimeout(lcdRequestWatchdog);
        lcdRequestPending = false;

        // Ответ на запрос, отправленный ещё до отпускания, может содержать
        // старый кадр. Не рисуем его, а сразу просим актуальное состояние.
        if (lcdRefreshAfterRelease) {
          requestLcdSnapshot();
          return;
        }

        drawSnapshot(data);
        scheduleLcdSnapshotRequest();
      }
      else if (data.length === 1 && data[0] === PACKET_LCD_WAIT) {
        clearTimeout(lcdRequestWatchdog);
        lcdRequestPending = false;

        clearTimeout(lcdRequestTimer);
        lcdRequestTimer = setTimeout(
          requestLcdSnapshot,
          LCD_BUSY_RETRY_MS
        );
      }
    };
  }

  function sendButtons() {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      return;
    }

    let message = 'B';

    for (let i = 0; i < 9; i++) {
      message += (buttonMask & (1 << i)) ? '1' : '0';
    }

    socket.send(message);

    const momentaryHeld =
      (buttonMask & MOMENTARY_MASK) !== 0;

    if (lastSentMomentaryHeld && !momentaryHeld) {
      // Сначала уже отправлено отпускание кнопки, затем запрашиваем свежий
      // LCD. Если старый запрос ещё в полёте, его ответ будет отброшен.
      lcdRefreshAfterRelease = true;
      clearTimeout(lcdRequestTimer);
      lcdRequestTimer = null;

      if (!lcdRequestPending) {
        requestLcdSnapshot();
      }
    }
    else if (momentaryHeld) {
      // Немедленно применяем команду кнопки и лишь переносим следующий
      // запрос LCD на более редкий интервал.
      scheduleLcdSnapshotRequest();
    }

    lastSentMomentaryHeld = momentaryHeld;
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
      displayedMainKind = null;
      pendingRawSignature = '';
      meterGlyphSignature = '';
      pendingMeterGlyphSignature = '';
      pendingMeterGlyphCount = 0;
      meterCgram.fill(0);

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
    const button = document.getElementById('b' + index);
    let pressed = false;

    const down = event => {
      event.preventDefault();
      if (pressed) return;

      pressed = true;

      if (button.setPointerCapture) {
        button.setPointerCapture(event.pointerId);
      }

      setButton(index, true);
    };

    const up = event => {
      if (!pressed) return;
      if (event) event.preventDefault();

      pressed = false;
      setButton(index, false);
    };

    button.addEventListener('pointerdown', down);
    button.addEventListener('pointerup', up);
    button.addEventListener('pointercancel', up);
    button.addEventListener('lostpointercapture', up);
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
// LCD: ДЕКОДИРОВАНИЕ
// ============================================================================

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
    if (address <= 0x0F) {
        return address;
    }

    if (address >= 0x40 && address <= 0x4F) {
        return 16 + (address - 0x40);
    }

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
    if (entryIncrement) {
        lcdAddress = (uint8_t)((lcdAddress + 1) & 0x7F);
    }
    else {
        lcdAddress = (uint8_t)((lcdAddress - 1) & 0x7F);
    }
}

void processCommand(uint8_t command, uint32_t now)
{
    if (command == 0x01) {
        portENTER_CRITICAL(&lcdMux);

        memset(lcdScreen, ' ', sizeof(lcdScreen));
        lcdVersion++;
        lastDdramChangeUs = now;

        portEXIT_CRITICAL(&lcdMux);

        lcdAddress = 0;
        lcdAddressSpace = LCD_SPACE_DDRAM;
        cgramCandidateActive = false;
        return;
    }

    if (command == 0x02) {
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
        addressCounter++;
        return;
    }

    if (isVisibleAddressCommand(command)) {
        lcdAddress = command & 0x7F;
        lcdAddressSpace = LCD_SPACE_DDRAM;
        cgramCandidateActive = false;
        addressCounter++;
        return;
    }

    // Остальные командные байты общей шины не меняют текущий адрес.
    ignoredCommandCounter++;
}

void processCgramData(uint8_t value, uint32_t now)
{
    if (
        !cgramCandidateActive ||
        (uint32_t)(now - cgramCandidateLastUs) >
            CGRAM_BLOCK_TIMEOUT_US ||
        (value & 0xE0) != 0
    ) {
        rejectedDataCounter++;
        cgramCandidateActive = false;
        lcdAddressSpace = LCD_SPACE_NONE;
        return;
    }

    uint8_t address = lcdCgramAddress & 0x3F;
    uint8_t character = address >> 3;
    uint8_t row = address & 0x07;
    uint8_t pixels = value & 0x1F;
    uint8_t rowBit = (uint8_t)(1U << row);

    portENTER_CRITICAL(&lcdMux);

    bool changed =
        (lcdCgramKnownRows[character] & rowBit) == 0 ||
        lcdCgram[character][row] != pixels;

    // CGRAM может меняться по одной строке. Остальные семь строк символа
    // остаются без изменений, точно как в настоящем HD44780.
    lcdCgram[character][row] = pixels;
    lcdCgramKnownRows[character] |= rowBit;

    if (changed) {
        lcdVersion++;
    }

    portEXIT_CRITICAL(&lcdMux);

    acceptedDataCounter++;
    cgramCandidateLastUs = now;

    if (entryIncrement) {
        lcdCgramAddress = (uint8_t)((address + 1) & 0x3F);
    }
    else {
        lcdCgramAddress = (uint8_t)((address - 1) & 0x3F);
    }
}

void processDdramData(uint8_t value, uint32_t now)
{
    int position = addressToPosition(lcdAddress);

    if (position < 0 || position >= 32) {
        rejectedDataCounter++;
        lcdAddressSpace = LCD_SPACE_NONE;
        return;
    }

    portENTER_CRITICAL(&lcdMux);

    if (lcdScreen[position] != value) {
        lcdScreen[position] = value;
        lcdVersion++;
        lastDdramChangeUs = now;
    }

    portEXIT_CRITICAL(&lcdMux);

    acceptedDataCounter++;
    advanceDdramAddress();

    if (addressToPosition(lcdAddress) < 0) {
        lcdAddressSpace = LCD_SPACE_NONE;
    }
}

void processData(uint8_t value, uint32_t now)
{
    if (lcdAddressSpace == LCD_SPACE_CGRAM) {
        processCgramData(value, now);
    }
    else if (lcdAddressSpace == LCD_SPACE_DDRAM) {
        processDdramData(value, now);
    }
    else {
        rejectedDataCounter++;
    }
}

static inline void processByte(
    uint8_t value,
    bool rs,
    uint32_t now
)
{
    byteCounter++;

    if (rs) {
        processData(value, now);
    }
    else {
        processCommand(value, now);
    }
}

static inline void processCapturedNibble(
    uint8_t nibble,
    bool currentRs,
    uint32_t now
) {
    if (
        lastBusTime != 0 &&
        (uint32_t)(now - lastBusTime) > NIBBLE_TIMEOUT_US
    ) {
        if (stage != 0) {
            timeoutCounter++;
        }

        // Сбрасываем только недособранный байт. Выбранную область и адрес
        // HD44780 после паузы не забывает — это поведение проверенной версии.
        stage = 0;
    }

    if (currentRs != lastRs) {
        if (stage != 0) {
            rsResetCounter++;
        }

        stage = 0;
    }

    lastRs = currentRs;
    lastBusTime = now;

    if (stage == 0) {
        firstNibble = nibble;
        stage = 1;
        return;
    }

    uint8_t value =
        (uint8_t)((firstNibble << 4) | nibble);

    stage = 0;
    processByte(value, currentRs, now);
}

// ============================================================================
// КНОПКИ
// ============================================================================

void clearCapturedLcdBeforePowerOn()
{
    // Вызывается, пока физическая линия POWER ещё находится в OFF.
    // Захват уже работает и примет полную инициализацию LCD после включения.
    portENTER_CRITICAL(&lcdMux);

    memset(lcdScreen, ' ', sizeof(lcdScreen));
    memset(lcdCgram, 0, sizeof(lcdCgram));
    memset(lcdCgramKnownRows, 0, sizeof(lcdCgramKnownRows));
    lcdVersion++;
    lastDdramChangeUs = micros();

    portEXIT_CRITICAL(&lcdMux);

    stage = 0;
    firstNibble = 0;
    lastRs = false;
    lastBusTime = 0;

    lcdAddress = 0;
    lcdCgramAddress = 0;
    lcdAddressSpace = LCD_SPACE_NONE;
    entryIncrement = true;

    cgramCandidateActive = false;
    cgramCandidateLastUs = 0;
}

void applyButtonMask(uint16_t newMask)
{
    newMask &= 0x01FF;

    bool powerWasOn =
        (buttonMask & (1U << 8)) != 0;

    bool powerWillBeOn =
        (newMask & (1U << 8)) != 0;

    if (!powerWasOn && powerWillBeOn) {
        clearCapturedLcdBeforePowerOn();
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
    if (client == nullptr) {
        return;
    }

    // Текстовые знакоместа ждём до конца короткой посимвольной записи.
    // CGRAM при этом может непрерывно анимироваться и кадр не блокирует.
    uint32_t change = lastDdramChangeUs;

    if (
        change != 0 &&
        (uint32_t)(micros() - change) < DISPLAY_IDLE_US
    ) {
        uint8_t waitPacket = 0xFC;
        client->binary(&waitPacket, 1);
        return;
    }

    // Клиент запрашивает следующий кадр только после получения предыдущего,
    // поэтому в очереди WebSocket находится не более одного снимка.
    uint8_t lcdPacket[98];
    lcdPacket[0] = 0xFD;

    portENTER_CRITICAL(&lcdMux);

    uint32_t changeInside = lastDdramChangeUs;

    if (
        changeInside != change ||
        (
            changeInside != 0 &&
            (uint32_t)(micros() - changeInside) < DISPLAY_IDLE_US
        )
    ) {
        portEXIT_CRITICAL(&lcdMux);

        uint8_t waitPacket = 0xFC;
        client->binary(&waitPacket, 1);
        return;
    }

    memcpy(&lcdPacket[1], lcdScreen, 32);
    memcpy(&lcdPacket[33], lcdCgram, 64);
    lcdPacket[97] = lcdVersion;

    portEXIT_CRITICAL(&lcdMux);

    client->binary(lcdPacket, sizeof(lcdPacket));
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

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// ============================================================================
// SETUP
// ============================================================================

void setup()
{
    Serial.begin(460800);
    setCpuFrequencyMhz(240);

    memset(lcdScreen, ' ', sizeof(lcdScreen));
    memset(lcdCgram, 0, sizeof(lcdCgram));
    memset(lcdCgramKnownRows, 0, sizeof(lcdCgramKnownRows));

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

        delay(300);
        ESP.restart();
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

                stage = 0;
                firstNibble = 0;
                lcdAddressSpace = LCD_SPACE_NONE;
                cgramCandidateActive = false;

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

    xTaskCreatePinnedToCore(
        TaskWeb,
        "Web",
        6144,
        nullptr,
        1,
        nullptr,
        0
    );

    Serial.println("LCD capture: SAMPLE_DELAY=110, capture=s1");
    Serial.println("Web LCD: DDRAM settles for 12 ms; CGRAM does not block frames");
    Serial.println("Nibble sync: reset incomplete byte only");
}

// ============================================================================
// ЗАХВАТ LCD — ЯДРО 1
// ============================================================================

void loop()
{
    if (webUpdateInProgress || webUpdateRestartPending) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    if (REG_READ(GPIO_IN_REG) & MASK_E) {
        uint32_t start = xthal_get_ccount();

        while (
            (uint32_t)(xthal_get_ccount() - start) < SAMPLE_DELAY
        ) {
            // Критический участок: ничего сюда не добавлять.
        }

        // Момент выборки полностью повторяет удачную терминальную версию.
        uint32_t s1 = REG_READ(GPIO_IN_REG);

        // Только диагностика. Для декодирования s2 не используется.
        uint32_t s2 = REG_READ(GPIO_IN_REG);

        uint32_t reg = s1;

        while (REG_READ(GPIO_IN_REG) & MASK_E) {
            // Один импульс E обрабатывается ровно один раз.
        }

        pulseCounter++;

        bool sampleUnstable =
            ((s1 ^ s2) & MASK_LCD_BUS) != 0;

        if (sampleUnstable) {
            sampleDifferenceCounter++;
        }

        bool currentRs =
            (reg & MASK_RS) != 0;

        uint8_t nibble =
            readNibble(reg);

        uint32_t captureNow = micros();

        processCapturedNibble(
            nibble,
            currentRs,
            captureNow
        );

    }
}
