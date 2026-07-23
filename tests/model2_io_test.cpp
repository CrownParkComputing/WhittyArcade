#include "sega/model2/model2_bus.h"
#include "test_platform.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>

int main() {
    // Keep the test hermetic while exercising the real save/reload path.
    const std::filesystem::path config_root =
        std::filesystem::temp_directory_path() / "whitty-model2-io-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(config_root, cleanup_error);
    if (!test_set_environment("XDG_CONFIG_HOME", config_root)) return 1;
    model2_roms roms;
    roms.copro_data.assign(0x1000, 0);
    roms.copro_tgp_tables.assign(0x40000, 0);
    const auto store_word = [](std::vector<uint8_t>& region,
                               std::size_t offset, uint32_t value) {
        region[offset + 0] = static_cast<uint8_t>(value);
        region[offset + 1] = static_cast<uint8_t>(value >> 8);
        region[offset + 2] = static_cast<uint8_t>(value >> 16);
        region[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    store_word(roms.copro_tgp_tables, 0, 0x3f800000);
    store_word(roms.copro_data, 0x20 * 4, 0xc3461cee);
    roms.set = model2_rom_set::sega_rally_revision_c;
    model2_bus bus;
    bus.attach(roms);

    // Video-control bit zero is not the 3D render-test flag. It halves only
    // geometry-buffer latching: display refresh and the 2D/HUD layers remain
    // at 60 Hz, with the previous 3D frame retained between even latches.
    assert(bus.geometry_frame_due());
    bus.write32(0x0098000c, 1);
    assert(bus.video_control() == 1);
    bus.vblank();
    assert(!bus.geometry_frame_due());
    bus.vblank();
    assert(bus.geometry_frame_due());
    bus.write32(0x0098000c, 0);
    bus.vblank();
    assert(bus.geometry_frame_due());
    bus.reset();

    // RF3's external-memory view replaces every TGP I/O address, including
    // offsets occupied by arithmetic lookup ports when the view is disabled.
    // Sega Rally reads road vertices through banked offsets 0x20-0x2b; if
    // sin/cos wins the decode there, road collision fails and rivals vanish.
    bus.tgp_io_write(0x20, 0);
    assert(bus.tgp_io_read(0x20) == 0x3f800000); // sin(quarter turn)
    bus.tgp_rf_write(3, 0x800000);
    assert(bus.tgp_io_read(0x20) == 0xc3461cee); // banked course ROM
    bus.write32(0x00900080, 0x4332c7f0);
    bus.tgp_rf_write(3, 0x400000);
    assert(bus.tgp_io_read(0x20) == 0x4332c7f0); // banked buffer RAM
    bus.tgp_io_write(0x20, 0xdeadbeef);
    assert(bus.read32(0x00900080) == 0xdeadbeef);
    bus.tgp_rf_write(3, 0);
    assert(bus.tgp_io_read(0x20) == 0x3f800000); // lookup port restored

    // The TGP data address space has an open hole between its two RAM banks.
    // Treating it as ordinary RAM subtly corrupts persistent scene state.
    bus.tgp_data_write(0x00ff, 0x11223344);
    bus.tgp_data_write(0x0100, 0x55667788);
    bus.tgp_data_write(0x01ff, 0x99aabbcc);
    bus.tgp_data_write(0x0200, 0xddeeff00);
    assert(bus.tgp_data_read(0x00ff) == 0x11223344);
    assert(bus.tgp_data_read(0x0100) == 0);
    assert(bus.tgp_data_read(0x01ff) == 0);
    assert(bus.tgp_data_read(0x0200) == 0xddeeff00);

    // Native i960 dword accesses must be identical to four little-endian
    // byte accesses, including mirrored geometry buffer RAM.
    bus.write32(0x00200040, 0x78563412);
    assert(bus.read32(0x00200040) == 0x78563412);
    assert(bus.read8(0x00200040) == 0x12);
    assert(bus.read8(0x00200043) == 0x78);
    bus.write32(0x00920020, 0xa1b2c3d4);
    assert(bus.read32(0x00900020) == 0xa1b2c3d4);
    // Unaligned accesses retain byte-wise semantics.
    assert(bus.read32(0x00200041) == 0x00785634);

    // The Model 2A texture interface consumes only the low halfword of each
    // i960 dword. Games also use STOS for small live texture changes, so a
    // write ending on lane 1 must reach the rasterizer without a later lane
    // 3 write. The second host dword occupies the upper half of the same
    // packed texture word, and the 2 MiB region is mirrored once.
    const uint64_t texture_generation = bus.texture_generation();
    bus.write8(0x12000000, 0x34);
    bus.write8(0x12000001, 0x12);
    assert(bus.texture_sheet_0()[0] == 0x34);
    assert(bus.texture_sheet_0()[1] == 0x12);
    assert(bus.texture_generation() > texture_generation);
    bus.write32(0x12000004, 0xaabb5678);
    assert(bus.texture_sheet_0()[2] == 0x78);
    assert(bus.texture_sheet_0()[3] == 0x56);
    bus.write8(0x12200000, 0xcd);
    assert(bus.texture_sheet_0()[0] == 0xcd);
    bus.write8(0x12400000, 0xef);
    bus.write8(0x12400001, 0xbe);
    assert(bus.texture_sheet_1()[0] == 0xef);
    assert(bus.texture_sheet_1()[1] == 0xbe);

    // i960 multiword transfers advance only on mappings which advertise
    // BURST. MAME's Model 2 map marks RAM/ROM explicitly, while scalar MMIO
    // such as timers and the TGP result FIFO repeats the same address. Sega
    // Rally relies on this distinction when measuring its render budget.
    assert(bus.access_flags(0x00200040) != 0); // local RAM
    assert(bus.access_flags(0x00500040) != 0); // work RAM
    assert(bus.access_flags(0x00920020) != 0); // mirrored buffer RAM
    assert(bus.access_flags(0x00f00008) == 0); // countdown timer MMIO
    assert(bus.access_flags(0x00884000) == 0); // TGP result FIFO MMIO
    assert(bus.access_flags(0x01c00000) == 0); // 315-5649 I/O MMIO

    constexpr uint32_t io_base = 0x01c00000;
    constexpr uint32_t port_b = io_base + 0x02;
    constexpr uint32_t port_c = io_base + 0x04;
    constexpr uint32_t analog = io_base + 0x1e;

    // Sega Rally uses this open-bus probe to select the on-board 315-5649
    // input path. Mirroring writable storage here selects a legacy external
    // I/O driver and makes Coin, Start and all driving controls inert.
    bus.write8(io_base + 0x202, 0x4d);
    assert(bus.read8(io_base + 0x202) != 0x4d);

    input_state input;
    input.coin1 = true;
    input.start = true;
    bus.set_inputs(input);
    assert(bus.read8(port_b) == 0xbe);
    assert(bus.read8(port_b + 1) == 0x00);
    // The shifter idles in neutral (gearbox code 0), matching MAME's idle
    // inputs; the attract loop reads an engaged gear as player activity.
    assert(bus.read8(port_c) == 0x8f);

    bus.write8(analog, 0);
    assert(bus.read8(analog) == 0x80); // exact ADC midpoint; no wheel drift
    assert(bus.read8(analog) == 0x00); // released accelerator
    assert(bus.read8(analog) == 0x00); // released brake

    input.steering = 0x280;
    bus.set_inputs(input);
    bus.write8(analog, 0);
    assert(bus.read8(analog) == 0x00); // full left endpoint
    input.steering = 0xd80;
    bus.set_inputs(input);
    bus.write8(analog, 0);
    assert(bus.read8(analog) == 0xff); // full right endpoint
    input.steering = 0x800;
    bus.set_inputs(input);

    input.shift_up = true;
    input.gas = 0x610;
    input.brake = 0x610;
    bus.set_inputs(input);
    assert(bus.read8(port_c) == 0xaf); // first gear, code 2
    bus.set_inputs(input);             // held button does not shift twice
    assert(bus.read8(port_c) == 0xaf);
    bus.write8(analog, 1);
    assert(bus.read8(analog) == 0xff);

    // PA0 switches PB4-PB7 to the 93C46 serial pins. Coin remains visible,
    // while Start is deliberately masked until EEPROM access finishes.
    bus.write8(io_base, 0x01);
    assert(bus.read8(port_b) == 0xfe);
    bus.write8(io_base, 0x00);
    assert(bus.read8(port_b) == 0xbe);
    assert(bus.read8(analog) == 0xff);

    // An absent/disabled communication board reads as pulled-up TTL lines.
    // Returning zero here makes Sega Rally believe a half-initialized linked
    // cabinet is attached and it refuses normal standalone coin/start input.
    assert(bus.read8(0x01a04000) == 0xfe);
    assert(bus.read8(0x01a04002) == 0xfe);
    if (!test_unset_environment("MODEL2_COMM_NODE")) return 1;
    if (!test_unset_environment("MODEL2_COMM_LINK")) return 1;
    bus.write8(0x01a04000, 1);
    assert(bus.read8(0x01a04000) == 0xff);
    assert(bus.read8(0x01a00000) == 0x00); // peer search pending
    assert(bus.read8(0x01a00001) == 0x02); // EPR-16726 pending/slave byte
    assert(bus.read8(0x01a00002) == 0xff); // no assigned node id
    assert(bus.read8(0x01a00003) == 0xff); // no detected node count
    bus.write8(0x01a04000, 0);

    // MAME's default local/remote socket pair links back to itself. The
    // board remains pending for its 232-tick discovery period, then reports
    // a one-node ring even when the game's cabinet setting is NOTLINK.
    if (!test_set_environment("MODEL2_COMM_LINK", "1")) return 1;
    bus.write8(0x01a04000, 1);
    bus.write8(0x01a04002, 1);
    for (unsigned tick = 0; tick < 0x00e7; ++tick) bus.vblank();
    assert(bus.read8(0x01a00000) == 0x00);
    bus.vblank();
    assert(bus.read8(0x01a00000) == 0x01);
    assert(bus.read8(0x01a00002) == 0x01);
    assert(bus.read8(0x01a00003) == 0x01);
    assert(bus.read8(0x01a04002) == 0xff);
    // TX is received one communication tick later, like the socket-backed
    // board, rather than copied from a partially updated current frame.
    bus.write8(0x01a02000, 0x5a);
    bus.write8(0x01a02001, 0xc3);
    bus.vblank();
    bus.vblank();
    assert(bus.read8(0x01a021c0) == 0x5a);
    assert(bus.read8(0x01a021c1) == 0xc3);
    bus.write8(0x01a04000, 0);
    if (!test_unset_environment("MODEL2_COMM_LINK")) return 1;
    assert(bus.read8(0x01a04000) == 0xfe);
    assert(bus.read8(0x01a04002) == 0xfe);

    // Paired WhittyArcade processes use two localhost UDP endpoints. Exercise
    // the same transport in-process: each cabinet receives the other
    // cabinet's complete preceding communication frame and reports a
    // two-node ring.
    model2_bus peer1;
    if (!test_set_environment("MODEL2_COMM_NODE", "1")) return 1;
    if (!test_set_environment("MODEL2_COMM_LOCAL_PORT", "35121")) return 1;
    if (!test_set_environment("MODEL2_COMM_PEER_PORT", "35122")) return 1;
    peer1.attach(roms);
    model2_bus peer2;
    if (!test_set_environment("MODEL2_COMM_NODE", "2")) return 1;
    if (!test_set_environment("MODEL2_COMM_LOCAL_PORT", "35122")) return 1;
    if (!test_set_environment("MODEL2_COMM_PEER_PORT", "35121")) return 1;
    peer2.attach(roms);
    // Paired launch powers both boards for discovery, but the game's CN
    // reset must still take effect. Sega Rally resets and then enables the
    // board after reading its CAR1/CAR2 operator setting.
    assert(peer1.read8(0x01a04000) == 0xff);
    assert(peer2.read8(0x01a04000) == 0xff);
    peer1.write8(0x01a04000, 0);
    peer2.write8(0x01a04000, 0);
    assert(peer1.read8(0x01a04000) == 0xfe);
    assert(peer2.read8(0x01a04000) == 0xfe);
    if (!test_set_environment("MODEL2_COMM_NODE", "1")) return 1;
    if (!test_set_environment("MODEL2_COMM_LOCAL_PORT", "35121")) return 1;
    if (!test_set_environment("MODEL2_COMM_PEER_PORT", "35122")) return 1;
    peer1.write8(0x01a04000, 1);
    if (!test_set_environment("MODEL2_COMM_NODE", "2")) return 1;
    if (!test_set_environment("MODEL2_COMM_LOCAL_PORT", "35122")) return 1;
    if (!test_set_environment("MODEL2_COMM_PEER_PORT", "35121")) return 1;
    peer2.write8(0x01a04000, 1);
    peer1.write8(0x01a02000, 0x11);
    peer2.write8(0x01a02000, 0x22);
    for (unsigned tick = 0; tick < 4; ++tick) {
        peer1.vblank();
        peer2.vblank();
    }
    assert(peer1.read8(0x01a00000) == 0x01);
    assert(peer2.read8(0x01a00000) == 0x01);
    assert(peer1.read8(0x01a00002) == 0x01);
    assert(peer2.read8(0x01a00002) == 0x02);
    assert(peer1.read8(0x01a00003) == 0x02);
    assert(peer2.read8(0x01a00003) == 0x02);
    assert(peer1.read8(0x01a021c0) == 0x22);
    assert(peer2.read8(0x01a021c0) == 0x11);
    if (!test_unset_environment("MODEL2_COMM_NODE")) return 1;
    if (!test_unset_environment("MODEL2_COMM_LOCAL_PORT")) return 1;
    if (!test_unset_environment("MODEL2_COMM_PEER_PORT")) return 1;
    peer1.write8(0x01a04000, 0);
    peer2.write8(0x01a04000, 0);

    // Network-cabinet mode uses the same packet protocol on two physical
    // computers. LAN broadcast removes the need to type the peer's address;
    // each role listens on its own fixed port and ignores its own node ID.
    {
        model2_bus network1;
        if (!test_set_environment("MODEL2_COMM_NETWORK", "1")) return 1;
        if (!test_set_environment("MODEL2_COMM_NODE", "1")) return 1;
        if (!test_set_environment("MODEL2_COMM_LOCAL_PORT", "35123")) return 1;
        if (!test_set_environment("MODEL2_COMM_PEER_PORT", "35124")) return 1;
        network1.attach(roms);
        model2_bus network2;
        if (!test_set_environment("MODEL2_COMM_NODE", "2")) return 1;
        if (!test_set_environment("MODEL2_COMM_LOCAL_PORT", "35124")) return 1;
        if (!test_set_environment("MODEL2_COMM_PEER_PORT", "35123")) return 1;
        network2.attach(roms);
        for (unsigned tick = 0; tick < 4; ++tick) {
            network1.vblank();
            network2.vblank();
        }
        assert(network1.read8(0x01a00000) == 0x01);
        assert(network2.read8(0x01a00000) == 0x01);
        if (!test_unset_environment("MODEL2_COMM_NETWORK")) return 1;
        if (!test_unset_environment("MODEL2_COMM_NODE")) return 1;
        if (!test_unset_environment("MODEL2_COMM_LOCAL_PORT")) return 1;
        if (!test_unset_environment("MODEL2_COMM_PEER_PORT")) return 1;
    }

    // The main CPU's 8251 UART must generate a fresh interrupt after every
    // transmitted byte. Sega Rally queues one MIDI byte per TXRDY edge.
    unsigned midi_bytes = 0;
    bus.set_sound_uart_callback([&midi_bytes](uint8_t) { ++midi_bytes; });
    bus.write8(0x00e80005, 0x04); // enable interrupt source 10
    assert((bus.read8(0x00e80001) & 0x04) != 0);
    bus.write8(0x01c80000, 0x90);
    assert(midi_bytes == 0); // byte is still on the serial wire
    // An idle 8251 moves the byte straight into its shift register and
    // reasserts TXRDY immediately; TXEMPTY remains low until serialization.
    assert(bus.read8(0x01c80002) == 0x81);
    assert((bus.read8(0x00e80001) & 0x04) != 0);
    bus.tick(7999);
    assert(bus.read8(0x01c80002) == 0x81);
    assert(midi_bytes == 0);
    bus.tick(1);
    assert(midi_bytes == 1);
    assert(bus.read8(0x01c80002) == 0x85); // DSR | TXRDY | TXEMPTY

    // TXRDY permits one byte to wait behind the active shift register, but
    // that second byte must not be delivered early or claim TXEMPTY.
    bus.write8(0x01c80000, 0x91);
    assert(bus.read8(0x01c80002) == 0x81);
    bus.write8(0x01c80000, 0x92);
    assert(bus.read8(0x01c80002) == 0x80);
    bus.tick(8000);
    assert(midi_bytes == 2);
    assert(bus.read8(0x01c80002) == 0x81);
    bus.write8(0x01c80000, 0x93);
    assert(bus.read8(0x01c80002) == 0x80);
    bus.tick(8000);
    assert(midi_bytes == 3);
    assert(bus.read8(0x01c80002) == 0x81);
    bus.tick(8000);
    assert(midi_bytes == 4);
    assert(bus.read8(0x01c80002) == 0x85);

    // Model 2 runs the i960 in roughly 6510-cycle scheduler slices.  A byte
    // boundary inside a slice must carry the unused cycles into the next
    // character instead of quantizing every character to two whole slices.
    bus.write8(0x01c80000, 0xa1);
    assert(bus.read8(0x01c80002) == 0x81);
    bus.write8(0x01c80000, 0xa2);
    bus.tick(6510);
    assert(midi_bytes == 4);
    assert(bus.read8(0x01c80002) == 0x80);
    bus.tick(1490);
    assert(midi_bytes == 5);
    assert(bus.read8(0x01c80002) == 0x81);
    bus.write8(0x01c80000, 0xa3);
    bus.tick(6510);
    assert(midi_bytes == 5);
    bus.tick(1490);
    assert(midi_bytes == 6);
    assert(bus.read8(0x01c80002) == 0x81);
    bus.tick(8000);
    assert(midi_bytes == 7);
    assert(bus.read8(0x01c80002) == 0x85);

    // Read factory word zero from the 93C46 (start=1, opcode=10,
    // address=000000). Port A carries DI/CS/CLK; port B bit 5 carries DO.
    const auto eeprom_lines = [](model2_bus& target, bool chip, bool clock,
                                 bool data) {
        target.write8(io_base, static_cast<uint8_t>(0x01 |
            (data ? 0x20 : 0) | (chip ? 0x40 : 0) |
            (clock ? 0x80 : 0)));
    };
    const auto send_eeprom_bits = [&](model2_bus& target, uint32_t value,
                                      int bits) {
        for (int bit = bits - 1; bit >= 0; --bit) {
            const bool data = (value >> bit) & 1;
            eeprom_lines(target, true, false, data);
            eeprom_lines(target, true, true, data);
            eeprom_lines(target, true, false, data);
        }
    };
    const auto read_eeprom_word = [&](model2_bus& target,
                                      uint8_t address) {
        eeprom_lines(target, false, false, false);
        eeprom_lines(target, true, false, false);
        send_eeprom_bits(target, 0x180 | address, 9);
        // The READ command is followed by a dummy zero. Data bit 15 appears
        // only after the next rising edge, matching the real 93C46/MAME.
        assert((target.read8(port_b) & 0x20) == 0);
        uint16_t result = 0;
        for (int bit = 15; bit >= 0; --bit) {
            eeprom_lines(target, true, true, false);
            eeprom_lines(target, true, false, false);
            if (target.read8(port_b) & 0x20)
                result |= uint16_t{1} << bit;
        }
        eeprom_lines(target, false, false, false);
        return result;
    };
    const uint16_t factory = read_eeprom_word(bus, 0);
    assert(factory == 0xeada);
    assert(bus.srally_link_type() == 0);
    assert(bus.set_srally_link_type(1));
    assert(bus.srally_link_type() == 1);
    assert(bus.set_srally_link_type(2));
    assert(bus.srally_link_type() == 2);
    assert(bus.set_srally_link_type(0));
    assert(bus.srally_link_type() == 0);

    // Unlock, write one complete x16 word, let the periodic NVRAM flush run,
    // and prove that a new board instance reads exactly the same bits back.
    eeprom_lines(bus, false, false, false);
    eeprom_lines(bus, true, false, false);
    send_eeprom_bits(bus, 0x130, 9); // EWEN: opcode 00, address prefix 11
    eeprom_lines(bus, false, false, false);
    eeprom_lines(bus, true, false, false);
    send_eeprom_bits(bus, 0x145, 9); // WRITE word 5
    send_eeprom_bits(bus, 0x5aa5, 16);
    eeprom_lines(bus, false, false, false);
    for (int frame = 0; frame < 60; ++frame) bus.vblank();

    model2_bus reloaded;
    reloaded.attach(roms);
    assert(read_eeprom_word(reloaded, 5) == 0x5aa5);
    return 0;
}
