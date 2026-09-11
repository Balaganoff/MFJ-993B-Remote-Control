# Hardware implementation / Аппаратная реализация

This page documents the **working reference installation** used by the current firmware. It is not a universal MFJ-993B modification guide: PCB revisions, button polarity and the power-switching circuit may differ.

Эта страница описывает **проверенную авторскую сборку**, под которую настроена текущая прошивка. Это не универсальная инструкция для всех ревизий MFJ-993B: перед монтажом необходимо проверить точки подключения и полярность сигналов на своём экземпляре.

![MFJ-993B, 74LVC244A, ESP32, PC817 and power-relay wiring](images/hardware-wiring.svg)

## Signal paths / Направления сигналов

There are two separate paths:

1. **Display capture:** MFJ PIC16F76 → LCD1602 bus → 74LVC244A → ESP32 inputs.
2. **Remote control:** browser → ESP32 outputs → PC817 optocouplers → physical-button contacts or the power-relay driver.

The 74LVC244A path is one-way. The ESP32 never drives the LCD bus. The reverse control path is implemented by optocouplers, not by sending data back through the LCD level shifter.

В конструкции два независимых тракта:

1. **Считывание дисплея:** PIC16F76 → шина LCD1602 → 74LVC244A → входы ESP32.
2. **Обратное управление:** браузер → выходы ESP32 → оптопары PC817 → контакты штатных кнопок или драйвер реле питания.

74LVC244A работает только в направлении от LCD к ESP32. ESP32 ничего не передаёт обратно в дисплей.

## Measured LCD-bus behavior / Измеренное поведение шины LCD

The PIC controls the HD44780-compatible LCD1602 in 4-bit mode using `RS`, `E` and `DB4…DB7`. `R/W` is not captured; the decoder treats each observed transfer as a write.

On the tested tuner:

- while the bus is idle, `E` is low and `RS`, `DB4…DB7` are high;
- the PIC writes in short bursts, then leaves the LCD controller holding the last DDRAM/CGRAM state;
- it normally writes only changed commands, characters or custom-character rows rather than retransmitting the complete 16×2 screen;
- the measured high plateau of `E` is approximately **520 ns**;
- `RS` and the data lines remain stable for approximately **1–2 µs** around a transfer;
- short transition ringing of up to approximately **0.5 V** and **10–20 ns** was observed.

These idle levels and timings are measurements from one working unit, not mandatory states for every HD44780-compatible module or MFJ PCB revision.

PIC управляет совместимым с HD44780 дисплеем LCD1602 в 4-битном режиме по линиям `RS`, `E`, `DB4…DB7`. Линия `R/W` прошивкой не считывается: каждый перехваченный обмен считается записью.

На проверенном тюнере наблюдается следующее:

- в паузе `E` находится в низком уровне, а `RS` и `DB4…DB7` — в высоком;
- PIC работает короткими пакетами: передаёт команды и изменившиеся символы, после чего занимается другими задачами;
- полный экран непрерывно не передаётся — DDRAM и CGRAM остаются сохранёнными внутри контроллера LCD;
- длительность высокого уровня `E` составляет около **520 нс**;
- `RS` и линии данных сохраняют устойчивое состояние примерно **1–2 мкс** вокруг обмена;
- на переходах замечен короткий звон до примерно **0,5 В** длительностью **10–20 нс**.

Это результаты измерений одной рабочей сборки, а не обязательные уровни и времена для любого совместимого дисплея.

### Why the capture point is late / Почему выборка выполняется ближе к концу импульса

In 4-bit mode each byte is transferred as two nibbles on `DB7…DB4`. During a write the LCD accepts the bus state at the falling edge of `E`. At a 4.5–5.5 V LCD supply, the HD44780U data sheet specifies a minimum 230 ns `E` high pulse, 80 ns data setup and 10 ns data hold.

The firmware detects `E` high and waits `SAMPLE_DELAY = 110` CPU cycles. At 240 MHz this delay alone is approximately **0.46 µs**, so the sample is taken near the end of the measured 0.52 µs plateau, where the data is already stable. The exact position also contains software detection latency; the value `110` was therefore selected and verified experimentally on the terminal capture sketch.

В 4-битном режиме один байт передаётся двумя полубайтами по `DB7…DB4`. При записи LCD принимает состояние шины по спаду `E`. Прошивка замечает высокий `E`, ждёт `110` тактов CPU — около **0,46 мкс** при 240 МГц — и считывает все линии одним чтением регистра GPIO. Точка получается около конца измеренной полки `E`, когда данные уже установились. Реальный момент включает задержку обнаружения фронта, поэтому значение `110` было подобрано и подтверждено отдельной терминальной прошивкой.

## 74LVC244A LCD input stage

The tested interface uses a 74LVC244A octal non-inverting buffer:

- power the buffer from **3.3 V**;
- connect both active-low output-enable pins (`/OE1`, `/OE2`) to GND;
- place a **100 nF** ceramic bypass capacitor close to the IC between VCC and GND;
- feed the six 5 V LCD lines into six `A` inputs and connect the corresponding `Y` outputs to the ESP32;
- do not leave the two unused `A` inputs floating;
- keep the LCD-side and ESP32-side wires short;
- use a common reference GND for the LCD sensing path.

The SN74LVC244A supply range is 1.65–3.6 V and its inputs accept up to 5.5 V, allowing 5 V input / 3.3 V output operation when the exact LVC part is powered at 3.3 V. Do not substitute an arbitrary `244`-family device without checking its data sheet.

