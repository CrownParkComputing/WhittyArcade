// Jetpac as a MANX plugin.
//
// This file is the whole boundary between the game and the arcade: it exports
// `manx_game_entry` and nothing else, and includes no host header but the
// ABI, so the plugin ships and updates without the arcade being rebuilt.
//
// The picture is rasterised on the CPU into an RGBA buffer - filled rectangles
// are enough to read and play this game, and a CPU rasteriser needs nothing
// from the host but a pixel buffer. The simulation names the THING each quad is
// rather than a colour, so artwork can replace these fills without the game
// logic knowing.

#include "manx_game_plugin.h"

#include "jetpac/jp_game.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// 256x192 at 4x. A whole multiple keeps every edge on a pixel boundary, and the
// resolution is ours to choose - this is not reproducing a Spectrum, only the
// game that ran on one.
constexpr uint32_t scale = 4;
constexpr uint32_t frame_width = static_cast<uint32_t>(jp::world_width) * scale;
constexpr uint32_t frame_height =
    static_cast<uint32_t>(jp::world_height) * scale;

struct rgba {
    uint8_t r, g, b, a;
};

// A Spectrum palette, because that is what the game looks like: saturated
// primaries on black, no shading. Deliberately NOT the 360 remake's look - the
// remake's own artwork is the thing this would swap in later.
constexpr rgba colour_space{6, 6, 14, 255};
constexpr rgba colour_ground{0, 168, 0, 255};
constexpr rgba colour_ground_top{0, 232, 0, 255};
constexpr rgba colour_platform{0, 200, 200, 255};
constexpr rgba colour_jetman{255, 255, 255, 255};
constexpr rgba colour_flame{255, 140, 0, 255};
constexpr rgba colour_laser{255, 255, 0, 255};
constexpr rgba colour_drone{216, 0, 216, 255};
constexpr rgba colour_ufo{0, 216, 216, 255};
constexpr rgba colour_rocket_base{200, 0, 0, 255};
constexpr rgba colour_rocket_mid{232, 232, 232, 255};
constexpr rgba colour_rocket_part{232, 160, 0, 255};
constexpr rgba colour_rocket_top{255, 0, 0, 255};
constexpr rgba colour_fuel{255, 216, 0, 255};
constexpr rgba colour_treasure{255, 0, 168, 255};
constexpr rgba colour_explosion{255, 255, 255, 255};

rgba colour_for(jp::sprite_kind kind) {
    switch (kind) {
    case jp::sprite_kind::ground: return colour_ground;
    case jp::sprite_kind::platform: return colour_platform;
    case jp::sprite_kind::jetman: return colour_jetman;
    case jp::sprite_kind::jetman_flame: return colour_flame;
    case jp::sprite_kind::laser: return colour_laser;
    case jp::sprite_kind::alien_drone: return colour_drone;
    case jp::sprite_kind::alien_ufo: return colour_ufo;
    case jp::sprite_kind::rocket_base: return colour_rocket_base;
    case jp::sprite_kind::rocket_mid: return colour_rocket_mid;
    case jp::sprite_kind::rocket_part: return colour_rocket_part;
    case jp::sprite_kind::rocket_top: return colour_rocket_top;
    case jp::sprite_kind::fuel_pod: return colour_fuel;
    case jp::sprite_kind::treasure: return colour_treasure;
    case jp::sprite_kind::explosion: return colour_explosion;
    }
    return colour_jetman;
}

struct instance {
    jp::game game;
    std::vector<uint8_t> pixels;
    std::vector<jp::cue> pending;

    instance()
        : pixels(static_cast<std::size_t>(frame_width) * frame_height * 4) {}
};

void fill_rect(std::vector<uint8_t>& pixels, int x0, int y0, int w, int h,
               rgba colour) {
    const int x1 = std::min<int>(x0 + w, static_cast<int>(frame_width));
    const int y1 = std::min<int>(y0 + h, static_cast<int>(frame_height));
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    for (int y = y0; y < y1; ++y) {
        uint8_t* row =
            pixels.data() + (static_cast<std::size_t>(y) * frame_width + x0) * 4;
        for (int x = x0; x < x1; ++x) {
            row[0] = colour.r;
            row[1] = colour.g;
            row[2] = colour.b;
            row[3] = colour.a;
            row += 4;
        }
    }
}

void render(instance& inst) {
    uint8_t* out = inst.pixels.data();
    for (std::size_t i = 0; i < inst.pixels.size(); i += 4) {
        out[i + 0] = colour_space.r;
        out[i + 1] = colour_space.g;
        out[i + 2] = colour_space.b;
        out[i + 3] = 255;
    }

    for (const jp::sprite_quad& quad : inst.game.quads()) {
        const int x = static_cast<int>(quad.x * scale);
        const int y = static_cast<int>(quad.y * scale);
        const int w = static_cast<int>(quad.w * scale);
        const int h = static_cast<int>(quad.h * scale);
        fill_rect(inst.pixels, x, y, w, h, colour_for(quad.kind));

        // A bright lip on the ground, which is the one piece of shading that
        // carries information: it is the line a player judges landings against.
        if (quad.kind == jp::sprite_kind::ground)
            fill_rect(inst.pixels, x, y, w, static_cast<int>(scale),
                      colour_ground_top);
    }
}

