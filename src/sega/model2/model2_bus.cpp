#include "sega/model2/model2_bus.h"
#include "sega/model1/model1_io_board.h"
#include "mb86233_native.h"
#include "platform_paths.h"
#include "wall_log.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr uint16_t i960_burst = 0x0001;
constexpr std::size_t comm_frame_start = 0x2000;
constexpr std::size_t comm_frame_size = 0x0e00;
constexpr std::size_t comm_frame_receive =
    comm_frame_start + 0x01c0;
constexpr std::size_t comm_packet_header = 16;
constexpr std::array<uint8_t, 4> comm_packet_magic{{'W', 'A', 'M', '2'}};

uint16_t environment_port(const char* name, uint16_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed > 0 && parsed <= 65535 ?
        static_cast<uint16_t>(parsed) : fallback;
}

int environment_comm_node() {
    const char* node = std::getenv("MODEL2_COMM_NODE");
    const int node_id = node ? std::atoi(node) : 0;
    // Ring position 1..8 - Daytona's hardware maximum. Node 1 is the
    // ring master; anything else runs as a slave car.
    return node_id >= 1 && node_id <= 8 ? node_id : 0;
}

int environment_comm_count() {
    const char* count = std::getenv("MODEL2_COMM_COUNT");
    const int cabinets = count ? std::atoi(count) : 2;
    return std::clamp(cabinets, 2, 8);
}

void write_packet_u32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t read_packet_u32(const uint8_t* source) {
    return static_cast<uint32_t>(source[0]) |
           (static_cast<uint32_t>(source[1]) << 8) |
           (static_cast<uint32_t>(source[2]) << 16) |
           (static_cast<uint32_t>(source[3]) << 24);
}

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket invalid_socket = INVALID_SOCKET;
native_socket to_native_socket(std::intptr_t value) {
    return static_cast<native_socket>(value);
}
#else
using native_socket = int;
constexpr native_socket invalid_socket = -1;
native_socket to_native_socket(std::intptr_t value) {
    return static_cast<native_socket>(value);
}
#endif

// Diagnostic switches, read once each: these sit on the memory-access paths
// and must not call getenv per access.
//
// MODEL2_WATCH_WRITE=<hex address> reports the i960 instruction that stores
// to a work-RAM variable, which is how a bad display-list value gets traced
// back to the code that computed it.
uint32_t watched_work_address() {
    static const uint32_t address = [] {
        const char* value = std::getenv("MODEL2_WATCH_WRITE");
        return value ? static_cast<uint32_t>(
            std::strtoul(value, nullptr, 16)) : 0U;
    }();
    return address;
}

// MODEL2_WATCH_GEO=<hex word> reports which bus master wrote a given word
// into the display-list buffer: the i960, the TGP or the geometry FIFO.
bool watched_geometry_word(uint32_t& word) {
    static const bool enabled = std::getenv("MODEL2_WATCH_GEO") != nullptr;
    static const uint32_t value = [] {
        const char* text = std::getenv("MODEL2_WATCH_GEO");
        return text ? static_cast<uint32_t>(
            std::strtoul(text, nullptr, 16)) : 0U;
    }();
    word = value;
    return enabled;
}

// MODEL2_TGP_TRACE_CMD=<hex command> starts a TGP instruction trace when that
// command word is dispatched, for diffing one routine against MAME's trace.
uint32_t traced_tgp_command() {
    static const uint32_t command = [] {
        const char* value = std::getenv("MODEL2_TGP_TRACE_CMD");
        return value ? static_cast<uint32_t>(
            std::strtoul(value, nullptr, 16)) : 0U;
    }();
    return command;
}

bool trace_fifo_words(uint32_t frame) {
    if (!std::getenv("MODEL2_FIFO_WORD_TRACE")) return false;
    static const uint32_t start = [] {
        const char* value = std::getenv("MODEL2_FIFO_TRACE_START");
        return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) :
                       0U;
    }();
    return frame >= start;
}

namespace fs = std::filesystem;

// Battery RAM and 93C46 contents persist per game, using MAME's exact
// nvram file names and layout so a working MAME configuration can be
// copied straight into place.
// Per-game NVRAM subdirectory (from the game profile) so games never
// overwrite each other's saves.
fs::path model2_nvram_directory(const char* leaf) {
    fs::path root = whitty_platform::config_root();
    if (root.empty()) root = ".";
    return root / "WhittyArcade" / "nvram" /
        whitty_platform::cabinet_scoped_name(leaf ? leaf : "srallyc");
}

bool read_exact(const fs::path& path, uint8_t* data, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.read(reinterpret_cast<char*>(data),
               static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size) &&
           input.peek() == std::ifstream::traits_type::eof();
}

bool write_exact(const fs::path& path, const uint8_t* data,
                 std::size_t size) {
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(data),
                     static_cast<std::streamsize>(size));
        if (!output.good()) return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (!error) return true;
    fs::remove(path, error);
    error.clear();
    fs::rename(temporary, path, error);
    return !error;
}

// Sega Rally's 0x24-byte operator record with COUNTRY (byte 0x09) set to
// EXPORT and COIN/CREDIT SETTING #1 (1 coin 1 credit on both chutes; the
// Japanese factory record runs setting #12, 2 coins 1 credit). The game
// wrote this record through its own GAME/COIN ASSIGNMENTS menus - its
// EXPORT save also retunes the game-parameter float at 0x10 from 1.0 to
// 1.2 - and reads it back with COUNTRY EXPORT and setting #1 displayed,
// so the whole record is applied rather than patching single bytes. A
// stored record whose country disagrees is corrected on the next launch,
// like Daytona.
constexpr std::array<uint16_t, 64> srally_factory_eeprom{{
    0xc894, 0x0024, 0x0003, 0x0000, 0x0201, 0x0001, 0x0000, 0x0401,
    0x999a, 0x3f99, 0xfff2, 0xffff, 0x0001, 0x0001, 0x0000, 0x0000,
    0x0001, 0x0001, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
}};

// Daytona's operator settings live in a 128-byte "SEGA" record the game
// keeps in the model1io board's EEPROM and twice at the start of backup RAM.
// Byte 0x0b is CAR NUMBER, 0x0c LINK ID (0 master, 1 slave), 0x1b and 0x7c
// COUNTRY (2 = EXPORT, the English program). The two checksum bytes at
// 0x08-0x09 are not the CRC-16/CCITT other Sega records use, so these
// records were written by the game itself through its GAME SYSTEM menu and
// are applied whole rather than field by field.
constexpr std::array<uint8_t, 0x80> daytona_operator_master{{
    0x53, 0x45, 0x47, 0x41, 0x40, 0x82, 0x02, 0x00,
    0xc7, 0x17, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x02, 0x14, 0x1c, 0x00, 0x01, 0x00, 0x01,
    0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
}};
// Slave records for cars 3-8 differ from the car-2 record only in the
// checksum, the car index at 0x0c and the group byte at 0x7c; each set of
// values below was likewise saved by the game itself.
struct daytona_slave_patch {
    uint8_t sum_high, sum_low, car_index, group;
};
constexpr std::array<daytona_slave_patch, 7> daytona_slave_patches{{
    {0xe6, 0x70, 0x01, 0x02}, // CAR 2
    {0x99, 0x67, 0x02, 0x03}, // CAR 3
    {0x3f, 0xb7, 0x03, 0x03}, // CAR 4
    {0x0f, 0xa4, 0x04, 0x03}, // CAR 5
    {0xa9, 0x74, 0x05, 0x03}, // CAR 6
    {0x62, 0x15, 0x06, 0x03}, // CAR 7
    {0xc4, 0xc5, 0x07, 0x03}, // CAR 8
}};

constexpr std::array<uint8_t, 0x80> daytona_operator_slave{{
    0x53, 0x45, 0x47, 0x41, 0x40, 0x82, 0x02, 0x00,
    0xe6, 0x70, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x02, 0x14, 0x1c, 0x00, 0x01, 0x00, 0x01,
    0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
}};

// Virtua Fighter 2 keeps its GAME ASSIGNMENT record at 0x3300 in backup RAM
// (magic eb0a, checksum, "VIRTUA FIGHTER 2", settings; COUNTRY at +0x50,
// 2 = EXPORT - the English program). Saved by the game itself through its
// test menu and applied whole, like Daytona's operator records.
constexpr std::size_t vf2_operator_offset = 0x3300;
constexpr std::array<uint8_t, 0x60> vf2_operator_export{{
    0xeb, 0x0a, 0xd4, 0x19, 0x00, 0x00, 0x18, 0x00,
    0x56, 0x49, 0x52, 0x54, 0x55, 0x41, 0x20, 0x46,
    0x49, 0x47, 0x48, 0x54, 0x45, 0x52, 0x20, 0x32,
    0x9e, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x0f,
    0x02, 0x08, 0xa0, 0x00, 0xc8, 0x00, 0x40, 0x40,
    0x40, 0x25, 0x25, 0x25, 0x1f, 0xff, 0xff, 0xff,
}};

// A fresh Virtua Fighter 2 board seeded with only the assignment record
// boots with uninitialised bookkeeping (a garbage credit counter), because
// a valid record makes the game skip its own factory pass. So a fresh
// board gets the complete backup image the game itself wrote on first
// power-up with COUNTRY already set to EXPORT - deflated, since it is
// 16 KiB of mostly repeating filler.
constexpr std::size_t vf2_factory_backup_size = 0x4000;
constexpr std::array<uint8_t, 322> vf2_factory_backup_deflate{{
    0x78, 0xda, 0xed, 0xd4, 0xad, 0x4e, 0xc3, 0x50, 0x18, 0xc6, 0xf1, 0x77,
    0xe5, 0x1c, 0x18, 0x33, 0x20, 0x10, 0xa0, 0x98, 0x99, 0x23, 0x84, 0x80,
    0x00, 0xd9, 0x06, 0xf6, 0x71, 0x68, 0xb6, 0x92, 0x75, 0x25, 0x0c, 0x0c,
    0x97, 0x81, 0x83, 0x7b, 0xc0, 0xa0, 0x60, 0x92, 0x84, 0x0b, 0xc0, 0x92,
    0xa9, 0x29, 0x14, 0x0a, 0x85, 0x21, 0x21, 0xc1, 0xc0, 0x05, 0x90, 0x71,
    0x7a, 0x08, 0x27, 0x04, 0x0c, 0x82, 0x4c, 0xb0, 0xff, 0x4f, 0xbd, 0x4f,
    0x7a, 0xd2, 0xbc, 0x6d, 0xda, 0x47, 0x8e, 0x01, 0xe0, 0xef, 0x08, 0xaf,
    0x00, 0x00, 0x9d, 0x02, 0x80, 0x4e, 0x01, 0x40, 0xa7, 0x00, 0x00, 0x9d,
    0x02, 0x80, 0x4e, 0x01, 0x40, 0xa7, 0x00, 0x00, 0x9d, 0x02, 0x80, 0x4e,
    0x01, 0x40, 0xa7, 0x00, 0x18, 0x43, 0xc3, 0x11, 0x90, 0xb5, 0xe1, 0x58,
    0x7b, 0x38, 0x55, 0xd2, 0xcd, 0x96, 0x45, 0x44, 0x4b, 0xee, 0xec, 0x42,
    0x89, 0x89, 0xab, 0x76, 0x9a, 0x74, 0x79, 0xe7, 0xca, 0xe6, 0xfd, 0xc8,
    0x4e, 0x53, 0x2e, 0x17, 0xaf, 0x95, 0x74, 0xa2, 0x38, 0x9f, 0x5c, 0xbe,
    0xe9, 0x2b, 0x49, 0x4d, 0xcb, 0x4e, 0x25, 0x97, 0x8f, 0x6e, 0xed, 0xf9,
    0x56, 0x22, 0x9f, 0x56, 0xee, 0xed, 0xf9, 0xa4, 0x69, 0xa7, 0x82, 0xcb,
    0x4f, 0x8f, 0x4a, 0x92, 0x7a, 0x66, 0xa7, 0xc0, 0xe5, 0xde, 0xab, 0x92,
    0x86, 0xd9, 0xb2, 0xd3, 0x84, 0xcb, 0x7b, 0x6f, 0xf6, 0x7a, 0x6a, 0xec,
    0xa4, 0x5c, 0x9e, 0x9d, 0xd6, 0xb2, 0x5d, 0xad, 0xf9, 0xfd, 0x06, 0x73,
    0x5a, 0x9a, 0x26, 0xf3, 0xfb, 0x9d, 0x94, 0xb5, 0x98, 0x34, 0xf1, 0xfb,
    0x6d, 0x2c, 0x69, 0xe9, 0x46, 0x4d, 0xbf, 0xdf, 0xcb, 0xba, 0xb6, 0xf7,
    0x8b, 0xfc, 0x7e, 0x97, 0x9b, 0x5a, 0x62, 0xb7, 0xff, 0x87, 0xc3, 0x24,
    0x3f, 0x1f, 0xfb, 0xfd, 0xe6, 0x0f, 0xf2, 0xeb, 0x89, 0xdf, 0xef, 0xbf,
    0x7b, 0x2e, 0xdd, 0x2d, 0xd8, 0xa7, 0x96, 0x5d, 0xd3, 0xee, 0x64, 0x51,
    0xb9, 0x66, 0xea, 0x8d, 0x4e, 0xb5, 0x5d, 0x5e, 0x3d, 0x2f, 0x7c, 0x3b,
    0x18, 0x38, 0x3f, 0xbe, 0x9f, 0x20, 0x28, 0x7c, 0x35, 0x13, 0x14, 0x7b,
    0x32, 0x90, 0x30, 0x0c, 0x2b, 0x95, 0xca, 0xe2, 0x6f, 0xfe, 0x3f, 0x00,
    0x00, 0x00, 0x00, 0x18, 0x91, 0x77, 0x78, 0x85, 0x5b, 0x8a,
}};

// The matching 93C46 image (serialised as 64 little-endian words), also
// written by the game itself on the same first power-up.
constexpr std::array<uint8_t, 0x30> vf2_factory_eeprom{{
    0xda, 0xea, 0x24, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04,
    0x00, 0x00, 0x80, 0x3f, 0xf2, 0xff, 0xff, 0xff,
    0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x00,
    0x01, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
}};

// Motor Raid keeps its assignments in the CPU-board 93C46: an "S32A"
// record (coin/credit setting index at 0x10, COUNTRY at 0x16, checksummed
// at 0x08) with a second copy at 0x2c, echoed by a smaller checksummed
// coin-rate record in backup RAM at 0x00/0xd4. A fresh Japanese board
// wants 2 coins per credit, so the first coin looks ignored, and its
// menus are Japanese. This is the pair of images the game itself wrote
// after its assignments were set to EXPORT and 1 coin 1 credit through
// its own test menu; they replace any stored pair whose country or coin
// setting disagrees, so a board that already saved the Japanese defaults
// is corrected on the next launch too.
constexpr std::size_t motoraid_factory_backup_size = 0x4000;
constexpr std::array<uint8_t, 281> motoraid_factory_backup_deflate{{
    0x78, 0xda, 0xed, 0xd4, 0x31, 0x4e, 0x02, 0x41, 0x14, 0xc6, 0xf1, 0xb7,
    0x6c, 0xc9, 0x2d, 0x3c, 0x80, 0x77, 0x18, 0xb6, 0x90, 0x51, 0xd8, 0x4d,
    0x60, 0x13, 0xb3, 0x9e, 0xc0, 0x13, 0x98, 0x78, 0x0a, 0x3b, 0x1a, 0x1a,
    0x1b, 0x1b, 0x1a, 0x1a, 0x0a, 0x69, 0x6c, 0xa0, 0xa0, 0xa1, 0xa1, 0xa1,
    0xd1, 0x82, 0xc6, 0xc6, 0xc6, 0x86, 0x86, 0x06, 0x67, 0xd0, 0x58, 0xec,
    0xf0, 0xb6, 0x33, 0xc4, 0xcd, 0xff, 0x97, 0xcc, 0x64, 0xf2, 0xb2, 0xfb,
    0xf2, 0xcd, 0x66, 0xf3, 0x44, 0x44, 0xee, 0x1e, 0xde, 0xee, 0xcf, 0x6e,
    0x45, 0x22, 0xf1, 0xcb, 0xef, 0x22, 0xfb, 0x1f, 0x72, 0x32, 0x9f, 0xad,
    0xea, 0xb5, 0x4b, 0xaa, 0x56, 0xdd, 0xee, 0x53, 0xd5, 0x79, 0xde, 0x16,
    0xb9, 0x36, 0xb9, 0x3b, 0xad, 0x82, 0xe7, 0xba, 0x87, 0xfa, 0x3a, 0x69,
    0x94, 0xea, 0x59, 0xe6, 0x5e, 0x92, 0xd7, 0x24, 0x2e, 0xd5, 0xaf, 0xac,
    0x7f, 0x7e, 0x93, 0x44, 0xa5, 0xba, 0xbd, 0xb4, 0x6e, 0x7f, 0x0f, 0xfa,
    0xe7, 0xe7, 0x99, 0xdb, 0x3f, 0x82, 0xfe, 0x6d, 0x53, 0xf8, 0x3b, 0x07,
    0xfd, 0x0b, 0xd3, 0x77, 0xfb, 0x36, 0xe8, 0xff, 0x9d, 0x7f, 0xa0, 0xe4,
    0x1f, 0x2a, 0xf9, 0x1f, 0x95, 0xfc, 0x4f, 0x4a, 0xfe, 0x91, 0x92, 0x7f,
    0xac, 0xe4, 0x9f, 0x28, 0xf9, 0xa7, 0x4a, 0xfe, 0x89, 0x92, 0x7f, 0xaa,
    0xe4, 0x7f, 0x51, 0xf2, 0xcf, 0x94, 0xfc, 0x0b, 0x25, 0xff, 0x52, 0xc9,
    0xbf, 0x52, 0xf2, 0xaf, 0x6b, 0xfa, 0xfd, 0xff, 0xef, 0xff, 0xdf, 0x6b,
    0xa5, 0xbf, 0xe7, 0xe7, 0xa6, 0x9b, 0x5b, 0x51, 0x23, 0xf6, 0xc3, 0xea,
    0x22, 0xbd, 0x39, 0x5e, 0xef, 0x98, 0xa3, 0x75, 0x7b, 0xc8, 0x13, 0xd6,
    0xff, 0xba, 0x3f, 0x33, 0x88, 0x19, 0xc4, 0x0c, 0x62, 0x06, 0x9d, 0x72,
    0x06, 0xed, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x46, 0xbe,
    0x00, 0x84, 0xd2, 0x02, 0xb0,
}};

constexpr std::array<uint8_t, 0x50> motoraid_factory_eeprom{{
    0x53, 0x33, 0x32, 0x41, 0x92, 0xbc, 0x00, 0x00,
    0xca, 0xe6, 0x00, 0x00, 0x66, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x40, 0x80, 0xc0, 0x40, 0x80, 0xc0, 0x40,
    0x80, 0xc0, 0x80, 0x00, 0xca, 0xe6, 0x00, 0x00,
    0x66, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x01,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x40, 0x80, 0xc0,
    0x40, 0x80, 0xc0, 0x40, 0x80, 0xc0, 0x80, 0x00,
}};