Для проверенной сборки используется восьмиканальный неинвертирующий буфер 74LVC244A. Он питается от **3,3 В**, принимает сигналы LCD на входы `A` и выдаёт безопасные для ESP32 уровни с выходов `Y`. Оба входа `/OE` подключаются к GND. Возле микросхемы обязателен керамический конденсатор **100 нФ**. Неиспользуемые входы нельзя оставлять плавающими.

### LCD pin map

| LCD signal | LCD module pin | 74LVC244A channel | ESP32 GPIO | ESP32 mode |
|---|---:|---|---:|---|
| RS | 4 | A1 → Y1 | 4 | INPUT |
| E | 6 | A2 → Y2 | 17 | INPUT_PULLDOWN |
| DB4 | 11 | A3 → Y3 | 25 | INPUT |
| DB5 | 12 | A4 → Y4 | 18 | INPUT |
| DB6 | 13 | A5 → Y5 | 19 | INPUT |
| DB7 | 14 | A6 → Y6 | 23 | INPUT |
| GND/VSS | 1 | GND | GND | common reference |

`DB0…DB3` are not used in 4-bit mode. The ESP32 does not need a connection to LCD `R/W`.

## PC817 button interfaces

Eight PC817 channels are wired electrically in parallel with the eight physical front-panel button contacts. Pressing a web button turns on the corresponding optocoupler LED; the PC817 phototransistor then closes the same circuit as the original button. The physical controls remain usable.

Восемь каналов PC817 распаяны параллельно восьми физическим кнопкам передней панели. Команда из браузера включает светодиод нужной оптопары, а её фототранзистор замыкает ту же цепь, что и штатная кнопка. Физические кнопки продолжают работать.

| Bit | Control | ESP32 GPIO | Firmware asserted level | UI behavior |
|---:|---|---:|---|---|
| 0 | ANT | 14 | HIGH | Latched ANT1/ANT2 state |
| 1 | C-UP | 26 | HIGH | Momentary, hold supported |
| 2 | L-UP | 27 | HIGH | Momentary, hold supported |
| 3 | AUTO | 33 | HIGH | Latched AUTO/MANUAL state |
| 4 | MODE | 13 | HIGH | Momentary, hold supported |
| 5 | C-DN | 2 | HIGH | Momentary, hold supported |
| 6 | L-DN | 5 | HIGH | Momentary, hold supported |
| 7 | TUNE | 21 | HIGH | Momentary, hold supported |

Each PC817 LED requires its own series resistor. Select the value from the actual LED current, PC817 rank/CTR and the tuner-side current; **330–470 Ω at 3.3 V is only a practical starting range, not a universal value**. On the isolated side, orient collector and emitter according to the measured polarity of the corresponding button circuit.

Service and setup combinations need no extra logic: the firmware activates several optocouplers at the same time, exactly as if the corresponding physical buttons were pressed together.

> GPIO2 and GPIO5 are strapping pins on a classic ESP32. The connected LED/resistor stages must not force an invalid level while the ESP32 is resetting or booting.

## Power relay

`POWER` uses a ninth isolated channel and an inverted firmware output:

| Control | ESP32 GPIO | Logical state | Physical GPIO level |
|---|---:|---|---|
| POWER | 32 | tuner ON | LOW |
| POWER | 32 | tuner OFF | HIGH |

In the reference design, current for the POWER optocoupler LED flows from 3.3 V through its resistor and LED into GPIO32, so driving GPIO32 low activates the isolated power-control path. Use an external pull-up or a relay module with a defined safe input state so reset/boot cannot switch power unexpectedly.

The PC817 phototransistor should drive a suitable relay input or a separate transistor/MOSFET relay driver. A bare PC817 must not supply a conventional relay coil directly. Fit the required flyback diode across a DC relay coil. The relay contacts switch the MFJ power feed; keep the 12–15 V power circuit separate from the ESP32 side.

GPIO32 управляет девятой оптопарой с обратной логикой: **LOW = тюнер включён**, **HIGH = выключен**. Выход PC817 должен подаваться на готовый вход релейного модуля либо на отдельный транзисторный/MOSFET-драйвер с защитным диодом. Питать катушку обычного реле непосредственно от PC817 нельзя.

## Safety and commissioning

- Never connect a 5 V LCD line directly to an ESP32 GPIO.
- Confirm the exact LVC buffer marking, its 3.3 V supply and both `/OE` levels before connecting the ESP32.
- Verify every button contact with a multimeter and confirm PC817 collector/emitter orientation before attaching all channels.
- Test the relay first with the tuner disconnected and confirm the safe state during ESP32 reset.
- The optocouplers isolate the switch contacts only if their tuner-side wiring and supplies remain separate from the ESP32 side.
- Disconnect the transmitter, antennas and 12–15 V supply before opening the tuner. Never work inside it while transmitting.
- Avoid rapid MFJ power cycling; follow the power and service warnings in the manufacturer manual.

## Component references

- [HD44780U manufacturer data sheet (archived PDF)](https://cdn.sparkfun.com/assets/9/5/f/7/b/HD44780.pdf)
- [Texas Instruments SN74LVC244A data sheet](https://www.ti.com/lit/ds/symlink/sn74lvc244a.pdf)
- [Sharp PC817XxNSZ1B data sheet](https://global.sharp/products/device/lineup/data/pdf/datasheet/PC817XxNSZ1B_e.pdf)
- [Espressif ESP32 Series data sheet](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf)
- [MFJ-993B instruction manual](https://cdn.shopify.com/s/files/1/0289/7782/3843/files/MFJ-993B.pdf?v=1586534115)
- [MFJ-991B/993B/994B/995 Rev. 2 schematic](https://cdn.shopify.com/s/files/1/0289/7782/3843/files/MFJ-991B_993B_994B_Rev_2_Schematic.pdf?v=1586534155)
