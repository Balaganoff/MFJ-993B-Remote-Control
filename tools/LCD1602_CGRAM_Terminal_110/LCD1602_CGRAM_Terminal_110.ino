#include <Arduino.h>
#include "soc/gpio_reg.h"

// ============================================================================
// РАСПИНОВКА
// ============================================================================

const int PIN_E   = 17;
const int PIN_RS  = 4;
const int PIN_DB4 = 25;
const int PIN_DB5 = 18;
const int PIN_DB6 = 19;
const int PIN_DB7 = 23;

// Линия виртуальной кнопки POWER из проверенной web-версии.
// Физическая полярность инверсная: LOW = tuner ON, HIGH = tuner OFF.
const int PIN_POWER = 32;
const uint8_t POWER_ON_LEVEL = LOW;
const uint8_t POWER_OFF_LEVEL = HIGH;
const uint32_t POWER_OFF_TIME_MS = 1500;

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
// Ждём окончания пачки записи, чтобы не печатать посимвольное стирание шкал.
// Экраны Setup Mode держатся намного дольше, поэтому 12 мс их не пропустят.
const uint32_t DISPLAY_IDLE_US = 12000;
const uint32_t CGRAM_BLOCK_TIMEOUT_US = 10000;

// ============================================================================
// СОСТОЯНИЕ HD44780
// ============================================================================

enum LcdAddressSpace : uint8_t {
    LCD_SPACE_NONE = 0,
    LCD_SPACE_DDRAM,
    LCD_SPACE_CGRAM
};

uint8_t lcdScreen[32];

// Восемь пользовательских символов, по восемь строк в каждом.
// В каждой строке используются младшие пять бит.
uint8_t lcdCgram[8][8];
uint8_t lcdCgramKnownRows[8];
uint8_t lcdCgramChangedRows[8];

volatile LcdAddressSpace lcdAddressSpace = LCD_SPACE_NONE;
volatile uint8_t lcdAddress = 0;
volatile uint8_t lcdCgramAddress = 0;

// Entry Mode Set: по умолчанию HD44780 увеличивает адрес.
volatile bool entryIncrement = true;

portMUX_TYPE lcdMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t lcdRevision = 0;
volatile uint32_t cgramRevision = 0;
volatile uint32_t lastLcdChangeUs = 0;

// ============================================================================
// СБОРКА БАЙТА
// ============================================================================

uint8_t stage = 0;
uint8_t firstNibble = 0;

bool lastRs = false;
uint32_t lastBusTime = 0;

// ============================================================================
// ДИАГНОСТИКА
// ============================================================================

volatile uint32_t pulseCounter = 0;
volatile uint32_t byteCounter = 0;

volatile uint32_t commandCounter = 0;
volatile uint32_t ddramAddressCounter = 0;
volatile uint32_t cgramAddressCounter = 0;
volatile uint32_t ignoredCommandCounter = 0;

volatile uint32_t ddramDataCounter = 0;
volatile uint32_t cgramDataCounter = 0;
volatile uint32_t hiddenDdramDataCounter = 0;
volatile uint32_t noAddressDataCounter = 0;

volatile uint32_t timeoutCounter = 0;
volatile uint32_t rsResetCounter = 0;
volatile uint32_t sampleDifferenceCounter = 0;

// CGRAM может обновляться частично: PIC имеет право записать одну строку
// матрицы и оставить остальные семь без изменения.
bool cgramCandidateActive = false;
uint32_t cgramCandidateLastUs = 0;

volatile uint32_t cgramCandidateCounter = 0;
volatile uint32_t cgramRejectedCounter = 0;

// ============================================================================
// УПРАВЛЕНИЕ ПИТАНИЕМ ИЗ SERIAL
// ============================================================================

enum PowerCycleState : uint8_t {
    POWER_CYCLE_IDLE = 0,
    POWER_CYCLE_WAIT_OFF
};

PowerCycleState powerCycleState = POWER_CYCLE_IDLE;
uint32_t powerOffStartedMs = 0;

