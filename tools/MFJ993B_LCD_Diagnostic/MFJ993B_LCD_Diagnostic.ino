// MFJ-993B Remote Control — LCD diagnostic build
//
// Ядро 1: непрерывный пассивный захват 4-битной шины HD44780.
// Ядро 0: Wi-Fi, WebSocket, кнопки, диагностика и защищённое HTTP OTA.
// Одновременно доступны виртуальный LCD, сырые DDRAM-байты и поток
// полубайтов RS/D4...D7. Текстовых якорей и исправления экрана нет.

// MFJ-993B remote control and literal HD44780 LCD mirror.
// LCD capture runs immediately on core 1; Wi-Fi, WebSocket and OTA use core 0.
// The browser does not parse screens or use textual anchors.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_ota_ops.h"
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

const char FIRMWARE_VERSION[] = "2026.09.15-diag1";
const uint32_t OTA_HEALTH_CONFIRM_MS = 15000;

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

volatile bool manualRollbackRequested = false;
volatile uint32_t manualRollbackAtMs = 0;
bool otaImageNeedsValidation = false;
uint32_t otaValidationStartedAtMs = 0;
bool otaGuardActive = false;

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
volatile bool lcdDisplayOn = true;

portMUX_TYPE lcdMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t lcdVersion = 0;
volatile uint16_t lcdClearGeneration = 0;

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
bool fourBitModeSeen = false;
uint8_t initializationNibbles = 0;
volatile bool decoderResetRequested = false;

// ============================================================================
// ДИАГНОСТИКА ЗАХВАТА
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
volatile uint32_t lastPulseAtMs = 0;

// Отдельная диагностическая сборка байтов не влияет на основной декодер.
uint8_t rawDiagnosticStage = 0;
uint8_t rawDiagnosticFirstNibble = 0;
bool rawDiagnosticLastRs = false;
uint32_t rawDiagnosticLastUs = 0;

enum RawEventFlags : uint8_t {
    RAW_FIRST_NIBBLE = 0x01,
    RAW_COMPLETE_BYTE = 0x02,
    RAW_TIMEOUT_RESET = 0x04,
    RAW_RS_RESET = 0x08,
    RAW_SAMPLE_DIFFERENCE = 0x10
};

struct RawBusEvent {
    uint32_t sequence;
    uint32_t gapUs;
    uint8_t nibble;
    uint8_t value;
    uint8_t flags;
    uint8_t rs;
};

// Один производитель на core 1 и один потребитель на core 0.
// Переполнение теряет только строку диагностики, но не импульс декодера.
const uint16_t RAW_EVENT_CAPACITY = 1024;
RawBusEvent rawEvents[RAW_EVENT_CAPACITY];
volatile uint16_t rawEventHead = 0;
volatile uint16_t rawEventTail = 0;
volatile uint32_t rawEventSequence = 0;
volatile uint32_t rawEventDropCounter = 0;

static inline void pushRawEvent(
    uint8_t nibble,
    bool rs,
    uint32_t gapUs,
    uint8_t flags,
    uint8_t value
) {
    uint16_t head = rawEventHead;
    uint16_t next = (uint16_t)((head + 1) % RAW_EVENT_CAPACITY);

    if (next == rawEventTail) {
        rawEventDropCounter++;
        return;
    }

    RawBusEvent &event = rawEvents[head];
    event.sequence = ++rawEventSequence;
    event.gapUs = gapUs;
    event.nibble = nibble;
    event.value = value;
    event.flags = flags;
    event.rs = rs ? 1 : 0;

    __sync_synchronize();
    rawEventHead = next;
}

bool popRawEvent(RawBusEvent &event)
{
    uint16_t tail = rawEventTail;

    if (tail == rawEventHead) {
        return false;
    }

    event = rawEvents[tail];
    __sync_synchronize();
    rawEventTail = (uint16_t)((tail + 1) % RAW_EVENT_CAPACITY);
    return true;
}

