#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
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
// WEB / WIFI / OTA
// ============================================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

volatile bool otaMode = false;
bool otaStarted = false;

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
  #otaButton {
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

  <div id="otaButton" onclick="startOta()">System Update (OTA)</div>

<script>
  const PACKET_LCD = 0xFD;
  const PACKET_OTA = 0xFE;

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
  let pendingUnanchoredValid = false;
  let pendingUnanchoredVersion = -1;

  const SPACE = 0x20;
  const MOMENTARY_MASK =
    (1 << 1) | (1 << 2) | (1 << 4) |
    (1 << 5) | (1 << 6) | (1 << 7);
  const LCD_POLL_IDLE_MS = 25;
  const LCD_POLL_HELD_MS = 100;
  const trustedScreen = new Uint8Array(32);
  const trustedCgram = new Uint8Array(64);
  const blankCgram = new Uint8Array(64);
  const pendingUnanchoredScreen = new Uint8Array(32);
  const pendingUnanchoredCgram = new Uint8Array(64);
  trustedScreen.fill(SPACE);

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
    glyph.width = 15;
    glyph.height = 24;

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

      context.clearRect(0, 0, 15, 24);
      context.fillStyle = '#4e4';
      context.shadowColor = '#0f0';
      context.shadowBlur = 1.2;

      for (let row = 0; row < 8; row++) {
        const pixels = forceFullCell
          ? 0x1F
          : cgram[offset + row] & 0x1F;

        for (let column = 0; column < 5; column++) {
          if (pixels & (1 << (4 - column))) {
            context.fillRect(
              column * 3 + 0.35,
              row * 3 + 0.35,
              2.3,
              2.3
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
    for (let i = 0; i < text.length; i++) {
      if (screen[offset + i] !== text.charCodeAt(i)) return false;
    }

    return true;
  }

  function writeAscii(screen, offset, text) {
    for (let i = 0; i < text.length; i++) {
      screen[offset + i] = text.charCodeAt(i);
    }
  }

  function sameBytes(left, right) {
    if (left.length !== right.length) return false;

    for (let i = 0; i < left.length; i++) {
      if (left[i] !== right[i]) return false;
    }

    return true;
  }

  function detectMainKind(screen) {
    let meterAnchors = 0;

    if (matchesAscii(screen, 6, 'MHz')) meterAnchors++;
    if (matchesAscii(screen, 16, 'FWD=')) meterAnchors++;
    if (matchesAscii(screen, 25, 'REF=')) meterAnchors++;

    // Двух независимых совпадений достаточно: третью постоянную надпись
    // можно безопасно восстановить, не трогая показания и спецсимволы.
    if (meterAnchors >= 2) return 'meter';

    if (matchesAscii(screen, 6, 'MHz')) {
      for (let i = 16; i < 29; i++) {
        // Пробегающие шкалы состоят из '=' и CGRAM-кодов. Их выводим
        // немедленно, а не ждём неподвижного кадра.
        if (screen[i] === 0x3D || screen[i] <= 0x07) {
          return 'bar';
        }
      }
    }

    if (
      screen[7] === 0xE4 &&
      screen[8] === 0x48 &&
      matchesAscii(screen, 26, 'pF')
    ) {
      return 'lc';
    }

    return null;
  }

  function applySnapshot(screen, cgram, mainKind) {
    trustedScreen.set(screen);
    trustedCgram.set(cgram);

    // Якоря исправляют только неизменные подписи основного экрана.
    // Цифры, запятые, слеши, CGRAM и все Setup-экраны не изменяются.
    if (mainKind === 'meter') {
      writeAscii(trustedScreen, 6, 'MHz');
      writeAscii(trustedScreen, 16, 'FWD=');
      writeAscii(trustedScreen, 25, 'REF=');
    }
    else if (mainKind === 'lc') {
      trustedScreen[7] = 0xE4;
      trustedScreen[8] = 0x48;
      writeAscii(trustedScreen, 26, 'pF');
    }

    drawScreen(trustedScreen, trustedCgram);
  }

  function drawSnapshot(data) {
    if ((buttonMask & (1 << 8)) === 0) {
      drawScreen(powerOffScreen, blankCgram);
      return;
    }

    const screen = data.subarray(1, 33);
    const cgram = data.subarray(33, 97);
    const version = data[97];
    const mainKind = detectMainKind(screen);

    if (mainKind) {
      pendingUnanchoredValid = false;
      displayedMainKind = mainKind;
      applySnapshot(screen, cgram, mainKind);
      return;
    }

    // При уходе с основного экрана ждём только одно подтверждение того же
    // снимка. Это отбрасывает посимвольный промежуточный кадр, но Setup Mode
    // появляется уже на следующем запросе и затем идёт без фильтрации.
    if (displayedMainKind) {
      if (
        pendingUnanchoredValid &&
        pendingUnanchoredVersion === version &&
        sameBytes(pendingUnanchoredScreen, screen) &&
        sameBytes(pendingUnanchoredCgram, cgram)
      ) {
        pendingUnanchoredValid = false;
        displayedMainKind = null;
        applySnapshot(screen, cgram, null);
      }
      else {
        pendingUnanchoredScreen.set(screen);
        pendingUnanchoredCgram.set(cgram);
        pendingUnanchoredVersion = version;
        pendingUnanchoredValid = true;
      }

      return;
    }

    applySnapshot(screen, cgram, null);
  }

  function showOtaMode() {
    clearTimeout(lcdRequestTimer);
    clearTimeout(lcdRequestWatchdog);
    lcdRequestPending = false;

    document.getElementById('controls').style.display = 'none';
    document.getElementById('otaButton').style.display = 'none';

    drawScreen(asciiScreen(
      '  UPDATE MODE   ',
      'START IDE UPLOAD'
    ), blankCgram);
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
      else if (data.length >= 1 && data[0] === PACKET_OTA) {
        showOtaMode();
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
      pendingUnanchoredValid = false;

      if (active) {
        trustedScreen.fill(SPACE);
        trustedCgram.fill(0);
        drawScreen(trustedScreen, trustedCgram);
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

  function startOta() {
    if (!confirm('ВКЛЮЧИТЬ РЕЖИМ ПРОШИВКИ?')) return;

    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send('START_OTA');
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

void processDdramData(uint8_t value)
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
        processDdramData(value);
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

        // Пауза сбрасывает только недособранный байт. Контроллер LCD
        // сохраняет выбранную DDRAM/CGRAM и текущий адрес.
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
// OTA
// ============================================================================

void startOtaIfRequested()
{
    if (!otaMode || otaStarted) {
        return;
    }

    otaStarted = true;

    ArduinoOTA.setHostname("MFJ-Remote");
    ArduinoOTA.setPort(3232);

    ArduinoOTA.onStart([]() {
        releaseMomentaryButtons();
        Serial.println("OTA: start");
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("OTA: complete");
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error: %u\n", error);
    });

    ArduinoOTA.begin();

    Serial.print("OTA ready: ");
    Serial.println(WiFi.localIP());
}

// ============================================================================
// WEBSOCKET
// ============================================================================

void sendLcdSnapshot(AsyncWebSocketClient *client)
{
    if (client == nullptr) {
        return;
    }

    // Клиент запрашивает следующий кадр только после получения предыдущего.
    // Поэтому в очереди WebSocket не накапливается история старых экранов.
    uint8_t lcdPacket[98];
    lcdPacket[0] = 0xFD;

    portENTER_CRITICAL(&lcdMux);

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
        !otaMode
    ) {
        sendLcdSnapshot(client);
        return;
    }

    if (
        length == 9 &&
        memcmp(data, "START_OTA", 9) == 0
    ) {
        if (!otaMode) {
            uint8_t message = 0xFE;

            ws.binaryAll(&message, 1);
            releaseMomentaryButtons();

            stage = 0;
            lcdAddressSpace = LCD_SPACE_NONE;
            cgramCandidateActive = false;
            otaMode = true;
        }

        return;
    }

    if (
        length == 10 &&
        data[0] == 'B' &&
        !otaMode
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
        if (otaMode) {
            startOtaIfRequested();
            ArduinoOTA.handle();
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        uint32_t now = millis();

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
    Serial.println("Web LCD: raw 32-byte DDRAM + 64-byte CGRAM snapshots");
}

// ============================================================================
// ЗАХВАТ LCD — ЯДРО 1
// ============================================================================

void loop()
{
    if (otaMode) {
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

        if (((s1 ^ s2) & MASK_LCD_BUS) != 0) {
            sampleDifferenceCounter++;
        }

        bool currentRs =
            (reg & MASK_RS) != 0;

        uint8_t nibble =
            readNibble(reg);

        processCapturedNibble(
            nibble,
            currentRs,
            micros()
        );
    }
}