void clearCapturedLcdState()
{
    portENTER_CRITICAL(&lcdMux);

    memset(lcdScreen, ' ', sizeof(lcdScreen));
    memset(lcdCgram, 0, sizeof(lcdCgram));
    memset(lcdCgramKnownRows, 0, sizeof(lcdCgramKnownRows));
    memset(lcdCgramChangedRows, 0xFF, sizeof(lcdCgramChangedRows));

    lcdRevision++;
    cgramRevision++;
    lastLcdChangeUs = micros();

    portEXIT_CRITICAL(&lcdMux);

    // К этому моменту POWER уже 1,5 секунды находится в OFF,
    // поэтому импульсов E нет и состояние сборщика можно обнулить.
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

void startPowerCycle()
{
    if (powerCycleState != POWER_CYCLE_IDLE) {
        Serial.println("POWER cycle already running");
        return;
    }

    Serial.println();
    Serial.println("POWER: OFF");

    digitalWrite(PIN_POWER, POWER_OFF_LEVEL);
    powerOffStartedMs = millis();
    powerCycleState = POWER_CYCLE_WAIT_OFF;
}

void servicePowerCycle()
{
    while (Serial.available() > 0) {
        char command = (char)Serial.read();

        if (command == 'P' || command == 'p') {
            startPowerCycle();
        }
    }

    if (
        powerCycleState == POWER_CYCLE_WAIT_OFF &&
        (uint32_t)(millis() - powerOffStartedMs) >=
            POWER_OFF_TIME_MS
    ) {
        clearCapturedLcdState();

        // Захват loop() на ядре 1 уже работает. Теперь включаем тюнер
        // и принимаем его полную инициализацию DDRAM и CGRAM.
        digitalWrite(PIN_POWER, POWER_ON_LEVEL);
        powerCycleState = POWER_CYCLE_IDLE;

        Serial.println("POWER: ON -- capturing LCD initialization");
    }
}

// ============================================================================
// ЧТЕНИЕ ПОЛУБАЙТА
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

// ============================================================================
// АДРЕСА LCD1602
// ============================================================================

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

// ============================================================================
// ОБРАБОТКА КОМАНД HD44780
// ============================================================================

void processCommand(uint8_t command, uint32_t now)
{
    commandCounter++;

    if (command == 0x01) {
        // Clear Display одновременно выбирает DDRAM с адресом 0.
        portENTER_CRITICAL(&lcdMux);

        memset(lcdScreen, ' ', sizeof(lcdScreen));
        lcdRevision++;
        lastLcdChangeUs = micros();

        portEXIT_CRITICAL(&lcdMux);

        lcdAddress = 0;
        lcdAddressSpace = LCD_SPACE_DDRAM;
        cgramCandidateActive = false;
        return;
    }

    if (command == 0x02) {
        // Return Home: выбирает DDRAM с адресом 0, не стирая экран.
        lcdAddress = 0;
        lcdAddressSpace = LCD_SPACE_DDRAM;
        cgramCandidateActive = false;
        return;
    }

    if ((command & 0xFC) == 0x04) {
        // Entry Mode Set: бит I/D задаёт направление изменения адреса.
        entryIncrement = (command & 0x02) != 0;
        cgramCandidateActive = false;
        return;
    }

    if ((command & 0xC0) == 0x40) {
        /*
           Возможный Set CGRAM Address: 0x40...0x7F.

           На общей шине встречаются ложные командные байты, поэтому
           одна такая команда ещё ничего не доказывает. Запись будет
           принята только если следом с RS=1 придёт допустимая строка
           пикселей 0x00...0x1F.
        */
        lcdCgramAddress = command & 0x3F;
        cgramCandidateLastUs = now;
        cgramCandidateActive = true;

        lcdAddressSpace = LCD_SPACE_CGRAM;
        cgramAddressCounter++;
        cgramCandidateCounter++;
        return;
    }

    if (isVisibleAddressCommand(command)) {
        // Только адреса реально видимых 16+16 ячеек.
        lcdAddress = command & 0x7F;
        lcdAddressSpace = LCD_SPACE_DDRAM;

        cgramCandidateActive = false;

        ddramAddressCounter++;
        return;
    }

    // Прочие байты общей шины игнорируем. Особенно важно не принимать
    // любой 0x80...0xFF за адрес DDRAM: именно это сломало прошлый тест.
    ignoredCommandCounter++;
}

// ============================================================================
// ОБРАБОТКА ДАННЫХ HD44780
// ============================================================================

void processCgramData(uint8_t value, uint32_t now)
{
    if (
        !cgramCandidateActive ||
        (uint32_t)(now - cgramCandidateLastUs) >
            CGRAM_BLOCK_TIMEOUT_US ||
        (value & 0xE0) != 0
    ) {
        cgramRejectedCounter++;
        cgramCandidateActive = false;
        lcdAddressSpace = LCD_SPACE_NONE;
        noAddressDataCounter++;
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

    // Меняем ровно одну адресованную строку. Все остальные пиксели
    // пользовательского символа остаются такими, какими были.
    lcdCgram[character][row] = pixels;
    lcdCgramKnownRows[character] |= rowBit;

    if (changed) {
        lcdCgramChangedRows[character] |= rowBit;
        cgramRevision++;
        lcdRevision++;
        lastLcdChangeUs = micros();
    }

    portEXIT_CRITICAL(&lcdMux);

    cgramDataCounter++;
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

    if (position >= 0 && position < 32) {
        portENTER_CRITICAL(&lcdMux);

        if (lcdScreen[position] != value) {
            lcdScreen[position] = value;
            lcdRevision++;
            lastLcdChangeUs = micros();
        }

        portEXIT_CRITICAL(&lcdMux);

        ddramDataCounter++;
    }
    else {
        hiddenDdramDataCounter++;
        lcdAddressSpace = LCD_SPACE_NONE;
        return;
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
        processDdramData(value);
    }
    else {
        noAddressDataCounter++;
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

// ============================================================================
// СБОРКА ДВУХ ПОЛУБАЙТОВ
// ============================================================================

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

        // Сбрасываем только незавершённый байт. Сам HD44780 после паузы
        // не забывает выбранные DDRAM/CGRAM и текущий адрес.
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
// ВЫВОД ЭКРАНА И CGRAM
// ============================================================================

void printLcdCharacter(uint8_t value)
{
    if (value <= 0x07) {
        // Цифра здесь означает номер CGRAM-символа. Его точная матрица
        // печатается ниже. Никаких выдуманных Y или ?.
        Serial.write((uint8_t)('0' + value));
    }
    else if (value == 0xE4) {
        Serial.print("µ");
    }
    else if (value >= 32 && value <= 126) {
        Serial.write(value);
    }
    else {
        Serial.write(' ');
    }
}

void printScreen(const uint8_t *screen)
{
    Serial.println();

    Serial.print('+');

    for (int i = 0; i < 16; i++) {
        printLcdCharacter(screen[i]);
    }

    Serial.println('+');
    Serial.print('+');

    for (int i = 16; i < 32; i++) {
        printLcdCharacter(screen[i]);
    }

    Serial.println('+');
}

uint8_t activeCgramMask(const uint8_t *screen)
{
    uint8_t mask = 0;

    for (uint8_t i = 0; i < 32; i++) {
        if (screen[i] <= 0x07) {
            mask |= (uint8_t)(1U << screen[i]);
        }
    }

    return mask;
}

void printChangedRows(const uint8_t *changedRows, uint8_t activeMask)
{
    Serial.print("Changed CGRAM rows:");
    bool printedAny = false;

    for (uint8_t character = 0; character < 8; character++) {
        uint8_t rows = changedRows[character];

        if (rows == 0) {
            continue;
        }

        printedAny = true;
        Serial.printf(
            " slot %u%s=",
            character,
            (activeMask & (1U << character)) ? "*" : ""
        );

        for (uint8_t row = 0; row < 8; row++) {
            if (rows & (1U << row)) {
                Serial.write((uint8_t)('0' + row));
            }
        }
    }

    if (!printedAny) {
        Serial.print(" none");
    }

    Serial.println();
    Serial.println("* = slot is present on the current screen");
}

void printActiveCgramAtlas(
    const uint8_t cgram[8][8],
    const uint8_t *knownRows,
    uint8_t activeMask
) {
    if (activeMask == 0) {
        Serial.println("No CGRAM symbols on this screen");
        return;
    }

    Serial.print("Active CGRAM slots:");

    for (uint8_t character = 0; character < 8; character++) {
        if (activeMask & (1U << character)) {
            Serial.printf(" %u", character);
        }
    }

    Serial.println();
    Serial.println("# = pixel ON, . = pixel OFF, - = row not captured yet");

    for (uint8_t character = 0; character < 8; character++) {
        if (activeMask & (1U << character)) {
            Serial.printf("   %u   ", character);
        }
    }

    Serial.println();

    for (uint8_t row = 0; row < 8; row++) {
        for (uint8_t character = 0; character < 8; character++) {
            if ((activeMask & (1U << character)) == 0) {
                continue;
            }

            uint8_t rowBit = (uint8_t)(1U << row);

            if ((knownRows[character] & rowBit) == 0) {
                Serial.print("----- ");
                continue;
            }

            uint8_t pixels = cgram[character][row];

            for (int8_t column = 4; column >= 0; column--) {
                Serial.write(
                    (pixels & (1U << column))
                        ? '#'
                        : '.'
                );
            }

            Serial.write(' ');
        }

        Serial.println();
    }
}

const char *addressSpaceName(LcdAddressSpace space)
{
    if (space == LCD_SPACE_DDRAM) return "DDRAM";
    if (space == LCD_SPACE_CGRAM) return "CGRAM";
    return "NONE";
}

// ============================================================================
// SERIAL — ЯДРО 0
// ============================================================================

void TaskSerial(void *parameter)
{
    uint8_t screenCopy[32];
    uint8_t cgramCopy[8][8];
    uint8_t knownRowsCopy[8];
    uint8_t changedRowsCopy[8];

    uint32_t printedRevision = 0;

    uint32_t lastPulses = 0;
    uint32_t lastBytes = 0;
    uint32_t lastDifferences = 0;
    uint32_t lastReportMs = 0;

    for (;;) {
        servicePowerCycle();

        uint32_t revision = lcdRevision;
        uint32_t activity = lastLcdChangeUs;

        if (
            revision != printedRevision &&
            (uint32_t)(micros() - activity) >= DISPLAY_IDLE_US
        ) {
            portENTER_CRITICAL(&lcdMux);

            memcpy(screenCopy, lcdScreen, sizeof(screenCopy));
            memcpy(cgramCopy, lcdCgram, sizeof(cgramCopy));
            memcpy(
                knownRowsCopy,
                lcdCgramKnownRows,
                sizeof(knownRowsCopy)
            );
            memcpy(
                changedRowsCopy,
                lcdCgramChangedRows,
                sizeof(changedRowsCopy)
            );
            memset(
                lcdCgramChangedRows,
                0,
                sizeof(lcdCgramChangedRows)
            );

            printedRevision = lcdRevision;

            portEXIT_CRITICAL(&lcdMux);

            printScreen(screenCopy);
            uint8_t activeMask = activeCgramMask(screenCopy);
            printChangedRows(changedRowsCopy, activeMask);
            printActiveCgramAtlas(cgramCopy, knownRowsCopy, activeMask);
        }

        if ((uint32_t)(millis() - lastReportMs) >= 1000) {
            lastReportMs = millis();

            uint32_t pulses = pulseCounter;
            uint32_t bytes = byteCounter;
            uint32_t differences = sampleDifferenceCounter;

            if (
                pulses != lastPulses ||
                bytes != lastBytes ||
                differences != lastDifferences
            ) {
                LcdAddressSpace space = lcdAddressSpace;
                uint8_t address =
                    (space == LCD_SPACE_CGRAM)
                        ? lcdCgramAddress
                        : lcdAddress;

                Serial.printf(
                    "pulses=%lu (+%lu) "
                    "bytes=%lu (+%lu) "
                    "DDaddr=%lu CGaddr=%lu CGrows=%lu CGbad=%lu "
                    "DDdata=%lu rejected=%lu "
                    "sampleDiff=%lu (+%lu) "
                    "timeouts=%lu rsReset=%lu "
                    "space=%s AC=0x%02X visible=%s\n",

                    (unsigned long)pulses,
                    (unsigned long)(pulses - lastPulses),
                    (unsigned long)bytes,
                    (unsigned long)(bytes - lastBytes),
                    (unsigned long)ddramAddressCounter,
                    (unsigned long)cgramCandidateCounter,
                    (unsigned long)cgramDataCounter,
                    (unsigned long)cgramRejectedCounter,
                    (unsigned long)ddramDataCounter,
                    (unsigned long)noAddressDataCounter,
                    (unsigned long)differences,
                    (unsigned long)(differences - lastDifferences),
                    (unsigned long)timeoutCounter,
                    (unsigned long)rsResetCounter,
                    addressSpaceName(space),
                    address,
                    (
                        space == LCD_SPACE_DDRAM &&
                        addressToPosition(address) >= 0
                    ) ? "YES" : "NO"
                );

                lastPulses = pulses;
                lastBytes = bytes;
                lastDifferences = differences;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
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
    memset(lcdCgramChangedRows, 0, sizeof(lcdCgramChangedRows));

    // Сначала задаём безопасный уровень ON, затем переводим GPIO в OUTPUT,
    // чтобы при старте ESP не возник короткий ложный импульс POWER OFF.
    digitalWrite(PIN_POWER, POWER_ON_LEVEL);
    pinMode(PIN_POWER, OUTPUT);

    pinMode(PIN_E, INPUT_PULLDOWN);
    pinMode(PIN_RS, INPUT);
    pinMode(PIN_DB4, INPUT);
    pinMode(PIN_DB5, INPUT);
    pinMode(PIN_DB6, INPUT);
    pinMode(PIN_DB7, INPUT);

    xTaskCreatePinnedToCore(
        TaskSerial,
        "SerialOutput",
        4096,
        nullptr,
        1,
        nullptr,
        0
    );

    Serial.println();
    Serial.println("LCD1602 DDRAM+CGRAM sniffer");
    Serial.println("E=17 RS=4 D4=25 D5=18 D6=19 D7=23");
    Serial.println("POWER=GPIO32, LOW=ON, HIGH=OFF");
    Serial.printf(
        "CPU=240MHz SAMPLE_DELAY=%lu capture=s1\n",
        (unsigned long)SAMPLE_DELAY
    );
    Serial.println("Stable screen output after 12 ms idle");
    Serial.println("CGRAM 0..7 is accumulated row-by-row as 5x8 matrices");
    Serial.println("Screen codes 0..7 are slots, not decimal digits");
    Serial.println("Only CGRAM slots used by the current screen are printed");
    Serial.println("Send P in Serial Monitor to cycle tuner OFF -> ON");
}

// ============================================================================
// ЗАХВАТ — ЯДРО 1
// ============================================================================

void loop()
{
    if (REG_READ(GPIO_IN_REG) & MASK_E) {
        uint32_t start = xthal_get_ccount();

        while (
            (uint32_t)(xthal_get_ccount() - start) < SAMPLE_DELAY
        ) {
            // Критический участок: ничего сюда не добавлять.
        }

        uint32_t s1 = REG_READ(GPIO_IN_REG);
        uint32_t s2 = REG_READ(GPIO_IN_REG);

        while (REG_READ(GPIO_IN_REG) & MASK_E) {
            // Один импульс E обрабатывается ровно один раз.
        }

        pulseCounter++;

        if (((s1 ^ s2) & MASK_LCD_BUS) != 0) {
            sampleDifferenceCounter++;
        }

        bool currentRs = (s1 & MASK_RS) != 0;
        uint8_t nibble = readNibble(s1);

        processCapturedNibble(
            nibble,
            currentRs,
            micros()
        );
    }
}
