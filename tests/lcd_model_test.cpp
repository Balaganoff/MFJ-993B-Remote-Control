#include "lcd_core_under_test.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <iostream>

static void byte(NibbleDecoder &decoder, LcdModel &lcd, uint8_t value, bool rs = false, bool gap = false) {
    decoder.accept((value >> 4) | (rs ? SAMPLE_RS : 0) | (gap ? SAMPLE_GAP : 0), lcd);
    decoder.accept((value & 15) | (rs ? SAMPLE_RS : 0), lcd);
}

int main() {
    LcdModel lcd; lcd.reset(); NibbleDecoder decoder; decoder.reset();
    uint8_t screen[32], cgram[64];

    // Direct addressing, partial text writes, and a long silent bus.
    byte(decoder,lcd,0x80); byte(decoder,lcd,'A',true);
    byte(decoder,lcd,'B',true,true);
    lcd.snapshot(screen,cgram); assert(screen[0]=='A' && screen[1]=='B');

    // An abandoned half-byte must not move the stored DDRAM cursor.
    decoder.accept(0x0F | SAMPLE_RS,lcd);
    byte(decoder,lcd,'C',true,true);
    lcd.snapshot(screen,cgram); assert(screen[2]=='C');

    // Hidden DDRAM is preserved and address 0x27 wraps to 0x40.
    byte(decoder,lcd,0x90); byte(decoder,lcd,'H',true);
    byte(decoder,lcd,0xA7); byte(decoder,lcd,'X',true); byte(decoder,lcd,'Y',true);
    lcd.snapshot(screen,cgram); assert(screen[16]=='Y');
    byte(decoder,lcd,0x18);
    lcd.snapshot(screen,cgram); assert(screen[15]=='H');
    byte(decoder,lcd,0x03);
    lcd.snapshot(screen,cgram); assert(screen[0]=='A');

    // 0x67 wraps to 0; decrement at 0 goes to 0x67, then 0x66.
    byte(decoder,lcd,0xE7); byte(decoder,lcd,'W',true); byte(decoder,lcd,'Z',true);
    lcd.snapshot(screen,cgram); assert(screen[0]=='Z');
    byte(decoder,lcd,0x04); byte(decoder,lcd,0x80);
    byte(decoder,lcd,'D',true); byte(decoder,lcd,'E',true);
    byte(decoder,lcd,0x1C);
    lcd.snapshot(screen,cgram); assert(screen[16]=='E');
    byte(decoder,lcd,0x02); byte(decoder,lcd,0x06);

    // CGRAM writes may be sparse and paused; only five pixel bits matter.
    byte(decoder,lcd,0x68); byte(decoder,lcd,0xFF,true);
    byte(decoder,lcd,0x02,true,true);
    byte(decoder,lcd,0x6E); byte(decoder,lcd,0x05,true);
    byte(decoder,lcd,0x01); // Clear resets DDRAM but not CGRAM.
    lcd.snapshot(screen,cgram);
    assert(cgram[40]==31 && cgram[41]==2 && cgram[46]==5 && cgram[42]==0);
    for (uint8_t ch: screen) assert(ch==' ');

    // Entry-mode display shift and Display Off/On.
    byte(decoder,lcd,0x80); byte(decoder,lcd,'K',true);
    byte(decoder,lcd,0x07); byte(decoder,lcd,'L',true);
    lcd.snapshot(screen,cgram); assert(screen[0]=='L');
    byte(decoder,lcd,0x08);
    lcd.snapshot(screen,cgram); for (uint8_t ch: screen) assert(ch==' ');
    byte(decoder,lcd,0x0C); byte(decoder,lcd,0x02); byte(decoder,lcd,0x06);
    lcd.snapshot(screen,cgram); assert(screen[0]=='K' && screen[1]=='L');

    // Observed overflow quarantines data until a real byte boundary.
    byte(decoder,lcd,'K',true);
    decoder.accept(SAMPLE_RS | SAMPLE_LOSS | 4,lcd);
    decoder.accept(SAMPLE_RS | 5,lcd);
    decoder.accept(SAMPLE_RS | 6,lcd);
    byte(decoder,lcd,0xC0); byte(decoder,lcd,'Q',true);
    lcd.snapshot(screen,cgram); assert(screen[16]=='Q' && screen[2]==' ');

    // Startup's single-nibble 8-bit preamble must not poison 4-bit pairing.
    lcd.reset(); decoder.reset();
    decoder.accept(3|SAMPLE_GAP,lcd); decoder.accept(3|SAMPLE_GAP,lcd);
    decoder.accept(3,lcd); decoder.accept(2,lcd);
    byte(decoder,lcd,0x28); byte(decoder,lcd,0x80); byte(decoder,lcd,'R',true);
    lcd.snapshot(screen,cgram); assert(screen[0]=='R');

    // FIFO ordering, full detection, and concurrent SPSC visibility.
    CaptureRing ring; CaptureSample sample{};
    for (uint32_t i=0;i<CaptureRing::Capacity;i++) assert(ring.push({i,i}));
    assert(!ring.push({99,99}));
    for (uint32_t i=0;i<CaptureRing::Capacity;i++) {
        assert(ring.pop(sample)); assert(sample.cycles==i && sample.payload==i);
    }
    assert(!ring.pop(sample));
    constexpr uint32_t total=250000;
    std::thread producer([&] {
        for (uint32_t i=0;i<total;i++)
            while (!ring.push({i,i^0x12345678})) std::this_thread::yield();
    });
    for (uint32_t i=0;i<total;i++) {
        while (!ring.pop(sample)) std::this_thread::yield();
        assert(sample.cycles==i && sample.payload==(i^0x12345678));
    }
    producer.join();
    std::cout << "LCD model, nibble recovery and SPSC stress tests passed\n";
}
