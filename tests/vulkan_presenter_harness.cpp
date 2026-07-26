// Drives the real alternate_presenter through a System 22 frame with the
// Vulkan validation layers enabled.
//
// The offscreen shader test proves the shaders; this proves everything around
// them - swapchain, render passes, descriptor sets, image layouts and view
// types - which is where every bug in this port has actually been. Those
// failures are invisible from the outside: a windowed presenter window is
// created hidden and only shown after its first successful present, so
// anything that goes wrong before then looks exactly like "no window".
//
// It opens a small real window, so it is not part of the default ctest run.
// Set WHITTY_PRESENTER_HARNESS=1 to run it:
//
//   WHITTY_PRESENTER_HARNESS=1 ./build/vulkan_presenter_harness
//
// Validation output goes to stderr; the caller greps for it. A clean run
// prints the two return values and nothing else.

#include "arcade_presenter.h"
#include "wall_log.h"
#include "namco/system22/system22_rom.h"
#include "arcade_settings.h"
#include "system22_vulkan_uniforms.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Matches draw_vertex in src/arcade_renderer.cpp, asserted there against
// system22_vertex_layout.
struct harness_vertex {
    float position[3];
    float u_over_z;
    float v_over_z;
    float brightness_over_z;
    float one_over_z;
    float palette[2];
    float color_mode;
    float texture_bank;
    float direct;
    float fog;
    float fog_color[3];
    float viewport[2];
    float clip_rect[4];
    float render_flags[3];
};
static_assert(sizeof(harness_vertex) == system22_vertex_layout::stride);

// Every sheet, at the exact geometry the presenter allocates.
struct sheet_fill {
    scene_sheet sheet;
    std::size_t bytes;
    const char* name;
};

const sheet_fill sheets[]{
    // Deliberately HALF the sprite sheet, and deliberately FIRST: Ridge
    // Racer's sprite ROM fills 8MB of the 16MB image, and uploading it while
    // the shared staging buffer is still small is what recorded a copy
    // reading past the buffer's end - the out-of-bounds read that killed the
    // wall columns' devices. Order matters to the repro: a full-sized upload
    // before it grows the buffer and hides the fault. A clean validation run
    // here is the proof that partial sheets are safe.
    {scene_sheet::sprite_tiles, std::size_t{1024} * 1024 * 8, "sprite_tiles"},
    {scene_sheet::texture_tiles, std::size_t{2048} * 1024 * 8, "texture_tiles"},
    {scene_sheet::texture_map, std::size_t{256} * 4096 * 2, "texture_map"},
    {scene_sheet::texture_attr, std::size_t{1024} * 512, "texture_attr"},
    {scene_sheet::palette, std::size_t{256} * 128 * 3, "palette"},
    {scene_sheet::character_data, std::size_t{512} * 256, "character_data"},
    {scene_sheet::text_map, std::size_t{64} * 64 * 2, "text_map"},
};

} // namespace


