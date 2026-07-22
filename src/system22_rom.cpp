// system22_rom.cpp - validated System 22 ROM and device firmware loading
#include "system22_rom.h"

#include <minizip/unzip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
struct asset_spec {
    const char* name;
    std::size_t size;
    uint32_t crc;
    std::size_t offset;
};

struct game_asset_set {
    const char* display_name;
    std::array<asset_spec, 4> program;
    std::array<asset_spec, 1> mcu;
    std::array<asset_spec, 4> textures;
    std::array<asset_spec, 2> tilemap;
    std::array<asset_spec, 6> points;
    std::array<asset_spec, 4> samples;
};

constexpr game_asset_set ridge_racer_assets{
    "Ridge Racer",
    {{{"rr2_prgllb.4d", 0x80000, 0xd50d4fa0, 3},
      {"rr2_prglmb.2d", 0x80000, 0x6a266f42, 2},
      {"rr2_prgumb.8d", 0x80000, 0x64c3aff1, 1},
      {"rr2_prguub.6d", 0x80000, 0xe6ff0b8d, 0}}},
    {{{"rr1data.6r", 0x80000, 0x18f5f748, 0}}},
    {{{"rr1cg0.bin", 0x200000, 0xb557a795, 0x800000},
      {"rr1cg1.bin", 0x200000, 0x0fa212d9, 0xa00000},
      {"rr1cg2.bin", 0x200000, 0x18e2d2bd, 0xc00000},
      {"rr1cg3.bin", 0x200000, 0x9564488b, 0xe00000}}},
    {{{"rr1ccrl.bin", 0x200000, 0x6092d181, 0x000000},
      {"rr1ccrh.bin", 0x080000, 0xdd332fd5, 0x200000}}},
    {{{"rr1potl0.5b", 0x080000, 0x3ac193e3, 0x000000},
      {"rr1potl1.4b", 0x080000, 0xac3ffba5, 0x080000},
      {"rr1potm0.5c", 0x080000, 0x42a3fa08, 0x100000},
      {"rr1potm1.4c", 0x080000, 0x1bc1ea7b, 0x180000},
      {"rr1potu0.5d", 0x080000, 0x5e367f72, 0x200000},
      {"rr1potu1.4d", 0x080000, 0x31d92475, 0x280000}}},
    {{{"rr1wav0.10r", 0x100000, 0xa8e85bde, 0x000000},
      {"rr1wav1.10p", 0x100000, 0x35f47c8e, 0x200000},
      {"rr1wav2.10n", 0x100000, 0x3244cb59, 0x100000},
      {"rr1wav3.10l", 0x100000, 0xc4cda1a7, 0x300000}}},
};

constexpr game_asset_set ridge_racer_full_scale_assets{
    "Ridge Racer Full Scale",
    {{{"rrf2_prgll.4d", 0x80000, 0x23c6144d, 3},
      {"rrf2_prglm.2d", 0x80000, 0x1ad638a1, 2},
      {"rrf2_prgum.8d", 0x80000, 0xd7e0aa16, 1},
      {"rrf2_prguu.6d", 0x80000, 0x12c808bb, 0}}},
    {{{"rrf1data.6r", 0x80000, 0xce3c6ed6, 0}}},
    {{{"rrf1cg0.bin", 0x200000, 0xb557a795, 0x800000},
      {"rrf1cg1.bin", 0x200000, 0x0fa212d9, 0xa00000},
      {"rrf1cg2.bin", 0x200000, 0x18e2d2bd, 0xc00000},
      {"rrf1cg3.bin", 0x200000, 0x9564488b, 0xe00000}}},
    {{{"rrf1ccrl.bin", 0x200000, 0x6092d181, 0x000000},
      {"rrf1ccrh.bin", 0x080000, 0xdd332fd5, 0x200000}}},
    {{{"rrf2potl0.l0", 0x080000, 0x9b762e60, 0x000000},
      {"rrf2potl1.l1", 0x080000, 0xab4d66b0, 0x080000},
      {"rrf2potm0.m0", 0x080000, 0x02d4daa3, 0x100000},
      {"rrf2potm1.m1", 0x080000, 0x37e005c2, 0x180000},
      {"rrf2potu0.u0", 0x080000, 0x86b3fe98, 0x200000},
      {"rrf2potu1.u1", 0x080000, 0xe0c6ce3d, 0x280000}}},
    {{{"rrf1wave0.10r", 0x100000, 0xa8e85bde, 0x000000},
      {"rrf1wave1.10p", 0x100000, 0x35f47c8e, 0x200000},
      {"rrf1wave2.10n", 0x100000, 0x4ceeae12, 0x100000},
      {"rrf1wave3.10l", 0x100000, 0xc4cda1a7, 0x300000}}},
};

constexpr game_asset_set ridge_racer_2_assets{
    "Ridge Racer 2",
    {{{"rrs2_prgll.4d", 0x80000, 0x88199c0f, 3},
      {"rrs2_prglm.2d", 0x80000, 0x8e86f199, 2},
      {"rrs2_prgum.8d", 0x80000, 0x78c360b6, 1},
      {"rrs2_prguu.6d", 0x80000, 0x60d6d4a4, 0}}},
    {{{"rrs1data.6r", 0x80000, 0xb7063aa8, 0}}},
    {{{"rrs1cg0.1a", 0x200000, 0x714c0091, 0x800000},
      {"rrs1cg1.2a", 0x200000, 0x836545c1, 0xa00000},
      {"rrs1cg2.3a", 0x200000, 0x00e9799d, 0xc00000},
      {"rrs1cg3.5a", 0x200000, 0x3858983f, 0xe00000}}},
    {{{"rrs1ccrl.5a", 0x200000, 0x304a8b57, 0x000000},
      {"rrs1ccrh.5c", 0x080000, 0xbd3c86ab, 0x200000}}},
    {{{"rrs1pol0.5b", 0x080000, 0x9376c384, 0x000000},
      {"rrs1pol1.4b", 0x080000, 0x094fa832, 0x080000},
      {"rrs1pom0.5c", 0x080000, 0xb47a7f8b, 0x100000},
      {"rrs1pom1.4c", 0x080000, 0x27260361, 0x180000},
      {"rrs1pou0.5d", 0x080000, 0x74d6ec84, 0x200000},
      {"rrs1pou1.4d", 0x080000, 0xf527caaa, 0x280000}}},
    {{{"rrs1wav0.10r", 0x100000, 0x99d11a2d, 0x000000},
      {"rrs1wav1.10p", 0x100000, 0xad28444a, 0x200000},
      {"rrs1wav2.10n", 0x100000, 0x6f0d4619, 0x100000},
      {"rrs1wav3.10l", 0x100000, 0x106e761f, 0x300000}}},
};

