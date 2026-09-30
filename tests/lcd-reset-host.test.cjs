const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(
  root,
  'firmware/MFJ993B_Remote_Control/MFJ993B_Remote_Control.ino'
), 'utf8');

assert.doesNotMatch(
  source,
  /sampleLate|lateSampleCounter/,
  'short valid E pulses must not be rejected after the delayed bus sample'
);
assert.match(
  source,
  /if \(sampleChanged\)[\s\S]*?markDecoderDesynchronized\([\s\S]*?return;/,
  'mismatched bus samples must desynchronize and return before decoding'
);
assert.match(source, /"\/status"/);
assert.match(source, /FW:%s/);

const config = source.slice(
  source.indexOf('const uint8_t BTN_PINS'),
  source.indexOf('// WEB / WIFI /')
);
const declarations = source.slice(
  source.indexOf('enum LcdAddressSpace'),
  source.indexOf('// ОСНОВНАЯ WEB-СТРАНИЦА')
);
const decoder = source.slice(
  source.indexOf('static inline uint8_t readNibble'),
  source.indexOf('// PUSH LCD — ЯДРО 0')
);

const shim = String.raw`
#include <cstdint>
#include <cstring>
#include <cassert>
#include <iostream>
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
const int HIGH = 1;
const int LOW = 0;
int levels[40] = {};
uint32_t fakeMicros = 1000;
uint32_t micros() { return ++fakeMicros; }
void digitalWrite(int pin, int level) { levels[pin] = level; }
`;

const cases = String.raw`
int main()
{
    volatile uint16_t beforeMask = buttonMask;
    decoderNeedsRsBoundary = false;
    observedRsValid = true;
    lastRs = true;
    observedRs = true;
    lastBusTime = 500;

    lcdCgram[3][4] = 0x15;
    lcdCgramKnownRows[3] = 1U << 4;
    processCommand(0x80, 600);
    processData('X', 601);
    assert(lcdScreen[0] == 'X');

    requestLcdCaptureReset(false, true);
    serviceLcdCaptureReset();

    assert(buttonMask == beforeMask);
    assert(levels[32] == 0);
    assert(lcdScreen[0] == ' ');
    assert(lcdCgram[3][4] == 0x15);
    assert(lcdCgramKnownRows[3] == (1U << 4));
    assert(lcdResetAckPending);
    assert(decoderNeedsRsBoundary);
    assert(observedRsValid && observedRs);

    processCapturedNibble(0x8, false, 700);
    processCapturedNibble(0x0, false, 701);
    assert(lcdAddressSpace == LCD_SPACE_DDRAM && lcdAddress == 0);
    processCapturedNibble(0x4, true, 702);
    processCapturedNibble(0x1, true, 703);
    assert(lcdScreen[0] == 'A');

    markDecoderDesynchronized(true, true);
    processCapturedNibble(0x4, true, 800);
    processCapturedNibble(0x2, true, 801);
    assert(lcdScreen[0] == 'A');
    processCapturedNibble(0x8, false, 802);
    processCapturedNibble(0x0, false, 803);
    processCapturedNibble(0x4, true, 804);
    processCapturedNibble(0x2, true, 805);
    assert(lcdScreen[0] == 'B');

    requestLcdCaptureReset(true, false);
    serviceLcdCaptureReset();
    assert(lcdCgram[3][4] == 0);
    assert(lcdCgramKnownRows[3] == 0);

    std::cout << "LCD reset/resync host tests: PASS\n";
}
`;

const temp = spawnSync('mktemp', ['-d'], {encoding: 'utf8'});
assert.equal(temp.status, 0);
const executable = path.join(temp.stdout.trim(), 'lcd-reset-tests');
const cpp = shim + '\n' + config +
  '\nvolatile uint16_t buttonMask = INITIAL_BUTTON_MASK;\n' +
  declarations + '\n' + decoder + '\n' + cases;
const built = spawnSync('g++', [
  '-std=c++11', '-pthread', '-Wall', '-Wextra', '-Werror',
  '-Wno-unused-parameter', '-x', 'c++', '-', '-o', executable
], {input: cpp, encoding: 'utf8'});
if (built.status !== 0) throw new Error(built.stderr);
const tested = spawnSync(executable, [], {encoding: 'utf8'});
process.stdout.write(tested.stdout);
if (tested.status !== 0) throw new Error(tested.stderr);