// ---------------------------------------------------------------------------
// System 22 decode replay: renders REAL game sheets through the Vulkan tile
// decoder and diffs every pixel against a CPU port of the same chain.
//
// WHITTY_HARNESS_S22_ROM=<path to set .zip> selects the board. The palette is
// synthetic - palette RAM belongs to the running game, which is absent here -
// chosen so every pen maps to a distinct colour. A mismatch therefore means
// the GPU decoded a different pen than the reference for that screen pixel,
// and the first divergent stage of the chain can be read straight off the
// numbers. Built to answer why Rave Racer garbles where Ridge Racer does not:
// rave uniquely uses base 0, all 8 texture layers and no attr high bit.
namespace s22_replay {

struct decode_tables {
    std::vector<uint16_t> tile_words;   // CCRL, 0x100000 entries
    const uint8_t* attr{};              // CCRH, 1024*512
    const uint8_t* tiles{};             // CG, uploaded window
    std::size_t tiles_bytes{};          // bytes actually uploaded
    uint32_t base{};                    // texture_address_base
    bool high_bit{};                    // tile_high_bit_from_attr
};

// The synthetic palette: component planes filled so pen -> colour is
// bijective. Must match the sheet uploaded to the GPU exactly.
uint8_t palette_component(int component, int x, int y) {
    switch (component) {
    case 0: return static_cast<uint8_t>(x);
    case 1: return static_cast<uint8_t>(y * 2 + 1);
    default: return static_cast<uint8_t>((x * 3 + y * 7) ^ 0x5a);
    }
}

// The fragment shader's decode, integer for integer.
void cpu_decode(const decode_tables& tables, uint32_t bank, uint32_t u,
                uint32_t v, uint8_t out_rgb[3]) {
    const uint32_t tx = u & 0xfffu;
    const uint32_t ty = (v & 0xfffu) | (bank << 12u);
    const uint32_t tile_offset = ((ty << 4u) & 0xfff00u) | (tx >> 4u);
    uint32_t tile = tables.tile_words[tile_offset & 0xfffffu];
    const uint32_t packed_index = tile_offset >> 1u;
    const uint8_t packed_attr = tables.attr[packed_index & 0x7ffffu];
    const uint32_t attr = ((tile_offset & 1u) == 0u)
                              ? static_cast<uint32_t>(packed_attr >> 4)
                              : static_cast<uint32_t>(packed_attr & 0x0f);
    if (tables.high_bit && (attr & 1u) == 0u)
        tile = (tile & 0x3fffu) | 0x8000u;
    uint32_t ix = tx & 0x0fu;
    uint32_t iy = ty & 0x0fu;
    if ((attr & 4u) != 0u) ix = 15u - ix;
    if ((attr & 2u) != 0u) iy = 15u - iy;
    if ((attr & 8u) != 0u) std::swap(ix, iy);
    uint32_t pen = 0;
    const uint32_t tile_address = (tile << 8u) | (iy << 4u) | ix;
    if (tile_address >= tables.base) {
        const uint32_t relative = tile_address - tables.base;
        // Mirrors the upload: bytes past what the board supplied are zero.
        pen = relative < tables.tiles_bytes ? tables.tiles[relative] : 0u;
    }
    // color_mode 0, palette_base 0: palette index is the pen itself.
    out_rgb[0] = palette_component(0, static_cast<int>(pen & 0xffu),
                                   static_cast<int>((pen >> 8u) & 0x7fu));
    out_rgb[1] = palette_component(1, static_cast<int>(pen & 0xffu),
                                   static_cast<int>((pen >> 8u) & 0x7fu));
    out_rgb[2] = palette_component(2, static_cast<int>(pen & 0xffu),
                                   static_cast<int>((pen >> 8u) & 0x7fu));
}

int run(alternate_presenter& presenter, const char* rom_path) {
    std::printf("S22 decode replay: %s\n", rom_path);
    const ridge_racer_roms roms = rom_loader::load_ridge_racer(rom_path);
    if (roms.texture_rom.empty() || roms.tilemap_rom.size() < 0x280000) {
        std::fprintf(stderr, "replay: ROM set did not load\n");
        return 1;
    }
    std::printf("replay: base=0x%zx banks=%zu high_bit=%d texture=%zuMB\n",
                roms.texture_region_offset, roms.texture_bank_count,
                roms.texture_tile_high_bit_from_attr ? 1 : 0,
                roms.texture_rom.size() >> 20);

    decode_tables tables;
    tables.tile_words.resize(0x100000);
    for (std::size_t index = 0; index < tables.tile_words.size(); ++index)
        tables.tile_words[index] = static_cast<uint16_t>(
            roms.tilemap_rom[index * 2] |
            (static_cast<uint16_t>(roms.tilemap_rom[index * 2 + 1]) << 8));
    tables.attr = roms.tilemap_rom.data() + 0x200000;
    constexpr std::size_t bank_size = 0x200000;
    tables.tiles = roms.texture_rom.data() + roms.texture_region_offset;
    tables.tiles_bytes = bank_size * roms.texture_bank_count;
    tables.base = static_cast<uint32_t>(roms.texture_region_offset);
    tables.high_bit = roms.texture_tile_high_bit_from_attr;

    // Sheets, exactly as submit_textures hands them over.
    if (!presenter.upload_scene_sheet(scene_sheet::texture_tiles,
                                      tables.tiles, tables.tiles_bytes) ||
        !presenter.upload_scene_sheet(
            scene_sheet::texture_map, tables.tile_words.data(),
            tables.tile_words.size() * sizeof(uint16_t)) ||
        !presenter.upload_scene_sheet(scene_sheet::texture_attr, tables.attr,
                                      std::size_t{1024} * 512)) {
        std::fprintf(stderr, "replay: sheet upload failed\n");
        return 1;
    }
    std::vector<uint8_t> palette(std::size_t{256} * 128 * 3);
    for (int component = 0; component < 3; ++component)
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 256; ++x)
                palette[static_cast<std::size_t>(component) * 256 * 128 +
                        static_cast<std::size_t>(y) * 256 +
                        static_cast<std::size_t>(x)] =
                    palette_component(component, x, y);
    if (!presenter.upload_scene_sheet(scene_sheet::palette, palette.data(),
                                      palette.size())) {
        std::fprintf(stderr, "replay: palette upload failed\n");
        return 1;
    }

