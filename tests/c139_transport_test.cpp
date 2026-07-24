// c139_transport_test.cpp - Unit tests for the System 22 cabinet-to-cabinet
// C139 link transport.
//
// Two layers of coverage:
//   1. The wire format (encode + decode) round-trips a frame, rejects
//      tampered packets, and surfaces the same magic 'WAR2' / sequence
//      counter / word-count / big-endian word layout that the original
//      inline implementation produced.
//   2. The full transport (two system22_c139_transport instances on
//      real UDP loopback) sends a frame from cabinet 1, gets it
//      delivered to cabinet 2's bus, with the bit-9 sync mark set
//      on the last word — matching the C139 receiver's expected
//      format in system22_cpu.cpp's receive_c139_frame.
//
// All tests run without spinning up the full emulator; the only
// dependency on the System 22 bus is through system22_bus's
// set_c139_link / take_c139_transmit_frame / receive_c139_frame API.

#include "namco/system22/system22_cpu.h"
#include "namco/system22/system22_c139_transport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

int expect(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
        return 1;
    }
    return 0;
}

int test_encode_decode_round_trip() {
    std::printf("-- test_encode_decode_round_trip\n");
    using system22_c139::encode;
    using system22_c139::decode;

    // Empty frame — still a valid header-only packet.
    {
        const auto bytes = encode(/*node=*/1, /*sequence=*/42,
                                  /*frame_words=*/{});
        if (expect(bytes.size() == system22_c139::packet_header_bytes,
                   "empty-frame packet must be header-only")) return 1;
        if (expect(bytes[0] == 'W' && bytes[1] == 'A' &&
                   bytes[2] == 'R' && bytes[3] == '2',
                   "magic must be 'WAR2'")) return 1;
        if (expect(bytes[4] == system22_c139::protocol_version,
                   "protocol version byte must be 1")) return 1;
        if (expect(bytes[5] == 1, "node id must round-trip")) return 1;
        if (expect(bytes[6] == 0, "Has-TX flag must be 0 for empty frame"))
            return 1;
        std::uint8_t node = 0;
        std::uint32_t sequence = 0;
        std::vector<std::uint16_t> frame;
        if (expect(decode(bytes.data(), bytes.size(), node, sequence, frame),
                   "empty frame must decode cleanly")) return 1;
        if (expect(node == 1 && sequence == 42 && frame.empty(),
                   "decoded empty frame must match")) return 1;
    }

    // Non-empty frame — round-trips with correct byte order.
    {
        const std::vector<std::uint16_t> original = {0x0011, 0x0022, 0x0033};
        const auto bytes = encode(2, 7, original);
        if (expect(bytes.size() == system22_c139::packet_header_bytes +
                                     original.size() * 2,
                   "packet size = header + 2*word_count")) return 1;
        if (expect(bytes[5] == 2, "node byte")) return 1;
        if (expect(bytes[6] == 1, "Has-TX flag must be 1")) return 1;
        // Each word is big-endian in the payload.
        if (expect(bytes[system22_c139::packet_header_bytes] == 0x00 &&
                   bytes[system22_c139::packet_header_bytes + 1] == 0x11,
                   "first word 0x0011 must encode as 0x00 0x11")) return 1;
        if (expect(bytes[system22_c139::packet_header_bytes + 2] == 0x00 &&
                   bytes[system22_c139::packet_header_bytes + 3] == 0x22,
                   "second word 0x0022 must encode as 0x00 0x22")) return 1;
        std::uint8_t node = 0;
        std::uint32_t sequence = 0;
        std::vector<std::uint16_t> frame;
        if (expect(decode(bytes.data(), bytes.size(), node, sequence, frame),
                   "non-empty frame must decode cleanly")) return 1;
        if (expect(node == 2 && sequence == 7,
                   "header fields must round-trip")) return 1;
        if (expect(frame == original,
                   "decoded word stream must match the original")) return 1;
    }

    // Sequence wraparound: large sequences must not corrupt the
    // little-endian u32 encoding.
    {
        const std::vector<std::uint16_t> frame = {0xbeef, 0xface};
        const auto bytes = encode(1, 0xDEADBEEFu, frame);
        std::uint8_t node = 0;
        std::uint32_t sequence = 0;
        std::vector<std::uint16_t> decoded;
        if (expect(decode(bytes.data(), bytes.size(), node, sequence, decoded),
                   "wrapped sequence must decode")) return 1;
        if (expect(sequence == 0xDEADBEEFu,
                   "sequence must round-trip through wraparound")) return 1;
    }
    return 0;
}

