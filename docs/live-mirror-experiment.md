# Live LCD mirror experiment / Экспериментальная живая копия LCD

Branch / ветка: `experiment/live-lcd-mirror-20260914`.
This is an alternative implementation, not a claim of a hardware-tested release.
Это альтернативная реализация, а не объявление проверенного на физическом тюнере релиза.
The existing `main` branch and its binaries are unchanged / `main` и его бинарники не изменены.

## English

### What changed

The firmware now separates three responsibilities:

1. **Acquisition, core 1:** sample the six input signals at the previously tested 110 CPU-cycle delay, wait for E to fall, and append one raw nibble to a 4096-entry SPSC ring. No LCD decoding, network work, timestamps from `micros()`, or shared model lock runs in this hot path. The ring consumes 32 KiB and absorbs short decoder stalls; it cannot recover electrical edges the CPU never observed.
2. **LCD model, core 0:** consume the samples and apply HD44780 commands to 80 bytes of DDRAM and 64 bytes of CGRAM. Keep the address and memory across silent periods. Handle Clear Display, both Return Home encodings, display on/off, entry increment/decrement, hidden DDRAM columns, address wrapping, and display shift. CGRAM changes are masked to five pixel bits and applied one row at a time, without an arbitrary block timeout.
3. **Browser:** request a current 98-byte snapshot, draw only cells whose final pixels changed, then request the next snapshot. There is one outstanding LCD request per connection. Requests use a 20 ms interval after replies normally and 40 ms while a momentary button is held. These are scheduling intervals, not guaranteed end-to-end frame rates: VPN round-trip time and Wi-Fi also matter. A 2-second timeout reconnects the socket instead of issuing duplicate requests on it.

There is **no 12 ms DDRAM quiet gate**, no requirement for two identical screens or CGRAM pictures, and no timer that keeps Setup in a separate parsing mode. A frame already in flight is displayed; releasing a button then requests a fresh frame rather than repeatedly discarding replies.

Wi-Fi startup runs independently on core 0, so capture begins before waiting for Wi-Fi. Reopening the page synchronizes ANT/AUTO/POWER from the ESP output mask instead of sending browser defaults. Lost pointer capture, page hiding and socket disconnection release momentary outputs.

Text and custom symbols share one soft 5x8 dot renderer. The ASCII patterns are original generic LCD-style patterns, **not an exact ROM dump**; unimplemented extended ROM characters appear blank. Underline/blinking cursor effects, single-line/5x10/8-bit operation and R/W reads are not mirrored. The implementation assumes the existing two-line 4-bit write-only wiring.

### Main display versus literal display

By default, a screen containing **MHz, FWD= and REF=** uses fixed numeric positions. Recognition is strict and instantaneous; other screens are rendered literally. Missing/invalid main values become blank in that frame instead of keeping old readings indefinitely.

The antenna numeral in this fixed main layout comes from the remote ANT output mask. The IntelliTune row and other indicators remain live CGRAM. This is **remote output state**, not independently measured feedback from a physical ANT button. Pressing controls directly on the tuner can make this synthetic numeral disagree.

A recognized tuning bar uses a full block for slot 0, preserving the previously requested cosmetic behavior. For unfixed text positions and completely literal CGRAM, open:

`http://<ESP-IP>/?raw=1`

This mode bypasses main-screen normalization, the synthetic antenna numeral, and the tuning-bar override. It does not change acquisition or button handling.

### Build and upload without Arduino IDE

Use the artifact produced by the successful **Build firmware** run for this branch/commit. It contains `MFJ993B_Remote_Control.ino.bin`, an application-only OTA image. Do not substitute an older artifact from another branch.

1. Download and unzip the Actions artifact.
2. Open the existing ESP page and follow **Firmware Update (.bin)**, or visit `http://<ESP-IP>/update`.
3. Select `MFJ993B_Remote_Control.ino.bin` and upload. Keep power and the connection active until completion; wait for the ESP restart.
4. Refresh the control page. If the ESP restarted while the tuner stayed powered, cycle the tuner with POWER after the page reconnects, allowing a few seconds OFF, so that a full initialization of DDRAM/CGRAM is captured.