    int failures = 0;
    for (uint32_t bank = 0;
         bank < static_cast<uint32_t>(roms.texture_bank_count); ++bank) {
        // A full-screen quad walking texels 1:1: u = screen x, v = screen y.
        const auto vertex = [&](float x, float y, float u, float v) {
            harness_vertex out{};
            out.position[0] = x;
            out.position[1] = y;
            out.position[2] = 1.0f;
            out.one_over_z = 1.0f;
            out.u_over_z = u;
            out.v_over_z = v;
            out.brightness_over_z = 64.0f;
            out.texture_bank = static_cast<float>(bank);
            out.clip_rect[1] = 639.0f;
            out.clip_rect[3] = 479.0f;
            return out;
        };
        const std::vector<harness_vertex> vertices{
            vertex(-320.0f, -240.0f, 0.0f, 0.0f),
            vertex(320.0f, -240.0f, 640.0f, 0.0f),
            vertex(-320.0f, 240.0f, 0.0f, 480.0f),
            vertex(320.0f, -240.0f, 640.0f, 0.0f),
            vertex(320.0f, 240.0f, 640.0f, 480.0f),
            vertex(-320.0f, 240.0f, 0.0f, 480.0f),
        };
        system22_scene scene;
        scene.width = 640;
        scene.height = 480;
        scene.vertices = vertices.data();
        scene.vertex_count = vertices.size();
        scene.uniforms.texture_control[0] = tables.base;
        scene.uniforms.texture_control[1] = tables.high_bit ? 1u : 0u;
        for (int i = 0; i < 4; ++i) scene.uniforms.view_matrix[i * 5] = 1.0f;
        const board_overlays overlays;
        if (!presenter.render_system22_scene(scene) ||
            !presenter.render_board_frame(nullptr, 0, 0, true, overlays,
                                          false, 640, 480)) {
            std::fprintf(stderr, "replay: bank %u frame failed\n", bank);
            return 1;
        }
        std::vector<uint8_t> gpu;
        int width = 0;
        int height = 0;
        if (!presenter.read_scene_image(gpu, width, height) || width != 640 ||
            height != 480) {
            std::fprintf(stderr, "replay: readback failed\n");
            return 1;
        }
        // Orientation is resolved empirically: whichever vertical mapping
        // matches better is the one the pipeline uses, and a real decode bug
        // shows up as mismatches under BOTH.
        std::size_t best_mismatch = SIZE_MAX;
        int best_flip = 0;
        for (int flip = 0; flip < 2; ++flip) {
            std::size_t mismatch = 0;
            for (int y = 0; y < 480; ++y) {
                const int source_row = flip ? 479 - y : y;
                for (int x = 0; x < 640; ++x) {
                    uint8_t expected[3];
                    cpu_decode(tables, bank, static_cast<uint32_t>(x),
                               static_cast<uint32_t>(y), expected);
                    const uint8_t* got =
                        gpu.data() +
                        (static_cast<std::size_t>(source_row) * 640 + x) * 4;
                    if (got[0] != expected[0] || got[1] != expected[1] ||
                        got[2] != expected[2])
                        ++mismatch;
                }
            }
            if (mismatch < best_mismatch) {
                best_mismatch = mismatch;
                best_flip = flip;
            }
        }
        std::printf("replay: bank %u mismatch %zu/%d pixels (flip=%d)\n",
                    bank, best_mismatch, 640 * 480, best_flip);
        if (best_mismatch > 0) {
            ++failures;
            // Name the first divergent pixel with the CPU's full trace.
            for (int y = 0; y < 480 && failures; ++y) {
                const int source_row = best_flip ? 479 - y : y;
                for (int x = 0; x < 640; ++x) {
                    uint8_t expected[3];
                    cpu_decode(tables, bank, static_cast<uint32_t>(x),
                               static_cast<uint32_t>(y), expected);
                    const uint8_t* got =
                        gpu.data() +
                        (static_cast<std::size_t>(source_row) * 640 + x) * 4;
                    if (got[0] == expected[0] && got[1] == expected[1] &&
                        got[2] == expected[2])
                        continue;
                    std::printf(
                        "replay: first diff bank %u at %d,%d gpu %02x%02x%02x"
                        " cpu %02x%02x%02x\n",
                        bank, x, y, got[0], got[1], got[2], expected[0],
                        expected[1], expected[2]);
                    y = 480;
                    break;
                }
            }
        }
    }
    std::printf(failures ? "replay: %d bank(s) diverge\n"
                         : "replay: GPU decode matches CPU reference on all "
                           "banks\n",
                failures);
    return failures ? 1 : 0;
}

} // namespace s22_replay