int test_decode_rejects_bad_packets() {
    std::printf("-- test_decode_rejects_bad_packets\n");
    using system22_c139::encode;
    using system22_c139::decode;

    auto good = encode(1, 1, {0x0001});
    // Magic is corrupted.
    {
        auto bad = good;
        bad[0] = 'X';
        std::uint8_t node;
        std::uint32_t sequence;
        std::vector<std::uint16_t> frame;
        if (expect(!decode(bad.data(), bad.size(), node, sequence, frame),
                   "decode must reject wrong magic")) return 1;
    }
    // Protocol version is wrong.
    {
        auto bad = good;
        bad[4] = 99;
        std::uint8_t node;
        std::uint32_t sequence;
        std::vector<std::uint16_t> frame;
        if (expect(!decode(bad.data(), bad.size(), node, sequence, frame),
                   "decode must reject wrong protocol version")) return 1;
    }
    // Node id is invalid (not 1 or 2).
    {
        auto bad = good;
        bad[5] = 3;
        std::uint8_t node;
        std::uint32_t sequence;
        std::vector<std::uint16_t> frame;
        if (expect(!decode(bad.data(), bad.size(), node, sequence, frame),
                   "decode must reject node id 3")) return 1;
    }
    // Truncated header.
    {
        std::uint8_t node;
        std::uint32_t sequence;
        std::vector<std::uint16_t> frame;
        if (expect(!decode(good.data(), 8, node, sequence, frame),
                   "decode must reject packets shorter than the header"))
            return 1;
    }
    // Truncated payload (header says 2 words but only 1.5).
    {
        auto bad = good;
        bad.resize(system22_c139::packet_header_bytes +
                   system22_c139::max_frame_words * 2);
        // Word count says 1 but we send 0 bytes of payload.
        std::uint8_t node;
        std::uint32_t sequence;
        std::vector<std::uint16_t> frame;
        if (expect(!decode(bad.data(), system22_c139::packet_header_bytes,
                            node, sequence, frame),
                   "decode must reject header-only packet when word "
                   "count says 1")) return 1;
    }
    return 0;
}

int test_loopback_exchange() {
    std::printf("-- test_loopback_exchange\n");
    system22_bus cabinet1;
    system22_bus cabinet2;
    cabinet1.set_c139_link(true, 1);
    cabinet2.set_c139_link(true, 2);

    // Two transports on real UDP loopback. Ports far above 1024 to avoid
    // colliding with system services; both directions are bound to
    // 127.0.0.1 only (default for SYSTEM22_C139_NETWORK unset).
    system22_c139_transport a;
    system22_c139_transport b;
    a.initialize(cabinet1, /*env=*/nullptr, /*forced_node=*/1);
    b.initialize(cabinet2, /*env=*/nullptr, /*forced_node=*/2);
    if (expect(a.enabled() && b.enabled(),
               "both transports must be enabled after forced_node init"))
        return 1;

    // Drive a known frame from cabinet 1's TX FIFO by writing to the
    // bus's 68020-side register file. The bus exposes the TX FIFO at
    // 0x20010000; writes there populate take_c139_transmit_frame().
    cabinet1.write16(0x20020004, 0x0001);     // control: link on, transmit
    cabinet1.write16(0x20010000, 0x0011);     // word 0
    cabinet1.write16(0x20010002, 0x0022);     // word 1
    cabinet1.write16(0x20010004, 0x0033);     // word 2
    cabinet1.write16(0x2002000a, 0x0003);     // payload length = 3
    cabinet1.write16(0x20020004, 0x0003);     // start transmit

    // Run exchanges for several "frames" — the datagram is non-blocking
    // so the receiver needs at least one call to drain its socket.
    for (int round = 0; round < 5; ++round) {
        a.exchange(cabinet1);
        b.exchange(cabinet2);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (expect(a.linked() && b.linked(),
               "both transports must be linked after exchange"))
        return 1;

    // cabinet 2 must have received the frame with the bit-9 sync mark
    // on the last word (per system22_cpu.cpp's receive_c139_frame).
    if (expect((cabinet2.read16(0x20020000) & 0x0002) != 0,
               "cabinet 2 must have set the RX-ready status bit")) return 1;
    if (expect(cabinet2.read16(0x2002000c) == 3,
               "cabinet 2 RX FIFO must have word_count = 3")) return 1;
    if (expect(cabinet2.read16(0x20012000) == 0x0011 &&
               cabinet2.read16(0x20012002) == 0x0022 &&
               cabinet2.read16(0x20012004) == 0x0133,
               "cabinet 2 RX FIFO must contain the sent words "
               "(with bit-9 sync on the last)")) return 1;

    // Reverse direction: cabinet 2 → cabinet 1. The RX FIFO pointer
    // advanced after the first frame, so the new words land at
    // 0x20012006 / 0x20012008.
    cabinet2.write16(0x20020004, 0x0001);
    cabinet2.write16(0x20010000, 0x0a0a);
    cabinet2.write16(0x20010002, 0x0b0b);
    cabinet2.write16(0x2002000a, 0x0002);
    cabinet2.write16(0x20020004, 0x0003);
    for (int round = 0; round < 5; ++round) {
        a.exchange(cabinet1);
        b.exchange(cabinet2);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // system22_bus::receive_c139_frame takes only the low byte of each
    // u16 word (the upper byte is reserved for the per-frame sync mark
    // which the bus sets itself on the last word). 0x0a0a -> (hi=0x00,
    // lo=0x0a); 0x0b0b + sync -> (hi=0x01, lo=0x0b). The pointer is
    // still at 0 after a single 2-word frame because the previous
    // round's pointer state on cabinet 1 was fresh (no earlier
    // received frame advanced it).
    if (expect(cabinet1.read16(0x20012000) == 0x000a &&
               cabinet1.read16(0x20012002) == 0x010b,
               "cabinet 1 RX FIFO must contain the second frame's low "
               "bytes (with bit-9 sync on the last)")) return 1;
    return 0;
}

} // namespace

int main() {
    int rc = 0;
    rc |= test_encode_decode_round_trip();
    rc |= test_decode_rejects_bad_packets();
    rc |= test_loopback_exchange();
    if (rc == 0 && failures == 0) {
        std::printf("c139_transport_test passed\n");
        return 0;
    }
    std::printf("c139_transport_test FAILED with %d failures\n", failures);
    return 1;
}