struct rave_racer_asset_set {
    const char* display_name;
    std::array<asset_spec, 4> program;
    std::array<asset_spec, 1> mcu;
    std::array<asset_spec, 8> textures;
    std::array<asset_spec, 2> tilemap;
    std::array<asset_spec, 12> points;
    std::array<asset_spec, 4> samples;
};

constexpr rave_racer_asset_set rave_racer_assets{
    "Rave Racer",
    {{{"rv2_prgllb.4d", 0x80000, 0x3017cd1e, 3},
      {"rv2_prglmb.2d", 0x80000, 0x894be0c3, 2},
      {"rv2_prgumb.8d", 0x80000, 0x6414a800, 1},
      {"rv2_prguub.6d", 0x80000, 0xa9f18714, 0}}},
    {{{"rv1data.6r", 0x80000, 0xd358ec20, 0}}},
    {{{"rv1cg0.1a", 0x200000, 0xc518f06b, 0x000000}, {"rv1cg1.1c", 0x200000, 0x6628f792, 0x200000},
      {"rv1cg2.1d", 0x200000, 0x0b707cc5, 0x400000}, {"rv1cg3.1e", 0x200000, 0x39b62921, 0x600000},
      {"rv1cg4.1f", 0x200000, 0xa9791ea2, 0x800000}, {"rv1cg5.1j", 0x200000, 0xb2c79ec1, 0xa00000},
      {"rv1cg6.1k", 0x200000, 0x8cddedc2, 0xc00000}, {"rv1cg7.1n", 0x200000, 0xb39147ca, 0xe00000}}},
    {{{"rv1ccrl.5a", 0x200000, 0xbc634f72, 0x000000}, {"rv1ccrh.5c", 0x080000, 0xa741b262, 0x200000}}},
    {{{"rv1potl0.5b", 0x080000, 0xde2ce519, 0x000000}, {"rv1potl1.4b", 0x080000, 0x2215cb5a, 0x080000},
      {"rv1potl2.3b", 0x080000, 0xddb15bf7, 0x100000}, {"rv1potl3.2b", 0x080000, 0xfa9361ca, 0x180000},
      {"rv1potm0.5c", 0x080000, 0x3c024f3a, 0x200000}, {"rv1potm1.4c", 0x080000, 0xb1a32a68, 0x280000},
      {"rv1potm2.3c", 0x080000, 0xa414fe15, 0x300000}, {"rv1potm3.2c", 0x080000, 0x2953bbb4, 0x380000},
      {"rv1potu0.5d", 0x080000, 0xb9eaf3cc, 0x400000}, {"rv1potu1.4d", 0x080000, 0xa5c55258, 0x480000},
      {"rv1potu2.3d", 0x080000, 0xc18fcb74, 0x500000}, {"rv1potu3.2d", 0x080000, 0x79735aaa, 0x580000}}},
    {{{"rv1wav0.10r", 0x100000, 0x5aef8143, 0x000000}, {"rv1wav1.10p", 0x100000, 0x9ed9e6b3, 0x200000},
      {"rv1wav2.10n", 0x100000, 0x5af9dc83, 0x100000}, {"rv1wav3.10l", 0x100000, 0xffb9ad75, 0x300000}}},
};

// Later System 22 games vary in both the number and placement of their ROMs.
// A dynamic descriptor keeps the loader data-driven without padding fixed
// arrays with fake entries or adding another game-specific loading function.
struct flexible_game_asset_set {
    const char* display_name;
    std::vector<asset_spec> program;
    std::vector<asset_spec> mcu;
    std::vector<asset_spec> textures;
    std::vector<asset_spec> tilemap;
    std::vector<asset_spec> points;
    std::vector<asset_spec> samples;
    std::vector<asset_spec> eeprom;
    std::size_t point_region_size;
    std::size_t texture_region_offset;
    std::size_t texture_bank_count;
    std::size_t mcu_mirror_period;
    bool texture_tile_high_bit_from_attr;
};

const flexible_game_asset_set ace_driver_assets{
    "Ace Driver: Racing Evolution",
    {{"ad2_prgll.4d", 0x80000, 0x808c5ff8, 3},
     {"ad2_prglm.2d", 0x80000, 0x5f726a10, 2},
     {"ad2_prgum.8d", 0x80000, 0xd5042d6e, 1},
     {"ad2_prguu.6d", 0x80000, 0x86d4661d, 0}},
    {{"ad1data.6r", 0x80000, 0x82024f74, 0}},
    {{"ad1cg0.1a", 0x200000, 0xfaaa1ee2, 0x800000},
     {"ad1cg1.2a", 0x200000, 0x1aab1eb7, 0xa00000},
     {"ad1cg2.3a", 0x200000, 0xcdcd1874, 0xc00000},
     {"ad1cg3.5a", 0x200000, 0xeffdd2cd, 0xe00000}},
    {{"ad1ccrl.1c", 0x200000, 0xbc3c9b12, 0},
     {"ad1ccrh.2c", 0x080000, 0x71f44526, 0x200000}},
    {{"ad1potl0.5b", 0x080000, 0xdfc7e729, 0x000000},
     {"ad1potl1.4b", 0x080000, 0x5914ef8e, 0x080000},
     {"ad1potm0.5c", 0x080000, 0x844bcd6b, 0x100000},
     {"ad1potm1.4c", 0x080000, 0x515cf541, 0x180000},
     {"ad1potu0.5d", 0x080000, 0xe0f44949, 0x200000},
     {"ad1potu1.4d", 0x080000, 0xf2cd2cbb, 0x280000}},
    {{"ad1wave0.10r", 0x100000, 0xc7879a72, 0x000000},
     {"ad1wave1.10p", 0x100000, 0x69c1d41e, 0x200000},
     {"ad1wave2.10n", 0x100000, 0x365a6831, 0x100000},
     {"ad1wave3.10l", 0x100000, 0xcd8ecb0b, 0x300000}},
    {}, 0x300000, 0x800000, 4, 0, true,
};

