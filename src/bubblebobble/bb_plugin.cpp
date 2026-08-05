// Bubble Bobble as a MANX plugin.
//
// This file is the whole boundary between the game and the arcade: it exports
// `manx_game_entry` and nothing else. It deliberately includes NO host header
// other than the ABI, so the plugin can be built and shipped without the
// arcade, and updated without rebuilding it.
//
// The picture is rasterised on the CPU into an RGBA buffer. Filled rectangles
// are all this game needs to be readable and playable, and a CPU rasteriser
// needs nothing from the host beyond a pixel buffer - so it proves the plugin
// path end to end before any renderer work, and it keeps this first cut honest
// about what has actually been built. Textured sprites come later, from the
// artwork the recompiled build decodes at run time; the simulation already
// names the THING each quad is (`sprite_kind`) rather than a colour, so that
// swap does not reach into the game logic.

#include "manx_game_plugin.h"

#include "bubblebobble/bb_game.h"

// The bundle's artwork is the GAME'S OWN, decoded out of the recompiled build
// and written into <bundle>/art. stb_image is header-only, so loading it costs
// the plugin no link dependency and the arcade nothing at all.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <string>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

// The world is 32x25 tiles of 8 units. Rendering it at a whole multiple keeps
// every edge on a pixel boundary, which is what stops a tile grid from
// shimmering. 4x is a deliberate choice over the console's 720p: this is not
// reproducing a machine, so the resolution is ours to pick.
constexpr uint32_t scale = 4;
constexpr uint32_t frame_width =
    static_cast<uint32_t>(bb::world_width) * scale;
constexpr uint32_t frame_height =
    static_cast<uint32_t>(bb::world_height) * scale;

struct rgba {
    uint8_t r, g, b, a;
};

// Colours measured from the game's own decoded textures where one exists.
// Bub's body and the Zen-chan's shell are the exact texels the recompiled
// build samples, read out of guest memory - so the palette is the game's,
// not a guess at it.
constexpr rgba colour_background{18, 12, 10, 255};
constexpr rgba colour_wall{232, 163, 61, 255};
constexpr rgba colour_block{240, 169, 60, 255};
constexpr rgba colour_block_top{80, 200, 72, 255};
constexpr rgba colour_player_green{50, 199, 78, 255};  // Bub, from 0866E000
constexpr rgba colour_player_blue{78, 140, 224, 255};
constexpr rgba colour_enemy{176, 176, 215, 255};       // Zen-chan, from 06F90000
constexpr rgba colour_enemy_angry{215, 120, 130, 255};
constexpr rgba colour_bubble{190, 233, 245, 255};
constexpr rgba colour_bubble_full{150, 214, 232, 255};
constexpr rgba colour_fruit{255, 111, 160, 255};
constexpr rgba colour_pop{255, 255, 255, 255};

rgba colour_for(bb::sprite_kind kind) {
    switch (kind) {
    case bb::sprite_kind::wall: return colour_wall;
    case bb::sprite_kind::block: return colour_block;
    case bb::sprite_kind::player_green: return colour_player_green;
    case bb::sprite_kind::player_blue: return colour_player_blue;
    case bb::sprite_kind::enemy: return colour_enemy;
    case bb::sprite_kind::enemy_angry: return colour_enemy_angry;
    case bb::sprite_kind::bubble: return colour_bubble;
    case bb::sprite_kind::bubble_full: return colour_bubble_full;
    case bb::sprite_kind::fruit: return colour_fruit;
    case bb::sprite_kind::pop: return colour_pop;
    }
    return colour_pop;
}

// A decoded image from the bundle. Absent is not an error: the game draws its
// flat fills when there is no artwork, so a bundle with nothing in it still
// plays. That is deliberate - artwork is owned content and cannot be a
// requirement for the code to run.
struct sheet {
    std::vector<uint8_t> rgba;
    int w{};
    int h{};
    bool loaded() const noexcept { return w > 0 && h > 0; }
};

sheet load_sheet(const std::string& path) {
    sheet out;
    int channels = 0;
    stbi_uc* data = stbi_load(path.c_str(), &out.w, &out.h, &channels, 4);
    if (data == nullptr) {
        out.w = 0;
        out.h = 0;
        return out;
    }
    out.rgba.assign(data, data + static_cast<std::size_t>(out.w) * out.h * 4);
    stbi_image_free(data);
    return out;
}

struct instance {
    bb::game game;
    std::vector<uint8_t> pixels;
    std::vector<bb::cue> pending;
    sheet tiles;
    sheet player;
    sheet enemy;

    instance() : pixels(static_cast<std::size_t>(frame_width) * frame_height * 4) {}
};

// Where in the tile sheet each thing is. The atlas is 16x16 tiles; these two
// were picked by eye off the decoded sheet and are recorded here rather than
// in a comment somewhere else, because they are the only magic numbers in the
// renderer.
constexpr int tile_px = 16;
constexpr int ledge_tile_x = 0;
constexpr int ledge_tile_y = 16;
constexpr int wall_tile_x = 0;
constexpr int wall_tile_y = 32;

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