// The same pair captured twice more through the test menu with the
// linked-play roles: cabinet 1 runs NETWORK TYPE Master with CABINET
// ID 1, cabinet 2 runs Slave with CABINET ID 2. NETWORK TYPE lives at
// byte 0x1e of the 93C46 record and the zero-based cabinet id at 0x1f.
constexpr std::array<uint8_t, 277> motoraid_master_backup_deflate{{
    0x78, 0xda, 0xed, 0xd4, 0xaf, 0x6e, 0xc2, 0x50, 0x14, 0xc7, 0xf1, 0x53,
    0x2a, 0xf7, 0x04, 0xd8, 0x3d, 0x00, 0xef, 0xd0, 0x56, 0x8c, 0x8e, 0x3f,
    0x4d, 0x06, 0x09, 0x81, 0x27, 0xe0, 0x89, 0x30, 0x33, 0x33, 0x33, 0x18,
    0x0c, 0x82, 0x1a, 0x0c, 0x08, 0x0c, 0x06, 0x53, 0x33, 0xc3, 0xc4, 0xcc,
    0x10, 0x33, 0x33, 0x33, 0x70, 0x2f, 0x90, 0x89, 0x5e, 0x4e, 0x1d, 0x21,
    0x34, 0xdf, 0x4f, 0x72, 0x6f, 0x6e, 0x4e, 0xda, 0x93, 0xdf, 0x6d, 0x9a,
    0x23, 0x22, 0xe2, 0x7f, 0x56, 0x77, 0x8f, 0x43, 0x11, 0x4f, 0xec, 0xb2,
    0xbb, 0xc8, 0xfe, 0x4c, 0x6e, 0xe6, 0x27, 0x2c, 0x5e, 0x7f, 0x51, 0xd1,
    0x2a, 0xdb, 0x7d, 0x8a, 0x3a, 0x2f, 0xeb, 0x22, 0xbd, 0xa0, 0x6b, 0x4e,
    0x1b, 0xe7, 0xb9, 0xd6, 0xb1, 0x9e, 0x45, 0x95, 0x5c, 0x3d, 0x49, 0xcc,
    0x4b, 0xf2, 0x11, 0xf9, 0xb9, 0x7a, 0x23, 0xb6, 0xcf, 0x6f, 0x23, 0x2f,
    0x57, 0x8f, 0x9f, 0x63, 0xb3, 0x7f, 0x39, 0xfd, 0xbb, 0xb5, 0xc4, 0xec,
    0xdf, 0x4e, 0xff, 0x7a, 0xd0, 0xb7, 0x77, 0x76, 0xfa, 0xf7, 0x83, 0x8e,
    0xd9, 0x7f, 0x9d, 0xfe, 0xa7, 0xfc, 0x23, 0x25, 0xff, 0xab, 0x92, 0xff,
    0x4d, 0xc9, 0xff, 0xae, 0xe4, 0x1f, 0x2b, 0xf9, 0x27, 0x4a, 0xfe, 0xa9,
    0x92, 0x3f, 0x55, 0xf2, 0x4f, 0x95, 0xfc, 0xa9, 0x92, 0x7f, 0xae, 0xe4,
    0x5f, 0x28, 0xf9, 0x57, 0x4a, 0xfe, 0xb5, 0x92, 0x7f, 0xa3, 0xe4, 0xcf,
    0x4a, 0xfa, 0xfd, 0xef, 0xf7, 0xff, 0x7f, 0x09, 0xdb, 0xff, 0xe7, 0xd9,
    0x83, 0x99, 0x5b, 0x5e, 0xc5, 0xb7, 0xc3, 0xea, 0xa9, 0x3d, 0xb8, 0x5c,
    0x6f, 0x06, 0x17, 0xeb, 0xf1, 0x31, 0x8f, 0x5b, 0xbf, 0x76, 0x7f, 0x66,
    0x10, 0x33, 0x88, 0x19, 0xc4, 0x0c, 0xba, 0xe5, 0x0c, 0xda, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x89, 0x1c, 0x00, 0x44, 0xce, 0x02,
    0x3f,
}};

constexpr std::array<uint8_t, 0x50> motoraid_master_eeprom{{
    0x53, 0x33, 0x32, 0x41, 0x92, 0xbc, 0x00, 0x00,
    0x6e, 0xa7, 0x00, 0x00, 0x67, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0x01, 0x00,
    0x00, 0x40, 0x80, 0xc0, 0x40, 0x80, 0xc0, 0x40,
    0x80, 0xc0, 0x80, 0x00, 0x6e, 0xa7, 0x00, 0x00,
    0x67, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x01,
    0x00, 0x03, 0x01, 0x00, 0x00, 0x40, 0x80, 0xc0,
    0x40, 0x80, 0xc0, 0x40, 0x80, 0xc0, 0x80, 0x00,
}};

constexpr std::array<uint8_t, 278> motoraid_slave_backup_deflate{{
    0x78, 0xda, 0xed, 0xd4, 0xb1, 0x4a, 0xc3, 0x50, 0x14, 0xc6, 0xf1, 0x93,
    0x66, 0xec, 0x5b, 0x94, 0xce, 0xbe, 0x43, 0x92, 0xc1, 0xa6, 0x6a, 0x02,
    0x1a, 0x90, 0xf4, 0x09, 0x7c, 0xa2, 0x42, 0x71, 0x71, 0x71, 0x11, 0xc4,
    0x25, 0x83, 0x59, 0x1c, 0xd4, 0xc1, 0xc5, 0xa5, 0x4b, 0x17, 0x97, 0x2e,
    0x2e, 0x5d, 0xba, 0xb8, 0x74, 0x89, 0xf7, 0xb6, 0xc5, 0x21, 0x37, 0x27,
    0x5b, 0x29, 0x86, 0xff, 0x0f, 0xee, 0xe5, 0x72, 0x48, 0x0e, 0xdf, 0x0d,
    0xe1, 0x88, 0x88, 0xcc, 0x5e, 0x1f, 0x87, 0x83, 0x1b, 0x11, 0x4f, 0xec,
    0xb2, 0xbb, 0x48, 0xb5, 0x27, 0x47, 0xb3, 0x0e, 0xdb, 0xd7, 0x26, 0x6a,
    0x5b, 0x5d, 0xbb, 0x4f, 0x5b, 0xe7, 0xf7, 0x91, 0xc8, 0x75, 0x90, 0x99,
    0xd3, 0xdc, 0x79, 0xee, 0x62, 0x5b, 0x5f, 0x44, 0xbd, 0x5a, 0x3d, 0x4d,
    0xcd, 0x4b, 0xf2, 0x15, 0xf9, 0xb5, 0xfa, 0x59, 0x6c, 0x9f, 0x5f, 0x46,
    0x5e, 0xad, 0x1e, 0x8f, 0x63, 0xb3, 0x7f, 0x3b, 0xfd, 0xb3, 0x93, 0xd4,
    0xec, 0x2b, 0xa7, 0xff, 0x28, 0xc8, 0xed, 0x9d, 0x9d, 0xfe, 0x79, 0x70,
    0x65, 0xf6, 0x1f, 0xa7, 0xff, 0x2e, 0xff, 0x54, 0xc9, 0x7f, 0xab, 0xe4,
    0xbf, 0x53, 0xf2, 0xdf, 0x2b, 0xf9, 0x1f, 0x94, 0xfc, 0x4f, 0x4a, 0xfe,
    0x42, 0xc9, 0x5f, 0x2a, 0xf9, 0x0b, 0x25, 0x7f, 0xa9, 0xe4, 0x7f, 0x51,
    0xf2, 0xbf, 0x29, 0xf9, 0x3f, 0x94, 0xfc, 0x9f, 0x4a, 0xfe, 0xb9, 0x92,
    0x7f, 0xd1, 0xd1, 0xef, 0xff, 0x7f, 0xff, 0xff, 0xcb, 0x30, 0xf9, 0x3b,
    0x3f, 0xf7, 0xcd, 0xdc, 0xf2, 0x7a, 0xbe, 0x1d, 0x56, 0xa7, 0xc9, 0xa4,
    0xb9, 0x7e, 0x1e, 0x34, 0xd6, 0xe3, 0x6d, 0x1e, 0xb7, 0x7e, 0xe8, 0xfe,
    0xcc, 0x20, 0x66, 0x10, 0x33, 0x88, 0x19, 0x74, 0xcc, 0x19, 0x54, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x87, 0xfc, 0x02, 0xc6, 0x06,
    0x02, 0x81,
}};

constexpr std::array<uint8_t, 0x50> motoraid_slave_eeprom{{
    0x53, 0x33, 0x32, 0x41, 0x92, 0xbc, 0x00, 0x00,
    0x8f, 0xb9, 0x00, 0x00, 0x67, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0x02, 0x01,
    0x00, 0x40, 0x80, 0xc0, 0x40, 0x80, 0xc0, 0x40,
    0x80, 0xc0, 0x80, 0x00, 0x8f, 0xb9, 0x00, 0x00,
    0x67, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x01,
    0x00, 0x03, 0x02, 0x01, 0x00, 0x40, 0x80, 0xc0,
    0x40, 0x80, 0xc0, 0x40, 0x80, 0xc0, 0x80, 0x00,
}};

// Manx TT boots into a Japanese "USED ONLY IN JAPAN" warning and Japanese
// menus on factory defaults. Its 93C46 record is checksummed at 0x00 with
// COUNTRY at 0x09, echoed by checksummed records in backup RAM (mirrored
// at +0x16ac); the coin assignments already default to 1 coin 1 credit.
// This is the pair the game itself wrote after COUNTRY was set to EXPORT
// through its test menu, from the set's shipped Twin-mode configuration,
// so CABINET TYPE and LINK TYPE keep their factory values.
constexpr std::size_t manxtt_factory_backup_size = 0x4000;
constexpr std::array<uint8_t, 548> manxtt_factory_backup_deflate{{
    0x78, 0xda, 0xed, 0x98, 0xbb, 0x2f, 0x43, 0x61, 0x18, 0xc6, 0x3f, 0x66,
    0x3d, 0xad, 0x73, 0xca, 0x52, 0x75, 0x89, 0xf6, 0xb8, 0x0c, 0x44, 0x6c,
    0x26, 0x4d, 0x5a, 0x97, 0xe8, 0x05, 0x29, 0x03, 0x89, 0x81, 0xc4, 0x20,
    0x24, 0x66, 0xfc, 0x01, 0xd2, 0xd1, 0x28, 0x06, 0x31, 0x88, 0x84, 0x95,
    0x58, 0x98, 0x44, 0x62, 0xb1, 0x30, 0x08, 0x26, 0x61, 0xb3, 0x30, 0x48,
    0x44, 0xe2, 0xf8, 0x2e, 0x8e, 0xa7, 0x22, 0x91, 0x58, 0x10, 0x9e, 0x5f,
    0x87, 0xbe, 0xbd, 0xa4, 0xbf, 0xe4, 0x3b, 0xed, 0x93, 0xa7, 0xef, 0xcd,
    0xde, 0x56, 0x65, 0x89, 0x50, 0x78, 0x9e, 0xb9, 0x17, 0xa2, 0x44, 0xf8,
    0x93, 0xa1, 0x54, 0xde, 0x8a, 0xa9, 0x7e, 0x7b, 0xdf, 0xff, 0xa4, 0xa7,
    0x50, 0xf6, 0x7a, 0x06, 0x9e, 0x57, 0x7c, 0x1e, 0xa3, 0xf3, 0xe6, 0xf1,
    0x77, 0xf3, 0x5f, 0xaf, 0xc3, 0x47, 0xcc, 0xf9, 0xbb, 0xb3, 0xef, 0xaf,
    0x43, 0x7f, 0x4e, 0x88, 0x8d, 0x06, 0xf5, 0x2a, 0xf9, 0x8c, 0xb9, 0xa8,
    0x10, 0xa9, 0x74, 0x5a, 0x7e, 0x9f, 0x3c, 0x6f, 0x45, 0xce, 0x89, 0xec,
    0x88, 0x9e, 0xd7, 0xe5, 0xdc, 0x9d, 0xeb, 0xd5, 0xf3, 0x8e, 0x9c, 0x93,
    0xa9, 0xac, 0x3e, 0xeb, 0x67, 0x39, 0xe7, 0xfa, 0x12, 0x7a, 0x8e, 0xc9,
    0x1f, 0x41, 0xd7, 0xd0, 0x88, 0x9e, 0xcf, 0xe4, 0x3c, 0xe8, 0x26, 0xf5,
    0xfb, 0x77, 0x6b, 0xe4, 0xe7, 0x64, 0xda, 0xf5, 0xbc, 0x5c, 0x2b, 0x44,
    0x26, 0x91, 0xd5, 0xf3, 0x42, 0x9d, 0x10, 0xf9, 0x96, 0xbc, 0x9e, 0xf7,
    0x2c, 0x78, 0x8f, 0x2d, 0x78, 0x4f, 0x2c, 0x78, 0x2f, 0x2c, 0x78, 0x3d,
    0x0b, 0x5e, 0x3b, 0x08, 0xef, 0x74, 0x10, 0xde, 0xcb, 0x20, 0xbc, 0x99,
    0x10, 0xbc, 0xfb, 0x21, 0x78, 0x09, 0xf9, 0x4b, 0x30, 0xf3, 0xbe, 0x4e,
    0x3a, 0x6e, 0x72, 0x45, 0xe5, 0xc1, 0x70, 0x1c, 0x39, 0x34, 0xe6, 0x0a,
    0x31, 0x34, 0xd8, 0xab, 0x73, 0x65, 0x52, 0xce, 0xf9, 0x94, 0xc9, 0x95,
    0x15, 0xd7, 0xe4, 0x93, 0x7a, 0xbe, 0xb3, 0xc1, 0xe4, 0x93, 0x9a, 0xd5,
    0x59, 0xfb, 0xd9, 0xb3, 0xd8, 0x88, 0xec, 0x99, 0x6a, 0x42, 0xf6, 0xa4,
    0x9b, 0x91, 0x3d, 0xeb, 0x36, 0xbc, 0x9b, 0x36, 0xbc, 0xad, 0x0e, 0xbc,
    0xed, 0x0e, 0xbc, 0x59, 0x07, 0xde, 0x55, 0x07, 0xde, 0x53, 0x07, 0xde,
    0x64, 0x18, 0xde, 0xed, 0x30, 0xbc, 0x6e, 0x05, 0x33, 0x8f, 0x30, 0xf3,
    0x88, 0x61, 0x27, 0x82, 0xbc, 0x39, 0x8b, 0xa0, 0x6f, 0x5d, 0x45, 0xd0,
    0xb7, 0xee, 0x23, 0xe8, 0x5b, 0x1d, 0x55, 0xe8, 0x5b, 0xe3, 0x55, 0xe8,
    0x5b, 0xe1, 0x28, 0xb2, 0xe7, 0x21, 0x8a, 0xec, 0x51, 0xfd, 0xcf, 0xcf,
    0x1e, 0xd5, 0xff, 0xfc, 0xec, 0xb9, 0x08, 0xc0, 0x7b, 0x1b, 0x80, 0xf7,
    0x2e, 0x00, 0xef, 0x63, 0x00, 0xde, 0x68, 0x51, 0xcf, 0x6b, 0xb3, 0xe0,
    0x2d, 0x58, 0xf0, 0x3e, 0x59, 0xf0, 0xaa, 0xfe, 0xe7, 0x7b, 0x55, 0xff,
    0x63, 0xe6, 0x11, 0x66, 0x1e, 0x51, 0x2c, 0xd6, 0xa3, 0x6f, 0x2d, 0xd5,
    0x23, 0x87, 0x96, 0x63, 0xe8, 0x5b, 0x6b, 0x31, 0xf4, 0xad, 0xf3, 0x18,
    0xfa, 0xd6, 0x42, 0x1c, 0x7d, 0xeb, 0x3a, 0x8e, 0xec, 0x39, 0x74, 0x91,
    0x3d, 0xea, 0x1a, 0xf8, 0xd9, 0xa3, 0xfa, 0x9f, 0x9f, 0x3d, 0x07, 0xe5,
    0xf0, 0x1e, 0x95, 0xc3, 0x9b, 0xb1, 0xe1, 0x1d, 0xb0, 0xe1, 0x9d, 0xb1,
    0xe1, 0xdd, 0xb7, 0xe1, 0xbd, 0xb7, 0xe1, 0x9d, 0x70, 0xe0, 0x55, 0xfd,
    0xcf, 0xf7, 0xaa, 0xfe, 0xc7, 0xcc, 0x23, 0x7f, 0x35, 0xf3, 0x6e, 0xb8,
    0xab, 0xe6, 0xae, 0x9a, 0xbb, 0x6a, 0xc2, 0x5d, 0x35, 0x21, 0xfc, 0x0f,
    0xc3, 0x5d, 0x35, 0x77, 0xd5, 0x84, 0x30, 0xf3, 0xb8, 0xab, 0xe6, 0xae,
    0x9a, 0x10, 0x66, 0x1e, 0x77, 0xd5, 0xdc, 0x55, 0x13, 0xf2, 0xc3, 0x99,
    0xc7, 0x53, 0x20, 0x84, 0x10, 0x42, 0x08, 0x21, 0x84, 0x10, 0x42, 0x08,
    0x21, 0xbf, 0x81, 0x17, 0x73, 0xd4, 0x57, 0x4f,
}};

constexpr std::array<uint8_t, 0x80> manxtt_factory_eeprom{{
    0x53, 0xda, 0x38, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x20, 0xe0, 0x20, 0xe0, 0x80,
    0x20, 0xe0, 0xff, 0xff, 0xf2, 0xff, 0xff, 0xff,
    0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
}};