const flexible_game_asset_set victory_lap_assets{
    "Ace Driver: Victory Lap",
    {{"adv2_prgllb.4d", 0x80000, 0x03ba6e42, 3},
     {"adv2_prglmb.2d", 0x80000, 0x9f4b49c0, 2},
     {"adv2_prgumb.8d", 0x80000, 0xccad3e90, 1},
     {"adv2_prguub.6d", 0x80000, 0xf3fffc41, 0}},
    {{"adv1data.6r", 0x80000, 0x10eecdb4, 0}},
    {{"adv1cg0.2a", 0x200000, 0x13353848, 0x000000},
     {"adv1cg1.1c", 0x200000, 0x1542066c, 0x200000},
     {"adv1cg2.2d", 0x200000, 0x111f371c, 0x400000},
     {"adv1cg3.1e", 0x200000, 0xa077831f, 0x600000},
     {"adv1cg4.2f", 0x200000, 0x71abdacf, 0x800000},
     {"adv1cg5.1j", 0x200000, 0xcd6cd798, 0xa00000},
     {"adv1cg6.2k", 0x200000, 0x94bdafba, 0xc00000},
     {"adv1cg7.1n", 0x200000, 0x18823475, 0xe00000}},
    {{"adv1ccrl.5a", 0x200000, 0xdd2b96ae, 0},
     {"adv1ccrh.5c", 0x080000, 0x5719844a, 0x200000}},
    {{"adv1pot.l0", 0x080000, 0x3b85b2a4, 0x000000},
     {"adv1pot.l1", 0x080000, 0x601d6488, 0x080000},
     {"adv1pot.l2", 0x080000, 0xa0323a84, 0x100000},
     {"adv1pot.m0", 0x080000, 0x20951aa2, 0x180000},
     {"adv1pot.m1", 0x080000, 0x5aed6fbf, 0x200000},
     {"adv1pot.m2", 0x080000, 0x00cbff92, 0x280000},
     {"adv1pot.u0", 0x080000, 0x6b73dd2a, 0x300000},
     {"adv1pot.u1", 0x080000, 0xc8788f74, 0x380000},
     {"adv1pot.u2", 0x080000, 0xe67f29c5, 0x400000}},
    {{"adv1wav0.10r", 0x100000, 0xf07b2d9d, 0x000000},
     {"adv1wav1.10p", 0x100000, 0x737f3c7a, 0x200000},
     {"adv1wav2.10n", 0x100000, 0xc1a5ca5e, 0x100000},
     {"adv1wav3.10l", 0x100000, 0xfc6b8004, 0x300000}},
    {}, 0x480000, 0, 8, 0, false,
};

const flexible_game_asset_set cyber_commando_assets{
    "Cyber Commando",
    {{"cy1prgll.4d", 0x80000, 0xb3eab156, 3},
     {"cy1prglm.2d", 0x80000, 0x884a5b0e, 2},
     {"cy1prgum.8d", 0x80000, 0xc9c4a921, 1},
     {"cy1prguu.6d", 0x80000, 0x5f22975b, 0}},
    {{"cy1data.6r", 0x20000, 0x10d0005b, 0}},
    {{"cyc1cg0.1a", 0x200000, 0xe839b9bd, 0x800000},
     {"cyc1cg1.2a", 0x200000, 0x7d13993f, 0xa00000},
     {"cyc1cg2.3a", 0x200000, 0x7c464566, 0xc00000},
     {"cyc1cg3.5a", 0x200000, 0x2222e16f, 0xe00000}},
    {{"cyc1ccrl.1c", 0x100000, 0x1a0dc5f0, 0},
     {"cyc1ccrh.2c", 0x080000, 0x8c4090b8, 0x200000}},
    {{"cyc1ptl0.5b", 0x080000, 0xd91de03d, 0x000000},
     {"cyc1ptl1.4b", 0x080000, 0xe5b98021, 0x080000},
     {"cyc1ptl2.3b", 0x080000, 0x7ba786c6, 0x100000},
     {"cyc1ptm0.5c", 0x080000, 0xd454b5c6, 0x180000},
     {"cyc1ptm1.4c", 0x080000, 0x74fdf8cc, 0x200000},
     {"cyc1ptm2.3c", 0x080000, 0xb9c99a45, 0x280000},
     {"cyc1ptu0.5d", 0x080000, 0x4d40897f, 0x300000},
     {"cyc1ptu1.4d", 0x080000, 0x3bdaeeeb, 0x380000},
     {"cyc1ptu2.3d", 0x080000, 0xa0e73674, 0x400000}},
    {{"cy1wav0.10r", 0x100000, 0xc6f366a2, 0x000000},
     {"cy1wav1.10p", 0x100000, 0xf30b5e37, 0x200000},
     {"cy1wav2.10n", 0x100000, 0xb98c1ca6, 0x100000},
     {"cy1wav3.10l", 0x100000, 0x43dbac19, 0x300000}},
    {{"cy1eeprm.9e", 0x2000, 0x8432c066, 0}},
    0x480000, 0x800000, 4, 0x20000, true,
};

// Super System 22 games share one region layout: a 4 MiB interleaved
// program, an M37710 data ROM that also supplies the internal MCU window,
// a sprite region alongside the polygon textures, and C352 samples. Only
// the point-ROM depth and a per-game sample transform vary.
struct super_system22_asset_set {
    const char* display_name;
    std::vector<asset_spec> program;
    std::vector<asset_spec> mcu;
    std::vector<asset_spec> sprites;
    std::vector<asset_spec> textures;
    std::vector<asset_spec> tilemap;
    std::vector<asset_spec> points;
    std::vector<asset_spec> samples;
    std::vector<asset_spec> eeprom;
    std::size_t point_region_size;
    // Time Crisis wires its first wave ROM as 16-bit words with the byte
    // lanes swapped; the other sets are byte-ordered as dumped.
    std::size_t swapped_sample_bytes;
};

constexpr std::array<asset_spec, 4> time_crisis_program{{
    {"ts2verb.1", 0x100000, 0x29b377f7, 3},
    {"ts2verb.2", 0x100000, 0x79512e25, 2},
    {"ts2verb.3", 0x100000, 0x9f4ced33, 1},
    {"ts2verb.4", 0x100000, 0x3e0cfb38, 0},
}};

constexpr std::array<asset_spec, 1> time_crisis_mcu{{
    {"ts1data.8k", 0x080000, 0xe68aa973, 0},
}};

constexpr std::array<asset_spec, 6> time_crisis_sprites{{
    {"ts1scg0.12f", 0x200000, 0x14a3674d, 0x000000},
    {"ts1scg1.10f", 0x200000, 0x11791dbf, 0x200000},
    {"ts1scg2.8f",  0x200000, 0xd630fff9, 0x400000},
    {"ts1scg3.7f",  0x200000, 0x1a62f015, 0x600000},
    {"ts1scg4.5f",  0x200000, 0x511b8dd6, 0x800000},
    {"ts1scg5.3f",  0x200000, 0x553bb246, 0xa00000},
}};