Do **not** upload a merged image, bootloader or partition table through this updater. This variant does not require erasing flash; saved Wi-Fi credentials remain in the same `wifi` namespace. ESP reboot initializes remote outputs to the project's existing ANT1/AUTO/POWER-ON mask. Preserve a known working application image for rollback.

For a local reproducible build: install Python and PlatformIO, then run `pio run` in the repository. The pinned environment uses classic ESP32 at 240 MHz and places AsyncTCP callbacks on core 0. Arduino IDE can compile the single sketch with the existing dependencies, but separate library translation units do not inherit a sketch-local AsyncTCP core macro; use the CI/PlatformIO build for the tested core-affinity configuration.

### Verification and limits

The build runs host tests against the actual C++ model/ring extracted from the sketch, plus the actual embedded browser script. Coverage includes partial writes, pauses, lost-nibble resynchronization, DDRAM wrapping/hidden columns/shifts, CGRAM preservation, display off, boot preamble, concurrent SPSC ordering, immediate menu/clear/CGRAM drawing, output-state synchronization and stale socket rejection. These are software checks, not a substitute for observing the physical bus.

If the raw ring actually overflows, the decoder invalidates its address and waits for a real RS transition or gap before re-pairing nibbles. An overflow, a missed E pulse, an incorrect sample point or electrical ringing can still cause incomplete state. Passive capture cannot request a refresh from the PIC and cannot reconstruct writes made while the ESP was off.

Electrical pins, PC817 outputs, inverted POWER logic and the working HTTP uploader are retained. Keep the device in a trusted LAN/VPN: the existing plain HTTP control/updater is unauthenticated and is not safe to expose directly to the Internet. A fixed main layout improves legibility; it does not repair missing bus data.

## Русский

### Что изменено

Прошивка разделяет три независимые задачи:

1. **Захват, ядро 1:** выборка шести входных сигналов с проверенной задержкой 110 тактов CPU, ожидание спада E и запись полубайта в SPSC-кольцо на 4096 элементов. В горячем участке нет разбора LCD, сетевой работы, `micros()` и блокировки модели. Кольцо занимает 32 КиБ и переживает короткие задержки декодера, но не восстанавливает фронты, которые CPU вообще не заметил.
2. **Модель LCD, ядро 0:** команды HD44780 изменяют 80 байт DDRAM и 64 байта CGRAM. Пауза не сбрасывает выбранную область и адрес. Реализованы Clear Display, оба кода Return Home, включение/выключение дисплея, увеличение/уменьшение адреса, скрытые знакоместа, переходы между строками памяти и сдвиг дисплея. CGRAM сохраняется построчно; используются младшие пять бит, без произвольного тайм-аута блока.
3. **Браузер:** запрос текущего снимка на 98 байт, отрисовка только изменившихся пикселей и следующий запрос. На соединении максимум один незавершённый запрос LCD. Интервал после ответа — 20 мс обычно и 40 мс при удержании кнопки. Это интервалы планирования, а не обещание такой частоты кадров: добавляются задержки VPN и Wi-Fi. После тайм-аута 2 секунды соединение пересоздаётся, а не засоряется повторными запросами.

**Ожидания тишины DDRAM 12 мс и двух одинаковых экранов/картинок CGRAM нет.** Setup не имеет отдельного временного режима парсинга. Ответ, уже находящийся в пути, отображается; после отпускания запрашивается новый снимок, без многократного отбрасывания кадров.

Настройка Wi-Fi выполняется отдельно на ядре 0: захват начинается до ожидания сети. При перезагрузке страницы ANT/AUTO/POWER берутся из маски выходов ESP, а не перезаписываются значениями браузера. Потеря захвата указателя, скрытие страницы и разрыв соединения отпускают моментальные кнопки.

Текст и CGRAM рисуются одним мягким точечным шрифтом 5x8. ASCII-шрифт — собственный типовой LCD-шрифт, **не точная копия ROM** конкретного контроллера; неподдержанные расширенные символы отображаются пустыми. Эффекты курсора/мигания, однострочный/5x10/8-битный режим и чтение R/W не копируются. Расчёт сделан на существующее двухстрочное четырёхбитное подключение только на запись.

### Главный экран и буквальная копия

По умолчанию экран с **MHz, FWD= и REF=** выводится с фиксированными позициями чисел. Распознавание строгое и немедленное, остальные экраны выводятся буквально. Если число в текущем кадре отсутствует/невалидно, поле становится пустым, а не хранит старое показание бесконечно.

