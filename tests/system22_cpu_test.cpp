#include "system22_cpu.h"
#include "system22_types.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
void put16(std::vector<uint8_t>& rom, std::size_t offset, uint16_t value) {
    rom[offset] = static_cast<uint8_t>(value >> 8);
    rom[offset + 1] = static_cast<uint8_t>(value);
}

void put32(std::vector<uint8_t>& rom, std::size_t offset, uint32_t value) {
    put16(rom, offset, static_cast<uint16_t>(value >> 16));
    put16(rom, offset + 2, static_cast<uint16_t>(value));
}

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}
} // namespace

int main() {
    if (!expect(std::fabs(dspfixed_to_nativefloat(0x7fff) - 1.0f) < 0.00001f &&
                std::fabs(dspfixed_to_nativefloat(0xc000) +
                          (16384.0f / 32767.0f)) < 0.00001f,
                "System 22 matrix coefficients must use signed s16/0x7fff"))
        return 1;
    if (!expect(signed24(0x007fffff) == 8388607 &&
                signed24(0x00800000) == -8388608 &&
                signed24(0x00ffffff) == -1,
                "24-bit sign extension must be portable at both boundaries"))
        return 1;

    system22_bus bus;

    bus.set_keycus(0x0172, 0);
    if (!expect(bus.read16(0x20000000) == 0x0172 &&
                bus.read8(0x20000000) == 0x01 &&
                bus.read8(0x20000001) == 0x72,
                "Ridge Racer 2 must receive its C370 KEYCUS ID"))
        return 1;
    bus.set_keycus(0x0173, 3);
    if (!expect(bus.read16(0x20000006) == 0x0173 &&
                bus.read8(0x20000006) == 0x01 &&
                bus.read8(0x20000007) == 0x73,
                "Ace Driver must receive its KEYCUS ID at register 3"))
        return 1;
    bus.set_keycus(0x0185, 1);
    if (!expect(bus.read16(0x20000002) == 0x0185,
                "Cyber Commando must receive its KEYCUS ID at register 1"))
        return 1;
    bus.set_keycus(0x0188, 2);
    if (!expect(bus.read16(0x20000004) == 0x0188,
                "Victory Lap must receive its KEYCUS ID at register 2"))
        return 1;
    bus.clear_keycus();

    int irq_level = -1;
    bus.set_irq_handler([&irq_level](int level) { irq_level = level; });
    if (!expect(irq_level == 0, "IRQ callback must start deasserted")) return 1;
    bus.write8(0x40000004, 0x14); // enable vblank at MC68020 level 4
    bus.signal_vblank();
    if (!expect(irq_level == 4, "vblank must assert its configured IRQ level")) return 1;
    bus.write8(0x40000009, 0x00); // acknowledge vblank source
    if (!expect(irq_level == 0, "vblank acknowledge must clear the IRQ")) return 1;
    bus.set_irq_handler({});

    bus.write32(0x10000000, 0x11223344);
    if (!expect(bus.read32(0x10000000) == 0x11223344,
                "main RAM must be big-endian and writable")) return 1;
    if (!expect(bus.read32(0x18000000) == 0x11223344,
                "main RAM mirror must resolve address bit 27")) return 1;

    if (!expect(bus.read16(0x20020000) == 0x0004,
                "standalone C139 SCI must report its ready status")) return 1;
    bus.write16(0x20020000, 0xffff);
    if (!expect(bus.read16(0x20020000) == 0x0004,
                "standalone C139 control writes must not clear ready status"))
        return 1;

    bus.write32(0x70000004, 0x00abcdef);
    if (!expect(bus.read32(0x70000004) == 0x00abcdef,
                "polygon RAM must be visible on the MC68020 bus")) return 1;
    if (!expect(bus.polygon_ram_data()[1] == 0x00abcdef,
                "polygon device and MC68020 must share storage")) return 1;

    bus.write8(0x60004020, 0x5a);
    if (!expect(bus.read_mcu_shared_byte(0x20) == 0x5a,
                "MC68020 and C74 must share the same MCU RAM bytes")) return 1;
    bus.write_mcu_shared_byte(0x21, 0xa5);
    if (!expect(bus.read8(0x60004021) == 0xa5,
                "C74 shared-RAM writes must be visible to the MC68020")) return 1;

    bus.write16(0x60005000, 0x1234);
    if (!expect(bus.read_c74_shared_byte(0x1000) == 0x34 &&
                bus.read_c74_shared_byte(0x1001) == 0x12,
                "C74 must see shared u16 words in little-endian byte order"))
        return 1;
    bus.write_c74_shared_byte(0x1000, 0xcd);
    bus.write_c74_shared_byte(0x1001, 0xab);
    if (!expect(bus.read16(0x60005000) == 0xabcd,
                "MC68020 must see C74 shared-word writes in big-endian order"))
        return 1;

    if (!expect(bus.read32(0x50000000) == 0xffffffff,
                "all physical DIP switches must default off")) return 1;
    bus.set_dip_switches(0x7ffe); // SW2:1 and SW3:8 on
    if (!expect(bus.read32(0x50000000) == 0x7ffeffff &&
                bus.read8(0x50000000) == 0x7f &&
                bus.read8(0x50000001) == 0xfe,
                "editable SW2/SW3 banks must reach the 68020 DIP port"))
        return 1;
    bus.write16(0x50000008, 0);
    if (!expect(bus.read16(0x50000008) == 1 &&
                bus.read16(0x5000000a) == 1,
                "unconnected serial debug controls must idle high"))
        return 1;

    bus.write8(0x90010003, 0x33);
    bus.write8(0x90020102, 0x44);
    bus.write8(0x90028007, 0x55);
    bus.write8(0x900a0003, 0x66);
    if (!expect(bus.read_cz_byte(3) == 0x33 &&
                bus.read_mixer_byte(0x102) == 0x44 &&
                bus.palette_ram_data()[7] == 0x55 &&
                bus.text_attr_data()[3] == 0x66,
                "video CZ, mixer, palette and text-attribute RAM must be shared"))
        return 1;

    bus.write32(0x9009e004, 0x1234abcd);
    if (!expect(bus.read32(0x9009e004) == 0x1234abcd,
                "text tile RAM must preserve the MC68020 big-endian words"))
        return 1;
    if (!expect(bus.character_ram_data()[0x1e004] == 0x12 &&
                bus.character_ram_data()[0x1e005] == 0x34 &&
                bus.character_ram_data()[0x1e006] == 0xab &&
                bus.character_ram_data()[0x1e007] == 0xcd,
                "text tile writes must mirror the top 8 KiB of character RAM"))
        return 1;

    input_state controls{};
    bus.update_driving_inputs(controls);
    if (!expect(bus.read16(0x60004030) == 0xfeff &&
                bus.read16(0x60004032) == 0x0960 &&
                bus.read16(0x60004034) == 0x0374 &&
                bus.read16(0x60004036) == 0x0329,
                "neutral standard-cabinet switches and calibrated ADCs must reach shared RAM"))
        return 1;

    controls.coin1 = true;
    controls.shift_up = true;
    controls.steering = 0xd80;
    controls.gas = 0x610;
    controls.brake = 0x610;
    bus.update_driving_inputs(controls);
    if (!expect(bus.read16(0x60004030) == 0xeefd &&
                bus.read16(0x60004032) == 0x0ee0 &&
                bus.read16(0x60004034) == 0x0984 &&
                bus.read16(0x60004036) == 0x0939 &&
                bus.read16(0x6000403a) == 0x0100,
                "coin, shifter and full-scale driving ADCs must use Ridge Racer wiring"))
        return 1;
    bus.update_driving_inputs(controls);
    if (!expect(bus.read16(0x6000403a) == 0x0100,
                "holding coin must not add credits repeatedly"))
        return 1;

    input_state rave_controls{};
    bus.set_driving_profile(system22_driving_profile::rave_racer);
    bus.update_driving_inputs(rave_controls);
    if (!expect(bus.read16(0x60004032) == 0x0820 &&
                bus.read16(0x60004034) == 0x03e0 &&
                bus.read16(0x60004036) == 0x0bc0,
                "Rave Racer must use its distinct neutral ADC calibration"))
        return 1;

    input_state ace_controls{};
    bus.set_driving_profile(system22_driving_profile::ace_driver);
    bus.update_game_inputs(ace_controls);
    if (!expect(bus.read16(0x60004030) == 0xffff &&
                bus.read16(0x60004032) == 0x1000 &&
                bus.read16(0x60004034) == 0x03e0 &&
                bus.read16(0x60004036) == 0x0bc0,
                "Ace Driver must use its cabinet flags and ADC calibration"))
        return 1;

    input_state cyber_controls{};
    cyber_controls.shift_down = true;
    cyber_controls.left_stick_x = 0x47;
    cyber_controls.left_stick_y = 0xb7;
    cyber_controls.right_stick_x = 0xb7;
    cyber_controls.right_stick_y = 0x47;
    bus.set_driving_profile(system22_driving_profile::cyber_commando);
    bus.update_game_inputs(cyber_controls);
    if (!expect(bus.read16(0x60004030) == 0xfffe &&
                bus.read16(0x60004032) == 0x0470 &&
                bus.read16(0x60004034) == 0x0b70 &&
                bus.read16(0x60004036) == 0x0b70 &&
                bus.read16(0x60004038) == 0x0470,
                "Cyber Commando must publish dual-stick inputs in cabinet order"))
        return 1;

    std::vector<uint8_t> rom(system22_bus::PROGRAM_ROM_SIZE, 0);
    put32(rom, 0x00, 0x1001fffc); // reset SP
    put32(rom, 0x04, 0x00000100); // reset PC
    put32(rom, 0x70, 0x00000200); // level 4 autovector

    // moveq #42,d0; move.l d0,$10000000.l; andi #$f8ff,sr; bra.s *
    put16(rom, 0x100, 0x702a);
    put16(rom, 0x102, 0x23c0);
    put32(rom, 0x104, 0x10000000);
    put16(rom, 0x108, 0x027c);
    put16(rom, 0x10a, 0xf8ff);
    put16(rom, 0x10c, 0x60fe);

    // Level 4 handler records that it ran, acknowledges vblank, then returns.
    put16(rom, 0x200, 0x7063); // moveq #99,d0
    put16(rom, 0x202, 0x23c0); // move.l d0,$10000004.l
    put32(rom, 0x204, 0x10000004);
    put16(rom, 0x208, 0x13fc); // move.b #0,$40000009.l
    put16(rom, 0x20a, 0x0000);
    put32(rom, 0x20c, 0x40000009);
    put16(rom, 0x210, 0x4e73); // rte

    mc68020_core cpu(bus);
    if (!expect(cpu.load_rom(rom.data(), rom.size()),
                "complete program ROM must load")) return 1;
    if (!expect(cpu.program_counter() == 0x00000100,
                "reset vector must initialize PC")) return 1;
    if (!expect(cpu.stack_pointer() == 0x1001fffc,
                "reset vector must initialize SP")) return 1;

    cpu.execute(200);
    if (!expect(bus.read32(0x10000000) == 42,
                "MC68020 must execute code and write mapped RAM")) return 1;

    bus.write8(0x40000004, 0x14);
    bus.signal_vblank();
    cpu.execute(200);
    if (!expect(bus.read32(0x10000004) == 99,
                "configured vblank must run the level 4 autovector")) return 1;

    std::puts("MC68020/System 22 bus test passed");
    return 0;
}