constexpr std::array<asset_spec, 7> time_crisis_textures{{
    {"ts1cg0.8d",  0x200000, 0xde07b22c, 0x000000},
    {"ts1cg1.10d", 0x200000, 0x992d26f6, 0x200000},
    {"ts1cg2.12d", 0x200000, 0x6273954f, 0x400000},
    {"ts1cg3.13d", 0x200000, 0x38171f24, 0x600000},
    {"ts1cg4.14d", 0x200000, 0x51f09856, 0x800000},
    {"ts1cg5.16d", 0x200000, 0x4cd9fd79, 0xa00000},
    {"ts1cg6.18d", 0x200000, 0xf17f2ec9, 0xc00000},
}};

constexpr std::array<asset_spec, 2> time_crisis_tilemap{{
    {"ts1ccrl.3d", 0x200000, 0x56cad2df, 0x000000},
    {"ts1ccrh.1d", 0x080000, 0xa1cc3741, 0x200000},
}};

constexpr std::array<asset_spec, 9> time_crisis_points{{
    {"ts1ptrl0.18k", 0x080000, 0xe5f2d275, 0x000000},
    {"ts1ptrl1.16k", 0x080000, 0x2bba3800, 0x080000},
    {"ts1ptrl2.15k", 0x080000, 0xd4441c08, 0x100000},
    {"ts1ptrm0.18j", 0x080000, 0x8aea02ba, 0x180000},
    {"ts1ptrm1.16j", 0x080000, 0xbccf19bc, 0x200000},
    {"ts1ptrm2.15j", 0x080000, 0x7280be31, 0x280000},
    {"ts1ptru0.18f", 0x080000, 0xc30d6332, 0x300000},
    {"ts1ptru1.16f", 0x080000, 0x993cde84, 0x380000},
    {"ts1ptru2.15f", 0x080000, 0x7cb25c73, 0x400000},
}};

constexpr std::array<asset_spec, 2> time_crisis_samples{{
    {"ts1wavea.2l", 0x400000, 0xd1123301, 0x000000},
    {"ts1waveb.1l", 0x200000, 0xbf4d7272, 0x800000},
}};

constexpr std::array<asset_spec, 4> dirt_dash_program{{
    {"dt2verc.rom1", 0x100000, 0xb2d7916c, 3},
    {"dt2verc.rom2", 0x100000, 0x5dc040d1, 2},
    {"dt2verc.rom3", 0x100000, 0xa55157e1, 1},
    {"dt2verc.rom4", 0x100000, 0xf3a9bedb, 0},
}};

constexpr std::array<asset_spec, 1> dirt_dash_mcu{{
    {"dt1dataa.8k", 0x080000, 0x9bcdea21, 0},
}};

constexpr std::array<asset_spec, 2> dirt_dash_sprites{{
    {"dt1scg0.12f", 0x200000, 0xa09b5760, 0x000000},
    {"dt1scg1.10f", 0x200000, 0xf9ac8111, 0x200000},
}};

constexpr std::array<asset_spec, 8> dirt_dash_textures{{
    {"dt1cg0.8d",  0x200000, 0x10ab95e0, 0x000000},
    {"dt1cg1.10d", 0x200000, 0xd9f1ba53, 0x200000},
    {"dt1cg2.12d", 0x200000, 0xbd8b1e0b, 0x400000},
    {"dt1cg3.13d", 0x200000, 0xba960663, 0x600000},
    {"dt1cg4.14d", 0x200000, 0x424b9652, 0x800000},
    {"dt1cg5.16d", 0x200000, 0x29516626, 0xa00000},
    {"dt1cg6.18d", 0x200000, 0xe6fa7180, 0xc00000},
    {"dt1cg7.19d", 0x200000, 0x2ca19153, 0xe00000},
}};

constexpr std::array<asset_spec, 2> dirt_dash_tilemap{{
    {"dt1ccrl.3d", 0x200000, 0xe536b313, 0x000000},
    {"dt1ccrh.1d", 0x080000, 0xaf257064, 0x200000},
}};

constexpr std::array<asset_spec, 9> dirt_dash_points{{
    {"dt1ptrl0.18k", 0x080000, 0x4e0cac3a, 0x000000},
    {"dt1ptrl1.16k", 0x080000, 0x59ba9dba, 0x080000},
    {"dt1ptrl2.15k", 0x080000, 0xcfe80c67, 0x100000},
    {"dt1ptrm0.18j", 0x080000, 0x41f34337, 0x180000},
    {"dt1ptrm1.16j", 0x080000, 0xf620fd41, 0x200000},
    {"dt1ptrm2.15j", 0x080000, 0x71e6714d, 0x280000},
    {"dt1ptru0.18f", 0x080000, 0x4909bd7d, 0x300000},
    {"dt1ptru1.16f", 0x080000, 0x4a5097df, 0x380000},
    {"dt1ptru2.15f", 0x080000, 0x1171eaf5, 0x400000},
}};

constexpr std::array<asset_spec, 2> dirt_dash_samples{{
    {"dt1wavea.2l", 0x400000, 0xcbd52e40, 0x000000},
    {"dt1waveb.1l", 0x400000, 0x6b736f94, 0x800000},
}};

constexpr std::array<asset_spec, 4> aqua_jet_program{{
    {"aj2verb.1", 0x100000, 0x3a67b9f4, 3},
    {"aj2verb.2", 0x100000, 0xf5e8fc96, 2},
    {"aj2verb.3", 0x100000, 0xef6ebcf7, 1},
    {"aj2verb.4", 0x100000, 0x7799b909, 0},
}};

constexpr std::array<asset_spec, 1> aqua_jet_mcu{{
    {"aj1data.8k", 0x080000, 0x52bcc6d5, 0},
}};

constexpr std::array<asset_spec, 3> aqua_jet_sprites{{
    {"aj1scg0.12f", 0x200000, 0x13ea766c, 0x000000},
    {"aj1scg1.10f", 0x200000, 0xcb3638de, 0x200000},
    {"aj1scg2.8f",  0x200000, 0x1048a09b, 0x400000},
}};

constexpr std::array<asset_spec, 8> aqua_jet_textures{{
    {"aj1cg0.8d",  0x200000, 0xb814e1eb, 0x000000},
    {"aj1cg1.10d", 0x200000, 0xdc63d496, 0x200000},
    {"aj1cg2.12d", 0x200000, 0x71fbb571, 0x400000},
    {"aj1cg3.13d", 0x200000, 0xe28052e2, 0x600000},
    {"aj1cg4.14d", 0x200000, 0xc77ae1a0, 0x800000},
    {"aj1cg5.16d", 0x200000, 0x15be0080, 0xa00000},
    {"aj1cg6.18d", 0x200000, 0x1a4f733a, 0xc00000},
    {"aj1cg7.19d", 0x200000, 0xea118130, 0xe00000},
}};

constexpr std::array<asset_spec, 2> aqua_jet_tilemap{{
    {"aj1ccrl.3d", 0x200000, 0x3cc7a247, 0x000000},
    {"aj1ccrh.1d", 0x080000, 0x9d936030, 0x200000},
}};