// Blit a whole sheet into a destination rect, nearest-neighbour, honouring
// alpha and optionally mirrored. Sprites are their own images rather than
// regions of an atlas, because they are rendered per character offline and a
// sheet layout would be a packing decision nothing here needs yet.
void blit_sprite(std::vector<uint8_t>& pixels, const sheet& from, int x0,
                 int y0, int w, int h, bool mirror) {
    const int x1 = std::min<int>(x0 + w, static_cast<int>(frame_width));
    const int y1 = std::min<int>(y0 + h, static_cast<int>(frame_height));
    const int start_x = std::max(0, x0);
    const int start_y = std::max(0, y0);
    for (int y = start_y; y < y1; ++y) {
        const int ty = (y - y0) * from.h / std::max(1, h);
        for (int x = start_x; x < x1; ++x) {
            int tx = (x - x0) * from.w / std::max(1, w);
            if (mirror) tx = from.w - 1 - tx;
            if (tx < 0 || tx >= from.w || ty < 0 || ty >= from.h) continue;
            const uint8_t* texel =
                from.rgba.data() +
                (static_cast<std::size_t>(ty) * from.w + tx) * 4;
            if (texel[3] < 128) continue;
            uint8_t* out = pixels.data() +
                           (static_cast<std::size_t>(y) * frame_width + x) * 4;
            out[0] = texel[0];
            out[1] = texel[1];
            out[2] = texel[2];
            out[3] = 255;
        }
    }
}

// Blit a 16x16 atlas tile into a destination rect, nearest-neighbour, honouring
// the texture's alpha. Nearest rather than filtered on purpose: this is pixel
// art at a whole-number scale, and smoothing it would only blur it.
void blit_tile(std::vector<uint8_t>& pixels, const sheet& from, int src_x,
               int src_y, int x0, int y0, int w, int h) {
    const int x1 = std::min<int>(x0 + w, static_cast<int>(frame_width));
    const int y1 = std::min<int>(y0 + h, static_cast<int>(frame_height));
    const int start_x = std::max(0, x0);
    const int start_y = std::max(0, y0);
    for (int y = start_y; y < y1; ++y) {
        const int ty = src_y + (y - y0) * tile_px / std::max(1, h);
        for (int x = start_x; x < x1; ++x) {
            const int tx = src_x + (x - x0) * tile_px / std::max(1, w);
            if (tx < 0 || tx >= from.w || ty < 0 || ty >= from.h) continue;
            const uint8_t* texel =
                from.rgba.data() +
                (static_cast<std::size_t>(ty) * from.w + tx) * 4;
            if (texel[3] < 128) continue; // transparent texel draws nothing
            uint8_t* out = pixels.data() +
                           (static_cast<std::size_t>(y) * frame_width + x) * 4;
            out[0] = texel[0];
            out[1] = texel[1];
            out[2] = texel[2];
            out[3] = 255;
        }
    }
}

void render(instance& inst) {
    // Clear. A memset is enough because the background is opaque and flat; the
    // moment it stops being either, this becomes a fill and the cost moves.
    uint8_t* out = inst.pixels.data();
    for (std::size_t i = 0; i < inst.pixels.size(); i += 4) {
        out[i + 0] = colour_background.r;
        out[i + 1] = colour_background.g;
        out[i + 2] = colour_background.b;
        out[i + 3] = 255;
    }

    for (const bb::sprite_quad& quad : inst.game.quads()) {
        const int x = static_cast<int>(quad.x * scale);
        const int y = static_cast<int>(quad.y * scale);
        const int w = static_cast<int>(quad.w * scale);
        const int h = static_cast<int>(quad.h * scale);
        // The game's own tiles where the bundle has them, flat fills where it
        // does not. Only the LEVEL is textured so far: the character skins in
        // the same bundle are 3D model textures, UV-mapped for a mesh Neo
        // draws, so painting one onto a flat quad would produce nonsense
        // rather than a Bub.
        // Characters first: their sprites were rasterised from the game's own
        // meshes, so this IS the game's artwork and not a stand-in for it.
        const sheet* character = nullptr;
        if (quad.kind == bb::sprite_kind::player_green && inst.player.loaded())
            character = &inst.player;
        if (quad.kind == bb::sprite_kind::enemy && inst.enemy.loaded())
            character = &inst.enemy;
        if (character != nullptr) {
            blit_sprite(inst.pixels, *character, x, y, w, h, quad.facing_left);
            continue;
        }

        const bool textured =
            inst.tiles.loaded() && (quad.kind == bb::sprite_kind::block ||
                                    quad.kind == bb::sprite_kind::wall);
        if (textured) {
            const bool ledge = quad.kind == bb::sprite_kind::block;
            blit_tile(inst.pixels, inst.tiles,
                      ledge ? ledge_tile_x : wall_tile_x,
                      ledge ? ledge_tile_y : wall_tile_y, x, y, w, h);
            continue;
        }

        fill_rect(inst.pixels, x, y, w, h, colour_for(quad.kind));

        // A ledge reads as a ledge because of the green line along its top -
        // that is the one piece of the original's look that carries gameplay
        // information, since it is what tells a player where they can stand.
        if (quad.kind == bb::sprite_kind::block)
            fill_rect(inst.pixels, x, y, w, static_cast<int>(scale),
                      colour_block_top);

        // Bubbles read as hollow. Cheaper than alpha and clearer at this size:
        // an inner rectangle in the background colour makes a ring.
        if (quad.kind == bb::sprite_kind::bubble)
            fill_rect(inst.pixels, x + static_cast<int>(scale),
                      y + static_cast<int>(scale),
                      w - static_cast<int>(scale) * 2,
                      h - static_cast<int>(scale) * 2, colour_background);
    }
}