// The same pair captured twice more with the linked-play roles the
// game itself understands, again through its own test menu: cabinet 1
// runs LINK TYPE Master with bike RED (No.1), cabinet 2 runs Slave
// with BLUE (No.2). LINK TYPE lives at byte 0x0b of the 93C46 record
// and the bike number at 0x0c.
constexpr std::array<uint8_t, 554> manxtt_master_backup_deflate{{
    0x78, 0xda, 0xed, 0x98, 0xbd, 0x4b, 0x5b, 0x51, 0x18, 0xc6, 0x5f, 0xdd,
    0x73, 0x63, 0xee, 0x4d, 0x5d, 0x92, 0xd4, 0x48, 0x6e, 0xae, 0x5f, 0x50,
    0x29, 0xd9, 0x9c, 0x0c, 0x24, 0x2d, 0x62, 0x3e, 0xaa, 0xc4, 0x0e, 0x0e,
    0x0e, 0x11, 0x3a, 0x88, 0x82, 0xb3, 0xe4, 0x0f, 0x08, 0x8e, 0x1d, 0x43,
    0x06, 0x71, 0x48, 0x0b, 0x75, 0x35, 0xb8, 0x34, 0x53, 0x29, 0x74, 0x71,
    0xb1, 0x83, 0x54, 0x27, 0x71, 0xec, 0x52, 0x87, 0x82, 0x14, 0x7a, 0x7b,
    0xce, 0xb9, 0x9c, 0x3e, 0xe9, 0x52, 0xe8, 0x52, 0x8b, 0x7d, 0x7e, 0x19,
    0xf2, 0x26, 0xb9, 0xe4, 0x07, 0xe7, 0x92, 0x87, 0x27, 0x6f, 0x33, 0x7b,
    0x34, 0x3e, 0x22, 0x9a, 0x30, 0x8c, 0x9e, 0x45, 0x46, 0xc4, 0x4e, 0x11,
    0xa3, 0xea, 0x31, 0xcc, 0xdc, 0xcf, 0xeb, 0xfe, 0x4f, 0xb2, 0xaf, 0xec,
    0x19, 0x84, 0xe1, 0xf0, 0x79, 0xe4, 0xae, 0xa3, 0xd7, 0x7f, 0x9b, 0x51,
    0x21, 0x32, 0x74, 0x3f, 0x82, 0xdd, 0x5f, 0xef, 0xc3, 0xb3, 0xba, 0xc8,
    0xeb, 0x29, 0xfd, 0x29, 0xf9, 0x1d, 0x7b, 0x19, 0x91, 0x72, 0xa5, 0xa2,
    0x7e, 0xd7, 0x61, 0xd8, 0x55, 0x73, 0xb1, 0xb6, 0x6e, 0xe6, 0x9e, 0x9a,
    0x9f, 0xd6, 0x97, 0xcc, 0xdc, 0x57, 0x73, 0xa9, 0x5c, 0x33, 0x67, 0xfd,
    0x5d, 0xcd, 0xf5, 0xe5, 0xa2, 0x99, 0xfd, 0x87, 0x22, 0x4f, 0xd6, 0xd6,
    0xcd, 0x7c, 0xae, 0xe6, 0xd5, 0xa0, 0x64, 0xae, 0x3f, 0x99, 0x50, 0xdf,
    0x53, 0x2d, 0x98, 0xb9, 0x93, 0x15, 0xa9, 0x16, 0x6b, 0x66, 0x6e, 0x4d,
    0x8a, 0x34, 0x1e, 0x35, 0xcc, 0xfc, 0xd6, 0x81, 0xf7, 0xd4, 0x81, 0xf7,
    0xcc, 0x81, 0xf7, 0xc2, 0x81, 0x37, 0x74, 0xe0, 0x75, 0xe3, 0xf0, 0x6e,
    0xc7, 0xe1, 0xbd, 0x8c, 0xc3, 0x5b, 0x1d, 0x83, 0x77, 0x30, 0x06, 0x2f,
    0x21, 0xf7, 0x09, 0x66, 0xde, 0x9f, 0x53, 0xc9, 0x47, 0xb9, 0xa2, 0xf3,
    0xe0, 0x79, 0x1e, 0x39, 0xb4, 0x11, 0x88, 0xac, 0xad, 0x2e, 0x99, 0x5c,
    0x79, 0xa1, 0xe6, 0x46, 0x39, 0xca, 0x95, 0x6e, 0x10, 0xe5, 0x93, 0x7e,
    0x7f, 0x71, 0x2a, 0xca, 0x27, 0x3d, 0xeb, 0xb3, 0xb6, 0xd9, 0xd3, 0x9e,
    0x46, 0xf6, 0x6c, 0xcd, 0x20, 0x7b, 0x2a, 0xb3, 0xc8, 0x9e, 0x9e, 0x0b,
    0xef, 0x1b, 0x17, 0xde, 0x79, 0x0f, 0xde, 0x82, 0x07, 0x6f, 0xcd, 0x83,
    0xf7, 0xc0, 0x83, 0xf7, 0xa3, 0x07, 0x6f, 0x29, 0x09, 0xef, 0x71, 0x12,
    0xde, 0xe0, 0x01, 0x33, 0x8f, 0x30, 0xf3, 0x48, 0x44, 0x3f, 0x85, 0xbc,
    0x39, 0x4f, 0xa1, 0x6f, 0x5d, 0xa5, 0xd0, 0xb7, 0x6e, 0x52, 0xe8, 0x5b,
    0x0b, 0x69, 0xf4, 0xad, 0x66, 0x1a, 0x7d, 0x2b, 0x99, 0x41, 0xf6, 0x7c,
    0xcd, 0x20, 0x7b, 0x74, 0xff, 0xb3, 0xd9, 0xa3, 0xfb, 0x9f, 0xcd, 0x9e,
    0x8b, 0x18, 0xbc, 0x9f, 0x63, 0xf0, 0x7e, 0x89, 0xc1, 0x7b, 0x1b, 0x83,
    0x37, 0x33, 0xd4, 0xf3, 0x1e, 0x3b, 0xf0, 0xee, 0x3b, 0xf0, 0x7e, 0x73,
    0xe0, 0xd5, 0xfd, 0xcf, 0x7a, 0x75, 0xff, 0x63, 0xe6, 0x11, 0x66, 0x1e,
    0xd1, 0xb4, 0x73, 0xe8, 0x5b, 0x2f, 0x73, 0xc8, 0xa1, 0x8e, 0x8f, 0xbe,
    0x75, 0xe8, 0xa3, 0x6f, 0x7d, 0xf2, 0xd1, 0xb7, 0x5a, 0x79, 0xf4, 0xad,
    0xeb, 0x3c, 0xb2, 0xe7, 0x7d, 0x80, 0xec, 0xd1, 0xf7, 0xc0, 0x66, 0x8f,
    0xee, 0x7f, 0x36, 0x7b, 0xde, 0x25, 0xe0, 0xfd, 0x90, 0x80, 0xb7, 0xea,
    0xc2, 0xbb, 0xe2, 0xc2, 0xbb, 0xe3, 0xc2, 0x3b, 0x70, 0xe1, 0xbd, 0x71,
    0xe1, 0xdd, 0xf4, 0xe0, 0xd5, 0xfd, 0xcf, 0x7a, 0x75, 0xff, 0x63, 0xe6,
    0x91, 0xfb, 0x9a, 0x79, 0x4d, 0xee, 0xaa, 0xb9, 0xab, 0xe6, 0xae, 0x9a,
    0x70, 0x57, 0x4d, 0x08, 0xff, 0xc3, 0x70, 0x57, 0xcd, 0x5d, 0x35, 0x21,
    0xcc, 0x3c, 0xee, 0xaa, 0xb9, 0xab, 0x26, 0x84, 0x99, 0xc7, 0x5d, 0x35,
    0x77, 0xd5, 0x84, 0xdc, 0x71, 0xe6, 0xf1, 0x14, 0x08, 0x21, 0x84, 0x10,
    0x42, 0x08, 0x21, 0x84, 0x10, 0x42, 0xc8, 0xbf, 0xc0, 0x0f, 0x86, 0xce,
    0x55, 0x93,
}};

constexpr std::array<uint8_t, 0x80> manxtt_master_eeprom{{
    0x91, 0x7e, 0x38, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x20, 0xe0, 0x20, 0xe0, 0x80,
    0x20, 0xe0, 0xff, 0xff, 0xf2, 0xff, 0xff, 0xff,
    0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
}};

constexpr std::array<uint8_t, 553> manxtt_slave_backup_deflate{{
    0x78, 0xda, 0xed, 0x98, 0x3f, 0x48, 0x1b, 0x61, 0x18, 0xc6, 0xdf, 0xb8,
    0xdf, 0x45, 0xef, 0xa2, 0x4b, 0x8c, 0x5a, 0x92, 0x9c, 0x7f, 0x86, 0x96,
    0x92, 0xcd, 0xa9, 0x81, 0x44, 0x91, 0xe6, 0x5f, 0x25, 0x3a, 0x38, 0x38,
    0x28, 0x38, 0x48, 0x0b, 0xce, 0xe2, 0xe4, 0x14, 0x1c, 0x1d, 0xc5, 0xa1,
    0x3a, 0x88, 0xd0, 0xae, 0x8a, 0x8b, 0x4e, 0x22, 0xb8, 0xb8, 0xe8, 0x20,
    0x46, 0x17, 0xc9, 0xe8, 0x52, 0x07, 0x41, 0x04, 0xcf, 0xef, 0xfb, 0xce,
    0xf3, 0x89, 0x8b, 0xd0, 0xa5, 0x8a, 0x7d, 0x7e, 0x19, 0xf2, 0x26, 0x39,
    0xf2, 0x83, 0xef, 0xc8, 0xc3, 0x93, 0x77, 0x71, 0xed, 0x77, 0x47, 0x44,
    0x34, 0xbe, 0x1f, 0x3c, 0x8b, 0x44, 0x24, 0x9c, 0x02, 0x5a, 0xd4, 0xa3,
    0x99, 0xcc, 0xd3, 0x75, 0xff, 0x27, 0x95, 0x0b, 0xeb, 0xf1, 0x0c, 0x7c,
    0xbf, 0xf9, 0x3c, 0x92, 0x8d, 0xe0, 0xf5, 0xbf, 0xa6, 0x45, 0x88, 0x34,
    0xdd, 0x0f, 0x6f, 0xee, 0xf9, 0x7d, 0xa8, 0x94, 0x45, 0x36, 0x7b, 0xf5,
    0xa7, 0xe4, 0x25, 0xe6, 0x13, 0x22, 0xf9, 0x42, 0x41, 0xfd, 0xae, 0x7d,
    0x7f, 0x55, 0xcd, 0xd9, 0xd2, 0x84, 0x99, 0x37, 0xd4, 0x3c, 0x5c, 0x1e,
    0x31, 0xf3, 0xb6, 0x9a, 0x73, 0xf9, 0x92, 0x39, 0xeb, 0x7b, 0x35, 0x97,
    0xbf, 0x66, 0xcd, 0x9c, 0xea, 0x12, 0x19, 0x1a, 0x9b, 0x30, 0xf3, 0xa9,
    0x9a, 0x47, 0xbd, 0x9c, 0xb9, 0x7e, 0xa7, 0x5b, 0x7d, 0x4f, 0x31, 0x63,
    0xe6, 0x95, 0x1e, 0x91, 0x62, 0xb6, 0x64, 0xe6, 0x85, 0x0f, 0x22, 0xd5,
    0x8f, 0x55, 0x33, 0xef, 0xda, 0xf0, 0x1e, 0xd9, 0xf0, 0x1e, 0xdb, 0xf0,
    0xd6, 0x6d, 0x78, 0x7d, 0x1b, 0x5e, 0x27, 0x0a, 0xef, 0xf7, 0x28, 0xbc,
    0xe7, 0x51, 0x78, 0x8b, 0xad, 0xf0, 0xee, 0xb5, 0xc2, 0x4b, 0xc8, 0x7b,
    0x82, 0x99, 0xf7, 0xf7, 0x14, 0xd2, 0x41, 0xae, 0xe8, 0x3c, 0x18, 0x4f,
    0x23, 0x87, 0x26, 0x3d, 0x91, 0xb1, 0xd1, 0x11, 0x93, 0x2b, 0x33, 0x6a,
    0xae, 0xe6, 0x83, 0x5c, 0x59, 0xf5, 0x82, 0x7c, 0xd2, 0xef, 0x7f, 0xe9,
    0x0d, 0xf2, 0x49, 0xcf, 0xfa, 0xac, 0xc3, 0xec, 0xa9, 0xf5, 0x21, 0x7b,
    0x66, 0xfb, 0x91, 0x3d, 0x85, 0x01, 0x64, 0xcf, 0x86, 0x03, 0xef, 0x2f,
    0x07, 0xde, 0x4f, 0x2e, 0xbc, 0x19, 0x17, 0xde, 0x92, 0x0b, 0xef, 0x4f,
    0x17, 0xde, 0x13, 0x17, 0xde, 0x5c, 0x0c, 0xde, 0xad, 0x18, 0xbc, 0x5e,
    0x3b, 0x33, 0x8f, 0x30, 0xf3, 0x48, 0xc0, 0x76, 0x1c, 0x79, 0x73, 0x1a,
    0x47, 0xdf, 0xba, 0x8c, 0xa3, 0x6f, 0x5d, 0xc7, 0xd1, 0xb7, 0x06, 0x3b,
    0xd1, 0xb7, 0xa6, 0x3a, 0xd1, 0xb7, 0x62, 0x09, 0x64, 0xcf, 0x4d, 0x02,
    0xd9, 0xa3, 0xfb, 0x5f, 0x98, 0x3d, 0xba, 0xff, 0x85, 0xd9, 0x53, 0xb7,
    0xe0, 0xbd, 0xb2, 0xe0, 0xfd, 0x63, 0xc1, 0x7b, 0x6b, 0xc1, 0x9b, 0x68,
    0xea, 0x79, 0x9f, 0x6d, 0x78, 0x97, 0x6c, 0x78, 0xef, 0x6c, 0x78, 0x75,
    0xff, 0x0b, 0xbd, 0xba, 0xff, 0x31, 0xf3, 0x08, 0x33, 0x8f, 0x68, 0x6a,
    0x49, 0xf4, 0xad, 0xe5, 0x24, 0x72, 0x68, 0x25, 0x85, 0xbe, 0xb5, 0x9e,
    0x42, 0xdf, 0x3a, 0x4b, 0xa1, 0x6f, 0x2d, 0xa4, 0xd1, 0xb7, 0x1a, 0x69,
    0x64, 0xcf, 0x81, 0x87, 0xec, 0xd1, 0xf7, 0x20, 0xcc, 0x1e, 0xdd, 0xff,
    0xc2, 0xec, 0xd9, 0x6f, 0x83, 0xf7, 0xb0, 0x0d, 0xde, 0xa2, 0x03, 0xef,
    0x37, 0x07, 0xde, 0x1f, 0x0e, 0xbc, 0x7b, 0x0e, 0xbc, 0xd7, 0x0e, 0xbc,
    0xd3, 0x2e, 0xbc, 0xba, 0xff, 0x85, 0x5e, 0xdd, 0xff, 0x98, 0x79, 0xe4,
    0xbd, 0x66, 0xde, 0x22, 0x77, 0xd5, 0xdc, 0x55, 0x73, 0x57, 0x4d, 0xb8,
    0xab, 0x26, 0x84, 0xff, 0x61, 0xb8, 0xab, 0xe6, 0xae, 0x9a, 0x10, 0x66,
    0x1e, 0x77, 0xd5, 0xdc, 0x55, 0x13, 0xc2, 0xcc, 0xe3, 0xae, 0x9a, 0xbb,
    0x6a, 0x42, 0x5e, 0x39, 0xf3, 0x78, 0x0a, 0x84, 0x10, 0x42, 0x08, 0x21,
    0x84, 0x10, 0x42, 0x08, 0x21, 0xe4, 0x2d, 0xf0, 0x00, 0xbb, 0x0a, 0x57,
    0xeb,
}};

constexpr std::array<uint8_t, 0x80> manxtt_slave_eeprom{{
    0x39, 0x55, 0x38, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x01, 0x02, 0x01, 0x00, 0x01, 0x01,
    0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x20, 0xe0, 0x20, 0xe0, 0x80,
    0x20, 0xe0, 0xff, 0xff, 0xf2, 0xff, 0xff, 0xff,
    0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
}};

void initialize_srally_backup(std::vector<uint8_t>& ram) {
    // The battery-backed SRAM is physically erased/high on first power-up.
    // Sega Rally validates and creates its accounting/configuration records
    // itself. Seeding guessed records can pass the checksum while selecting
    // unintended gameplay options (including traffic configuration).
    std::fill(ram.begin(), ram.end(), 0xff);
}