// Aqua Jet carries a full four-ROM point bank per lane, so its point
// region is 6 MiB rather than the 4.5 MiB used by Time Crisis and Dirt
// Dash. The DSP splits the region into equal low/middle/high thirds.
constexpr std::array<asset_spec, 12> aqua_jet_points{{
    {"aj1ptrl0.18k", 0x080000, 0x16205d45, 0x000000},
    {"aj1ptrl1.16k", 0x080000, 0x6f114da7, 0x080000},
    {"aj1ptrl2.15k", 0x080000, 0x719b73f0, 0x100000},
    {"aj1ptrl3.14k", 0x080000, 0x9555fe31, 0x180000},
    {"aj1ptrm0.18j", 0x080000, 0x89bee2e0, 0x200000},
    {"aj1ptrm1.16j", 0x080000, 0x0ecf88c7, 0x280000},
    {"aj1ptrm2.15j", 0x080000, 0x829bc7ba, 0x300000},
    {"aj1ptrm3.14j", 0x080000, 0x7d0a222a, 0x380000},
    {"aj1ptru0.18f", 0x080000, 0x90d4e36a, 0x400000},
    {"aj1ptru1.16f", 0x080000, 0xbf0cf4bf, 0x480000},
    {"aj1ptru2.15f", 0x080000, 0x91ffbb77, 0x500000},
    {"aj1ptru3.14f", 0x080000, 0xd83d8d42, 0x580000},
}};

constexpr std::array<asset_spec, 2> aqua_jet_samples{{
    {"aj1wavea.2l", 0x400000, 0x8c72ea59, 0x000000},
    {"aj1waveb.1l", 0x400000, 0xab5a457f, 0x800000},
}};

constexpr std::array<asset_spec, 1> aqua_jet_eeprom{{
    {"aquajet_defaults.nv", 0x2000, 0xa00b3e44, 0},
}};

const super_system22_asset_set time_crisis_assets{
    "Time Crisis (World, TS2 Ver.B)",
    {time_crisis_program.begin(), time_crisis_program.end()},
    {time_crisis_mcu.begin(), time_crisis_mcu.end()},
    {time_crisis_sprites.begin(), time_crisis_sprites.end()},
    {time_crisis_textures.begin(), time_crisis_textures.end()},
    {time_crisis_tilemap.begin(), time_crisis_tilemap.end()},
    {time_crisis_points.begin(), time_crisis_points.end()},
    {time_crisis_samples.begin(), time_crisis_samples.end()},
    {}, 0x480000, 0x400000,
};

const super_system22_asset_set dirt_dash_assets{
    "Dirt Dash (World, DT2 Ver.C)",
    {dirt_dash_program.begin(), dirt_dash_program.end()},
    {dirt_dash_mcu.begin(), dirt_dash_mcu.end()},
    {dirt_dash_sprites.begin(), dirt_dash_sprites.end()},
    {dirt_dash_textures.begin(), dirt_dash_textures.end()},
    {dirt_dash_tilemap.begin(), dirt_dash_tilemap.end()},
    {dirt_dash_points.begin(), dirt_dash_points.end()},
    {dirt_dash_samples.begin(), dirt_dash_samples.end()},
    {}, 0x480000, 0,
};

const super_system22_asset_set aqua_jet_assets{
    "Aqua Jet (World, AJ2 Ver.B)",
    {aqua_jet_program.begin(), aqua_jet_program.end()},
    {aqua_jet_mcu.begin(), aqua_jet_mcu.end()},
    {aqua_jet_sprites.begin(), aqua_jet_sprites.end()},
    {aqua_jet_textures.begin(), aqua_jet_textures.end()},
    {aqua_jet_tilemap.begin(), aqua_jet_tilemap.end()},
    {aqua_jet_points.begin(), aqua_jet_points.end()},
    {aqua_jet_samples.begin(), aqua_jet_samples.end()},
    {aqua_jet_eeprom.begin(), aqua_jet_eeprom.end()}, 0x600000, 0,
};

bool has_zip_extension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".zip";
}

bool read_file_bytes(const fs::path& path, std::vector<uint8_t>& data) {
    data.clear();
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (!file) return false;

    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }

    data.resize(static_cast<std::size_t>(length));
    const std::size_t read = data.empty() ? 0 :
        std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (read != data.size()) {
        data.clear();
        return false;
    }
    return true;
}

bool locate_zip_basename(unzFile archive, const std::string& wanted) {
    if (unzLocateFile(archive, wanted.c_str(), 2) == UNZ_OK) return true;
    if (unzGoToFirstFile(archive) != UNZ_OK) return false;

    do {
        unz_file_info64 info{};
        std::array<char, 1024> name{};
        if (unzGetCurrentFileInfo64(archive, &info, name.data(), name.size(),
                                    nullptr, 0, nullptr, 0) != UNZ_OK)
            return false;
        if (fs::path(name.data()).filename() == wanted) return true;
    } while (unzGoToNextFile(archive) == UNZ_OK);
    return false;
}

bool read_zip_entry(const fs::path& archive_path, const std::string& entry,
                    std::vector<uint8_t>& data) {
    data.clear();
    unzFile archive = unzOpen64(archive_path.string().c_str());
    if (!archive) return false;

    if (!locate_zip_basename(archive, entry)) {
        unzClose(archive);
        return false;
    }

    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(archive, &info, nullptr, 0, nullptr, 0,
                                nullptr, 0) != UNZ_OK ||
        info.uncompressed_size > static_cast<ZPOS64_T>(SIZE_MAX) ||
        unzOpenCurrentFile(archive) != UNZ_OK) {
        unzClose(archive);
        return false;
    }

    data.resize(static_cast<std::size_t>(info.uncompressed_size));
    std::size_t total = 0;
    while (total < data.size()) {
        const unsigned chunk = static_cast<unsigned>(
            std::min<std::size_t>(data.size() - total, 1u << 20));
        const int count = unzReadCurrentFile(archive, data.data() + total, chunk);
        if (count <= 0) break;
        total += static_cast<std::size_t>(count);
    }

    const int close_result = unzCloseCurrentFile(archive);
    unzClose(archive);
    if (total != data.size() || close_result != UNZ_OK) {
        data.clear();
        return false;
    }
    return true;
}

std::string alternate_asset_name(std::string name) {
    // Older merged collections include a hyphen that MAME's current
    // canonical Ver.B filenames omit, for both Time Crisis and Aqua Jet.
    if (name.rfind("ts2verb.", 0) == 0)
        name.replace(0, 7, "ts2ver-b");
    if (name.rfind("aj2verb.", 0) == 0)
        name.replace(0, 7, "aj2ver-b");
    return name;
}