Цифра антенны на фиксированном основном экране берётся из маски удалённого ANT; строка IntelliTune и остальные индикаторы остаются живой CGRAM. Это **состояние удалённого выхода**, не отдельное измерение физической кнопки ANT. Управление непосредственно на тюнере может сделать синтетическую цифру неверной.

На распознанной шкале настройки слот 0 выводится полным знакоместом, как было запрошено ранее. Для вывода без фиксации текста, синтетической антенны и подмены шкалы откройте:

`http://<IP-ESP>/?raw=1`

Захват шины и управление кнопками в этом режиме не меняются.

### Сборка и прошивка без Arduino IDE

Скачивайте артефакт успешного запуска **Build firmware** именно для этой ветки/коммита. Внутри `MFJ993B_Remote_Control.ino.bin` — основной образ приложения для OTA. Не используйте старый артефакт другой ветки.

1. Скачайте ZIP-артефакт Actions и распакуйте.
2. Откройте работающую страницу ESP → **Firmware Update (.bin)** либо `http://<IP-ESP>/update`.
3. Выберите `MFJ993B_Remote_Control.ino.bin`, загрузите, не прерывая питание/сеть, и дождитесь перезагрузки ESP.
4. Обновите страницу управления. Если перезапускалась только ESP, после восстановления страницы выключите тюнер через POWER на несколько секунд и включите: это позволяет перехватить полную инициализацию DDRAM/CGRAM.

**Merged-образ, bootloader и partitions через эту страницу не загружать.** Стирание flash не требуется, сохранённый Wi-Fi остаётся в прежнем пространстве `wifi`. После перезагрузки ESP выходы получают прежнюю стартовую маску ANT1/AUTO/POWER ON. Сохраните рабочий основной `.ino.bin` для отката.

Для воспроизводимой локальной сборки установите Python и PlatformIO, затем выполните `pio run` из репозитория. Закреплённые зависимости, классический ESP32, CPU 240 МГц; AsyncTCP работает на ядре 0. Один `.ino` можно собрать в Arduino IDE с прежними библиотеками, но отдельная библиотека AsyncTCP не наследует макрос ядра, объявленный только в скетче. Для проверенной конфигурации привязки ядер используйте CI/PlatformIO.

### Проверка и ограничения

Перед сборкой запускаются тесты реальной модели/кольца C++, извлечённых из скетча, и реального встроенного JavaScript. Проверяются частичные записи, паузы, восстановление после потери полубайта, скрытая DDRAM/адреса/сдвиги, сохранение CGRAM, выключение дисплея, стартовая последовательность, порядок SPSC при параллельной работе, немедленное меню/очистка/CGRAM, синхронизация выходов и отбрасывание старого соединения. Это программная проверка, а не проверка физической шины.

При настоящем переполнении кольца адрес становится неизвестным; повторная сборка полубайтов начинается только после реальной границы — смены RS или паузы. Переполнение, пропущенный E, неверная задержка выборки и электрические переходные процессы всё ещё могут оставить неполный экран. Пассивный перехват не умеет запросить перерисовку у PIC и восстановить записи, сделанные при выключенной ESP.

Пины, PC817, обратная логика POWER и рабочая HTTP-прошивалка сохранены. Держите устройство в доверенной LAN/VPN: прежнее управление и обновление по HTTP не имеют авторизации, публиковать их прямо в Интернет небезопасно. Фиксация главного экрана улучшает читаемость, но не исправляет пропущенные данные шины.

## Primary documentation / Первоисточники

- [Hitachi HD44780U datasheet](https://www.sparkfun.com/datasheets/LCD/HD44780.pdf): DDRAM/CGRAM, entry mode, address counter, display shift and commands.
- [ESP32Async configuration](https://esp32async.github.io/ESPAsyncWebServer/configuration/): AsyncTCP core affinity and asynchronous callback constraints.
- [Arduino-ESP32 Update implementation](https://github.com/espressif/arduino-esp32/blob/master/libraries/Update/src/Updater.cpp): application flash updates.
- [Build workflow](../.github/workflows/build.yml), [pinned PlatformIO environment](../platformio.ini).