uint16_t sega_record_crc(const uint8_t* data, std::size_t size) {
    // Sega's settings records use CRC-16/CCITT over the record after its
    // two-byte checksum. The stored value is the one's complement.
    uint16_t crc = 0xffff;
    while (size-- > 0) {
        crc ^= static_cast<uint16_t>(*data++) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>(
                (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return static_cast<uint16_t>(~crc);
}

uint8_t dword_byte(uint32_t value, uint32_t address) {
    return static_cast<uint8_t>(value >> ((address & 3) * 8));
}

void set_dword_byte(uint32_t& destination, uint32_t address, uint8_t value) {
    const unsigned shift = (address & 3) * 8;
    destination = (destination & ~(0xffU << shift)) |
                  (static_cast<uint32_t>(value) << shift);
}

uint32_t read_dword(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void write_dword(std::vector<uint8_t>& bytes, std::size_t offset,
                 uint32_t value) {
    if (offset + 3 >= bytes.size()) return;
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}
}

model2_bus::model2_bus() = default;

model2_bus::~model2_bus() {
    close_comm_peer();
    if (!m_backup_ram.empty()) save_nvram();
}

bool model2_bus::open_comm_peer() {
    close_comm_peer();
#if defined(_WIN32)
    static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) return false;
#endif
    const native_socket socket_handle =
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == invalid_socket) return false;

    int reuse = 1;
    setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (m_comm_network)
        setsockopt(socket_handle, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
        closesocket(socket_handle);
        return false;
    }
#else
    const int flags = fcntl(socket_handle, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socket_handle);
        return false;
    }
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(m_comm_local_port);
    local.sin_addr.s_addr =
        htonl(m_comm_network ? INADDR_ANY : INADDR_LOOPBACK);
    if (::bind(socket_handle, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0) {
#if defined(_WIN32)
        closesocket(socket_handle);
#else
        ::close(socket_handle);
#endif
        return false;
    }
    m_comm_socket = static_cast<std::intptr_t>(socket_handle);
    std::printf("Model 2 cabinet %u: link UDP %u -> %u\n",
                m_comm_node_id, m_comm_local_port, m_comm_peer_port);
    return true;
}

void model2_bus::close_comm_peer() {
    if (m_comm_socket == -1) return;
    const native_socket socket_handle = to_native_socket(m_comm_socket);
#if defined(_WIN32)
    closesocket(socket_handle);
#else
    ::close(socket_handle);
#endif
    m_comm_socket = -1;
}

void model2_bus::initialize_comm_board() {
    std::fill(m_communication_ram.begin(), m_communication_ram.end(), 0);
    m_communication_ram[0x01] = 0x02;
    m_communication_ram[0x12] = 0x00;
    m_communication_ram[0x13] = 0x0e; // 0x0e00-byte frame
    m_communication_ram[0x14] = 0xc0;
    m_communication_ram[0x15] = 0x01; // ring offset 0x01c0
    m_comm_zfg = 0;

    const char* link = std::getenv("MODEL2_COMM_LINK");
    const int node_id = environment_comm_node();
    m_comm_peer_mode = node_id != 0;
    m_comm_network =
        m_comm_peer_mode && std::getenv("MODEL2_COMM_NETWORK") != nullptr;
    m_comm_node_id =
        m_comm_peer_mode ? static_cast<uint8_t>(node_id) : 0;
    // Ring of up to 8 cabinets: node N listens on base+N-1 and transmits to
    // its successor (wrapping to the master). The launcher sets the ports
    // explicitly; the defaults serve hand-started test runs.
    m_comm_link_count = static_cast<uint8_t>(
        m_comm_peer_mode ? std::max(environment_comm_count(), node_id) : 0);
    const uint16_t default_local = static_cast<uint16_t>(
        15112 + (node_id > 0 ? node_id - 1 : 0));
    const uint16_t default_peer = static_cast<uint16_t>(
        15112 + (m_comm_peer_mode ? node_id % m_comm_link_count : 1));
    m_comm_local_port =
        environment_port("MODEL2_COMM_LOCAL_PORT", default_local);
    m_comm_peer_port =
        environment_port("MODEL2_COMM_PEER_PORT", default_peer);
    // MAME's default endpoints listen on 0.0.0.0:15112 and connect back to
    // 127.0.0.1:15112. Thus a NOTLINK cabinet still forms a one-node ring
    // after the communication board's four-second discovery period.
    m_comm_loopback = !m_comm_peer_mode &&
        (!link || std::strcmp(link, "0") != 0);
    m_comm_peer_seen = false;
    m_comm_link_alive = false;
    m_comm_link_reported = false;
    m_comm_link_timer = 0x00e8; // 58 Hz * 4 seconds in EPR-16726
    m_comm_loopback_frame_valid = false;
    m_communication_ram[0x00] = 0x00;
    m_communication_ram[0x02] = 0xff;
    m_communication_ram[0x03] = 0xff;

    if (m_comm_peer_mode && !open_comm_peer())
        std::fprintf(stderr,
                     "Model 2 cabinet %u could not open link port %u\n",
                     m_comm_node_id, m_comm_local_port);
    if (std::getenv("MODEL2_COMM_TRACE"))
        std::fprintf(stderr,
                     "Model 2 comm enabled frame=%u loopback=%d "
                     "peer=%d node=%u\n",
                     m_frame_number, m_comm_loopback,
                     m_comm_peer_mode, m_comm_node_id);
}

void model2_bus::tick_comm_peer() {
    if (m_comm_socket == -1 || m_communication_ram.size() <
            comm_frame_receive + comm_frame_size)
        return;
    // The master paces the ring: it transmits once per vblank and the
    // slave answers each received frame from comm_peer_receive() - the
    // request/response shape MAME's m2comm implements, where the reply
    // happens while the game polls FG inside the same video frame. Both
    // roles transmitting blindly per vblank starves Daytona's versus
    // handshake, which retries a step every frame until the answer
    // arrives within it.
    if (m_comm_link_timer > 0) --m_comm_link_timer;
    if (m_comm_node_id == 1 || !m_comm_link_alive) comm_peer_send();
    comm_peer_receive();
}

void model2_bus::comm_peer_send() {
    if (m_comm_socket == -1 || m_communication_ram.size() <
            comm_frame_receive + comm_frame_size)
        return;

    std::array<uint8_t, comm_packet_header + comm_frame_size> packet{};
    std::copy(comm_packet_magic.begin(), comm_packet_magic.end(),
              packet.begin());
    packet[4] = 1;
    packet[5] = m_comm_node_id;
    packet[6] = 1;
    write_packet_u32(packet.data() + 8, ++m_comm_sequence);
    write_packet_u32(packet.data() + 12,
                     static_cast<uint32_t>(comm_frame_size));
    std::copy_n(m_communication_ram.begin() + comm_frame_start,
                comm_frame_size, packet.begin() + comm_packet_header);

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(m_comm_peer_port);
    peer.sin_addr.s_addr = htonl(
        m_comm_network ? INADDR_BROADCAST : INADDR_LOOPBACK);
    const native_socket socket_handle = to_native_socket(m_comm_socket);
    sendto(socket_handle, reinterpret_cast<const char*>(packet.data()),
           static_cast<int>(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    // Also loop the LAN packet onto this host. This supports two-computer
    // testing on one machine and does not affect remote discovery; packets
    // still carry a node ID and self-originated frames are ignored.
    if (m_comm_network) {
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(socket_handle, reinterpret_cast<const char*>(packet.data()),
               static_cast<int>(packet.size()), 0,
               reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    }
}

bool model2_bus::comm_peer_receive() {
    if (m_comm_socket == -1 || m_communication_ram.size() <
            comm_frame_receive + comm_frame_size)
        return false;
    const native_socket socket_handle = to_native_socket(m_comm_socket);

    bool received_payload = false;
    for (;;) {
        std::array<uint8_t, comm_packet_header + comm_frame_size> incoming{};
        sockaddr_in sender{};
#if defined(_WIN32)
        int sender_size = sizeof(sender);
#else
        socklen_t sender_size = sizeof(sender);
#endif
        const int received = static_cast<int>(recvfrom(
            socket_handle, reinterpret_cast<char*>(incoming.data()),
            static_cast<int>(incoming.size()), 0,
            reinterpret_cast<sockaddr*>(&sender), &sender_size));
        if (received < static_cast<int>(comm_packet_header)) break;
        if (!std::equal(comm_packet_magic.begin(), comm_packet_magic.end(),
                        incoming.begin()) ||
            incoming[4] != 1 || incoming[5] == m_comm_node_id)
            continue;
        m_comm_peer_seen = true;
        const uint32_t payload_size = read_packet_u32(incoming.data() + 12);
        if ((incoming[6] & 1) && payload_size == comm_frame_size &&
            received == static_cast<int>(comm_packet_header +
                                         comm_frame_size)) {
            std::copy_n(incoming.begin() + comm_packet_header,
                        comm_frame_size,
                        m_communication_ram.begin() + comm_frame_receive);
            received_payload = true;
            // Each delivered frame is one ring hop: the receive buffer
            // overlaps the transmit buffer 0x1c0 in, so answering NOW
            // forwards the whole shifted ring - a node's own slot followed
            // by everything upstream. Answering once after draining the
            // socket collapsed queued hops into one and the ring's shift
            // register lost slots, which Daytona's network check measures.
            m_comm_zfg ^= 1;
            if (m_comm_node_id >= 2 && m_comm_link_alive) comm_peer_send();
        }
    }

    // The board spends its discovery period pending (status 0, id and
    // count 0xff) before the ring goes alive - the EPR-16726 sequence the
    // games watch. Going alive on the first packet raced the other
    // cabinets' power-on: with three or more boards the network check
    // started counting a half-formed ring and never finished. The timer
    // runs from tick_comm_peer; here the link only completes once the
    // period has elapsed and a predecessor has actually been heard.
    if (m_comm_peer_seen && !m_comm_link_alive && m_comm_link_timer == 0) {
        m_comm_link_alive = true;
        m_communication_ram[0x00] = 0x01;
        // EPR-16726's ID exchange numbers the ring DOWNWARDS from the
        // master: the node the master transmits to takes the highest id
        // and the node transmitting into the master takes 2 (observed on
        // MAME's m2comm with three cabinets: 1, 3, 2 in launch order).
        // For a twin both orders coincide; with more cabinets each game
        // derives its data slot from this id, and numbering upwards left
        // every ring bigger than two stuck after its network check.
        m_communication_ram[0x02] = m_comm_node_id <= 1 ? 1 :
            static_cast<uint8_t>(m_comm_link_count - m_comm_node_id + 2);
        m_communication_ram[0x03] = m_comm_link_count;
        whitty_wall_log::note("comm: linked, local port %u peer port %u",
                              m_comm_local_port, m_comm_peer_port);
        std::printf("Model 2 cabinet %u: linked as node %u of %u\n",
                    m_comm_node_id, m_comm_node_id, m_comm_link_count);
    } else if (!m_comm_link_alive) {
        m_communication_ram[0x00] = 0x00;
        m_communication_ram[0x02] = 0xff;
        m_communication_ram[0x03] = 0xff;
    }
    return received_payload;
}

uint32_t model2_bus::video_control() const {
    if (m_control_registers.size() < 0x10) return 0;
    return read_dword(m_control_registers, 0x0c) & 3;
}

bool model2_bus::geometry_frame_due() const {
    // MAME/model2.cpp screen_vblank(): in 30 Hz mode a completed geometry
    // list is latched only on even display frames. The rasterizer presents
    // the preceding 3D image on the intervening 60 Hz display frame.
    return (video_control() & 1) == 0 || (m_frame_number & 1) == 0;
}

uint32_t model2_bus::master_z_clip() const {
    if (m_z_clip.size() < 4) return 0xff;
    return read_dword(m_z_clip, 0);
}

namespace {
// Every linked cabinet is its own machine with its own operator settings
// and bookkeeping; sharing one NVRAM directory would let the cabinets
// overwrite each other's records mid-run (last writer wins). Cabinet 1 and
// single launches keep the plain per-game directory.
fs::path cabinet_nvram_directory(const char* leaf) {
    const int node = environment_comm_node();
    if (node >= 2) {
        const std::string cabinet_leaf =
            std::string(leaf) + "-cab" + std::to_string(node);
        return model2_nvram_directory(cabinet_leaf.c_str());
    }
    return model2_nvram_directory(leaf);
}
} // namespace

void model2_bus::load_nvram() {
    const fs::path directory = cabinet_nvram_directory(m_profile.nvram_leaf);

    if (m_profile.io == model2_game_profile::io_kind::model1io2_dpram ||
        m_profile.io == model2_game_profile::io_kind::model1io_dpram) {
        // I/O-board cabinets keep bookkeeping and service settings in the
        // board's serial EEPROM. Start it blank (the firmware writes factory
        // defaults) and restore any saved image so it survives a restart.
        m_iob_eeprom.fill(0xff);
        read_exact(directory / "ioboard_eeprom", m_iob_eeprom.data(),
                   m_iob_eeprom.size());
        m_iob_eeprom_saved = m_iob_eeprom;
        // model1io2 (Virtua Cop) uses nothing on the CPU board; the Daytona
        // cabinet also persists the CPU-board backup RAM and 93C46 below.
        if (m_profile.io == model2_game_profile::io_kind::model1io2_dpram)
            return;
    }

    if (m_profile.io == model2_game_profile::io_kind::model1io_dpram) {
        // Daytona initialises its own records on a blank board; the Sega
        // Rally factory image would only trip its checksum check.
        std::fill(m_backup_ram.begin(), m_backup_ram.end(), 0);
        m_eeprom_words.fill(0);
    } else {
        initialize_srally_backup(m_backup_ram);
        m_eeprom_words = srally_factory_eeprom;
    }

    std::vector<uint8_t> backup(m_backup_ram.size());
    const bool backup_loaded =
        read_exact(directory / "backup1", backup.data(), backup.size());
    if (backup_loaded) m_backup_ram = std::move(backup);
    std::array<uint8_t, 128> eeprom{};
    if (read_exact(directory / "eeprom", eeprom.data(), eeprom.size())) {
        // MAME serializes the 93C46 as 64 little-endian 16-bit words.
        for (unsigned word = 0; word < m_eeprom_words.size(); ++word)
            m_eeprom_words[word] = static_cast<uint16_t>(
                eeprom[word * 2] | (eeprom[word * 2 + 1] << 8));
    } else if (m_roms && m_roms->default_eeprom.size() == 0x80) {
        // The set ships a factory 93C46 image (Manx TT's Twin-mode
        // configuration); a fresh board starts from it.
        for (unsigned word = 0; word < m_eeprom_words.size(); ++word)
            m_eeprom_words[word] = static_cast<uint16_t>(
                m_roms->default_eeprom[word * 2] |
                (m_roms->default_eeprom[word * 2 + 1] << 8));
    }

    if (m_roms && m_roms->set == model2_rom_set::sega_rally_revision_c) {
        // A record saved as JPN before this build keeps Japanese attract
        // and coin behaviour forever, so it is corrected like Daytona's:
        // the stored operator record is replaced with the game-written
        // EXPORT one when its COUNTRY byte (0x09) disagrees. The session
        // reasserts LINK TYPE afterwards.
        const uint8_t country =
            static_cast<uint8_t>(m_eeprom_words[0x09 / 2] >> 8);
        if (country != 0x02) {
            m_eeprom_words = srally_factory_eeprom;
            m_nvram_dirty = true;
        }
    }

    if (m_profile.io == model2_game_profile::io_kind::crx_fighter &&
        std::strcmp(m_profile.nvram_leaf, "vf2") == 0 && !backup_loaded &&
        m_backup_ram.size() >= vf2_factory_backup_size) {
        // First power-up: the complete image the game itself wrote,
        // COUNTRY already EXPORT and bookkeeping properly zeroed. Seeding
        // only the assignment record made the game skip its factory pass
        // and boot with a garbage credit counter.
        uLongf inflated = vf2_factory_backup_size;
        if (uncompress(m_backup_ram.data(), &inflated,
                       vf2_factory_backup_deflate.data(),
                       vf2_factory_backup_deflate.size()) == Z_OK &&
            inflated == vf2_factory_backup_size) {
            for (unsigned word = 0; word < m_eeprom_words.size(); ++word) {
                const std::size_t offset = word * 2;
                m_eeprom_words[word] = offset + 1 <
                        vf2_factory_eeprom.size() ?
                    static_cast<uint16_t>(
                        vf2_factory_eeprom[offset] |
                        (vf2_factory_eeprom[offset + 1] << 8)) :
                    0xffff;
            }
            m_nvram_dirty = true;
        }
    }

    if (m_profile.io == model2_game_profile::io_kind::crx_fighter &&
        std::strcmp(m_profile.nvram_leaf, "vf2") == 0 &&
        m_backup_ram.size() >= vf2_operator_offset +
                                  vf2_operator_export.size() &&
        m_backup_ram[vf2_operator_offset + 0x50] !=
            vf2_operator_export[0x50]) {
        // COUNTRY must stay EXPORT for the English program; the record is
        // replaced only when that field disagrees, so other assignment
        // changes survive ordinary launches.
        std::copy(vf2_operator_export.begin(), vf2_operator_export.end(),
                  m_backup_ram.begin() + vf2_operator_offset);
        m_nvram_dirty = true;
    }

    if (m_profile.io == model2_game_profile::io_kind::crx_bike &&
        std::strcmp(m_profile.nvram_leaf, "motoraid") == 0 &&
        m_backup_ram.size() >= motoraid_factory_backup_size) {
        // A single cabinet runs NETWORK TYPE Stand Alone; cabinet 1 of a
        // ring is Master with CABINET ID 1 and every later cabinet Slave
        // with its own id - the game's menu cycles IDs 1 through 4, its
        // four-cabinet maximum (LIVE is the non-playing spectator type),
        // or the game never runs its network check and no cabinet
        // invites the other. Roles beyond the captured Slave/ID2 pair
        // patch the id into that record; the S32A checksum is not the
        // standard Sega record CRC, so each id's checksum bytes were
        // captured from a record the game itself wrote (both copies of
        // the record carry the same values). Slave backups written under
        // different ids are byte-identical, so the one slave backup
        // image serves every id.
        const int node = environment_comm_node();
        std::array<uint8_t, 0x50> wanted_eeprom = motoraid_factory_eeprom;
        const uint8_t* wanted_backup =
            motoraid_factory_backup_deflate.data();
        uLong wanted_backup_size = motoraid_factory_backup_deflate.size();
        if (node == 1) {
            wanted_eeprom = motoraid_master_eeprom;
            wanted_backup = motoraid_master_backup_deflate.data();
            wanted_backup_size = motoraid_master_backup_deflate.size();
        } else if (node >= 2) {
            struct slave_patch { uint8_t sum0, sum1, id; };
            constexpr std::array<slave_patch, 3> patches{{
                {0x8f, 0xb9, 0x01}, // CABINET ID 2
                {0xed, 0xc4, 0x02}, // CABINET ID 3
                {0xeb, 0x20, 0x03}, // CABINET ID 4
            }};
            const slave_patch& patch =
                patches[static_cast<std::size_t>(std::min(node, 4) - 2)];
            wanted_eeprom = motoraid_slave_eeprom;
            for (const std::size_t record : {std::size_t{0x08},
                                             std::size_t{0x2c}}) {
                wanted_eeprom[record] = patch.sum0;
                wanted_eeprom[record + 1] = patch.sum1;
                wanted_eeprom[record + 0x17] = patch.id;
            }
            wanted_backup = motoraid_slave_backup_deflate.data();
            wanted_backup_size = motoraid_slave_backup_deflate.size();
        }
        const auto eeprom_byte = [this](std::size_t offset) {
            const uint16_t word = m_eeprom_words[offset / 2];
            return static_cast<uint8_t>(offset & 1 ? word >> 8 : word);
        };
        // Coin/credit setting #1 is index 0 at 0x10; EXPORT is 2 at 0x16;
        // NETWORK TYPE at 0x1e with the zero-based cabinet id at 0x1f.
        // Anything else - a fresh board included - gets the wanted pair.
        if (eeprom_byte(0x10) != wanted_eeprom[0x10] ||
            eeprom_byte(0x16) != wanted_eeprom[0x16] ||
            eeprom_byte(0x1e) != wanted_eeprom[0x1e] ||
            eeprom_byte(0x1f) != wanted_eeprom[0x1f]) {
            uLongf inflated = motoraid_factory_backup_size;
            if (uncompress(m_backup_ram.data(), &inflated, wanted_backup,
                           wanted_backup_size) == Z_OK &&
                inflated == motoraid_factory_backup_size) {
                for (unsigned word = 0; word < m_eeprom_words.size();
                     ++word) {
                    const std::size_t offset = word * 2;
                    m_eeprom_words[word] = offset + 1 < 0x50 ?
                        static_cast<uint16_t>(
                            wanted_eeprom[offset] |
                            (wanted_eeprom[offset + 1] << 8)) :
                        0xffff;
                }
                m_nvram_dirty = true;
            }
        }
    }

    if (m_profile.io == model2_game_profile::io_kind::crx_bike &&
        std::strcmp(m_profile.nvram_leaf, "manxttc") == 0 &&
        m_backup_ram.size() >= manxtt_factory_backup_size) {
        // The launch decides the wanted assignments: a single cabinet runs
        // Not Link, cabinet 1 of a ring is Master with bike RED (No.1),
        // ring middles are Relay and the final cabinet Slave, each with
        // the bike number of its ring position (the game's own menu
        // cycles bikes No.1 through No.8, so Manx TT rings match
        // Daytona's eight-cabinet maximum). COUNTRY (0x09, 2 = EXPORT),
        // LINK TYPE (0x0b, 1 Master / 2 Slave / 3 Relay) and BIKE COLOR
        // (0x0c) in the 93C46 record identify the stored configuration.
        // Only the eeprom record stores those assignments - backups
        // written under different roles are byte-identical - so records
        // beyond the captured Master/Slave pair are synthesized with the
        // standard Sega record CRC, byte-identical to what the game's
        // test menu writes (verified against game-written Relay/No.3
        // and Slave/No.4 records).
        const int node = environment_comm_node();
        std::array<uint8_t, 0x80> wanted_eeprom = manxtt_factory_eeprom;
        const uint8_t* wanted_backup = manxtt_factory_backup_deflate.data();
        uLong wanted_backup_size = manxtt_factory_backup_deflate.size();
        if (node == 1) {
            wanted_eeprom = manxtt_master_eeprom;
            wanted_backup = manxtt_master_backup_deflate.data();
            wanted_backup_size = manxtt_master_backup_deflate.size();
        } else if (node >= 2) {
            const int count = environment_comm_count();
            wanted_eeprom = manxtt_slave_eeprom;
            wanted_eeprom[0x0b] = node >= count ? 0x02 : 0x03;
            wanted_eeprom[0x0c] = static_cast<uint8_t>(node - 1);
            const std::size_t record_size = wanted_eeprom[2];
            const uint16_t checksum = sega_record_crc(
                wanted_eeprom.data() + 2, record_size - 2);
            wanted_eeprom[0] = static_cast<uint8_t>(checksum);
            wanted_eeprom[1] = static_cast<uint8_t>(checksum >> 8);
            wanted_backup = manxtt_slave_backup_deflate.data();
            wanted_backup_size = manxtt_slave_backup_deflate.size();
        }
        const auto eeprom_byte = [this](std::size_t offset) {
            const uint16_t word = m_eeprom_words[offset / 2];
            return static_cast<uint8_t>(offset & 1 ? word >> 8 : word);
        };
        if (eeprom_byte(0x09) != wanted_eeprom[0x09] ||
            eeprom_byte(0x0b) != wanted_eeprom[0x0b] ||
            eeprom_byte(0x0c) != wanted_eeprom[0x0c]) {
            uLongf inflated = manxtt_factory_backup_size;
            if (uncompress(m_backup_ram.data(), &inflated, wanted_backup,
                           wanted_backup_size) == Z_OK &&
                inflated == manxtt_factory_backup_size) {
                for (unsigned word = 0; word < m_eeprom_words.size();
                     ++word)
                    m_eeprom_words[word] = static_cast<uint16_t>(
                        wanted_eeprom[word * 2] |
                        (wanted_eeprom[word * 2 + 1] << 8));
                m_nvram_dirty = true;
            }
        }
    }

    if (m_profile.io == model2_game_profile::io_kind::model1io_dpram) {
        // Enforce Daytona's operator record for this launch: every linked
        // cabinet after the first must run SLAVE with its own car number
        // or several boards claim the ring's master role and the versus
        // handshake deadlocks, and COUNTRY must stay EXPORT for the
        // English program. The whole record is replaced only when one of
        // those fields disagrees, so other operator settings survive
        // ordinary launches.
        const int node = environment_comm_node();
        std::array<uint8_t, 0x80> record = daytona_operator_master;
        if (node >= 2) {
            record = daytona_operator_slave;
            const daytona_slave_patch& patch =
                daytona_slave_patches[static_cast<std::size_t>(node - 2)];
            record[0x08] = patch.sum_high;
            record[0x09] = patch.sum_low;
            record[0x0c] = patch.car_index;
            record[0x7c] = patch.group;
        }
        const bool matches =
            m_iob_eeprom[0x0b] == record[0x0b] &&
            m_iob_eeprom[0x0c] == record[0x0c] &&
            m_iob_eeprom[0x1b] == record[0x1b] &&
            m_iob_eeprom[0x7c] == record[0x7c];
        if (!matches && m_backup_ram.size() >= 0x100) {
            std::copy(record.begin(), record.end(), m_iob_eeprom.begin());
            std::copy(record.begin(), record.end(), m_backup_ram.begin());
            std::copy(record.begin(), record.end(),
                      m_backup_ram.begin() + 0x80);
            m_nvram_dirty = true;
        }
    }
}

bool model2_bus::save_nvram() {
    const fs::path directory = cabinet_nvram_directory(m_profile.nvram_leaf);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) return false;

    if (m_profile.io == model2_game_profile::io_kind::model1io2_dpram ||
        m_profile.io == model2_game_profile::io_kind::model1io_dpram) {
        const bool io_saved = write_exact(directory / "ioboard_eeprom",
                                          m_iob_eeprom.data(),
                                          m_iob_eeprom.size());
        if (io_saved) m_iob_eeprom_saved = m_iob_eeprom;
        if (m_profile.io == model2_game_profile::io_kind::model1io2_dpram) {
            if (io_saved) {
                m_nvram_dirty = false;
                m_last_nvram_save_frame = m_frame_number;
            }
            return io_saved;
        }
    }

    bool saved = write_exact(directory / "backup1", m_backup_ram.data(),
                             m_backup_ram.size());
    std::array<uint8_t, 128> eeprom{};
    for (unsigned word = 0; word < m_eeprom_words.size(); ++word) {
        eeprom[word * 2] = static_cast<uint8_t>(m_eeprom_words[word]);
        eeprom[word * 2 + 1] = static_cast<uint8_t>(m_eeprom_words[word] >> 8);
    }
    saved = write_exact(directory / "eeprom", eeprom.data(), eeprom.size()) &&
            saved;
    if (saved) {
        m_nvram_dirty = false;
        m_last_nvram_save_frame = m_frame_number;
    }
    return saved;
}

uint8_t model2_bus::srally_link_type() const {
    // Sega Rally's 0x24-byte operator-settings record begins at EEPROM word
    // zero. Byte 0x0b is LINK TYPE: 0=NOTLINK, then CAR1 through CAR4 -
    // the game's own link menu stops at four cars (5=RELAY, a non-playing
    // pass-through unit, stays unexposed).
    constexpr std::size_t link_type_offset = 0x0b;
    const std::size_t word = link_type_offset / 2;
    const uint8_t value = static_cast<uint8_t>(
        m_eeprom_words[word] >> ((link_type_offset & 1) * 8));
    return value <= 4 ? value : 0;
}

bool model2_bus::set_srally_link_type(uint8_t type) {
    if (type > 4 ||
        !m_roms ||
        m_roms->set != model2_rom_set::sega_rally_revision_c)
        return false;

    std::array<uint8_t, 128> bytes{};
    for (std::size_t word = 0; word < m_eeprom_words.size(); ++word) {
        bytes[word * 2] = static_cast<uint8_t>(m_eeprom_words[word]);
        bytes[word * 2 + 1] =
            static_cast<uint8_t>(m_eeprom_words[word] >> 8);
    }
    constexpr std::size_t record_size = 0x24;
    constexpr std::size_t link_type_offset = 0x0b;
    if (bytes[2] != record_size || bytes[3] != 0) return false;
    m_forced_srally_link_type = static_cast<int8_t>(type);
    if (bytes[link_type_offset] == type) return true;

    bytes[link_type_offset] = type;
    // Match an original service-menu save by advancing its revision counter.
    uint32_t revision = static_cast<uint32_t>(bytes[4]) |
        (static_cast<uint32_t>(bytes[5]) << 8) |
        (static_cast<uint32_t>(bytes[6]) << 16) |
        (static_cast<uint32_t>(bytes[7]) << 24);
    ++revision;
    bytes[4] = static_cast<uint8_t>(revision);
    bytes[5] = static_cast<uint8_t>(revision >> 8);
    bytes[6] = static_cast<uint8_t>(revision >> 16);
    bytes[7] = static_cast<uint8_t>(revision >> 24);
    const uint16_t checksum =
        sega_record_crc(bytes.data() + 2, record_size - 2);
    bytes[0] = static_cast<uint8_t>(checksum);
    bytes[1] = static_cast<uint8_t>(checksum >> 8);
    for (std::size_t word = 0; word < m_eeprom_words.size(); ++word)
        m_eeprom_words[word] = static_cast<uint16_t>(
            bytes[word * 2] | (bytes[word * 2 + 1] << 8));
    if (std::getenv("MODEL2_NVRAM_TRACE"))
        std::fprintf(stderr,
                     "Sega Rally EEPROM LINK TYPE=%u revision=%u crc=%04x\n",
                     type, revision, checksum);
    m_nvram_dirty = true;
    return save_nvram();
}

bool model2_bus::in_range(uint32_t address, uint32_t base,
                          std::size_t size) {
    return address >= base &&
           static_cast<uint64_t>(address) - base < size;
}

uint8_t model2_bus::read_region(const std::vector<uint8_t>& region,
                                uint32_t address, uint32_t base,
                                uint8_t fallback) {
    if (!in_range(address, base, region.size())) return fallback;
    return region[static_cast<std::size_t>(address - base)];
}

bool model2_bus::write_region(std::vector<uint8_t>& region,
                              uint32_t address, uint32_t base,
                              uint8_t value) {
    if (!in_range(address, base, region.size())) return false;
    region[static_cast<std::size_t>(address - base)] = value;
    return true;
}

bool model2_bus::write_video_region(std::vector<uint8_t>& region,
                                    uint32_t address, uint32_t base,
                                    uint8_t value, uint64_t& generation) {
    if (!in_range(address, base, region.size())) return false;
    uint8_t& destination = region[static_cast<std::size_t>(address - base)];
    if (destination != value) {
        destination = value;
        ++generation;
    }
    return true;
}

void model2_bus::attach(const model2_roms& roms,
                        const model2_game_profile& profile) {
    m_roms = &roms;
    m_profile = profile;
    m_local_ram.resize(0x40000);
    m_work_ram.resize(0x100000);
    m_geometry_ram.resize(0x4000);
    m_geometry_program.resize(0x4000);
    m_buffer_ram.resize(0x20000);
    m_control_registers.resize(0x40);
    m_cpu_control.resize(0x38);
    m_tile_ram.resize(0x10000);
    m_character_ram.resize(0x80000);
    m_palette_ram.resize(0x4000);
    m_color_translation.resize(0xc000);
    m_z_clip.resize(4);
    m_communication_ram.resize(0x4000);
    m_communication_control.resize(4);
    m_backup_ram.resize(0x4000);
    // The 315-5649 only decodes its 16 byte-wide registers on even i960
    // lanes.  In particular, +0x202 must remain open bus: Sega Rally probes
    // that address to distinguish the CRX I/O controller from the older
    // external I/O board.  Backing the probe with RAM selects the wrong
    // driver and leaves every control permanently neutral.
    m_io_registers.resize(0x20);
    m_uart_registers.resize(4);
    m_texture_ram_0.resize(0x200000);
    m_texture_ram_1.resize(0x200000);
    m_texture_sheet_0.resize(0x100000);
    m_texture_sheet_1.resize(0x100000);
    m_luma_ram.resize(0x20000);
    m_luma_table.resize(0x8000);
    m_framebuffer_a.resize(0x80000);
    m_framebuffer_b.resize(0x80000);
    m_tgp_data_ram.resize(0x400);

    // Original Model 2 cabinets share dual-port RAM with an I/O board at
    // 0x01c00000: Virtua Cop the model1io2 (TMPZ84C015, epr-17181), Daytona
    // the earlier model1io driving board (315-5338A, epr-14869) the Model 1
    // racers use. Build whichever the profile selects and run its real
    // firmware; other cabinets read the 315-5296 chip there instead.
    if ((m_profile.io == model2_game_profile::io_kind::model1io2_dpram ||
         m_profile.io == model2_game_profile::io_kind::model1io_dpram) &&
        roms.io_cpu.size() == 0x10000) {
        m_iob_dpram.assign(0x800, 0);
        m_iob = std::make_unique<model1_io_board>(
            m_profile.io == model2_game_profile::io_kind::model1io2_dpram ?
                model1_io_board_type::advanced_tmpz84c015 :
                model1_io_board_type::standard_315_5338a,
            roms.io_cpu, m_iob_dpram, m_iob_eeprom);
    }
    reset();
}

void model2_bus::reset() {
    close_comm_peer();
    auto clear = [](std::vector<uint8_t>& memory, uint8_t value = 0) {
        std::fill(memory.begin(), memory.end(), value);
    };
    clear(m_local_ram);
    if (m_profile.original_model2 && m_roms &&
        m_roms->main_cpu.size() >= 0x40000) {
        // Original Model 2 maps 0x00220000-0x0023ffff as program ROM
        // (maincpu offset 0x20000) where Model 2A has RAM. The i960 burst
        // path reads local RAM through a raw pointer, so the window is
        // mirrored into it here and the write paths below discard stores
        // to keep it behaving as ROM.
        std::copy(m_roms->main_cpu.begin() + 0x20000,
                  m_roms->main_cpu.begin() + 0x40000,
                  m_local_ram.begin() + 0x20000);
    }
    clear(m_work_ram);
    clear(m_geometry_ram);
    clear(m_geometry_program);
    clear(m_buffer_ram);
    clear(m_control_registers);
    clear(m_cpu_control);
    clear(m_tile_ram);
    clear(m_character_ram);
    clear(m_palette_ram);
    clear(m_color_translation);
    clear(m_z_clip);
    clear(m_communication_ram);
    clear(m_communication_control);
    load_nvram();
    m_nvram_dirty = false;
    m_last_nvram_save_frame = 0;
    clear(m_io_registers, 0xff);
    clear(m_uart_registers);
    clear(m_texture_ram_0);
    clear(m_texture_ram_1);
    clear(m_texture_sheet_0);
    clear(m_texture_sheet_1);
    clear(m_luma_ram);
    clear(m_luma_table);
    clear(m_framebuffer_a);
    clear(m_framebuffer_b);
    for (std::size_t offset = 0; offset < m_buffer_ram.size(); offset += 4) {
        m_buffer_ram[offset + 0] = 0x0f;
        m_buffer_ram[offset + 1] = 0x0f;
        m_buffer_ram[offset + 2] = 0x80;
        m_buffer_ram[offset + 3] = 0x07;
    }
    m_timer_values.fill(0x000fffff);
    m_timer_original.fill(0x000fffff);
    m_timer_running.fill(false);
    m_irq_request = 0;
    m_irq_enable = 0;
    m_irq_ack_latch = 0xffffffffU;
    m_frame_number = 0;
    m_comm_cn = 0;
    m_comm_fg = 0;
    m_comm_zfg = 0;
    m_comm_loopback = false;
    m_comm_peer_mode = false;
    m_comm_peer_seen = false;
    m_comm_link_alive = false;
    m_comm_link_timer = 0;
    m_comm_node_id = 0;
    m_comm_link_count = 0;
    m_comm_fg_poll_counter = 0;
    m_comm_local_port = 0;
    m_comm_peer_port = 0;
    m_comm_sequence = 0;
    m_comm_loopback_frame.fill(0);
    m_comm_loopback_frame_valid = false;
    ++m_video_generation;
    ++m_texture_generation;
    ++m_color_generation;
    m_inputs = {};
    m_io_port_values.fill(0xff);
    m_io_port_config = 0xff;
    m_io_mode = 0;
    m_io_analog_channel = 0;
    if (m_iob) m_iob->reset();
    m_iob_clock_accum = 0;
    m_uart_tx_ready = true;
    m_uart_expect_mode = true;
    m_uart_tx_enabled = false;
    m_uart_rx_enabled = false;
    m_uart_tx_empty_cycles = 0;
    m_uart_tx_empty = true;
    m_uart_tx_shift_active = false;
    m_uart_tx_holding_full = false;
    m_uart_tx_shift_data = 0;
    m_uart_tx_holding_data = 0;
    m_uart_rx_pending.store(false, std::memory_order_release);
    m_eeprom_control_mode = false;
    m_eeprom_data_out = true;
    m_eeprom_chip_select = false;
    m_eeprom_clock = false;
    m_eeprom_write_enabled = false;
    m_eeprom_write_pending = false;
    m_eeprom_address = 0;
    m_eeprom_command_bits = 0;
    m_eeprom_output_bits = 0;
    m_eeprom_write_bits = 0;
    m_eeprom_shift = 0;
    m_eeprom_output_shift = 0;
    m_eeprom_write_shift = 0;
    // The shifter powers up in neutral (gearbox code 0), matching MAME's
    // idle input state. Resting in first gear made the attract loop treat
    // the cabinet as occupied: it fired announcer speech within seconds,
    // cut the title music early and switched to the muted between-attract
    // piece, which read as "music and speech missing".
    m_gear = 0;
    m_shift_down_previous = false;
    m_shift_up_previous = false;
    m_render_control = 0;
    m_horizontal_sync = 0;
    m_vertical_sync = 0;
    m_horizontal_offset = 90;
    m_vertical_offset = -8;
    m_geo_host_write_latch = 0;
    m_geo_read_address = 0;
    m_geo_write_address = 0;
    m_geo_upload_count = 0;
    std::fill(m_tgp_data_ram.begin(), m_tgp_data_ram.end(), 0);
    m_tgp_fifo_in.clear();
    m_tgp_fifo_out.clear();
    m_tgp_host_write_latch = 0;
    m_tgp_host_read_latch = 0xffffffffU;
    m_tgp_program_index = 0;
    m_tgp_bank = 0;
    m_tgp_sincos_base = 0;
    m_tgp_atan_base.fill(0);
    m_tgp_inv_base = 0;
    m_tgp_isqrt_base = 0;
    m_tgp_boot_request = false;
    m_tgp_gpio0 = false;
    m_unmapped_reads = 0;
    m_unmapped_writes = 0;
    m_last_unmapped_read = 0;
    m_last_unmapped_write = 0;

    // A paired WhittyArcade launch represents two cabinets with their
    // communication boards physically attached. Bring the board up before
    // the game boots so the two processes can discover one another even when
    // an old battery-RAM image still says NOTLINK. Normal single-cabinet
    // launches have no MODEL2_COMM_NODE and retain the game's CN behaviour.
    if (environment_comm_node() != 0) {
        m_comm_cn = 1;
        initialize_comm_board();
    }
}

uint8_t model2_bus::read8(uint32_t address) {
    if (!m_roms) return 0xff;
    if (in_range(address, 0x00000000, m_roms->main_cpu.size()))
        return read_region(m_roms->main_cpu, address, 0x00000000);
    if (in_range(address, 0x00200000, m_local_ram.size()))
        return read_region(m_local_ram, address, 0x00200000);
    if (in_range(address, 0x00500000, m_work_ram.size()))
        return read_region(m_work_ram, address, 0x00500000);
    if (in_range(address, 0x00802008, 4))
        return dword_byte(m_geo_write_address, address);
    if (in_range(address, 0x00803008, 4))
        return dword_byte(m_geo_read_address, address);
    if (in_range(address, 0x00800000, m_geometry_ram.size()))
        return read_region(m_geometry_ram, address, 0x00800000, 0);
    if (in_range(address, 0x00804000, m_geometry_program.size()))
        return 0xff; // write-only geometry upload/data port
    if (address >= 0x00884000 && address <= 0x00887fff) {
        if ((address & 3) == 0) {
            m_tgp_host_read_latch = m_tgp_fifo_out.empty() ? 0xffffffffU :
                m_tgp_fifo_out.front();
            if (!m_tgp_fifo_out.empty()) m_tgp_fifo_out.pop_front();
        }
        return dword_byte(m_tgp_host_read_latch, address);
    }
    if (in_range(address, 0x00900000, 0x80000))
        return m_buffer_ram[(address - 0x00900000) & 0x1ffff];
    if (in_range(address, 0x00980004, 4))
        return dword_byte(m_tgp_fifo_out.empty() ? 1U : 0U, address);
    if (in_range(address, 0x0098000c, 4)) {
        const bool sixty_hz = (m_render_control & 4) != 0;
        const uint32_t frame_bit = sixty_hz ?
            ((m_frame_number & 1) << 2) :
            ((m_frame_number & 2) << 1);
        const uint32_t video_control = frame_bit | this->video_control();
        return dword_byte(video_control, address);
    }
    if (in_range(address, 0x00980000, m_control_registers.size()))
        return read_region(m_control_registers, address, 0x00980000, 0);
    if (in_range(address, 0x00e00000, m_cpu_control.size()))
        return read_region(m_cpu_control, address, 0x00e00000, 0);
    if (in_range(address, 0x00e80000, 4))
        return dword_byte(m_irq_request, address);
    if (in_range(address, 0x00e80004, 4))
        return dword_byte(m_irq_enable, address);
    if (in_range(address, 0x00f00000, 16))
        return dword_byte(m_timer_values[(address - 0x00f00000) >> 2],
                          address);

    // System 24 tile and character RAM have mirrors in the next 1 MiB.
    const uint32_t tile_address = address & ~0x00110000u;
    if (in_range(tile_address, 0x01000000, m_tile_ram.size()))
        return read_region(m_tile_ram, tile_address, 0x01000000, 0);
    const uint32_t character_address = address & ~0x00100000u;
    if (in_range(character_address, 0x01080000, m_character_ram.size()))
        return read_region(m_character_ram, character_address, 0x01080000, 0);

    if (in_range(address, 0x01800000, m_palette_ram.size()))
        return read_region(m_palette_ram, address, 0x01800000, 0);
    if (in_range(address, 0x01810000, m_color_translation.size()))
        return read_region(m_color_translation, address, 0x01810000, 0);
    if (in_range(address, 0x0181c000, m_z_clip.size()))
        return read_region(m_z_clip, address, 0x0181c000, 0);
    if (in_range(address, 0x01a00000, m_communication_ram.size()) ||
        in_range(address, 0x01a10000, m_communication_ram.size())) {
        const uint8_t value =
            m_communication_ram[(address - 0x01a00000) & 0x3fff];
        if (std::getenv("MODEL2_COMM_RW_TRACE")) {
            static unsigned reported = 0;
            if (reported < 4096) {
                ++reported;
                std::fprintf(stderr, "COMMR %08x=%02x pc=%08x f=%u\n",
                             address, value,
                             m_program_counter_probe ?
                                 m_program_counter_probe() : 0,
                             m_frame_number);
            }
        }
        return value;
    }
    if (address == 0x01a04000 || address == 0x01a14000)
        return static_cast<uint8_t>(m_comm_cn | 0xfe);
    if (address == 0x01a04002 || address == 0x01a14002) {
        // The board processes ring traffic while the game polls this
        // register (MAME m2comm read_fg does the same): a versus handshake
        // step sends a frame and spins on FG for the reply within the same
        // video frame. Polled at most every 8th read so a tight i960 wait
        // loop does not turn into a syscall storm.
        if (m_comm_peer_mode && m_comm_cn &&
            ++m_comm_fg_poll_counter >= 8) {
            m_comm_fg_poll_counter = 0;
            comm_peer_receive();
        }
        const uint8_t value = static_cast<uint8_t>(
            m_comm_fg | ((~m_comm_zfg & 1) << 7) | 0x7e);
        if (std::getenv("MODEL2_COMM_RW_TRACE")) {
            static unsigned reported = 0;
            if (reported < 512) {
                ++reported;
                std::fprintf(stderr, "COMMFG %08x=%02x pc=%08x f=%u\n",
                             address, value,
                             m_program_counter_probe ?
                                 m_program_counter_probe() : 0,
                             m_frame_number);
            }
        }
        return value;
    }
    if (m_iob && in_range(address, 0x01c00000, 0x1000)) {
        // Virtua Cop: the whole 0x01c00000 window is the model1io2 dual-port
        // RAM. Byte-wide on even i960 lanes (umask 0x00ff00ff), same layout as
        // the 315-5296 it replaces; odd lanes read back as zero.
        if (address & 1) return 0x00;
        const std::size_t index = (address - 0x01c00000) >> 1;
        return index < m_iob_dpram.size() ? m_iob_dpram[index] : 0xff;
    }
    if (in_range(address, 0x01c00040, 4))
        return 0; // write-only 2A-CRX board control
    if (in_range(address, 0x01c00000, 0x20)) {
        // The 315-5649 is byte-wide on lanes 0 and 2. An i960 LDOS reads
        // the active byte plus an electrically unused lane; MAME/hardware
        // returns zero for that lane, yielding 0x00fe for Coin 1. Returning
        // 0xff here produced 0xfffe and broke every firmware input compare.
        if (address & 1) return 0x00;
        const uint8_t offset = static_cast<uint8_t>(
            (address - 0x01c00000) >> 1);
        return io_read(offset);
    }
    if (in_range(address, 0x01c00020, m_io_registers.size() - 0x20))
        return read_region(m_io_registers, address, 0x01c00000, 0xff);
    if (in_range(address, 0x01c80000, m_uart_registers.size())) {
        // uPD71051C on the low byte of each 16-bit register. The sound link
        // is normally one-way.  TXRDY/TXEMPTY drop while a byte is shifting;
        // Sega Rally uses their rising edge as interrupt source 10 to send
        // the next byte of each MIDI command.
        if ((address & 2) != 0) {
            // Status register: bit0 TXRDY, bit1 RXRDY, bit2 TXEMPTY. The
            // sound board answers commands over SCSP MIDI-out, so RXRDY must
            // reflect bytes queued via sound_midi_receive.
            // The on-board uPD71051 has DSR asserted (active-low input), so
            // its status bit 7 reads high. Sega Rally treats a missing DSR
            // as a disconnected/failed sound board and eventually reports
            // SOUND BUFFER FULL / ROM ERROR. MAME reports 0x85 when idle.
            uint8_t status = static_cast<uint8_t>(0x80 |
                (m_uart_tx_ready ? 0x01 : 0x00) |
                (m_uart_tx_empty ? 0x04 : 0x00));
            if (m_uart_rx_pending.load(std::memory_order_acquire))
                status |= 0x02;
            static const bool trace =
                std::getenv("MODEL2_UART_TRACE") != nullptr;
            if (trace && (address & 1) == 0) {
                static uint64_t reads = 0;
                if (reads < 32 || (reads & 0x3ff) == 0)
                    std::fprintf(stderr,
                                 "Model 2 UART status poll #%llu value=%02x\n",
                                 static_cast<unsigned long long>(reads),
                                 status);
                ++reads;
            }
            return (address & 1) ? 0x00 : status;
        }
        if (address & 1) return 0x00;
        if (m_uart_rx_pending.load(std::memory_order_acquire)) {
            m_uart_rx_pending.store(false, std::memory_order_release);
            return m_uart_rx_data.load(std::memory_order_relaxed);
        }
        return m_uart_registers[0];
    }
    if (in_range(address, 0x01d00000, m_backup_ram.size())) {
        return read_region(m_backup_ram, address, 0x01d00000, 0xff);
    }
    if (in_range(address, 0x02000000, 0x2000000))
        return read_region(m_roms->main_data, address, 0x02000000);
    if (in_range(address, 0x06000000, 0x1000000))
        return read_region(m_roms->main_data, address, 0x05000000);
    if (address >= 0x10000000 && address <= 0x101fffff)
        return dword_byte(m_render_control, address);
    if (address >= 0x10200000 && address <= 0x105fffff)
        return 0;
    // Model 2A exposes the polygon count at this address. Returning zero is
    // the power-on/no-render-work value until the rasterizer supplies a real
    // count; games poll it while building their first display lists.
    if (address >= 0x10800000 && address <= 0x10800003)
        return 0;
    if (in_range(address, 0x11600000, m_framebuffer_a.size()))
        return read_region(m_framebuffer_a, address, 0x11600000, 0);
    if (in_range(address, 0x11680000, m_framebuffer_b.size()))
        return read_region(m_framebuffer_b, address, 0x11680000, 0);
    if (in_range(address, 0x12000000, 0x400000))
        return m_texture_ram_0[(address - 0x12000000) & 0x1fffff];
    if (in_range(address, 0x12400000, 0x400000))
        return m_texture_ram_1[(address - 0x12400000) & 0x1fffff];
    if (in_range(address, 0x12800000, m_luma_ram.size()))
        return read_region(m_luma_ram, address, 0x12800000, 0);

    ++m_unmapped_reads;
    m_last_unmapped_read = address;
    if (m_unmapped_callback && m_unmapped_reads <= 16)
        m_unmapped_callback(false, address);
    return 0;
}

uint32_t model2_bus::read32(uint32_t address) {
    const auto contained = [address](uint32_t base, std::size_t size) {
        return address >= base &&
               static_cast<uint64_t>(address) - base + 3 < size;
    };
    const auto region = [address](const std::vector<uint8_t>& bytes,
                                  uint32_t base) {
        return read_dword(bytes, static_cast<std::size_t>(address - base));
    };

    if (m_iob && in_range(address, 0x01c00000, 0x1000)) {
        // model1io2 dual-port RAM: two bytes per 32-bit word on lanes 0 and 2.
        const std::size_t index = (address - 0x01c00000) >> 1;
        const uint32_t low = index < m_iob_dpram.size() ?
            m_iob_dpram[index] : 0xff;
        const uint32_t high = index + 1 < m_iob_dpram.size() ?
            m_iob_dpram[index + 1] : 0xff;
        return low | (high << 16);
    }

    if (m_roms && contained(0x00000000, m_roms->main_cpu.size()))
        return region(m_roms->main_cpu, 0x00000000);
    if (contained(0x00200000, m_local_ram.size()))
        return region(m_local_ram, 0x00200000);
    if (contained(0x00500000, m_work_ram.size()))
        return region(m_work_ram, 0x00500000);
    if (in_range(address, 0x00900000, 0x80000)) {
        const std::size_t offset = (address - 0x00900000) & 0x1ffff;
        if (offset + 3 < m_buffer_ram.size())
            return read_dword(m_buffer_ram, offset);
    }
    if (m_roms && contained(0x02000000, m_roms->main_data.size()))
        return region(m_roms->main_data, 0x02000000);
    if (m_roms && address >= 0x06000000) {
        const uint64_t offset = static_cast<uint64_t>(address) - 0x05000000;
        if (offset + 3 < m_roms->main_data.size())
            return read_dword(m_roms->main_data,
                              static_cast<std::size_t>(offset));
    }

    return static_cast<uint32_t>(read8(address)) |
           (static_cast<uint32_t>(read8(address + 1)) << 8) |
           (static_cast<uint32_t>(read8(address + 2)) << 16) |
           (static_cast<uint32_t>(read8(address + 3)) << 24);
}

void model2_bus::write32(uint32_t address, uint32_t value) {
    const auto contained = [address](uint32_t base, std::size_t size) {
        return address >= base &&
               static_cast<uint64_t>(address) - base + 3 < size;
    };
    if (m_iob && in_range(address, 0x01c00000, 0x1000)) {
        const std::size_t index = (address - 0x01c00000) >> 1;
        if (index < m_iob_dpram.size())
            m_iob_dpram[index] = static_cast<uint8_t>(value);
        if (index + 1 < m_iob_dpram.size())
            m_iob_dpram[index + 1] = static_cast<uint8_t>(value >> 16);
        return;
    }
    if (contained(0x00200000, m_local_ram.size())) {
        // The upper half is a ROM window on original Model 2 boards.
        if (m_profile.original_model2 && address >= 0x00220000) return;
        write_dword(m_local_ram, address - 0x00200000, value);
        return;
    }
    if (contained(0x00500000, m_work_ram.size())) {
        if (const uint32_t watch = watched_work_address()) {
            // Report the whole 16-byte block: game variables travel in small
            // structures, and the neighbours identify what a field means.
            static unsigned reported = 0;
            if ((address & ~0xfU) == (watch & ~0xfU) && reported < 4096) {
                ++reported;
                std::printf("WORKWRITE addr=%08x value=%08x pc=%08x\n",
                            address, value,
                            m_program_counter_probe ?
                                m_program_counter_probe() : 0);
            }
        }
        write_dword(m_work_ram, address - 0x00500000, value);
        return;
    }
    if (in_range(address, 0x00900000, 0x80000)) {
        const std::size_t offset = (address - 0x00900000) & 0x1ffff;
        if (offset + 3 < m_buffer_ram.size()) {
            trace_geometry_write("i960", offset, value);
            write_dword(m_buffer_ram, offset, value);
            return;
        }
    }

    write8(address, static_cast<uint8_t>(value));
    write8(address + 1, static_cast<uint8_t>(value >> 8));
    write8(address + 2, static_cast<uint8_t>(value >> 16));
    write8(address + 3, static_cast<uint8_t>(value >> 24));
}

void model2_bus::write16(uint32_t address, uint16_t value) {
    // A halfword store is one bus transaction. The TGP command and argument
    // FIFO consumes one word per transaction, so splitting a 16-bit store
    // into byte-lane writes - which only push on lane 3 - swallows the word
    // outright. Daytona's versus handshake pushes a 16-bit argument exactly
    // this way (stis to 0x00884000) and then deadlocks against the TGP
    // waiting for results that can never come.
    if (address >= 0x00880000 && address <= 0x00887fff) {
        const uint32_t data = value;
        if (address < 0x00884000) {
            const uint32_t function =
                (((address & ~3U) - 0x00880000) >> 4) & 0xff;
            const uint32_t command =
                (data & 0x800fffffU) | (function << 23);
            m_tgp_fifo_in.push_back(command);
            if (trace_fifo_words(m_frame_number))
                std::fprintf(stderr, "%u IN %08x (halfword)\n",
                             m_frame_number, command);
        } else if (read_dword(m_control_registers, 0) & 0x80000000U) {
            write_dword(m_geometry_program, m_tgp_program_index * 4, data);
            ++m_tgp_program_index;
        } else {
            m_tgp_fifo_in.push_back(data);
            if (trace_fifo_words(m_frame_number))
                std::fprintf(stderr, "%u IN %08x (halfword)\n",
                             m_frame_number, data);
        }
        return;
    }
    write8(address, static_cast<uint8_t>(value));
    write8(address + 1, static_cast<uint8_t>(value >> 8));
}

void model2_bus::write8(uint32_t address, uint8_t value) {
    if (!m_roms) return;
    if (const uint32_t watch = watched_work_address()) {
        static unsigned reported = 0;
        if ((address & ~3U) == (watch & ~3U) && reported < 256) {
            ++reported;
            std::printf("WORKWRITE8 addr=%08x value=%02x pc=%08x\n",
                        address, value,
                        m_program_counter_probe ?
                            m_program_counter_probe() : 0);
        }
    }
    // The upper half of local RAM is a ROM window on original Model 2.
    if (m_profile.original_model2 &&
        in_range(address, 0x00220000, 0x20000))
        return;
    if (write_region(m_local_ram, address, 0x00200000, value) ||
        write_region(m_work_ram, address, 0x00500000, value))
        return;
    if (in_range(address, 0x00800000, m_geometry_ram.size())) {
        write_region(m_geometry_ram, address, 0x00800000, value);
        set_dword_byte(m_geo_host_write_latch, address, value);
        if ((address & 3) != 3) return;
        const uint32_t port = (address & ~3U) - 0x00800000;
        if (port < 0x1000) {
            if (m_geo_host_write_latch & 0x80000000U) {
                push_geometry_word(
                    (m_geo_host_write_latch & 0x800fffffU) |
                    (((port >> 4) & 0x3f) << 23));
            } else if ((port & 0xf) == 0) {
                uint32_t command = (m_geo_host_write_latch & 0x000fffffU) |
                    (((port >> 4) & 0x3f) << 23);
                if (port & 0xc00) {
                    const uint8_t function = (port >> 4) & 0x3f;
                    if (function == 1)
                        command |= ((port >> 10) & 3) << 29;
                }
                push_geometry_word(command);
            }
        } else if (port == 0x1008) {
            m_geo_write_address = m_geo_host_write_latch & 0xfffff;
            if (std::getenv("MODEL2_GEOMETRY_TRACE"))
                std::fprintf(stderr,
                             "Model 2 geometry frame=%u write-start=%05x\n",
                             m_frame_number, m_geo_write_address);
        } else if (port == 0x3008) {
            m_geo_read_address = m_geo_host_write_latch & 0xfffff;
            if (std::getenv("MODEL2_GEOMETRY_TRACE"))
                std::fprintf(stderr,
                             "Model 2 geometry frame=%u read-start=%05x\n",
                             m_frame_number, m_geo_read_address);
        }
        return;
    }
    if (in_range(address, 0x00804000, 0x4000)) {
        set_dword_byte(m_geo_host_write_latch, address, value);
        if ((address & 3) == 3) {
            if (read_dword(m_control_registers, 8) & 0x80000000U)
                ++m_geo_upload_count;
            else
                push_geometry_word(m_geo_host_write_latch);
        }
        return;
    }
    if (address >= 0x00880000 && address <= 0x00887fff) {
        set_dword_byte(m_tgp_host_write_latch, address, value);
        if ((address & 3) != 3) return;

        if (address < 0x00884000) {
            const uint32_t function =
                (((address & ~3U) - 0x00880000) >> 4) & 0xff;
            const uint32_t command =
                (m_tgp_host_write_latch & 0x800fffffU) |
                (function << 23);
            m_tgp_fifo_in.push_back(command);
            if (trace_fifo_words(m_frame_number))
                std::fprintf(stderr, "%u IN %08x\n", m_frame_number,
                             command);
            if (std::getenv("MODEL2_FIFO_TRACE") &&
                m_tgp_fifo_in.size() > tgp_fifo_capacity)
                std::fprintf(stderr,
                             "Model 2 TGP input overflow frame=%u size=%zu\n",
                             m_frame_number, m_tgp_fifo_in.size());
        } else if (read_dword(m_control_registers, 0) & 0x80000000U) {
            write_dword(m_geometry_program, m_tgp_program_index * 4,
                        m_tgp_host_write_latch);
            ++m_tgp_program_index;
        } else {
            m_tgp_fifo_in.push_back(m_tgp_host_write_latch);
            if (trace_fifo_words(m_frame_number))
                std::fprintf(stderr, "%u IN %08x\n", m_frame_number,
                             m_tgp_host_write_latch);
            if (std::getenv("MODEL2_FIFO_TRACE") &&
                m_tgp_fifo_in.size() > tgp_fifo_capacity)
                std::fprintf(stderr,
                             "Model 2 TGP input overflow frame=%u size=%zu\n",
                             m_frame_number, m_tgp_fifo_in.size());
        }
        return;
    }
    if (in_range(address, 0x00900000, 0x80000)) {
        m_buffer_ram[(address - 0x00900000) & 0x1ffff] = value;
        return;
    }
    if (in_range(address, 0x00980000, 4)) {
        const uint32_t old_control = read_dword(m_control_registers, 0);
        write_region(m_control_registers, address, 0x00980000, value);
        if ((address & 3) == 3) {
            const uint32_t control = read_dword(m_control_registers, 0);
            if ((old_control ^ control) & 0x80000000U) {
                if (control & 0x80000000U)
                    m_tgp_program_index = 0;
                else
                    m_tgp_boot_request = true;
            }
        }
        return;
    }
    if (write_region(m_control_registers, address, 0x00980000, value) ||
        write_region(m_cpu_control, address, 0x00e00000, value))
        return;
    if (in_range(address, 0x00e80000, 4)) {
        const uint32_t before = m_irq_request;
        set_dword_byte(m_irq_ack_latch, address, value);
        m_irq_request &= m_irq_ack_latch;
        if (std::getenv("MODEL2_UART_TRACE") &&
            ((before ^ m_irq_request) & (1U << 10)))
            std::fprintf(stderr,
                         "Model 2 UART IRQ ack value=%08x req=%08x\n",
                         m_irq_ack_latch, m_irq_request);
        return;
    }
    if (in_range(address, 0x00e80004, 4)) {
        const uint32_t before = m_irq_enable;
        set_dword_byte(m_irq_enable, address, value);
        // No retroactive raise: the request latch registers TXRDY/RXRDY
        // EDGES that occur while the source is enabled, exactly like
        // MAME's sound_ready_w. Re-checking levels on an enable write
        // handed Virtua Fighter 2 an interrupt from a TXRDY that had been
        // sitting high since UART configuration, and its table still
        // pointed that vector at the unexpected-interrupt stub.
        if (std::getenv("MODEL2_UART_TRACE") &&
            ((before ^ m_irq_enable) & (1U << 10)))
            std::fprintf(stderr,
                         "Model 2 UART IRQ enable=%08x req=%08x ready=%d\n",
                         m_irq_enable, m_irq_request, m_uart_tx_ready);
        return;
    }
    if (in_range(address, 0x00f00000, 16)) {
        const unsigned timer = (address - 0x00f00000) >> 2;
        set_dword_byte(m_timer_values[timer], address, value);
        m_timer_original[timer] = m_timer_values[timer];
        m_timer_running[timer] = true;
        return;
    }

    const uint32_t tile_address = address & ~0x00110000u;
    if (write_video_region(m_tile_ram, tile_address, 0x01000000, value,
                           m_video_generation)) return;
    const uint32_t character_address = address & ~0x00100000u;
    if (write_video_region(m_character_ram, character_address, 0x01080000,
                           value, m_video_generation))
        return;
    const uint32_t video_register = address & ~0x00100000U;
    if (video_register >= 0x01040000 && video_register <= 0x01040001) {
        const unsigned shift = (video_register & 1) * 8;
        m_horizontal_sync = static_cast<uint16_t>(
            (m_horizontal_sync & ~(0xffU << shift)) |
            (static_cast<uint16_t>(value) << shift));
        m_horizontal_offset = static_cast<int16_t>(
            84 + static_cast<int16_t>(m_horizontal_sync));
        return;
    }
    if (video_register >= 0x01060000 && video_register <= 0x01060001) {
        const unsigned shift = (video_register & 1) * 8;
        m_vertical_sync = static_cast<uint16_t>(
            (m_vertical_sync & ~(0xffU << shift)) |
            (static_cast<uint16_t>(value) << shift));
        m_vertical_offset = static_cast<int16_t>(
            130 + static_cast<int16_t>(m_vertical_sync));
        return;
    }
    if ((address >= 0x01020000 && address <= 0x01070003) ||
        (address >= 0x01120000 && address <= 0x01170003))
        return;

    if (m_iob && in_range(address, 0x01c00000, 0x1000)) {
        // Virtua Cop: model1io2 dual-port RAM, even i960 lanes only.
        if ((address & 1) == 0) {
            const std::size_t index = (address - 0x01c00000) >> 1;
            if (index < m_iob_dpram.size()) m_iob_dpram[index] = value;
        }
        return;
    }

    if (in_range(address, 0x01c00040, 4))
        return; // write-only 2A-CRX board control

    if (in_range(address, 0x01c00000, 0x20)) {
        if ((address & 1) == 0) {
            const uint8_t offset = static_cast<uint8_t>(
                (address - 0x01c00000) >> 1);
            io_write(offset, value);
        }
        return;
    }

    if (in_range(address, 0x01800000, m_palette_ram.size())) {
        const uint64_t before = m_video_generation;
        write_video_region(m_palette_ram, address, 0x01800000, value,
                           m_video_generation);
        if (before != m_video_generation) ++m_color_generation;
        return;
    }
    if (in_range(address, 0x01810000, m_color_translation.size())) {
        const uint64_t before = m_video_generation;
        write_video_region(m_color_translation, address, 0x01810000, value,
                           m_video_generation);
        if (before != m_video_generation) ++m_color_generation;
        return;
    }
    if (in_range(address, 0x01d00000, m_backup_ram.size())) {
        uint8_t& destination =
            m_backup_ram[static_cast<std::size_t>(address - 0x01d00000)];
        if (destination != value) {
            destination = value;
            m_nvram_dirty = true;
        }
        return;
    }
    if (write_region(m_z_clip, address, 0x0181c000, value) ||
        write_region(m_io_registers, address, 0x01c00000, value) ||
        write_region(m_framebuffer_a, address, 0x11600000, value) ||
        write_region(m_framebuffer_b, address, 0x11680000, value))
        return;
    if (in_range(address, 0x01c80000, m_uart_registers.size())) {
        m_uart_registers[address - 0x01c80000] = value;
        if ((address & 3) == 0) {
            if (std::getenv("MODEL2_UART_TRACE"))
                std::fprintf(stderr,
                             "Model 2 UART TX %02x enable=%08x req=%08x\n",
                             value, m_irq_enable, m_irq_request);
            // 31.25 kbaud, ten serial bits per byte, on a 25 MHz main CPU.
            // Do not deliver the byte here: the SCSP sees it only after all
            // ten serial bits.  The old immediate callback compressed packet
            // bursts by about 10x and overran Sega Rally's sound command
            // parser even though the status flags looked plausible.
            // A data write clears TXRDY. If the shift register is idle, the
            // 8251 transfers the byte into it immediately and reasserts
            // TXRDY in the same operation (MAME i8251::check_for_tx_start).
            // Only a write behind an active character remains not-ready.
            m_uart_tx_ready = false;
            m_irq_request &= ~(1U << 10);
            if (!m_uart_tx_shift_active) {
                m_uart_tx_shift_active = true;
                m_uart_tx_shift_data = value;
                m_uart_tx_empty_cycles = 8000;
                m_uart_tx_ready = true;
                if (m_irq_enable & (1U << 10))
                    m_irq_request |= 1U << 10;
            } else {
                // Software writes only while TXRDY is asserted.  Keep one
                // hardware holding byte; it cannot enter the shifter until
                // the character already on the wire has completed.
                m_uart_tx_holding_data = value;
                m_uart_tx_holding_full = true;
            }
            m_uart_tx_empty = false;
        } else if ((address & 3) == 2) {
            if (std::getenv("MODEL2_UART_TRACE"))
                std::fprintf(stderr, "Model 2 UART control %02x\n", value);
            // 8251 programming sequence: the first control write after a
            // reset is the mode word, everything after is a command whose
            // bit 0 enables the transmitter, bit 2 the receiver and bit 6
            // returns to the mode phase. Until TxEN, TXRDY must not raise
            // the sound interrupt: Virtua Fighter 2 writes its interrupt
            // mask before programming the UART, and an immediate vector
            // from a reset-state "ready" transmitter lands in its
            // unexpected-interrupt stub, which prints Interrupt Halt over
            // the whole attract.
            if (m_uart_expect_mode) {
                m_uart_expect_mode = false;
            } else if (value & 0x40) {
                m_uart_expect_mode = true;
                m_uart_tx_enabled = false;
                m_uart_rx_enabled = false;
            } else {
                m_uart_tx_enabled = (value & 0x01) != 0;
                m_uart_rx_enabled = (value & 0x04) != 0;
                if (m_uart_tx_enabled && m_uart_tx_ready &&
                    (m_irq_enable & (1U << 10)))
                    m_irq_request |= 1U << 10;
            }
        }
        return;
    }
    if (in_range(address, 0x01a00000, m_communication_ram.size()) ||
        in_range(address, 0x01a10000, m_communication_ram.size())) {
        m_communication_ram[(address - 0x01a00000) & 0x3fff] = value;
        return;
    }
    if (address == 0x01a04000 || address == 0x01a14000) {
        if (std::getenv("MODEL2_COMM_TRACE"))
            std::fprintf(stderr,
                         "Model 2 comm CN=%u frame=%u (was %u)\n",
                         value & 1, m_frame_number, m_comm_cn);
        const bool enabling = !m_comm_cn && (value & 1);
        m_comm_cn = value & 1;
        if (!m_comm_cn) {
            close_comm_peer();
            m_comm_fg = 0;
            m_comm_zfg = 0;
            m_comm_loopback = false;
            m_comm_peer_mode = false;
            m_comm_peer_seen = false;
            m_comm_link_alive = false;
            m_comm_link_timer = 0;
            m_comm_loopback_frame_valid = false;
        } else if (enabling) {
            initialize_comm_board();
        }
        return;
    }
    if (address == 0x01a04002 || address == 0x01a14002) {
        if (std::getenv("MODEL2_COMM_RW_TRACE"))
            std::fprintf(stderr, "COMMFGW %02x pc=%08x f=%u\n", value,
                         m_program_counter_probe ?
                             m_program_counter_probe() : 0,
                         m_frame_number);
        m_comm_fg = value & 1;
        return;
    }
    if (in_range(address, 0x12000000, 0x400000)) {
        const uint32_t host_offset = (address - 0x12000000) & 0x1fffff;
        m_texture_ram_0[host_offset] = value;
        // Each pair of i960 dwords supplies the low 16 bits of one rasterizer
        // texture word. Byte and halfword stores are legal here, so publish
        // either decoded byte as soon as its lane is written; waiting for
        // lane 3 silently loses low-halfword-only animation updates.
        const uint32_t lane = host_offset & 3;
        if (lane < 2) {
            const uint32_t input_word = host_offset >> 2;
            const std::size_t output =
                static_cast<std::size_t>(input_word >> 1) * 4 +
                ((input_word & 1) ? 2 : 0);
            if (output + lane < m_texture_sheet_0.size() &&
                m_texture_sheet_0[output + lane] != value) {
                m_texture_sheet_0[output + lane] = value;
                ++m_texture_generation;
            }
        }
        return;
    }
    if (in_range(address, 0x12400000, 0x400000)) {
        const uint32_t host_offset = (address - 0x12400000) & 0x1fffff;
        m_texture_ram_1[host_offset] = value;
        const uint32_t lane = host_offset & 3;
        if (lane < 2) {
            const uint32_t input_word = host_offset >> 2;
            const std::size_t output =
                static_cast<std::size_t>(input_word >> 1) * 4 +
                ((input_word & 1) ? 2 : 0);
            if (output + lane < m_texture_sheet_1.size() &&
                m_texture_sheet_1[output + lane] != value) {
                m_texture_sheet_1[output + lane] = value;
                ++m_texture_generation;
            }
        }
        return;
    }
    if (in_range(address, 0x12800000, m_luma_ram.size())) {
        const std::size_t host_offset = address - 0x12800000;
        if (m_luma_ram[host_offset] != value) {
            m_luma_ram[host_offset] = value;
            ++m_color_generation;
        }
        if ((address & 3) == 0)
            m_luma_table[host_offset >> 2] = value;
        return;
    }
    if (address >= 0x10000000 && address <= 0x101fffff) {
        set_dword_byte(m_render_control, address, value);
        return;
    }
    if (address >= 0x10200000 && address <= 0x115fffff)
        return;

    ++m_unmapped_writes;
    m_last_unmapped_write = address;
    if (m_unmapped_callback && m_unmapped_writes <= 16)
        m_unmapped_callback(true, address);
}

void model2_bus::set_inputs(const input_state& state) {
    m_inputs = state;
    if (state.shift_down && !m_shift_down_previous && m_gear > 0)
        --m_gear;
    if (state.shift_up && !m_shift_up_previous && m_gear < 4)
        ++m_gear;
    m_shift_down_previous = state.shift_down;
    m_shift_up_previous = state.shift_up;
    if (m_iob) {
        if (m_profile.io == model2_game_profile::io_kind::model1io_dpram) {
            static constexpr std::array<uint8_t, 5> gear_code{0, 2, 1, 6, 5};
            m_iob->set_daytona_inputs(state, gear_code[m_gear]);
        } else {
            m_iob->set_gun_inputs(state);
        }
    }
    if (std::getenv("MODEL2_INPUT_TRACE") &&
        (state.coin1 || state.start || state.gas || state.brake ||
         state.steering != 0x800))
        std::fprintf(stderr,
                     "Model 2 input frame=%u coin=%d start=%d steer=%03x "
                     "gas=%03x brake=%03x\n",
                     m_frame_number, state.coin1, state.start,
                     state.steering, state.gas, state.brake);
}

uint8_t model2_bus::io_read(uint8_t offset) {
    // 315-5296 light-gun cabinets (Virtua Cop 2): coins/start use the base
    // model2 IN0 bits (Sega Rally moves START to 0x40), triggers are on IN1,
    // and the guns arrive through the serial-channel-2 mux at registers
    // 0x0a/0x0c rather than the wheel/pedal ADC.
    const bool gun = m_profile.io == model2_game_profile::io_kind::crx_gun;
    const bool fighter =
        m_profile.io == model2_game_profile::io_kind::crx_fighter;
    const bool bike =
        m_profile.io == model2_game_profile::io_kind::crx_bike;
    // Virtua Fighter 2: punch/kick/guard on bits 0-2, the 8-way stick on
    // bits 4-7 (down/up/right/left), one pad per port, all active low.
    const auto fighter_pad = [](const uint8_t* buttons, uint8_t x,
                                uint8_t y) {
        uint8_t value = 0xff;
        if (buttons[0]) value &= ~uint8_t{0x01}; // Punch
        if (buttons[1]) value &= ~uint8_t{0x02}; // Kick
        if (buttons[2]) value &= ~uint8_t{0x04}; // Guard
        if (y > 0xaf) value &= ~uint8_t{0x10};   // Down
        if (y < 0x50) value &= ~uint8_t{0x20};   // Up
        if (x > 0xaf) value &= ~uint8_t{0x40};   // Right
        if (x < 0x50) value &= ~uint8_t{0x80};   // Left
        return value;
    };
    const auto gun_axis = [this](unsigned port) -> uint16_t {
        // 10-bit ADC ranges from MAME's vcop2 light-gun calibration; port
        // order matches m_lightgun_ports {P1_Y, P1_X, P2_Y, P2_X}.
        const auto scale = [](uint8_t v, int lo, int hi) {
            return static_cast<uint16_t>(lo + v * (hi - lo) / 255);
        };
        switch (port) {
        case 0: return scale(m_inputs.left_stick_y, 36, 425);   // P1_Y
        case 1: return scale(m_inputs.left_stick_x, 137, 630);  // P1_X
        case 2: return scale(m_inputs.p2_stick_y, 36, 425);     // P2_Y
        default: return scale(m_inputs.p2_stick_x, 134, 627);   // P2_X
        }
    };
    const auto analog_scale = [](uint16_t value, uint16_t maximum) {
        // The 8-bit ADC rounds to the nearest code.  Truncation maps the
        // exact steering midpoint to 0x7f; Sega Rally then mirrors that to
        // 0x81 and treats a centred wheel as a small right turn.  Apart from
        // making the car drift, that changes the first opponent-path setup
        // and prevents the rival cars from being activated.
        return static_cast<uint8_t>(std::min<uint32_t>(
            255, (static_cast<uint32_t>(value) * 255 + maximum / 2) /
                     maximum));
    };
    const auto input_port = [this, gun, fighter, bike,
                             &fighter_pad](unsigned port) {
        if (port == 1) {
            uint8_t value = 0xff;
            if (m_inputs.coin1) value &= ~uint8_t{0x01};
            if (m_inputs.coin2) value &= ~uint8_t{0x02};
            if (m_inputs.test) value &= ~uint8_t{0x04};
            if (m_inputs.service) value &= ~uint8_t{0x08};
            if (gun || fighter) {
                if (m_inputs.start) value &= ~uint8_t{0x10};    // START1
                if (m_inputs.p2_start) value &= ~uint8_t{0x20}; // START2
            } else {
                if (m_inputs.view) value &= ~uint8_t{0x20};
                if (m_inputs.start) value &= ~uint8_t{0x40};
            }
            // Port A bit 0 multiplexes the upper nibble of port B onto the
            // serial EEPROM pins. Coin/test/service stay live in the lower
            // nibble; start is visible again as soon as the transaction ends.
            if (m_eeprom_control_mode)
                value = static_cast<uint8_t>(0xd0 |
                    (m_eeprom_data_out ? 0x20 : 0x00) | (value & 0x0f));
            if (std::getenv("MODEL2_INPUT_TRACE") &&
                (m_inputs.coin1 || m_inputs.start))
                std::fprintf(stderr,
                             "Model 2 port B frame=%u value=%02x dir=%02x "
                             "eeprom=%d\n",
                             m_frame_number, value, m_io_port_config,
                             m_eeprom_control_mode);
            return value;
        }
        if (port == 2) {
            if (bike) {
                // Manx TT IN1: shift up 0x10, shift down 0x20, active low.
                uint8_t value = 0xff;
                if (m_inputs.shift_up) value &= ~uint8_t{0x10};
                if (m_inputs.shift_down) value &= ~uint8_t{0x20};
                return value;
            }
            if (fighter)
                return fighter_pad(m_inputs.buttons, m_inputs.left_stick_x,
                                   m_inputs.left_stick_y);
            if (gun) {
                // IN1: P1/P2 triggers (active low), rest unused.
                uint8_t value = 0xff;
                if (m_inputs.buttons[0]) value &= ~uint8_t{0x01};
                if (m_inputs.p2_buttons[0]) value &= ~uint8_t{0x02};
                return value;
            }
            static constexpr std::array<uint8_t, 5> gear_code{0, 2, 1, 6, 5};
            return static_cast<uint8_t>(0x8f | (gear_code[m_gear] << 4));
        }
        if (port == 3) {
            if (bike) return uint8_t{0xff}; // IN2 unused
            if (fighter)
                return fighter_pad(m_inputs.p2_buttons, m_inputs.p2_stick_x,
                                   m_inputs.p2_stick_y);
            if (gun) return uint8_t{0xff}; // IN2: DIP bank, all off
            return m_inputs.view4 ? uint8_t{0xff} : uint8_t{0x00};
        }
        return uint8_t{0xff};
    };

    if (offset <= 6) {
        const uint8_t value = ((m_io_port_config >> offset) & 1) ?
            input_port(offset) : m_io_port_values[offset];
        return value;
    }
    if (gun && offset == 0x0c) {
        // Serial-channel-2 read = light-gun mux (lightgun_mux_r): a coordinate
        // byte for mux 0..7, otherwise the off-screen/reload status.
        if (m_lightgun_mux < 8) {
            const uint16_t axis = gun_axis(m_lightgun_mux >> 1);
            return static_cast<uint8_t>(
                (m_lightgun_mux & 1) ? (axis >> 8) : axis);
        }
        uint8_t data = 0xfc;
        if (m_inputs.buttons[1]) data |= 0x01;    // P1 off-screen / reload
        if (m_inputs.p2_buttons[1]) data |= 0x02; // P2 off-screen / reload
        return data;
    }
    if (offset == 0x0d) return 0x0c;
    if (offset == 0x0f) {
        uint8_t value = 0xff;
        if (fighter) {
            // No ADC use on the fighter cabinet.
        } else if (bike) {
            // Manx TT: channel 0 throttle, 1 brake, 2 bank (the handlebar
            // paddle, reversed on the hardware, resting at centre).
            switch (m_io_analog_channel) {
            case 0:
                value = analog_scale(std::min<uint16_t>(m_inputs.gas, 0x610),
                                     0x610);
                break;
            case 1:
                value = analog_scale(
                    std::min<uint16_t>(m_inputs.brake, 0x610), 0x610);
                break;
            case 2: {
                const uint16_t steering = std::clamp<uint16_t>(
                    m_inputs.steering, 0x280, 0xd80);
                value = static_cast<uint8_t>(255 - analog_scale(
                    static_cast<uint16_t>(steering - 0x280), 0xb00));
                break;
            }
            default:
                break;
            }
        } else switch (m_io_analog_channel) {
        case 0: {
            const uint16_t steering = std::clamp<uint16_t>(
                m_inputs.steering, 0x280, 0xd80);
            value = analog_scale(static_cast<uint16_t>(steering - 0x280),
                                 0xb00);
            break;
        }
        case 1:
            value = analog_scale(std::min<uint16_t>(m_inputs.gas, 0x610),
                                 0x610);
            break;
        case 2:
            value = analog_scale(std::min<uint16_t>(m_inputs.brake, 0x610),
                                 0x610);
            break;
        default:
            break;
        }
        m_io_analog_channel = (m_io_analog_channel + 1) & 7;
        return value;
    }
    return 0xff;
}

void model2_bus::io_write(uint8_t offset, uint8_t value) {
    if (offset <= 6) {
        m_io_port_values[offset] = value;
        if (offset == 0) eeprom_write_lines(value);
        return;
    }
    // 315-5296 light-gun cabinets select the gun axis through serial channel 2.
    if (offset == 0x0a &&
        m_profile.io == model2_game_profile::io_kind::crx_gun) {
        m_lightgun_mux = value;
        return;
    }
    if (offset == 0x08) {
        m_io_port_config = value;
        return;
    }
    if (offset == 0x0e) {
        m_io_mode = value;
        return;
    }
    if (offset == 0x0f)
        m_io_analog_channel = value & 7;
}

void model2_bus::eeprom_write_lines(uint8_t value) {
    m_eeprom_control_mode = (value & 0x01) != 0;
    const bool data_in = (value & 0x20) != 0;
    const bool chip_select = (value & 0x40) != 0;
    const bool clock = (value & 0x80) != 0;

    if (!chip_select) {
        m_eeprom_command_bits = 0;
        m_eeprom_output_bits = 0;
        m_eeprom_write_bits = 0;
        m_eeprom_write_pending = false;
        m_eeprom_shift = 0;
        m_eeprom_data_out = true;
    } else if (!m_eeprom_clock && clock) {
        eeprom_clock_rising(data_in);
    }
    m_eeprom_chip_select = chip_select;
    m_eeprom_clock = clock;
}

void model2_bus::eeprom_clock_rising(bool data_in) {
    if (m_eeprom_output_bits) {
        --m_eeprom_output_bits;
        if (m_eeprom_output_bits) {
            m_eeprom_data_out =
                ((m_eeprom_output_shift >> (m_eeprom_output_bits - 1)) & 1) != 0;
        } else {
            // 93C46 sequential-read mode continues into the next word.
            m_eeprom_address = (m_eeprom_address + 1) & 0x3f;
            m_eeprom_output_shift = m_eeprom_words[m_eeprom_address];
            m_eeprom_output_bits = 16;
            m_eeprom_data_out = (m_eeprom_output_shift & 0x8000) != 0;
        }
        return;
    }

    if (m_eeprom_write_pending) {
        m_eeprom_write_shift = static_cast<uint16_t>(
            (m_eeprom_write_shift << 1) | (data_in ? 1 : 0));
        if (++m_eeprom_write_bits == 16) {
            if (m_eeprom_write_enabled) {
                uint16_t write_value = m_eeprom_write_shift;
                // LINK TYPE is the high byte of word five.  The launcher
                // selects the physical cabinet role, so make Sega Rally's
                // startup transaction itself observe/write that role.  A
                // later checksum-only repair leaves the game's in-memory
                // settings as NOTLINK for the whole current boot.
                if (m_eeprom_address == 5 &&
                    m_forced_srally_link_type >= 0 &&
                    environment_comm_node() != 0) {
                    write_value = static_cast<uint16_t>(
                        (write_value & 0x00ff) |
                        (static_cast<uint16_t>(
                             m_forced_srally_link_type) << 8));
                }
                if (std::getenv("MODEL2_NVRAM_TRACE") &&
                    m_eeprom_address <= 0x11)
                    std::fprintf(stderr,
                                 "Sega Rally EEPROM word %u: %04x -> %04x\n",
                                 m_eeprom_address,
                                 m_eeprom_words[m_eeprom_address],
                                 write_value);
                m_eeprom_words[m_eeprom_address] = write_value;
                // A paired host chooses the physical cabinet role. Keep
                // Sega Rally's startup rewrite from reverting CAR1/CAR2 to
                // NOTLINK while the communication ring is still starting.
                // The game writes its complete 0x24-byte settings record in
                // order, so repair the role and checksum when its final word
                // arrives.
                if (m_eeprom_address == 0x11 &&
                    m_forced_srally_link_type >= 0) {
                    constexpr std::size_t record_size = 0x24;
                    constexpr std::size_t link_type_offset = 0x0b;
                    std::array<uint8_t, record_size> record{};
                    for (std::size_t word = 0; word < record.size() / 2;
                         ++word) {
                        record[word * 2] =
                            static_cast<uint8_t>(m_eeprom_words[word]);
                        record[word * 2 + 1] = static_cast<uint8_t>(
                            m_eeprom_words[word] >> 8);
                    }
                    record[link_type_offset] =
                        static_cast<uint8_t>(m_forced_srally_link_type);
                    const uint16_t checksum = sega_record_crc(
                        record.data() + 2, record_size - 2);
                    record[0] = static_cast<uint8_t>(checksum);
                    record[1] = static_cast<uint8_t>(checksum >> 8);
                    for (std::size_t word = 0; word < record.size() / 2;
                         ++word) {
                        m_eeprom_words[word] = static_cast<uint16_t>(
                            record[word * 2] |
                            (record[word * 2 + 1] << 8));
                    }
                }
                m_nvram_dirty = true;
            }
            m_eeprom_write_pending = false;
            m_eeprom_write_bits = 0;
            m_eeprom_data_out = true;
        }
        return;
    }

    // 93C46 x16 command: start, two-bit opcode, six-bit word address.
    if (m_eeprom_command_bits == 0 && !data_in) return;
    m_eeprom_shift = (m_eeprom_shift << 1) | (data_in ? 1U : 0U);
    if (++m_eeprom_command_bits != 9) return;

    const unsigned opcode = (m_eeprom_shift >> 6) & 3;
    m_eeprom_address = static_cast<uint8_t>(m_eeprom_shift & 0x3f);
    if (opcode == 2) { // READ
        m_eeprom_output_shift = m_eeprom_words[m_eeprom_address];
        // A 93C46 presents one dummy zero after the command. The first
        // following rising clock makes data bit 15 visible. Exposing bit 15
        // immediately shifted every word read by Sega Rally and caused the
        // service menu to write a corrupt checksum/configuration back out.
        m_eeprom_output_bits = 17;
        m_eeprom_data_out = false;
    } else if (opcode == 1) { // WRITE
        m_eeprom_write_pending = true;
        m_eeprom_write_bits = 0;
        m_eeprom_write_shift = 0;
    } else if (opcode == 3) { // ERASE
        if (m_eeprom_write_enabled) {
            m_eeprom_words[m_eeprom_address] = 0xffff;
            m_nvram_dirty = true;
        }
    } else {
        // EWEN uses address bits 5-4=11; EWDS uses 00.
        const unsigned control = (m_eeprom_address >> 4) & 3;
        if (control == 3) m_eeprom_write_enabled = true;
        if (control == 0) m_eeprom_write_enabled = false;
    }
    m_eeprom_command_bits = 0;
    m_eeprom_shift = 0;
}

uint16_t model2_bus::access_flags(uint32_t address) const {
    // The i960 advances the address between words of LDL/LDT/LDQ only when
    // the mapped device advertises BURST.  Plain RAM/ROM and the explicitly
    // burst-capable video memories do; scalar MMIO handlers do not.  This is
    // observable software behaviour: Sega Rally uses LDL on timer 2 and
    // expects both destination registers to receive that one timer value.
    // Advertising BURST for the whole address space made the second word
    // come from timer 3, corrupting the opponent visibility budget.
    const auto burst_range = [](uint32_t candidate, uint32_t base,
                                uint32_t size) {
        return candidate >= base && candidate - base < size;
    };

    const uint32_t tile_address = address & ~0x00110000U;
    const uint32_t character_address = address & ~0x00100000U;
    const bool tile_ram = burst_range(tile_address, 0x01000000, 0x10000);
    const bool absel = (address & ~0x00100000U) >= 0x01020000U &&
                       (address & ~0x00100000U) <= 0x01020003U;
    const bool character_ram =
        burst_range(character_address, 0x01080000, 0x80000);
    const bool communication_ram =
        burst_range(address, 0x01a00000, 0x4000) ||
        burst_range(address, 0x01a10000, 0x4000);

    const bool capable =
        burst_range(address, 0x00000000, 0x200000) || // program ROM
        burst_range(address, 0x00200000, 0x40000) ||  // local RAM
        burst_range(address, 0x00500000, 0x100000) || // work RAM
        burst_range(address, 0x00804000, 0x4000) ||   // TGP program
        burst_range(address, 0x00880000, 0x4000) ||   // TGP commands
        burst_range(address, 0x00900000, 0x80000) ||  // buffer RAM/mirrors
        tile_ram || absel || character_ram ||
        burst_range(address, 0x01800000, 0x4000) ||   // palette RAM
        burst_range(address, 0x01810000, 0xc000) ||   // colour lookup
        communication_ram ||
        burst_range(address, 0x01d00000, 0x4000) ||   // battery RAM
        burst_range(address, 0x02000000, 0x2000000) ||// data ROM
        burst_range(address, 0x06000000, 0x1000000) ||// extra data ROM
        burst_range(address, 0x11600000, 0x100000) || // frame buffers
        burst_range(address, 0x12000000, 0x800000) || // texture RAM
        burst_range(address, 0x12800000, 0x20000);    // luma RAM
    return capable ? i960_burst : 0;
}

void model2_bus::sound_midi_receive(uint8_t data) {
    // Called from the audio thread. Only the pending flag and data byte are
    // touched here; the IRQ request is raised from tick() on the CPU worker
    // thread to keep m_irq_request single-threaded.
    m_uart_rx_data.store(data, std::memory_order_relaxed);
    m_uart_rx_pending.store(true, std::memory_order_release);
    if (std::getenv("MODEL2_UART_TRACE"))
        std::fprintf(stderr, "Model 2 UART RX %02x\n", data);
}

void model2_bus::tick(uint32_t cycles) {
    if (m_iob) {
        // The model1io2 Z80 runs at 9.8304 MHz alongside the 25 MHz i960.
        // Interleave it with the CPU slices so the shared-RAM request/response
        // handshake advances within a frame rather than lagging a whole one.
        m_iob_clock_accum += static_cast<uint64_t>(cycles) * 9830400ULL;
        const uint32_t board_cycles =
            static_cast<uint32_t>(m_iob_clock_accum / 25000000ULL);
        m_iob_clock_accum -= static_cast<uint64_t>(board_cycles) * 25000000ULL;
        if (board_cycles) m_iob->execute(static_cast<int>(board_cycles));
    }
    if (m_uart_rx_pending.load(std::memory_order_acquire) &&
        m_uart_rx_enabled && (m_irq_enable & (1U << 10)))
        m_irq_request |= 1U << 10;
    if (m_uart_tx_shift_active) {
        // A scheduler slice is not an integer number of MIDI bit periods.
        // Preserve surplus cycles across character boundaries; throwing them
        // away made the 31.25-kbaud link about 40% too slow with Model 2's
        // 6510-cycle CPU slices and eventually filled the game's sound queue.
        uint32_t serial_cycles = cycles;
        while (m_uart_tx_shift_active &&
               serial_cycles >= m_uart_tx_empty_cycles) {
            serial_cycles -= m_uart_tx_empty_cycles;
            if (m_sound_uart_callback)
                m_sound_uart_callback(m_uart_tx_shift_data);
            if (m_uart_tx_holding_full) {
                m_uart_tx_shift_data = m_uart_tx_holding_data;
                m_uart_tx_holding_full = false;
                m_uart_tx_empty_cycles = 8000;
                m_uart_tx_ready = true;
                if (m_irq_enable & (1U << 10))
                    m_irq_request |= 1U << 10;
            } else {
                m_uart_tx_empty_cycles = 0;
                m_uart_tx_shift_active = false;
                m_uart_tx_empty = true;
            }
        }
        if (m_uart_tx_shift_active)
            m_uart_tx_empty_cycles -= serial_cycles;
    }
    for (unsigned timer = 0; timer < m_timer_values.size(); ++timer) {
        if (!m_timer_running[timer]) continue;
        if (cycles < m_timer_values[timer]) {
            m_timer_values[timer] -= cycles;
            continue;
        }

        const uint32_t line = 1U << (timer + 2);
        if (m_irq_enable & line) m_irq_request |= line;
        m_timer_values[timer] = 0x000fffff;
        m_timer_running[timer] = false;
    }
}

void model2_bus::vblank() {
    ++m_frame_number;
    // Service settings and bookkeeping must survive a compositor/session
    // kill as well as a clean destructor path. Coalesce battery-RAM writes
    // and EEPROM transactions into at most one small save per second. The
    // model1io2 board writes its 93C46 internally, so watch for changes here.
    if (m_iob && m_iob_eeprom != m_iob_eeprom_saved) m_nvram_dirty = true;
    if (m_nvram_dirty &&
        m_frame_number - m_last_nvram_save_frame >= 60)
        save_nvram();
    // Match MAME's default self-socket: discovery remains pending for the
    // EPR-16726 timeout, then a one-node ring receives the preceding frame.
    // Keeping a separate staging frame matters because TX and RX regions
    // overlap; copying the current bytes immediately made opponents consume
    // partially updated state and disappear intermittently.
    if (m_comm_cn && m_comm_peer_mode) {
        tick_comm_peer();
    } else if (m_comm_cn && m_comm_loopback) {
        if (!m_comm_link_alive) {
            m_communication_ram[0x00] = 0x00;
            m_communication_ram[0x02] = 0xff;
            m_communication_ram[0x03] = 0xff;
            // A connected socket clocks ZFG during ring discovery. Only the
            // master starts the ID exchange; Sega Rally selects it via FG.
            m_comm_zfg ^= 1;
            if (m_comm_fg && m_comm_link_timer != 0)
                --m_comm_link_timer;
            if (m_comm_fg && m_comm_link_timer == 0 && m_comm_peer_mode &&
                !m_comm_peer_seen && !m_comm_link_reported) {
                m_comm_link_reported = true;
                std::fprintf(stderr,
                             "Model 2 cabinet %u: no other cabinet answered "
                             "on port %u in four seconds - this cabinet will "
                             "run alone (peer port %u)\n",
                             m_comm_node_id, m_comm_local_port,
                             m_comm_peer_port);
                whitty_wall_log::note(
                    "comm: no peer on local port %u (peer port %u) after 4s",
                    m_comm_local_port, m_comm_peer_port);
            }
            if (m_comm_fg && m_comm_link_timer == 0) {
                m_comm_link_alive = true;
                m_communication_ram[0x00] = 0x01;
                m_communication_ram[0x02] = 0x01;
                m_communication_ram[0x03] = 0x01;
                if (std::getenv("MODEL2_COMM_TRACE"))
                    std::fprintf(stderr,
                                 "Model 2 comm linked frame=%u id=1 count=1\n",
                                 m_frame_number);
            }
        } else if (comm_frame_receive + comm_frame_size <=
                   m_communication_ram.size()) {
            if (m_comm_loopback_frame_valid) {
                std::copy(m_comm_loopback_frame.begin(),
                          m_comm_loopback_frame.end(),
                          m_communication_ram.begin() + comm_frame_receive);
                m_comm_zfg ^= 1;
            }
            std::copy_n(m_communication_ram.begin() + comm_frame_start,
                        comm_frame_size, m_comm_loopback_frame.begin());
            m_comm_loopback_frame_valid = true;
        }
    } else if (m_comm_cn) {
        // MODEL2_COMM_LINK=0 models a physically unreachable peer.
        m_communication_ram[0x00] = 0x00;
        m_communication_ram[0x02] = 0xff;
        m_communication_ram[0x03] = 0xff;
    }
    if (m_irq_enable & 1U) m_irq_request |= 1U;
}

bool model2_bus::irq_asserted(unsigned line) const {
    if (line == 0) return (m_irq_request & 0x001) != 0;
    if (line == 1) return (m_irq_request & 0x002) != 0;
    if (line == 2) return (m_irq_request & 0x3fc) != 0;
    if (line == 3) return (m_irq_request & 0xc00) != 0;
    return false;
}

uint32_t model2_bus::tgp_program_read(uint16_t address) const {
    const std::size_t offset = static_cast<std::size_t>(address) * 4;
    return offset + 3 < m_geometry_program.size() ?
        read_dword(m_geometry_program, offset) : 0;
}

uint32_t model2_bus::tgp_data_read(uint16_t address) const {
    // The MB86233 has two physically decoded data-RAM banks.  0x0100-0x01ff
    // is an unmapped hole, not scratch RAM; retaining writes there changes
    // long-running scene state (notably Sega Rally's traffic/display list).
    const bool mapped = address <= 0x00ff ||
                        (address >= 0x0200 && address <= 0x03ff);
    return mapped && address < m_tgp_data_ram.size() ?
        m_tgp_data_ram[address] : 0;
}

void model2_bus::tgp_data_write(uint16_t address, uint32_t value) {
    const bool mapped = address <= 0x00ff ||
                        (address >= 0x0200 && address <= 0x03ff);
    if (mapped && address < m_tgp_data_ram.size())
        m_tgp_data_ram[address] = value;
}

uint32_t model2_bus::tgp_io_read(uint16_t address) {
    if (!m_roms) return 0;
    const auto table = [this](uint32_t index) {
        const std::size_t offset = static_cast<std::size_t>(index) * 4;
        return offset + 3 < m_roms->copro_tgp_tables.size() ?
            read_dword(m_roms->copro_tgp_tables, offset) : 0U;
    };

    // RF3 selects a view across the *whole* MB86234 I/O address space.
    // While either external-memory enable bit is set, the banked ROM/buffer
    // window replaces the arithmetic lookup ports too. Checking 0x20-0x2b
    // first made those offsets hit sin/cos and reciprocal tables during
    // Sega Rally's road-polygon walk. The resulting bogus vertices caused
    // collision queries to return no road and rivals to disappear.
    if (m_tgp_bank & 0xc00000U) {
        const uint32_t banked = (m_tgp_bank & 0xff0000U) | address;
        if (banked & 0x800000U) {
            const uint32_t mask =
                static_cast<uint32_t>(m_roms->copro_data.size() / 4 - 1);
            return read_dword(m_roms->copro_data,
                              static_cast<std::size_t>(banked & mask) * 4);
        }
        if (banked & 0x400000U)
            return read_dword(m_buffer_ram,
                              static_cast<std::size_t>(banked & 0x7fff) * 4);
        return 0;
    }

    if (address >= 0x20 && address <= 0x23) {
        const uint32_t angle = m_tgp_sincos_base +
            static_cast<uint32_t>(address - 0x20) * 0x4000;
        uint32_t index = angle & 0x3fff;
        if (angle & 0x4000)
            index = std::min<uint32_t>(0x4000 - index, 0x3fff);
        uint32_t result = table(index);
        if (angle & 0x8000) result ^= 0x80000000U;
        return result;
    }
    if (address >= 0x24 && address <= 0x27) {
        // The arctangent unit answers on every word of its port, not just the
        // first: the four addresses latch the operands, and a read of any of
        // them returns the same result. Virtua Cop 2 reads it back from 0x27,
        // so decoding only 0x24 returned zero for every camera-direction
        // query. That collapsed the attract camera's field of view to zero,
        // which made the focal length infinite and every polygon vanish.
        const uint8_t exponent =
            0x88 - static_cast<uint8_t>(m_tgp_atan_base[3] >> 23);
        const bool sign0 = (m_tgp_atan_base[0] & 0x80000000U) != 0;
        const bool sign1 = (m_tgp_atan_base[1] & 0x80000000U) != 0;
        const bool second = (m_tgp_atan_base[0] & 0x7fffffffU) <=
                            (m_tgp_atan_base[1] & 0x7fffffffU);
        const uint32_t mantissa = m_tgp_atan_base[3] & 0x7fffff;
        uint32_t index = exponent <= 0x17 ?
            (mantissa | 0x800000) >> exponent : 0;
        if (index == 0x4000) index = 0x3fff;
        uint32_t result = table(index | 0x4000);
        if (sign0 ^ sign1 ^ second) result >>= 16;
        if (second) result += 0x4000;
        if ((sign0 && !second) || (sign1 && second)) result += 0x8000;
        return result & 0xffff;
    }
    if (address == 0x28 || address == 0x29) {
        const uint32_t index = ((m_tgp_inv_base >> 9) & 0x3ffe) |
                               (address & 1);
        uint32_t result = table(index | 0x8000);
        const uint8_t base_exponent = m_tgp_inv_base >> 23;
        const uint8_t exponent = static_cast<uint8_t>(result >> 23) +
                                 (0x7f - base_exponent);
        result = (result & 0x007fffffU) |
                 (static_cast<uint32_t>(exponent) << 23);
        if ((m_tgp_inv_base & 0x80000000U) && (address & 1))
            result |= 0x80000000U;
        return result;
    }
    if (address == 0x2a || address == 0x2b) {
        const uint32_t index = 0x2000 ^
            (((m_tgp_isqrt_base >> 10) & 0x3ffe) | (address & 1));
        uint32_t result = table(index | 0xc000);
        const uint8_t base_exponent = (m_tgp_isqrt_base >> 24) & 0x7f;
        const uint8_t exponent = static_cast<uint8_t>(result >> 23) +
                                 (0x3f - base_exponent);
        result = (result & 0x807fffffU) |
                 (static_cast<uint32_t>(exponent) << 23);
        if (!(address & 1)) result &= 0x7fffffffU;
        return result;
    }

    return 0;
}

void model2_bus::tgp_io_write(uint16_t address, uint32_t value) {
    if (m_tgp_bank & 0xc00000U) {
        const uint32_t banked = (m_tgp_bank & 0xff0000U) | address;
        if (banked & 0x400000U) {
            trace_geometry_write("tgp",
                                 static_cast<std::size_t>(banked & 0x7fff) * 4,
                                 value);
            write_dword(m_buffer_ram,
                        static_cast<std::size_t>(banked & 0x7fff) * 4,
                        value);
        }
        return;
    }

    if (address >= 0x20 && address <= 0x23) {
        m_tgp_sincos_base = value;
        return;
    }
    if (address >= 0x24 && address <= 0x27) {
        m_tgp_atan_base[address - 0x24] = value;
        m_tgp_gpio0 = (m_tgp_atan_base[0] & 0x7fffffffU) <=
                      (m_tgp_atan_base[1] & 0x7fffffffU);
        return;
    }
    if (address == 0x28 || address == 0x29) {
        m_tgp_inv_base = value;
        return;
    }
    if (address == 0x2a || address == 0x2b) {
        m_tgp_isqrt_base = value;
        return;
    }

}

uint32_t model2_bus::tgp_rf_read(uint16_t address) {
    if (address == 1 && !m_tgp_fifo_in.empty()) {
        const uint32_t value = m_tgp_fifo_in.front();
        m_tgp_fifo_in.pop_front();
        if (trace_fifo_words(m_frame_number))
            std::fprintf(stderr, "%u IN-POP %08x\n", m_frame_number,
                         value);
        // Start a TGP instruction trace when the chosen command is
        // dispatched, so one routine can be compared with MAME's trace.
        if (const uint32_t watched = traced_tgp_command())
            if (value == watched) mb86233_native_core::trace_arm(720);
        return value;
    }
    return 0;
}

void model2_bus::tgp_rf_write(uint16_t address, uint32_t value) {
    if (address == 2) {
        m_tgp_fifo_out.push_back(value);
        if (trace_fifo_words(m_frame_number))
            std::fprintf(stderr, "%u OUT %08x\n", m_frame_number, value);
        if (std::getenv("MODEL2_FIFO_TRACE") &&
            m_tgp_fifo_out.size() > tgp_fifo_capacity)
            std::fprintf(stderr,
                         "Model 2 TGP output overflow frame=%u size=%zu\n",
                         m_frame_number, m_tgp_fifo_out.size());
    } else if (address == 3) {
        m_tgp_bank = value;
        if (std::getenv("MODEL2_GEOMETRY_TRACE"))
            std::fprintf(stderr, "Model 2 TGP frame=%u bank=%08x\n",
                         m_frame_number, m_tgp_bank);
    }
}

bool model2_bus::take_tgp_boot_request() {
    const bool requested = m_tgp_boot_request;
    m_tgp_boot_request = false;
    return requested;
}

void model2_bus::trace_geometry_write(const char* source, std::size_t offset,
                                      uint32_t value) {
    uint32_t watch = 0;
    if (!watched_geometry_word(watch) || value != watch) return;
    static unsigned reported = 0;
    if (reported >= 8) return;
    ++reported;
    std::printf("GEOWRITE source=%s offset=%05zx value=%08x pc=%08x\n",
                source, offset, value,
                m_program_counter_probe ? m_program_counter_probe() : 0);
}

void model2_bus::push_geometry_word(uint32_t value) {
    const std::size_t offset = m_geo_write_address & 0x1ffff;
    trace_geometry_write("fifo", offset, value);
    write_dword(m_buffer_ram, offset, value);
    m_geo_write_address = (m_geo_write_address + 4) & 0xfffff;
}

uint32_t model2_bus::geometry_buffer_word(uint32_t byte_address) const {
    return read_dword(m_buffer_ram, byte_address & 0x1ffff);
}