bool read_from_source(const fs::path& source, const std::string& name,
                      std::vector<uint8_t>& data) {
    if (has_zip_extension(source)) {
        if (read_zip_entry(source, name, data)) return true;
        // Alternate MAME naming used by ridgera2.zip.
        std::string alias = name;
        if (alias.rfind("rrs2_prg", 0) == 0)
            alias.replace(0, 8, "rrs_8_prg");
        if (alias != name && read_zip_entry(source, alias, data)) return true;
        alias = alternate_asset_name(name);
        if (alias != name && read_zip_entry(source, alias, data)) return true;
        return false;
    }
    if (read_file_bytes(source / name, data)) return true;
    const std::string alias = alternate_asset_name(name);
    return alias != name && read_file_bytes(source / alias, data);
}

bool source_contains(const fs::path& source, const char* name) {
    const auto contains = [&](const std::string& candidate) {
        if (!has_zip_extension(source)) return fs::exists(source / candidate);
        unzFile archive = unzOpen64(source.string().c_str());
        if (!archive) return false;
        const bool found = locate_zip_basename(archive, candidate);
        unzClose(archive);
        return found;
    };
    if (contains(name)) return true;
    const std::string alias = alternate_asset_name(name);
    return alias != name && contains(alias);
}

ridge_racer_rom_set identify_source(const fs::path& source) {
    if (source_contains(source, dirt_dash_program[0].name))
        return ridge_racer_rom_set::dirt_dash;
    if (source_contains(source, time_crisis_program[0].name))
        return ridge_racer_rom_set::time_crisis;
    if (source_contains(source, aqua_jet_program[0].name))
        return ridge_racer_rom_set::aqua_jet;
    if (source_contains(source, ace_driver_assets.program[0].name))
        return ridge_racer_rom_set::ace_driver;
    if (source_contains(source, victory_lap_assets.program[0].name))
        return ridge_racer_rom_set::victory_lap;
    if (source_contains(source, cyber_commando_assets.program[0].name))
        return ridge_racer_rom_set::cyber_commando;
    if (source_contains(source, ridge_racer_2_assets.program[0].name))
        return ridge_racer_rom_set::ridge_racer_2;
    if (source_contains(source, "rrs_8_prgll.4d"))
        return ridge_racer_rom_set::ridge_racer_2;
    if (source_contains(source, rave_racer_assets.program[0].name))
        return ridge_racer_rom_set::rave_racer;
    if (source_contains(source, ridge_racer_full_scale_assets.program[0].name))
        return ridge_racer_rom_set::ridge_racer_full_scale;
    if (source_contains(source, ridge_racer_assets.program[0].name))
        return ridge_racer_rom_set::ridge_racer;
    return ridge_racer_rom_set::unknown;
}

const game_asset_set* assets_for(ridge_racer_rom_set set) {
    switch (set) {
    case ridge_racer_rom_set::ridge_racer:
        return &ridge_racer_assets;
    case ridge_racer_rom_set::ridge_racer_full_scale:
        return &ridge_racer_full_scale_assets;
    case ridge_racer_rom_set::ridge_racer_2:
        return &ridge_racer_2_assets;
    case ridge_racer_rom_set::rave_racer:
    case ridge_racer_rom_set::ace_driver:
    case ridge_racer_rom_set::victory_lap:
    case ridge_racer_rom_set::cyber_commando:
    case ridge_racer_rom_set::time_crisis:
    case ridge_racer_rom_set::dirt_dash:
    case ridge_racer_rom_set::aqua_jet:
    case ridge_racer_rom_set::unknown:
        return nullptr;
    }
    return nullptr;
}

const super_system22_asset_set* super_assets_for(ridge_racer_rom_set set) {
    switch (set) {
    case ridge_racer_rom_set::time_crisis:
        return &time_crisis_assets;
    case ridge_racer_rom_set::dirt_dash:
        return &dirt_dash_assets;
    case ridge_racer_rom_set::aqua_jet:
        return &aqua_jet_assets;
    case ridge_racer_rom_set::ace_driver:
    case ridge_racer_rom_set::victory_lap:
    case ridge_racer_rom_set::cyber_commando:
    case ridge_racer_rom_set::unknown:
    case ridge_racer_rom_set::ridge_racer:
    case ridge_racer_rom_set::ridge_racer_full_scale:
    case ridge_racer_rom_set::ridge_racer_2:
    case ridge_racer_rom_set::rave_racer:
        return nullptr;
    }
    return nullptr;
}

const flexible_game_asset_set* flexible_assets_for(ridge_racer_rom_set set) {
    switch (set) {
    case ridge_racer_rom_set::ace_driver:
        return &ace_driver_assets;
    case ridge_racer_rom_set::victory_lap:
        return &victory_lap_assets;
    case ridge_racer_rom_set::cyber_commando:
        return &cyber_commando_assets;
    case ridge_racer_rom_set::time_crisis:
    case ridge_racer_rom_set::dirt_dash:
    case ridge_racer_rom_set::aqua_jet:
    case ridge_racer_rom_set::unknown:
    case ridge_racer_rom_set::ridge_racer:
    case ridge_racer_rom_set::ridge_racer_full_scale:
    case ridge_racer_rom_set::ridge_racer_2:
    case ridge_racer_rom_set::rave_racer:
        return nullptr;
    }
    return nullptr;
}

uint32_t calculate_crc(const std::vector<uint8_t>& data) {
    uLong crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc, data.data(), static_cast<uInt>(data.size()));
    return static_cast<uint32_t>(crc);
}

bool load_checked(const fs::path& source, const asset_spec& spec,
                  std::vector<uint8_t>& data) {
    if (!read_from_source(source, spec.name, data)) {
        std::fprintf(stderr, "Missing ROM: %s\n", spec.name);
        return false;
    }
    if (data.size() != spec.size) {
        std::fprintf(stderr, "Wrong size for %s: expected 0x%zx, got 0x%zx\n",
                     spec.name, spec.size, data.size());
        data.clear();
        return false;
    }
    const uint32_t crc = calculate_crc(data);
    if (crc != spec.crc) {
        if (std::string(spec.name).rfind("rr2_prg", 0) == 0) return true;
        std::fprintf(stderr, "Wrong CRC for %s: expected %08x, got %08x\n",
                     spec.name, spec.crc, crc);
        data.clear();
        return false;
    }
    return true;
}