int main() {
    if (const char* enabled = std::getenv("WHITTY_PRESENTER_HARNESS");
        !enabled || *enabled != '1') {
        std::printf("Set WHITTY_PRESENTER_HARNESS=1 to run; skipping.\n");
        return 0;
    }
    // Turn the layers on before the loader builds an instance. Doing it here
    // rather than relying on the caller means the harness always validates.
    // Only if the caller has not chosen for itself - hence the lookup first,
    // which is also the whole of what setenv's overwrite flag does.
    if (!std::getenv("VK_LOADER_LAYERS_ENABLE")) {
#if defined(_WIN32)
        _putenv_s("VK_LOADER_LAYERS_ENABLE", "*validation*");
#else
        setenv("VK_LOADER_LAYERS_ENABLE", "*validation*", 1);
#endif
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    emulator_settings settings;
    settings.renderer = renderer_backend::vulkan;
    settings.fullscreen = false;
    settings.vsync = false;
    settings.window_width = 640;
    // Wall mode, for reproducing multi-process wall failures without a game:
    // WHITTY_HARNESS_WALL="slot,count" makes this instance one column of a
    // wall, taking the same early-show and compositor-placement path a real
    // column takes during startup.
    if (const char* wall = std::getenv("WHITTY_HARNESS_WALL")) {
        int slot = 0;
        int count = 0;
        if (std::sscanf(wall, "%d,%d", &slot, &count) == 2 && count > 1) {
            settings.wall_slot = slot;
            settings.wall_count = count;
            // The real wall opens its column log in main; the harness has to
            // do the same or presenter notes written before the first
            // presented frame vanish.
            whitty_wall_log::begin(slot, count);
        }
    }

    const char* replay_rom = std::getenv("WHITTY_HARNESS_S22_ROM");

    alternate_presenter presenter;
    if (!presenter.initialize(renderer_backend::vulkan, 640, 480, settings)) {
        std::fprintf(stderr, "FAIL: presenter did not initialise\n");
        SDL_Quit();
        return 1;
    }

    if (replay_rom) {
        const int result = s22_replay::run(presenter, replay_rom);
        presenter.shutdown();
        SDL_Quit();
        return result;
    }


    // Fill every sheet. A board that leaves one empty is a case that has
    // already bitten once - Ridge Racer has no sprite region at all - but
    // here the point is to exercise the uploads themselves.
    for (const sheet_fill& fill : sheets) {
        std::vector<uint8_t> pixels(fill.bytes, 0);
        for (std::size_t index = 0; index < pixels.size(); index += 997)
            pixels[index] = static_cast<uint8_t>(index);
        if (!presenter.upload_scene_sheet(fill.sheet, pixels.data(),
                                          pixels.size())) {
            std::fprintf(stderr, "FAIL: sheet upload rejected: %s\n",
                         fill.name);
            SDL_Quit();
            return 1;
        }
    }

    // A quad across the top half of the board, in the direct-polygon mode
    // that needs no perspective set-up.
    const auto vertex = [](float x, float y) {
        harness_vertex v{};
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = 1.0f;
        v.one_over_z = 1.0f;
        v.brightness_over_z = 64.0f;
        v.direct = 1.0f;
        v.clip_rect[1] = 639.0f;
        v.clip_rect[3] = 479.0f;
        return v;
    };
    const std::vector<harness_vertex> vertices{
        vertex(-320.0f, 0.0f), vertex(320.0f, 0.0f), vertex(-320.0f, 240.0f),
        vertex(320.0f, 0.0f), vertex(320.0f, 240.0f), vertex(-320.0f, 240.0f),
    };

    system22_scene scene;
    scene.width = 640;
    scene.height = 480;
    scene.vertices = vertices.data();
    scene.vertex_count = vertices.size();
    for (int i = 0; i < 4; ++i) scene.uniforms.view_matrix[i * 5] = 1.0f;
    scene.draw_text = true;
    scene.apply_board_color = true;

    // Several frames: a one-frame run would miss anything that only goes
    // wrong once a resource is reused or a fence has actually been waited on.
    int failures = 0;
    for (int frame = 0; frame < 8; ++frame) {
        const bool rasterised = presenter.render_system22_scene(scene);
        const board_overlays overlays;
        const bool presented =
            rasterised && presenter.render_board_frame(nullptr, 0, 0, true,
                                                       overlays, false, 640,
                                                       480);
        if (!rasterised || !presented) {
            std::fprintf(stderr,
                         "FAIL: frame %d scene=%s present=%s\n", frame,
                         rasterised ? "ok" : "FAILED",
                         presented ? "ok" : "FAILED");
            ++failures;
            break;
        }
        SDL_PumpEvents();
    }

    // The arcade wall: three columns of one surface, each fed its own picture
    // and all presented together. This is the path that decides whether a
    // wall is one window or four, and every resource it touches - a pane
    // image and descriptor set per column - exists only here, so nothing
    // else in the harness would exercise it.
    emulator_settings wall = settings;
    wall.wall_count = 3;
    wall.fullscreen = true;
    presenter.apply_settings(wall);
    // Polygon ordering: System 22 composites in the DSP's priority order -
    // the order of the vertex array - NOT by depth; the OpenGL path draws
    // with GL_DEPTH_TEST disabled for exactly this reason. Two overlapping
    // full-screen quads with different palette bases are drawn in both
    // orders. Painter's semantics make the outcome depend on the order (the
    // later quad wins); a depth test resolves both orders the same way -
    // which is precisely the bug that garbled Rave Racer's dense scenes
    // while Ridge Racer's open roads looked fine.
    {
        // A palette where different bases decode to different colours.
        std::vector<uint8_t> pin_palette(std::size_t{256} * 128 * 3);
        for (int component = 0; component < 3; ++component)
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 256; ++x)
                    pin_palette[static_cast<std::size_t>(component) * 256 *
                                    128 +
                                static_cast<std::size_t>(y) * 256 +
                                static_cast<std::size_t>(x)] =
                        static_cast<uint8_t>(component == 0 ? x : x ^ 0xa5);
        if (!presenter.upload_scene_sheet(scene_sheet::palette,
                                          pin_palette.data(),
                                          pin_palette.size())) {
            std::fprintf(stderr, "FAIL: ordering palette upload\n");
            ++failures;
        }
        const auto quad = [&](float z, float palette_base) {
            std::vector<harness_vertex> corners;
            const auto corner = [&](float x, float y) {
                harness_vertex v{};
                v.position[0] = x;
                v.position[1] = y;
                v.position[2] = z;
                v.one_over_z = 1.0f;
                v.brightness_over_z = 64.0f;
                v.direct = 1.0f;
                v.palette[0] = palette_base;
                v.clip_rect[1] = 639.0f;
                v.clip_rect[3] = 479.0f;
                return v;
            };
            corners = {corner(-320.0f, -240.0f), corner(320.0f, -240.0f),
                       corner(-320.0f, 240.0f),  corner(320.0f, -240.0f),
                       corner(320.0f, 240.0f),   corner(-320.0f, 240.0f)};
            return corners;
        };
        const auto centre_of = [&](const std::vector<harness_vertex>& first,
                                   const std::vector<harness_vertex>& second,
                                   uint8_t out[3]) {
            std::vector<harness_vertex> vertices = first;
            vertices.insert(vertices.end(), second.begin(), second.end());
            system22_scene ordering;
            ordering.width = 640;
            ordering.height = 480;
            ordering.vertices = vertices.data();
            ordering.vertex_count = vertices.size();
            for (int i = 0; i < 4; ++i)
                ordering.uniforms.view_matrix[i * 5] = 1.0f;
            const board_overlays none;
            if (!presenter.render_system22_scene(ordering) ||
                !presenter.render_board_frame(nullptr, 0, 0, true, none,
                                              false, 640, 480))
                return false;
            std::vector<uint8_t> shot;
            int width = 0;
            int height = 0;
            if (!presenter.read_scene_image(shot, width, height) ||
                width != 640)
                return false;
            const uint8_t* centre =
                shot.data() + (std::size_t{240} * 640 + 320) * 4;
            out[0] = centre[0];
            out[1] = centre[1];
            out[2] = centre[2];
            return true;
        };
        // The far quad drawn first, then reversed. Depth testing makes the
        // far quad lose both times; painter order hands the win to whichever
        // is drawn last, so the two results must differ.
        const std::vector<harness_vertex> near_quad = quad(1.0f, 0.0f);
        const std::vector<harness_vertex> far_quad = quad(5.0f, 128.0f);
        uint8_t far_then_near[3]{};
        uint8_t near_then_far[3]{};
        if (!centre_of(far_quad, near_quad, far_then_near) ||
            !centre_of(near_quad, far_quad, near_then_far)) {
            std::fprintf(stderr, "FAIL: ordering scenes did not render\n");
            ++failures;
        } else if (far_then_near[0] == near_then_far[0] &&
                   far_then_near[1] == near_then_far[1] &&
                   far_then_near[2] == near_then_far[2]) {
            std::fprintf(stderr,
                         "FAIL: polygon order ignored - a depth test is "
                         "resolving what must composite painter-style "
                         "(both orders gave %02x %02x %02x)\n",
                         far_then_near[0], far_then_near[1],
                         far_then_near[2]);
            ++failures;
        }
    }

    // Topology: the polygon array is a triangle LIST. Drawn as a strip -
    // which one flag flip silently caused - every consecutive vertex triplet
    // becomes a phantom triangle, filling the screen with junk. Two tiny
    // triangles in opposite corners: the space between them must stay
    // untouched.
    {
        const auto point = [&](float x, float y) {
            harness_vertex v{};
            v.position[0] = x;
            v.position[1] = y;
            v.position[2] = 1.0f;
            v.one_over_z = 1.0f;
            v.brightness_over_z = 64.0f;
            v.direct = 1.0f;
            v.clip_rect[1] = 639.0f;
            v.clip_rect[3] = 479.0f;
            return v;
        };
        const std::vector<harness_vertex> two_triangles{
            point(-310.0f, -230.0f), point(-280.0f, -230.0f),
            point(-310.0f, -200.0f), point(280.0f, 200.0f),
            point(310.0f, 200.0f),   point(280.0f, 230.0f),
        };
        system22_scene sparse;
        sparse.width = 640;
        sparse.height = 480;
        sparse.vertices = two_triangles.data();
        sparse.vertex_count = two_triangles.size();
        for (int i = 0; i < 4; ++i)
            sparse.uniforms.view_matrix[i * 5] = 1.0f;
        const board_overlays none;
        if (presenter.render_system22_scene(sparse) &&
            presenter.render_board_frame(nullptr, 0, 0, true, none, false,
                                         640, 480)) {
            std::vector<uint8_t> shot;
            int shot_w = 0;
            int shot_h = 0;
            if (presenter.read_scene_image(shot, shot_w, shot_h) &&
                shot_w == 640) {
                const uint8_t* centre =
                    shot.data() + (std::size_t{240} * 640 + 320) * 4;
                if (centre[0] || centre[1] || centre[2]) {
                    std::fprintf(stderr,
                                 "FAIL: a phantom triangle crossed the "
                                 "screen centre (%02x %02x %02x) - the "
                                 "polygon list is being drawn as a strip\n",
                                 centre[0], centre[1], centre[2]);
                    ++failures;
                }
            } else {
                std::fprintf(stderr, "FAIL: topology readback\n");
                ++failures;
            }
        } else {
            std::fprintf(stderr, "FAIL: topology scene\n");
            ++failures;
        }
    }

    // A cabinet bezel, and a twin-cabinet surface: two panes, both sampling
    // the one board picture. That "both panes" part is worth driving here -
    // a pane that stops being drawn is invisible from any unit test and
    // shows up only as a twin cabinet with a dead half.
    std::vector<uint8_t> art(400 * 300 * 4, 200);
    bezel_frame art_frame;
    art_frame.pixels = art.data();
    art_frame.width = 400;
    art_frame.height = 300;
    art_frame.cutout_x = 20;
    art_frame.cutout_y = 15;
    art_frame.cutout_width = 360;
    art_frame.cutout_height = 270;
    if (!presenter.set_bezel(art_frame)) {
        std::fprintf(stderr, "FAIL: bezel upload\n");
        ++failures;
    }

    emulator_settings twin = settings;
    twin.output = output_mode::dual;
    twin.fullscreen = true;
    presenter.apply_settings(twin);
    for (int frame = 0; frame < 8 && !failures; ++frame) {
        const bool rasterised = presenter.render_system22_scene(scene);
        const board_overlays overlays;
        if (rasterised && presenter.render_board_frame(nullptr, 0, 0, true,
                                                       overlays, false, 640,
                                                       480))
            continue;
        std::fprintf(stderr, "FAIL: twin frame %d\n", frame);
        ++failures;
    }

    presenter.shutdown();
    SDL_Quit();
    if (failures) return 1;
    std::printf("Presenter drove 8 System 22 frames: scene=ok present=ok\n");
    std::printf("Presenter drove 8 bezelled twin-cabinet frames: ok\n");
    return 0;
}
