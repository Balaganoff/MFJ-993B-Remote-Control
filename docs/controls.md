# Controls and combinations / Кнопки и сочетания

The web page mirrors the nine physical controls. ANT, AUTO, and POWER are software-latched. C-UP, L-UP, MODE, C-DN, L-DN, and TUNE remain asserted for exactly as long as the pointer is held.

Веб-страница повторяет девять органов управления. ANT, AUTO и POWER фиксируются программно. C-UP, L-UP, MODE, C-DN, L-DN и TUNE остаются нажатыми всё время удержания мыши или пальца.

Button state packets are sent immediately. LCD requests continue every 100 ms during a hold so changing values and entry into Setup remain visible; the button command itself does not wait for an LCD frame.

Команда кнопки отправляется сразу. Во время удержания LCD продолжает опрашиваться раз в 100 мс, поэтому изменение параметров и вход в Setup остаются видны.

> A newly opened browser page starts with ANT1, AUTO and POWER ON selected and sends that state to the ESP32. Latched ANT/AUTO state is not read back from the tuner or stored between browser sessions.

> После открытия страницы начальное состояние интерфейса — ANT1, AUTO и POWER ON. Положение ANT/AUTO не считывается обратно из тюнера и не сохраняется между сеансами браузера.

## MODE and TUNE timing / Временные функции MODE и TUNE

According to the MFJ manual:

- A brief MODE press advances through the four main screens or through the Setup screens.
- Holding MODE for two seconds enters Setup; holding it for two seconds again exits Setup.
- Setup automatically exits after eight seconds without a button press.
- C-UP/L-UP increase or enable the current Setup value; C-DN/L-DN decrease or disable it.
- TUNE has different actions for a quick press, a 0.5-2 second hold, and a hold longer than two seconds.

Поэтому прошивка не имеет таймера автоотпускания: длительность определяет пользователь.

## Normal-operation combinations / Комбинации обычного режима

The **Доп. комбинации** section implements the shortcuts from Figure 3 of the MFJ manual.

| Web button | Held controls | Function |
|---|---|---|
| CAP INPUT/OUTPUT | C-UP + C-DN | Move capacitance between input/output side |
| SWR BEEP | L-UP + L-DN | Toggle SWR Beep |
| C + L UP | C-UP + L-UP | Increase capacitance and inductance together |
| BYPASS | C-DN + L-DN | Bypass tuner |
| TARGET SWR | TUNE + C-UP | Toggle target SWR 1.5/2.0 |
| AUTO TUNE SWR | TUNE + L-UP | Cycle threshold 0.5/1.0/1.5 above target |
| MEMORY A-D | TUNE + C-DN | Cycle memory bank A/B/C/D |
| METER RANGE | TUNE + L-DN | Cycle 30 W/300 W/auto range |
| POWER 300/150 | TUNE + C-UP + L-UP | Toggle power level 300/150 W |
| SAVE CURRENT | TUNE + C-DN + L-DN | Overwrite memory with current tuner setting |

`LC LIMIT (SETUP)` is intended only after Setup is already open. It asserts MODE, then adds C-UP + L-UP within two seconds as specified by the manual.

## Power-on operations / Операции при включении

For these commands the ESP32 releases momentary controls, powers the tuner off for 2.2 seconds, holds the selected controls, powers it on, waits 1.8 seconds, and releases them. Dangerous operations require browser confirmation.

| Web button | Held while powering on |
|---|---|
| FIRMWARE VERSION | C-UP |
| SELF TEST | L-UP |
| RELAY TEST | C-DN |
| POWER-DOWN TEST | L-DN |
| WATTMETER CAL | C-UP + C-DN |
| AUDIO / VOLUME | L-UP + L-DN |
| SWR BRIDGE CAL | C-UP + L-UP |
| FREQ COUNTER CAL | C-DN + L-DN |
| DELETE ANT MEMORY | TUNE + C-DN + ANT |
| FACTORY DEFAULTS | TUNE + L-DN |
| TOTAL RESET | TUNE + C-DN + L-DN |

For DELETE ANT MEMORY and TOTAL RESET, the tuner itself asks for confirmation: C-UP = YES, L-UP = NO.

## Service-operation warning / Предупреждение

- Disconnect or power down the transmitter before relay tests and calibration procedures.
- Do not perform calibration without the test equipment and exact procedure in the MFJ manual.
- Self Test can restore factory settings.
- DELETE ANT MEMORY and TOTAL RESET erase stored matching data.
- The MFJ manual requires the radio interface cable to be disconnected before power-on operations if the radio is off.

Подробности и обязательные условия смотрите в разделах **Setup Mode Menus**, **Resetting the Tuner**, **Self Test** и **Calibration** официального [руководства MFJ-993B](https://cdn.shopify.com/s/files/1/0289/7782/3843/files/MFJ-993B.pdf?v=1586534115).