jp::player_intent to_intent(const manx_game_input& in) {
    jp::player_intent intent;
    intent.connected = in.connected != 0;
    intent.move_x = in.move_x;
    // Thrust on the second button and on the stick pushed up: a jetpack read
    // only from a button is awkward on a stick and only from a stick is awkward
    // on a pad, so it takes either.
    intent.thrust = (in.buttons & manx_game_button_secondary) != 0 ||
                    in.move_y > 0.4f;
    intent.fire = (in.buttons & manx_game_button_fire) != 0;
    intent.start = (in.buttons & manx_game_button_start) != 0;
    return intent;
}

void describe(manx_game_info* out_info) {
    if (out_info == nullptr) return;
    // NOT "jetpac": that key belongs to the recompiled Jetpac Refuelled, and
    // the catalogue REPLACES a row when two games share a short name rather
    // than listing both - so registering under it would make this plugin
    // silently vanish behind the recomp. They keep separate keys while the
    // recomp is still the reference for what this one has to do.
    out_info->short_name = "jetpac_native";
    out_info->display_name = "Jetpac (native)";
    out_info->publisher = "Ultimate Play The Game";
    out_info->max_players = 2;
    out_info->supports_network = 1;
    out_info->refresh_hz = 60.0;
}

manx_game_instance* create(const char* /*bundle_path*/) {
    return reinterpret_cast<manx_game_instance*>(new (std::nothrow) instance());
}

void destroy(manx_game_instance* handle) {
    delete reinterpret_cast<instance*>(handle);
}

void run_frame(manx_game_instance* handle, const manx_game_input* inputs,
               uint32_t player_count, manx_game_frame* out_frame) {
    instance* inst = reinterpret_cast<instance*>(handle);
    if (inst == nullptr || out_frame == nullptr) return;

    jp::player_intent intents[jp::game::max_players];
    const uint32_t seats =
        std::min<uint32_t>(player_count, jp::game::max_players);
    for (uint32_t seat = 0; seat < seats; ++seat)
        intents[seat] = to_intent(inputs[seat]);

    inst->game.step(intents, static_cast<int>(seats));
    for (jp::cue what : inst->game.take_cues()) inst->pending.push_back(what);
    render(*inst);

    out_frame->pixels = inst->pixels.data();
    out_frame->width = frame_width;
    out_frame->height = frame_height;
    out_frame->aspect_x = jp::tiles_wide;
    out_frame->aspect_y = jp::tiles_high;
}

void reset_game(manx_game_instance* handle) {
    if (instance* inst = reinterpret_cast<instance*>(handle)) {
        inst->game.reset();
        inst->pending.clear();
    }
}

void set_paused(manx_game_instance*, uint32_t) {
    // Pausing is the host holding back run_frame; a second paused state here
    // would be a second clock.
}

uint64_t score(manx_game_instance* handle) {
    const instance* inst = reinterpret_cast<const instance*>(handle);
    return inst != nullptr ? inst->game.score() : 0;
}

uint64_t state_checksum(manx_game_instance* handle) {
    const instance* inst = reinterpret_cast<const instance*>(handle);
    return inst != nullptr ? inst->game.checksum() : 0;
}

uint32_t take_audio_cues(manx_game_instance* handle,
                         manx_game_audio_cue* out, uint32_t max_cues) {
    instance* inst = reinterpret_cast<instance*>(handle);
    if (inst == nullptr || out == nullptr) return 0;
    const uint32_t count = std::min<uint32_t>(
        max_cues, static_cast<uint32_t>(inst->pending.size()));
    for (uint32_t i = 0; i < count; ++i) {
        out[i].cue = static_cast<uint32_t>(inst->pending[i]);
        out[i].gain = 1.0f;
        out[i].pan = 0.0f;
    }
    inst->pending.erase(inst->pending.begin(), inst->pending.begin() + count);
    return count;
}

const char* const cue_names[] = {"thrust", "fire",   "alien_hit", "collect",
                                 "deliver", "launch", "death"};

uint32_t describe_audio_cues(const char** out_names, uint32_t max_names) {
    const uint32_t total =
        static_cast<uint32_t>(sizeof(cue_names) / sizeof(cue_names[0]));
    if (out_names == nullptr) return total;
    const uint32_t count = std::min(max_names, total);
    for (uint32_t i = 0; i < count; ++i) out_names[i] = cue_names[i];
    return count;
}

const manx_game_api api = {
    MANX_GAME_ABI_VERSION,
    describe,
    create,
    destroy,
    run_frame,
    reset_game,
    set_paused,
    score,
    state_checksum,
    take_audio_cues,
    describe_audio_cues,
};

} // namespace

extern "C" MANX_GAME_EXPORT const manx_game_api* manx_game_entry(void) {
    return &api;
}