static inline void recordRawNibble(
    uint8_t nibble,
    bool currentRs,
    uint32_t now,
    bool sampleUnstable
) {
    uint8_t flags = sampleUnstable ? RAW_SAMPLE_DIFFERENCE : 0;
    uint8_t value = 0;
    uint32_t gapUs =
        rawDiagnosticLastUs == 0
            ? 0
            : (uint32_t)(now - rawDiagnosticLastUs);

    if (
        rawDiagnosticLastUs != 0 &&
        gapUs > NIBBLE_TIMEOUT_US
    ) {
        if (rawDiagnosticStage != 0) {
            flags |= RAW_TIMEOUT_RESET;
        }
        rawDiagnosticStage = 0;
    }

    if (
        rawDiagnosticLastUs != 0 &&
        currentRs != rawDiagnosticLastRs
    ) {
        if (rawDiagnosticStage != 0) {
            flags |= RAW_RS_RESET;
        }
        rawDiagnosticStage = 0;
    }

    rawDiagnosticLastRs = currentRs;
    rawDiagnosticLastUs = now;

    if (rawDiagnosticStage == 0) {
        rawDiagnosticFirstNibble = nibble;
        rawDiagnosticStage = 1;
        flags |= RAW_FIRST_NIBBLE;
    }
    else {
        value = (uint8_t)(
            (rawDiagnosticFirstNibble << 4) | nibble
        );
        rawDiagnosticStage = 0;
        flags |= RAW_COMPLETE_BYTE;
    }

    pushRawEvent(
        nibble,
        currentRs,
        gapUs,
        flags,
        value
    );
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
    display: block;
    width: 15px;
    height: 24px;
    opacity: .94;
    filter: drop-shadow(0 0 1px #0f0);
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
  #diagnostics {
    width: min(760px, 100%);
    margin-top: 12px;
    border: 1px solid #3a3a3a;
    border-radius: 6px;
    background: #171717;
    color: #bbb;
  }
  #diagnostics summary {
    padding: 12px;
    cursor: pointer;
    color: #6c9;
    font-weight: bold;
    text-align: center;
  }
  .diag-body { padding: 0 10px 10px; }
  .diag-title {
    margin: 10px 0 5px;
    color: #7d9;
    font-size: 11px;
  }
  .diag-note {
    margin: 4px 0 8px;
    color: #888;
    font-size: 10px;
    line-height: 1.35;
  }
  .diag-toolbar {
    display: flex;
    gap: 8px;
    margin-bottom: 6px;
  }
  .diag-toolbar button {
    width: auto;
    height: 30px;
    padding: 0 12px;
    font-size: 10px;
  }
  pre.diag {
    margin: 0;
    padding: 9px;
    overflow: auto;
    border: 1px solid #333;
    border-radius: 4px;
    background: #0b0b0b;
    color: #aaa;
    font: 11px/1.35 monospace;
    user-select: text;
    white-space: pre;
  }
  #captureStats { color: #8bd; }
  #rawMemory { color: #7d7; }
  #busLog { height: 260px; color: #bbb; }
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

  <details id="diagnostics" open>
    <summary>Диагностика захвата LCD</summary>
    <div class="diag-body">
      <div class="diag-note">
        Верхний дисплей — итог декодера. Ниже те же 32 байта без оформления
        и фактические полубайты RS/D4...D7. Экранных фильтров здесь нет.
      </div>

      <div class="diag-title">Состояние захвата</div>
      <pre id="captureStats" class="diag">ожидание соединения...</pre>

      <div class="diag-title">Сырая DDRAM: символы и HEX</div>
      <pre id="rawMemory" class="diag">R1 TXT |                |
R1 HEX 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20
R2 TXT |                |
R2 HEX 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20</pre>

      <div class="diag-title">Сырые импульсы E / полубайты</div>
      <div class="diag-toolbar">
        <button id="pauseRaw" type="button">ПАУЗА</button>
        <button id="clearRaw" type="button">ОЧИСТИТЬ</button>
      </div>
      <pre id="busLog" class="diag">SEQ       GAPus   RS HALF NIB BYTE FLAGS
</pre>
    </div>
  </details>

  <div id="modal">
    <div>ВЫКЛЮЧИТЬ?</div>
    <button onclick="confirmPower(true)" style="background:#800;color:#fff">ДА</button>
    <button onclick="confirmPower(false)">НЕТ</button>
  </div>

  <div id="updateButton" onclick="location.href='/update'">Firmware Update (.bin)</div>