template <typename Container>
bool load_region(const fs::path& source, const Container& specs,
                 std::size_t region_size, std::vector<uint8_t>& region,
                 uint8_t fill = 0) {
    region.assign(region_size, fill);
    std::vector<uint8_t> file;
    bool complete = true;
    for (const asset_spec& spec : specs) {
        if (!load_checked(source, spec, file)) {
            complete = false;
            continue;
        }
        if (spec.offset > region.size() ||
            file.size() > region.size() - spec.offset) {
            std::fprintf(stderr,
                         "ROM region overflow for %s: offset=%zu size=%zu region=%zu\n",
                         spec.name, spec.offset, file.size(), region.size());
            complete = false;
            continue;
        }
        const auto destination = region.begin() +
            static_cast<std::vector<uint8_t>::difference_type>(spec.offset);
        std::copy(file.begin(), file.end(), destination);
    }
    if (!complete) region.clear();
    return complete;
}

bool load_firmware_asset(const fs::path& source, const char* archive_name,
                         const asset_spec& spec, std::vector<uint8_t>& data) {
    if (source.empty()) return false;
    if (has_zip_extension(source)) return load_checked(source, spec, data);

    const fs::path loose_file = source / spec.name;
    if (fs::exists(loose_file) && load_checked(source, spec, data)) return true;
    const fs::path archive = source / archive_name;
    if (fs::exists(archive) && load_checked(archive, spec, data)) return true;
    std::fprintf(stderr, "Missing device firmware: %s (or %s)\n",
                 spec.name, archive_name);
    data.clear();
    return false;
}

ridge_racer_roms load_game_set(const fs::path& source,
                               const game_asset_set& assets) {
    ridge_racer_roms roms;

    roms.maincpu_rom.assign(0x200000, 0);
    std::vector<uint8_t> file;
    bool program_complete = true;
    for (const asset_spec& spec : assets.program) {
        if (!load_checked(source, spec, file)) {
            program_complete = false;
            continue;
        }
        for (std::size_t i = 0; i < file.size(); ++i)
            roms.maincpu_rom[i * 4 + spec.offset] = file[i];
    }
    if (!program_complete) roms.maincpu_rom.clear();

    load_region(source, assets.mcu, 0x80000, roms.mcu_rom);
    load_region(source, assets.textures, 0x1000000, roms.texture_rom);
    load_region(source, assets.tilemap, 0x280000, roms.tilemap_rom);
    load_region(source, assets.points, 0x300000, roms.point_rom);
    load_region(source, assets.samples, 0x1000000, roms.c352_samples);

    constexpr std::array<asset_spec, 3> gamma{{
        {"rr1gam.2d", 0x100, 0xb2161bce, 0x000},
        {"rr1gam.3d", 0x100, 0xb2161bce, 0x100},
        {"rr1gam.4d", 0x100, 0xb2161bce, 0x200},
    }};
    load_region(source, gamma, 0x300, roms.gamma_proms);

    return roms;
}

ridge_racer_roms load_rave_racer_set(const fs::path& source) {
    ridge_racer_roms roms;
    roms.texture_region_offset = 0;
    roms.texture_bank_count = 8;
    roms.texture_tile_high_bit_from_attr = false;
    roms.maincpu_rom.assign(0x200000, 0);
    std::vector<uint8_t> file;
    bool complete = true;
    for (const asset_spec& spec : rave_racer_assets.program) {
        if (!load_checked(source, spec, file)) { complete = false; continue; }
        for (std::size_t i = 0; i < file.size(); ++i)
            roms.maincpu_rom[i * 4 + spec.offset] = file[i];
    }
    if (!complete) roms.maincpu_rom.clear();
    load_region(source, rave_racer_assets.mcu, 0x80000, roms.mcu_rom);
    load_region(source, rave_racer_assets.textures, 0x1000000, roms.texture_rom);
    load_region(source, rave_racer_assets.tilemap, 0x280000, roms.tilemap_rom);
    load_region(source, rave_racer_assets.points, 0x600000, roms.point_rom);
    load_region(source, rave_racer_assets.samples, 0x1000000, roms.c352_samples);
    const asset_spec eeprom{"rv1eeprm.9e", 0x2000, 0xe00dd412, 0};
    load_checked(source, eeprom, roms.eeprom);
    constexpr std::array<asset_spec, 3> gamma{{
        {"rr1gam.2d", 0x100, 0xb2161bce, 0}, {"rr1gam.3d", 0x100, 0xb2161bce, 0x100},
        {"rr1gam.4d", 0x100, 0xb2161bce, 0x200}}};
    load_region(source, gamma, 0x300, roms.gamma_proms);
    return roms;
}

ridge_racer_roms load_flexible_game_set(
    const fs::path& source, const flexible_game_asset_set& assets) {
    ridge_racer_roms roms;
    roms.texture_region_offset = assets.texture_region_offset;
    roms.texture_bank_count = assets.texture_bank_count;
    roms.texture_tile_high_bit_from_attr =
        assets.texture_tile_high_bit_from_attr;

    roms.maincpu_rom.assign(0x200000, 0);
    std::vector<uint8_t> file;
    bool program_complete = true;
    for (const asset_spec& spec : assets.program) {
        if (!load_checked(source, spec, file)) {
            program_complete = false;
            continue;
        }
        for (std::size_t index = 0; index < file.size(); ++index)
            roms.maincpu_rom[index * 4 + spec.offset] = file[index];
    }
    if (!program_complete) roms.maincpu_rom.clear();

    load_region(source, assets.mcu, 0x80000, roms.mcu_rom);
    if (!roms.mcu_rom.empty() && assets.mcu_mirror_period != 0) {
        for (std::size_t offset = assets.mcu_mirror_period;
             offset < roms.mcu_rom.size(); offset += assets.mcu_mirror_period) {
            std::copy_n(roms.mcu_rom.begin(), assets.mcu_mirror_period,
                        roms.mcu_rom.begin() +
                            static_cast<std::vector<uint8_t>::difference_type>(offset));
        }
    }
    load_region(source, assets.textures, 0x1000000, roms.texture_rom);
    load_region(source, assets.tilemap, 0x280000, roms.tilemap_rom);
    load_region(source, assets.points, assets.point_region_size, roms.point_rom);
    load_region(source, assets.samples, 0x1000000, roms.c352_samples);
    if (!assets.eeprom.empty())
        load_region(source, assets.eeprom, 0x2000, roms.eeprom);

    constexpr std::array<asset_spec, 3> gamma{{
        {"rr1gam.2d", 0x100, 0xb2161bce, 0},
        {"rr1gam.3d", 0x100, 0xb2161bce, 0x100},
        {"rr1gam.4d", 0x100, 0xb2161bce, 0x200},
    }};
    load_region(source, gamma, 0x300, roms.gamma_proms);
    return roms;
}