bb::player_intent to_intent(const manx_game_input& in) {
    bb::player_intent intent;
    intent.connected = in.connected != 0;
    intent.move_x = in.move_x;
    // Jump on the second button, and on the stick pushed up. A platformer read
    // only from a button is awkward on a stick, and one read only from the
    // stick is awkward on a pad, so it takes either.
    intent.jump = (in.buttons & manx_game_button_secondary) != 0 ||
                  in.move_y > 0.5f;
    intent.fire = (in.buttons & manx_game_button_fire) != 0;
    intent.start = (in.buttons & manx_game_button_start) != 0;
    return intent;
}

void describe(manx_game_info* out_info) {
    if (out_info == nullptr) return;
    // NOT "bubblebobble": that key already belongs to the recompiled Neo!
    // title, and the catalogue REPLACES a row rather than joining it when two
    // games share a short name - so registering under it made this plugin
    // vanish behind the recomp with nothing reported. Both have to be
    // launchable while the recomp is still the reference for what this one has
    // to do, so they get separate keys, separate bundles and separate scores.
    out_info->short_name = "bubblebobble_native";
    out_info->display_name = "Bubble Bobble (native)";
    out_info->publisher = "Taito";
    out_info->max_players = 2;
    out_info->supports_network = 1;
    out_info->refresh_hz = 60.0;
}

manx_game_instance* create(const char* bundle_path) {
    instance* made = new (std::nothrow) instance();
    if (made == nullptr) return nullptr;
    // Artwork is optional. A bundle with no art/ plays exactly as before, in
    // flat colours - the code has no owned content in it and does not require
    // any to run.
    if (bundle_path != nullptr && *bundle_path != 0) {
        const std::string art = std::string(bundle_path) + "/art/";
        made->tiles = load_sheet(art + "tiles.png");
        made->player = load_sheet(art + "player_green.png");
        made->enemy = load_sheet(art + "enemy.png");
    }
    return reinterpret_cast<manx_game_instance*>(made);
}

void destroy(manx_game_instance* handle) {
    delete reinterpret_cast<instance*>(handle);
}

void run_frame(manx_game_instance* handle, const manx_game_input* inputs,
               uint32_t player_count, manx_game_frame* out_frame) {
    instance* inst = reinterpret_cast<instance*>(handle);
    if (inst == nullptr || out_frame == nullptr) return;

    bb::player_intent intents[bb::game::max_players];
    const uint32_t seats =
        std::min<uint32_t>(player_count, bb::game::max_players);
    for (uint32_t seat = 0; seat < seats; ++seat)
        intents[seat] = to_intent(inputs[seat]);

    inst->game.step(intents, static_cast<int>(seats));
    for (bb::cue what : inst->game.take_cues()) inst->pending.push_back(what);
    render(*inst);

    out_frame->pixels = inst->pixels.data();
    out_frame->width = frame_width;
    out_frame->height = frame_height;
    // Given as a ratio so the host can letterbox exactly rather than to the
    // nearest float.
    out_frame->aspect_x = bb::tiles_wide;
    out_frame->aspect_y = bb::tiles_high;
}

void reset_game(manx_game_instance* handle) {
    if (instance* inst = reinterpret_cast<instance*>(handle)) {
        inst->game.reset();
        inst->pending.clear();
    }
}

void set_paused(manx_game_instance*, uint32_t) {
    // Pausing is the host holding back run_frame; there is no separate paused
    // state to keep, and pretending otherwise would be a second clock.
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
    const uint32_t count =
        std::min<uint32_t>(max_cues, static_cast<uint32_t>(inst->pending.size()));
    for (uint32_t i = 0; i < count; ++i) {
        out[i].cue = static_cast<uint32_t>(inst->pending[i]);
        out[i].gain = 1.0f;
        out[i].pan = 0.0f;
    }
    inst->pending.erase(inst->pending.begin(), inst->pending.begin() + count);
    return count;
}

const char* const cue_names[] = {"jump",  "shoot", "pop",         "trap",
                                 "fruit", "death", "level_clear"};

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
