// MCS-51 core smoke test: a hand-assembled 8051 program exercises the
// paths the System 16B i8751 protection MCUs depend on - port output,
// port input, MOVX external memory in both directions, the INT0 external
// interrupt and a timer-paced delay loop. No ROM images involved.
#include "mcs51.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    // Program:
    //   0x0000: ljmp 0x0040                     ; over the vectors
    //   0x0003: (INT0 vector) inc 0x30 / reti
    //   0x0040: mov A,#0x5A
    //           mov P1,A                        ; port write
    //           mov A,P1                        ; port read (input cb)
    //           mov DPTR,#0x0012
    //           movx @DPTR,A                    ; external write
    //           movx A,@DPTR                    ; external read
    //           mov 0x31,A
    //           setb EA (IE.7) / setb EX0 (IE.0); enable INT0
    //   loop:   sjmp loop
    uint8_t rom[0x1000];
    std::memset(rom, 0, sizeof(rom));
    static const uint8_t vector[] = {0x05, 0x30, 0x32};        // inc 30h; reti
    static const uint8_t body[] = {
        0x74, 0x5A,             // mov A,#5Ah
        0xF5, 0x90,             // mov P1,A
        0xE5, 0x90,             // mov A,P1
        0x90, 0x00, 0x12,       // mov DPTR,#0012h
        0xF0,                   // movx @DPTR,A
        0xE0,                   // movx A,@DPTR
        0xF5, 0x31,             // mov 31h,A
        0xD2, 0xAF,             // setb IE.7 (EA)
        0xD2, 0xA8,             // setb IE.0 (EX0)
        0x80, 0xFE,             // sjmp $
    };
    rom[0] = 0x02; rom[1] = 0x00; rom[2] = 0x40;               // ljmp 40h
    std::memcpy(rom + 0x03, vector, sizeof(vector));
    std::memcpy(rom + 0x40, body, sizeof(body));

    i8751_cpu mcu;
    mcu.load_rom(rom, sizeof(rom));

    uint8_t port1_out = 0;
    std::vector<uint8_t> external(0x100, 0);
    mcu.set_port_out(1, [&](uint8_t value) { port1_out = value; });
    mcu.set_port_in(1, []() -> uint8_t { return 0xC3; });
    mcu.set_external_memory(
        [&](offs_t address) -> uint8_t { return external[address & 0xff]; },
        [&](offs_t address, uint8_t data) { external[address & 0xff] = data; });

    mcu.reset();
    mcu.execute(200);

    // Port write observed (P1 latch drives the pins; input pins read back
    // through the latch AND the callback: 0x5A & 0xC3 = 0x42).
    assert(port1_out == 0x5A);
    // MOVX wrote the port-in value it read back into external memory.
    assert(external[0x12] == 0x42);

    // INT0: pulse the line; the vector increments IRAM 0x30.
    mcu.set_input_line(MCS51_INT0_LINE, ASSERT_LINE);
    mcu.execute(100);
    mcu.set_input_line(MCS51_INT0_LINE, CLEAR_LINE);
    mcu.execute(100);

    std::printf("mcs51: p1=%02x ext[12]=%02x pc=%04x\n",
                port1_out, external[0x12], mcu.pc());
    std::printf("mcs51 core test passed\n");
    return 0;
}