ridge_racer_roms load_super_system22_set(
    const fs::path& source, const super_system22_asset_set& assets) {
    ridge_racer_roms roms;
    roms.super_system22 = true;
    roms.texture_region_offset = 0;
    roms.texture_bank_count = 8;
    roms.texture_tile_high_bit_from_attr = false;

    roms.maincpu_rom.assign(0x400000, 0);
    std::vector<uint8_t> file;
    bool program_complete = true;
    for (const asset_spec& spec : assets.program) {
        if (!load_checked(source, spec, file)) {
            program_complete = false;
            continue;
        }
        for (std::size_t index = 0; index < file.size(); ++index)
            roms.maincpu_rom[index * 4 + spec.offset] = file[index];
    }
    if (!program_complete) roms.maincpu_rom.clear();

    load_region(source, assets.mcu, 0x080000, roms.mcu_rom);
    load_region(source, assets.sprites, 0x1000000, roms.sprite_rom, 0xff);
    load_region(source, assets.textures, 0x1000000, roms.texture_rom);
    load_region(source, assets.tilemap, 0x280000, roms.tilemap_rom);
    load_region(source, assets.points, assets.point_region_size,
                roms.point_rom);
    if (load_region(source, assets.samples, 0x1000000, roms.c352_samples)) {
        for (std::size_t offset = 0; offset < assets.swapped_sample_bytes;
             offset += 2)
            std::swap(roms.c352_samples[offset],
                      roms.c352_samples[offset + 1]);
    }
    if (!assets.eeprom.empty())
        load_region(source, assets.eeprom, 0x2000, roms.eeprom);
    return roms;
}
} // namespace

void rom_loader::load_system_firmware(const std::string& path,
                                      ridge_racer_roms& roms,
                                      bool load_c74) {
    if (path.empty()) return;
    const fs::path source(path);
    const asset_spec c71{"c71.bin", 0x2000, 0x47c623ab, 0};
    const asset_spec c74{"c74.bin", 0x4000, 0xa3dce360, 0};
    load_firmware_asset(source, "namcoc71.zip", c71, roms.c71_firmware);
    if (load_c74)
        load_firmware_asset(source, "namcoc74.zip", c74, roms.c74_firmware);
}

ridge_racer_roms rom_loader::load_ridge_racer(const std::string& path,
                                               const std::string& bios_path) {
    const ridge_racer_rom_set set = identify_set(path);
    if (!is_working_set(set)) {
        if (set == ridge_racer_rom_set::ridge_racer_full_scale) {
            std::fprintf(stderr,
                         "Ridge Racer Full Scale is not working: its video ROM "
                         "dump and linked-cabinet display path are incomplete.\n");
        } else {
            std::fprintf(stderr, "Unsupported System 22 ROM set: %s\n",
                         path.c_str());
        }
        return {};
    }
    const game_asset_set* assets = assets_for(set);
    const flexible_game_asset_set* flexible_assets = flexible_assets_for(set);
    const super_system22_asset_set* super_assets = super_assets_for(set);
    if (!assets && !flexible_assets && !super_assets &&
        set != ridge_racer_rom_set::rave_racer) {
        std::fprintf(stderr, "Unsupported System 22 ROM set: %s\n",
                     path.c_str());
        return {};
    }

    std::printf("Loading %s ROMs from: %s\n", set_display_name(set),
                path.c_str());
    ridge_racer_roms roms;
    if (super_assets)
        roms = load_super_system22_set(fs::path(path), *super_assets);
    else if (set == ridge_racer_rom_set::rave_racer)
        roms = load_rave_racer_set(fs::path(path));
    else if (flexible_assets)
        roms = load_flexible_game_set(fs::path(path), *flexible_assets);
    else
        roms = load_game_set(fs::path(path), *assets);
    // Super System 22 maps its MCU window from the game data ROM and has no
    // separate C74 image; only the C71 DSP firmware is shared.
    load_system_firmware(bios_path.empty() ? path : bios_path, roms,
                         super_assets == nullptr);
    std::printf("ROM regions: main=%zu texture=%zu tilemap=%zu sprite=%zu "
                "point=%zu samples=%zu gamma=%zu c71=%zu c74=%zu\n",
                roms.maincpu_rom.size(), roms.texture_rom.size(),
                roms.tilemap_rom.size(), roms.sprite_rom.size(),
                roms.point_rom.size(),
                roms.c352_samples.size(), roms.gamma_proms.size(),
                roms.c71_firmware.size(), roms.c74_firmware.size());
    return roms;
}

ridge_racer_rom_set rom_loader::identify_set(const std::string& path) {
    return identify_source(fs::path(path));
}

const char* rom_loader::set_short_name(ridge_racer_rom_set set) {
    switch (set) {
    case ridge_racer_rom_set::ridge_racer: return "ridgerac";
    case ridge_racer_rom_set::ridge_racer_full_scale: return "ridgeracf";
    case ridge_racer_rom_set::ridge_racer_2: return "ridgera2";
    case ridge_racer_rom_set::rave_racer: return "raverace";
    case ridge_racer_rom_set::ace_driver: return "acedrive";
    case ridge_racer_rom_set::victory_lap: return "victlap";
    case ridge_racer_rom_set::cyber_commando: return "cybrcomm";
    case ridge_racer_rom_set::time_crisis: return "timecris";
    case ridge_racer_rom_set::dirt_dash: return "dirtdash";
    case ridge_racer_rom_set::aqua_jet: return "aquajet";
    case ridge_racer_rom_set::unknown: return "unknown";
    }
    return "unknown";
}

const char* rom_loader::set_display_name(ridge_racer_rom_set set) {
    if (set == ridge_racer_rom_set::ridge_racer_full_scale)
        return "Ridge Racer Full Scale — not working";
    if (set == ridge_racer_rom_set::rave_racer)
        return "Rave Racer";
    if (const super_system22_asset_set* assets = super_assets_for(set))
        return assets->display_name;
    if (const flexible_game_asset_set* assets = flexible_assets_for(set))
        return assets->display_name;
    const game_asset_set* assets = assets_for(set);
    return assets ? assets->display_name : "Unsupported ROM set";
}

bool rom_loader::is_working_set(ridge_racer_rom_set set) {
    return set == ridge_racer_rom_set::ridge_racer ||
           set == ridge_racer_rom_set::ridge_racer_2 ||
           set == ridge_racer_rom_set::rave_racer ||
           set == ridge_racer_rom_set::ace_driver ||
           set == ridge_racer_rom_set::victory_lap ||
           set == ridge_racer_rom_set::cyber_commando ||
           set == ridge_racer_rom_set::time_crisis ||
           set == ridge_racer_rom_set::dirt_dash ||
           set == ridge_racer_rom_set::aqua_jet;
}