<script>
  const PACKET_LCD = 0xFD;
  const PACKET_SIZE = 109;
  const MOMENTARY_MASK =
    (1 << 1) | (1 << 2) | (1 << 4) |
    (1 << 5) | (1 << 6) | (1 << 7);
  const LCD_POLL_IDLE_MS = 25;
  const LCD_POLL_HELD_MS = 50;

  let socket = null;
  let reconnectTimer = null;
  let lcdRequestTimer = null;
  let lcdRequestWatchdog = null;
  let lcdRequestPending = false;
  let buttonMask = (1 << 3) | (1 << 8);
  let lastClearGeneration = null;
  let lastCellSignatures = Array(32).fill('');
  let specialSequenceRunning = false;
  let rawPaused = false;
  let rawLines = ['SEQ       GAPus   RS HALF NIB BYTE FLAGS'];

  const captureStats = document.getElementById('captureStats');
  const rawMemory = document.getElementById('rawMemory');
  const busLog = document.getElementById('busLog');
  const pauseRaw = document.getElementById('pauseRaw');
  const clearRaw = document.getElementById('clearRaw');

  const FONT = {
    ' ':'00000000000000','!':'04040404040004','"':'0A0A0A00000000',
    '#':'0A0A1F0A1F0A0A','$':'040F140E051E04','%':'18190204081303',
    '&':'0C12140A15120D',"'":'04040800000000','(':'02040808080402',
    ')':'08040202020408','*':'000A041F040A00','+':'0004041F040400',
    ',':'00000000000408','-':'0000001F000000','.':'00000000000004',
    '/':'01010204081010','0':'0E11131519110E','1':'040C040404040E',
    '2':'0E11010204081F','3':'1F01020601110E','4':'02060A121F0202',
    '5':'1F101E0101110E','6':'0608101E11110E','7':'1F010204080808',
    '8':'0E11110E11110E','9':'0E11110F01020C',':':'00040400040400',
    ';':'00040400040408','<':'02040810080402','=':'00001F001F0000',
    '>':'08040201020408','?':'0E110102040004','@':'0E11171517100E',
    'A':'0E1111111F1111','B':'1E11111E11111E','C':'0E11101010110E',
    'D':'1E11111111111E','E':'1F10101E10101F','F':'1F10101E101010',
    'G':'0E11101711110F','H':'1111111F111111','I':'0E04040404040E',
    'J':'0702020202120C','K':'11121418141211','L':'1010101010101F',
    'M':'111B1515111111','N':'11191513111111','O':'0E11111111110E',
    'P':'1E11111E101010','Q':'0E11111115120D','R':'1E11111E141211',
    'S':'0F10100E01011E','T':'1F040404040404','U':'1111111111110E',
    'V':'11111111110A04','W':'11111115151B11','X':'11110A040A1111',
    'Y':'11110A04040404','Z':'1F01020408101F','[':'0E08080808080E',
    '\\':'10100804020101',']':'0E02020202020E','^':'040A1100000000',
    '_':'0000000000001F','`':'08040200000000','a':'00000E010F110F',
    'b':'1010161911111E','c':'00000E1110110E','d':'01010D1311110F',
    'e':'00000E111F100E','f':'0609091C080808','g':'00000F11110F01',
    'h':'10101619111111','i':'04000C0404040E','j':'0200060202120C',
    'k':'10101214181412','l':'0C04040404040E','m':'00001A15151515',
    'n':'00001619111111','o':'00000E1111110E','p':'00001E11111E10',
    'q':'00000F11110F01','r':'00001619101010','s':'00000F100E011E',
    't':'08081C08080906','u':'0000111111130D','v':'00001111110A04',
    'w':'0000111115150A','x':'0000110A040A11','y':'00001111110F01',
    'z':'00001F0204081F','{':'02040408040402','|':'04040404040404',
    '}':'08040402040408','~':'00000815020000'
  };

  const MICRO_ROWS = [0, 0, 0x11, 0x11, 0x13, 0x1D, 0x10, 0x10];
  const blankCgram = new Uint8Array(64);
  const blankKnown = new Uint8Array(8);
  const rows = [
    document.getElementById('row1'),
    document.getElementById('row2')
  ];

  function makeAsciiScreen(row1, row2) {
    const result = new Uint8Array(32);
    const value = row1.padEnd(16).slice(0, 16) +
                  row2.padEnd(16).slice(0, 16);
    for (let i = 0; i < 32; i++) result[i] = value.charCodeAt(i);
    return result;
  }

  const blankScreen = makeAsciiScreen('', '');
  const powerOffScreen = makeAsciiScreen('   POWER OFF    ', '');

  for (let i = 0; i < 32; i++) {
    const cell = document.createElement('div');
    const canvas = document.createElement('canvas');
    cell.className = 'cell';
    canvas.className = 'lcd-glyph';
    canvas.width = 30;
    canvas.height = 48;
    cell.appendChild(canvas);
    rows[i < 16 ? 0 : 1].appendChild(cell);
  }

  function romRows(value) {
    if (value === 0xE4) return MICRO_ROWS;
    const character = value >= 0x20 && value <= 0x7E
      ? String.fromCharCode(value)
      : '?';
    const packed = FONT[character] || FONT['?'];
    const result = [];
    for (let row = 0; row < 7; row++) {
      result.push(parseInt(packed.slice(row * 2, row * 2 + 2), 16));
    }
    result.push(0);
    return result;
  }

  function drawCell(index, value, cgram, knownRows) {
    let pixels;
    let signature;

    if (value <= 0x07) {
      const offset = value * 8;
      pixels = cgram.subarray(offset, offset + 8);
      signature = 'c' + value + ':' + knownRows[value] + ':' +
        Array.from(pixels).join(',');
    }
    else {
      pixels = romRows(value);
      signature = 'r' + value;
    }

    if (lastCellSignatures[index] === signature) return;
    lastCellSignatures[index] = signature;

    const canvas = rows[index < 16 ? 0 : 1]
      .children[index % 16]
      .firstChild;
    const context = canvas.getContext('2d');
    context.clearRect(0, 0, 30, 48);
    context.fillStyle = '#4e4';
    context.shadowColor = '#0f0';
    context.shadowBlur = 2;

    for (let row = 0; row < 8; row++) {
      const rowBits = pixels[row] & 0x1F;
      for (let column = 0; column < 5; column++) {
        if (rowBits & (1 << (4 - column))) {
          context.fillRect(column * 6 + 1, row * 6 + 1, 4, 4);
        }
      }
    }
  }

  function drawScreen(screen, cgram = blankCgram, knownRows = blankKnown) {
    for (let i = 0; i < 32; i++) {
      drawCell(i, screen[i], cgram, knownRows);
    }
  }

  function rawCharacter(value) {
    if (value <= 0x07) return '<' + value + '>';
    if (value === 0xE4) return 'µ';
    if (value >= 0x20 && value <= 0x7E) {
      return String.fromCharCode(value);
    }
    return '·';
  }

  function rawTextRow(bytes) {
    return Array.from(bytes, rawCharacter).join('');
  }

  function rawHexRow(bytes) {
    return Array.from(bytes, value =>
      value.toString(16).toUpperCase().padStart(2, '0')
    ).join(' ');
  }

  function updateRawMemory(screen) {
    const row1 = screen.subarray(0, 16);
    const row2 = screen.subarray(16, 32);

    rawMemory.textContent =
      'R1 TXT |' + rawTextRow(row1) + '|\n' +
      'R1 HEX ' + rawHexRow(row1) + '\n' +
      'R2 TXT |' + rawTextRow(row2) + '|\n' +
      'R2 HEX ' + rawHexRow(row2);
  }

  function acceptCaptureStats(message) {
    try {
      const value = JSON.parse(message.slice(1));
      const age = value.age < 0
        ? 'ещё не было'
        : value.age + ' ms назад';

      captureStats.textContent =
        'pulses=' + value.pulses +
        '  bytes=' + value.bytes +
        '  accepted=' + value.accepted +
        '  rejected=' + value.rejected + '\n' +
        'addresses=' + value.addresses +
        '  commands_ignored=' + value.ignored +
        '  space=' + value.space +
        '  AC=0x' + value.address + '\n' +
        'timeouts=' + value.timeouts +
        '  rs_resets=' + value.rsResets +
        '  sample_diff=' + value.sampleDiff +
        '  raw_dropped=' + value.rawDropped + '\n' +
        'stage=' + value.stage +
        '  last E pulse: ' + age;
    }
    catch (_) {
      captureStats.textContent = 'Ошибка диагностического пакета';
    }
  }

  function acceptRawBus(message) {
    if (rawPaused) return;

    const lines = message.slice(1).split('\n').filter(Boolean);
    rawLines.push(...lines);

    if (rawLines.length > 241) {
      rawLines.splice(1, rawLines.length - 241);
    }

    busLog.textContent = rawLines.join('\n') + '\n';
    busLog.scrollTop = busLog.scrollHeight;
  }

  function resetRawView() {
    rawLines = ['SEQ       GAPus   RS HALF NIB BYTE FLAGS'];
    busLog.textContent = rawLines[0] + '\n';
  }

  pauseRaw.addEventListener('click', () => {
    rawPaused = !rawPaused;
    pauseRaw.textContent = rawPaused ? 'ПРОДОЛЖИТЬ' : 'ПАУЗА';
    pauseRaw.classList.toggle('active', rawPaused);
  });

  clearRaw.addEventListener('click', resetRawView);

  function drawSnapshot(data) {
    const capturedScreen = data.subarray(1, 33);
    updateRawMemory(capturedScreen);

    if ((buttonMask & (1 << 8)) === 0) {
      drawScreen(powerOffScreen);
      return;
    }

    if ((data[108] & 0x01) === 0) {
      drawScreen(blankScreen);
      return;
    }

    const clearGeneration = data[98] | (data[99] << 8);
    if (lastClearGeneration !== null && clearGeneration !== lastClearGeneration) {
      lastCellSignatures.fill('');
    }
    lastClearGeneration = clearGeneration;

    drawScreen(
      capturedScreen,
      data.subarray(33, 97),
      data.subarray(100, 108)
    );
  }

  function requestLcdSnapshot() {
    lcdRequestTimer = null;
    if (lcdRequestPending || !socket || socket.readyState !== WebSocket.OPEN) return;

    lcdRequestPending = true;
    socket.send('L');
    clearTimeout(lcdRequestWatchdog);
    lcdRequestWatchdog = setTimeout(() => {
      lcdRequestPending = false;
      requestLcdSnapshot();
    }, 750);
  }

  function scheduleLcdSnapshotRequest() {
    clearTimeout(lcdRequestTimer);
    const delay = (buttonMask & MOMENTARY_MASK)
      ? LCD_POLL_HELD_MS
      : LCD_POLL_IDLE_MS;
    lcdRequestTimer = setTimeout(requestLcdSnapshot, delay);
  }

  function acceptButtonState(message) {
    if (!/^S[01]{9}$/.test(message)) return;
    buttonMask = 0;
    for (let i = 0; i < 9; i++) {
      if (message[i + 1] === '1') buttonMask |= 1 << i;
      updateButtonVisual(i);
    }
  }

  function connect() {
    if (socket &&
        (socket.readyState === WebSocket.OPEN ||
         socket.readyState === WebSocket.CONNECTING)) return;

    const protocol = location.protocol === 'https:' ? 'wss://' : 'ws://';
    socket = new WebSocket(protocol + location.host + '/ws');
    socket.binaryType = 'arraybuffer';

    socket.onopen = () => {
      document.getElementById('status').className = 'on';
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
        }, 1500);
      }
    };

    socket.onerror = () => {
      document.getElementById('status').className = 'off';
    };

    socket.onmessage = event => {
      if (typeof event.data === 'string') {
        if (event.data.startsWith('S')) {
          acceptButtonState(event.data);
        }
        else if (event.data.startsWith('J')) {
          acceptCaptureStats(event.data);
        }
        else if (event.data.startsWith('R')) {
          acceptRawBus(event.data);
        }
        return;
      }

      const data = new Uint8Array(event.data);
      if (data.length !== PACKET_SIZE || data[0] !== PACKET_LCD) return;

      clearTimeout(lcdRequestWatchdog);
      lcdRequestPending = false;
      drawSnapshot(data);
      scheduleLcdSnapshotRequest();
    };
  }

  function sendButtons() {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;

    let message = 'B';
    for (let i = 0; i < 9; i++) {
      message += (buttonMask & (1 << i)) ? '1' : '0';
    }
    socket.send(message);

    clearTimeout(lcdRequestTimer);
    lcdRequestTimer = null;
    if (!lcdRequestPending) requestLcdSnapshot();
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

      if (active) {
        resetRawView();
        drawScreen(blankScreen);
      }
      else {
        drawScreen(powerOffScreen);
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
    &nbsp;·&nbsp;
    <a href="/info">Состояние</a>
    &nbsp;·&nbsp;
    <a href="/rollback">Вернуться к предыдущей версии</a>
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

  upload.addEventListener('click', async () => {
    const file = firmware.files[0];

    if (!file || !file.name.toLowerCase().endsWith('.ino.bin')) return;
    if (!confirm('Начать обновление прошивки?')) return;

    upload.disabled = true;
    firmware.disabled = true;
    progress.style.width = '0%';
    status.textContent = 'Включение защиты обновления...';

    try {
      const guard = await fetch('/ota-arm', {
        method: 'POST',
        headers: {'X-OTA-Guard': 'ARM'},
        cache: 'no-store'
      });

      if (!guard.ok) throw new Error(await guard.text());
    }
    catch (error) {
      status.textContent = 'Защита OTA не включилась: ' + error.message;
      upload.disabled = false;
      firmware.disabled = false;
      return;
    }

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
// СТРАНИЦА РУЧНОГО ВОЗВРАТА НА ПРЕДЫДУЩИЙ OTA-СЛОТ
// ============================================================================

const char rollbackHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MFJ-993B Rollback</title>
<style>
body{background:#111;color:#bbb;font-family:sans-serif;display:grid;place-items:center;min-height:100vh;margin:0;padding:18px}
.panel{max-width:430px;background:#191919;border:1px solid #444;border-radius:10px;padding:22px}
h2{color:#d98;margin-top:0}p{line-height:1.45}button{width:100%;padding:13px;background:#742;color:#fff;border:1px solid #a64;border-radius:5px;font-weight:bold}a{color:#799}#status{min-height:22px;color:#d99;margin-top:14px}
</style>
</head>
<body><div class="panel">
<h2>Возврат прошивки</h2>
<p>ESP переключится на предыдущий исправный OTA-слот и перезагрузится. Текущая версия останется во втором слоте.</p>
<button id="rollback">ВЕРНУТЬ ПРЕДЫДУЩУЮ ВЕРСИЮ</button>
<div id="status"></div>
<p><a href="/">Вернуться к управлению</a></p>
</div>
<script>
document.getElementById('rollback').addEventListener('click', async event => {
  if (!confirm('Переключиться на предыдущую прошивку?')) return;
  event.currentTarget.disabled = true;
  const status = document.getElementById('status');
  status.textContent = 'Проверка предыдущего слота...';
  try {
    const response = await fetch('/rollback', {
      method:'POST',
      headers:{'X-Confirm-Rollback':'YES'},
      cache:'no-store'
    });
    const message = await response.text();
    if (!response.ok) throw new Error(message);
    status.textContent = message;
    setTimeout(() => { location.href = '/'; }, 8000);
  }
  catch (error) {
    status.textContent = 'Ошибка: ' + error.message;
    event.currentTarget.disabled = false;
  }
});
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// ЗАЩИТА OTA И ВОЗВРАТ
// ============================================================================

void clearOtaGuardRecord()
{
    prefs.remove("ota_pending");
    prefs.remove("ota_from");
    prefs.remove("ota_boots");
    otaGuardActive = false;
}

bool switchToPreviousOtaSlot()
{
    if (!Update.canRollBack()) {
        return false;
    }

    return Update.rollBack();
}

void inspectOtaBootState()
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (running != nullptr) {
        esp_ota_img_states_t state;
        otaImageNeedsValidation =
            esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY;
    }

    if (!prefs.getBool("ota_pending", false) || running == nullptr) {
        otaValidationStartedAtMs = millis();
        return;
    }

    const uint32_t previousAddress = prefs.getULong("ota_from", 0);

    // Старый слот снова загрузился: передача была прервана или возврат уже
    // состоялся. Такой старт не должен запускать ещё один rollback.
    if (previousAddress == running->address) {
        clearOtaGuardRecord();
        otaValidationStartedAtMs = millis();
        return;
    }

    uint8_t bootAttempts = prefs.getUChar("ota_boots", 0);
    bootAttempts++;
    prefs.putUChar("ota_boots", bootAttempts);
    otaGuardActive = true;
    otaValidationStartedAtMs = millis();

    // Если новая прошивка уже один раз перезапустилась до подтверждения,
    // немедленно выбираем оставшийся рабочий OTA-слот.
    if (bootAttempts > 1 && switchToPreviousOtaSlot()) {
        clearOtaGuardRecord();
        delay(100);
        ESP.restart();
    }
}

void confirmHealthyOtaImage()
{
    if (otaImageNeedsValidation) {
        esp_ota_mark_app_valid_cancel_rollback();
        otaImageNeedsValidation = false;
    }

    clearOtaGuardRecord();
}

void rollbackUnhealthyOtaImage()
{
    if (otaImageNeedsValidation) {
        // При загрузчике с включённым rollback эта функция не вернётся.
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }

    // Совместимый запасной путь для обычного Arduino-загрузчика.
    if (switchToPreviousOtaSlot()) {
        clearOtaGuardRecord();
        delay(100);
        ESP.restart();
    }

    // Не зацикливаем перезагрузки, если второй слот отсутствует или испорчен.
    clearOtaGuardRecord();
}

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

static inline bool isValidDdramValue(uint8_t value)
{
    return
        value <= 0x07 ||
        (value >= 0x20 && value <= 0x7E) ||
        value == 0xE4;
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
        lcdClearGeneration++;

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

    if ((command & 0xF8) == 0x08) {
        bool displayOn = (command & 0x04) != 0;

        portENTER_CRITICAL(&lcdMux);
        if (lcdDisplayOn != displayOn) {
            lcdDisplayOn = displayOn;
            lcdVersion++;
        }
        portEXIT_CRITICAL(&lcdMux);

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

    if (command & 0x80) {
        lcdAddress = command & 0x7F;
        lcdAddressSpace =
            addressToPosition(lcdAddress) >= 0
                ? LCD_SPACE_DDRAM
                : LCD_SPACE_NONE;
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
    (void)now;
    int position = addressToPosition(lcdAddress);

    if (position < 0 || position >= 32) {
        rejectedDataCounter++;
        lcdAddressSpace = LCD_SPACE_NONE;
        return;
    }

    if (isValidDdramValue(value)) {
        portENTER_CRITICAL(&lcdMux);

        if (lcdScreen[position] != value) {
            lcdScreen[position] = value;
            lcdVersion++;
        }

        portEXIT_CRITICAL(&lcdMux);
        acceptedDataCounter++;
    }
    else {
        // На настоящем LCD адрес всё равно сдвинулся бы. Сохраняем старый
        // символ, но сдвигаем нашу копию, чтобы единичный мусор не сместил
        // всю оставшуюся строку.
        rejectedDataCounter++;
    }

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
    if (decoderResetRequested) {
        decoderResetRequested = false;
        stage = 0;
        firstNibble = 0;
        lastRs = currentRs;
        lastBusTime = 0;
        fourBitModeSeen = false;
        initializationNibbles = 0;
    }

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

    // При общем включении питания HD44780 сначала получает одиночные
    // командные 0x3, затем одиночный 0x2 для перехода в 4-битный режим.
    // Эти импульсы не являются парами полубайтов и не должны превращаться
    // в ложные команды. Если ESP подключился к уже работающему LCD, первый
    // обычный полубайт сразу переводит декодер в рабочий режим.
    if (!fourBitModeSeen && currentRs) {
        fourBitModeSeen = true;
        initializationNibbles = 0;
    }

    if (!fourBitModeSeen && !currentRs && stage == 0) {
        if (nibble == 0x03 && initializationNibbles < 3) {
            initializationNibbles++;
            return;
        }

        if (nibble == 0x02 && initializationNibbles >= 2) {
            fourBitModeSeen = true;
            initializationNibbles = 0;
            return;
        }

        fourBitModeSeen = true;
        initializationNibbles = 0;
    }

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
    lcdClearGeneration++;
    lcdDisplayOn = true;

    portEXIT_CRITICAL(&lcdMux);

    decoderResetRequested = true;

    lcdAddress = 0;
    lcdCgramAddress = 0;
    lcdAddressSpace = LCD_SPACE_NONE;
    entryIncrement = true;

    cgramCandidateActive = false;
    cgramCandidateLastUs = 0;

    // POWER OFF -> POWER ON начинает отдельный чистый диагностический сеанс.
    pulseCounter = 0;
    byteCounter = 0;
    addressCounter = 0;
    ignoredCommandCounter = 0;
    acceptedDataCounter = 0;
    rejectedDataCounter = 0;
    timeoutCounter = 0;
    rsResetCounter = 0;
    sampleDifferenceCounter = 0;
    lastPulseAtMs = 0;

    rawEventTail = rawEventHead;
    rawEventSequence = 0;
    rawEventDropCounter = 0;
    rawDiagnosticStage = 0;
    rawDiagnosticFirstNibble = 0;
    rawDiagnosticLastRs = false;
    rawDiagnosticLastUs = 0;
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

    // Клиент запрашивает следующий кадр только после получения предыдущего,
    // поэтому старые кадры не копятся. Копирование под одним lock даёт
    // атомарные DDRAM, CGRAM и номер настоящей команды Clear Display.
    uint8_t lcdPacket[109];
    lcdPacket[0] = 0xFD;

    portENTER_CRITICAL(&lcdMux);
    memcpy(&lcdPacket[1], lcdScreen, 32);
    memcpy(&lcdPacket[33], lcdCgram, 64);
    lcdPacket[97] = lcdVersion;
    lcdPacket[98] = (uint8_t)(lcdClearGeneration & 0xFF);
    lcdPacket[99] = (uint8_t)(lcdClearGeneration >> 8);
    memcpy(&lcdPacket[100], lcdCgramKnownRows, 8);
    lcdPacket[108] = lcdDisplayOn ? 0x01 : 0x00;

    portEXIT_CRITICAL(&lcdMux);

    client->binary(lcdPacket, sizeof(lcdPacket));
}

void sendButtonState(AsyncWebSocketClient *client)
{
    if (client == nullptr) {
        return;
    }

    char state[11] = "S000000000";
    uint16_t mask = buttonMask;

    for (uint8_t i = 0; i < 9; i++) {
        state[i + 1] = (mask & (1U << i)) ? '1' : '0';
    }

    client->text(state, 10);
}

String buildCaptureStats()
{
    uint32_t now = millis();
    uint32_t lastPulse = lastPulseAtMs;
    int32_t age =
        lastPulse == 0
            ? -1
            : (int32_t)(now - lastPulse);

    const char *spaceName = "NONE";
    uint8_t address = 0;

    if (lcdAddressSpace == LCD_SPACE_DDRAM) {
        spaceName = "DDRAM";
        address = lcdAddress;
    }
    else if (lcdAddressSpace == LCD_SPACE_CGRAM) {
        spaceName = "CGRAM";
        address = lcdCgramAddress;
    }

    char addressHex[3];
    snprintf(addressHex, sizeof(addressHex), "%02X", address);

    String message;
    message.reserve(384);
    message += 'J';
    message += "{\"pulses\":";
    message += pulseCounter;
    message += ",\"bytes\":";
    message += byteCounter;
    message += ",\"addresses\":";
    message += addressCounter;
    message += ",\"ignored\":";
    message += ignoredCommandCounter;
    message += ",\"accepted\":";
    message += acceptedDataCounter;
    message += ",\"rejected\":";
    message += rejectedDataCounter;
    message += ",\"timeouts\":";
    message += timeoutCounter;
    message += ",\"rsResets\":";
    message += rsResetCounter;
    message += ",\"sampleDiff\":";
    message += sampleDifferenceCounter;
    message += ",\"rawDropped\":";
    message += rawEventDropCounter;
    message += ",\"stage\":";
    message += (unsigned int)stage;
    message += ",\"space\":\"";
    message += spaceName;
    message += "\",\"address\":\"";
    message += addressHex;
    message += "\",\"age\":";
    message += age;
    message += '}';

    return message;
}

void drainRawBusEvents()
{
    RawBusEvent event;
    String packet;
    packet.reserve(2300);
    packet = 'R';
    uint8_t count = 0;

    while (count < 24 && popRawEvent(event)) {
        char flags[5];
        uint8_t flagPosition = 0;

        if (event.flags & RAW_TIMEOUT_RESET) {
            flags[flagPosition++] = 'T';
        }
        if (event.flags & RAW_RS_RESET) {
            flags[flagPosition++] = 'R';
        }
        if (event.flags & RAW_SAMPLE_DIFFERENCE) {
            flags[flagPosition++] = 'U';
        }
        if (flagPosition == 0) {
            flags[flagPosition++] = '-';
        }
        flags[flagPosition] = 0;

        char byteText[3] = "--";

        if (event.flags & RAW_COMPLETE_BYTE) {
            snprintf(
                byteText,
                sizeof(byteText),
                "%02X",
                event.value
            );
        }

        char line[96];
        snprintf(
            line,
            sizeof(line),
            "%08lu %7lu   %c   %c    %X   %s   %s\n",
            (unsigned long)event.sequence,
            (unsigned long)event.gapUs,
            event.rs ? 'D' : 'C',
            (event.flags & RAW_FIRST_NIBBLE) ? 'H' : 'L',
            event.nibble,
            byteText,
            flags
        );

        Serial.print("[BUS] ");
        Serial.print(line);
        packet += line;
        count++;
    }

    if (count > 0 && ws.count() > 0) {
        ws.textAll(packet);
    }
}

void printStableScreenToSerial(uint32_t now)
{
    static uint32_t observedVersion = UINT32_MAX;
    static uint32_t printedVersion = UINT32_MAX;
    static uint32_t changedAtMs = 0;

    uint32_t version = lcdVersion;

    if (version != observedVersion) {
        observedVersion = version;
        changedAtMs = now;
        return;
    }

    if (
        version == printedVersion ||
        (uint32_t)(now - changedAtMs) < 15
    ) {
        return;
    }

    uint8_t screen[32];

    portENTER_CRITICAL(&lcdMux);
    memcpy(screen, lcdScreen, sizeof(screen));
    version = lcdVersion;
    portEXIT_CRITICAL(&lcdMux);

    if (version != observedVersion) {
        observedVersion = version;
        changedAtMs = now;
        return;
    }

    printedVersion = version;
    Serial.printf("\n[LCD v=%lu]\n", (unsigned long)version);

    for (uint8_t row = 0; row < 2; row++) {
        Serial.printf("R%u TXT |", row + 1);

        for (uint8_t column = 0; column < 16; column++) {
            uint8_t value = screen[row * 16 + column];

            if (value <= 0x07) {
                Serial.printf("<%u>", value);
            }
            else if (value >= 0x20 && value <= 0x7E) {
                Serial.write(value);
            }
            else if (value == 0xE4) {
                Serial.print("<u>");
            }
            else {
                Serial.write('.');
            }
        }

        Serial.println('|');
        Serial.printf("R%u HEX ", row + 1);

        for (uint8_t column = 0; column < 16; column++) {
            Serial.printf(
                "%02X%s",
                screen[row * 16 + column],
                column == 15 ? "\n" : " "
            );
        }
    }

    Serial.println();
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

void initializeNetworkAndWeb()
{
    prefs.begin("wifi", false);
    inspectOtaBootState();

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

    server.on("/info", HTTP_GET, [](AsyncWebServerRequest *request) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);

        String json;
        json.reserve(256);
        json += "{\"firmware\":\"";
        json += FIRMWARE_VERSION;
        json += "\",\"ip\":\"";
        json += (WiFi.status() == WL_CONNECTED)
            ? WiFi.localIP().toString()
            : WiFi.softAPIP().toString();
        json += "\",\"running_partition\":\"";
        json += running ? running->label : "unknown";
        json += "\",\"update_partition\":\"";
        json += next ? next->label : "none";
        json += "\",\"rollback_available\":";
        json += Update.canRollBack() ? "true" : "false";
        json += ",\"free_heap\":";
        json += ESP.getFreeHeap();
        json += "}";

        AsyncWebServerResponse *response =
            request->beginResponse(
                200,
                "application/json; charset=utf-8",
                json
            );
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    server.on("/ota-arm", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (
            !request->hasHeader("X-OTA-Guard") ||
            request->getHeader("X-OTA-Guard")->value() != "ARM" ||
            webUpdateInProgress
        ) {
            request->send(403, "text/plain", "OTA guard rejected");
            return;
        }

        const esp_partition_t *running = esp_ota_get_running_partition();

        if (running == nullptr) {
            request->send(500, "text/plain", "Running partition unknown");
            return;
        }

        prefs.putULong("ota_from", running->address);
        prefs.putUChar("ota_boots", 0);
        prefs.putBool("ota_pending", true);
        request->send(200, "text/plain", "OTA guard armed");
    });

    server.on("/rollback", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(
            200,
            "text/html; charset=utf-8",
            rollbackHtml
        );
    });

    server.on("/rollback", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (
            !request->hasHeader("X-Confirm-Rollback") ||
            request->getHeader("X-Confirm-Rollback")->value() != "YES" ||
            webUpdateInProgress ||
            !Update.canRollBack()
        ) {
            request->send(
                409,
                "text/plain; charset=utf-8",
                "Предыдущий исправный OTA-слот недоступен"
            );
            return;
        }

        manualRollbackAtMs = millis() + 1500;
        manualRollbackRequested = true;
        request->send(
            200,
            "text/plain; charset=utf-8",
            "Возврат подготовлен. ESP перезагружается..."
        );
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

}

// ============================================================================
// СЕТЬ, WEB И OTA — ЯДРО 0
// ============================================================================

void TaskNetwork(void *parameter)
{
    (void)parameter;
    initializeNetworkAndWeb();
    uint32_t lastCleanupMs = 0;
    uint32_t lastStatsMs = 0;

    for (;;) {
        uint32_t now = millis();

        drainRawBusEvents();
        printStableScreenToSerial(now);

        if (
            ws.count() > 0 &&
            (uint32_t)(now - lastStatsMs) >= 250
        ) {
            lastStatsMs = now;
            ws.textAll(buildCaptureStats());
        }

        if (
            manualRollbackRequested &&
            (int32_t)(now - manualRollbackAtMs) >= 0
        ) {
            manualRollbackRequested = false;

            if (switchToPreviousOtaSlot()) {
                clearOtaGuardRecord();
                ESP.restart();
            }
        }

        if (
            !webUpdateInProgress &&
            (otaGuardActive || otaImageNeedsValidation)
        ) {
            uint32_t validationAge =
                (uint32_t)(now - otaValidationStartedAtMs);

            if (
                validationAge >= OTA_HEALTH_CONFIRM_MS &&
                WiFi.status() == WL_CONNECTED
            ) {
                // Сеть, AsyncWebServer и отдельная web-задача уже работают.
                confirmHealthyOtaImage();
                Serial.println("OTA image confirmed healthy");
            }
            else if (validationAge >= 45000) {
                Serial.println("OTA health check failed: rollback");
                rollbackUnhealthyOtaImage();
            }
        }

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
        bool logicalState =
            (INITIAL_BUTTON_MASK & (1U << i)) != 0;
        bool outputLevel =
            (i == 8) ? !logicalState : logicalState;

        // Сначала задаём выходной latch, затем включаем OUTPUT. Так линия
        // активного LOW питания не получает случайного импульса при старте.
        digitalWrite(BTN_PINS[i], outputLevel ? HIGH : LOW);
        pinMode(BTN_PINS[i], OUTPUT);
    }

    applyButtonMask(INITIAL_BUTTON_MASK);

    pinMode(PIN_E, INPUT_PULLDOWN);
    pinMode(PIN_RS, INPUT);
    pinMode(PIN_DB4, INPUT);
    pinMode(PIN_DB5, INPUT);
    pinMode(PIN_DB6, INPUT);
    pinMode(PIN_DB7, INPUT);

    const BaseType_t taskCreated = xTaskCreatePinnedToCore(
        TaskNetwork,
        "Network",
        12288,
        nullptr,
        1,
        nullptr,
        0
    );

    if (taskCreated != pdPASS) {
        Serial.println("Network task creation failed");
        delay(1000);
        ESP.restart();
    }
    Serial.println("LCD capture: SAMPLE_DELAY=110, capture=s1");
    Serial.println("Web LCD: atomic live snapshots, no frame-matching delay");
    Serial.println("Nibble sync: reset incomplete byte only");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
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
        lastPulseAtMs = millis();

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

        recordRawNibble(
            nibble,
            currentRs,
            captureNow,
            sampleUnstable
        );

        processCapturedNibble(
            nibble,
            currentRs,
            captureNow
        );

    }
}
