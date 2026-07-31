// whitty_game_plugin.h - the contract between WhittyArcade and a game.
//
// A game is a shared library discovered beside its data, the way a ROM set is
// discovered: drop it in, it appears. Nothing here is compiled into the host,
// so adding or updating a game does not rebuild the arcade.
//
// WHY THIS IS C AND NOT THE C++ SESSION INTERFACE. Inside the host, a board is
// an `emulator_session` - a C++ class with virtual functions, std::string and
// std::unique_ptr. None of that can safely cross a shared-library boundary: the
// vtable layout, the standard library's ABI and exception unwinding all have to
// match exactly, so a plugin built with a different compiler, a different
// standard-library version or even different flags would link and then corrupt
// memory at the first call. A plain C struct of function pointers has none of
// those problems, which is why every stable plugin ABI looks like this one.
// The host wraps a loaded plugin in an `emulator_session` on its side, so the
// frontend, input mapper, high-score table and multiplayer lobby continue to
// see one uniform kind of cabinet.
//
// RULES FOR EVERYTHING BELOW, and they are not style preferences:
//   * Plain types only. No std::, no classes, no references, no exceptions
//     crossing this boundary. A plugin that throws must catch it itself.
//   * Structs are append-only. Once published, a field never moves and never
//     changes meaning; new information goes on the end and is guarded by the
//     version. Reordering one field silently misreads every plugin already
//     built.
//   * The host owns the window, the audio device and the pads. A plugin never
//     opens any of them; it is handed input and hands back a picture.
#ifndef WHITTY_GAME_PLUGIN_H
#define WHITTY_GAME_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped only when the layout below changes incompatibly. The host refuses a
// plugin whose version it does not know, which is the difference between a
// clear "this game is too old for this arcade" and a crash inside a call whose
// arguments moved.
#define WHITTY_GAME_ABI_VERSION 1u

// The symbol every plugin exports. Looked up by name after dlopen.
#define WHITTY_GAME_ENTRY_SYMBOL "whitty_game_entry"

// What one player is doing this frame, already mapped and normalised by the
// host. A plugin never sees a device, a key or a mapping - only intent - so a
// game does not have to be rebuilt when a pad or a binding changes.
typedef struct whitty_game_input {
    float move_x;      // -1 .. 1, left stick / d-pad
    float move_y;      // -1 .. 1, up is positive
    float aim_x;       // -1 .. 1, right stick
    float aim_y;       // -1 .. 1, up is positive
    float trigger_l;   // 0 .. 1
    float trigger_r;   // 0 .. 1
    uint32_t buttons;  // bitfield, see whitty_game_button
    uint8_t connected; // 0 when this seat has no pad
    uint8_t reserved[3];
} whitty_game_input;

enum whitty_game_button {
    whitty_game_button_fire = 1u << 0,
    whitty_game_button_secondary = 1u << 1,
    whitty_game_button_bomb = 1u << 2,
    whitty_game_button_special = 1u << 3,
    whitty_game_button_start = 1u << 4,
    whitty_game_button_select = 1u << 5,
    whitty_game_button_pause = 1u << 6,
};

// The picture a plugin produces for one frame. `pixels` is tightly packed RGBA
// owned by the PLUGIN and must stay valid until the next run_frame; the host
// copies what it needs. Aspect is given as a ratio rather than a float so a
// 16:9 game and a 4:3 game are both exact.
typedef struct whitty_game_frame {
    const uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t aspect_x;
    uint32_t aspect_y;
} whitty_game_frame;

// What a game says about itself, read before it is started so the launcher can
// list it without loading its data.
typedef struct whitty_game_info {
    const char* short_name;   // key for scores, artwork, bundles
    const char* display_name;
    const char* publisher;
    uint32_t max_players;     // seats this game can fill at once
    uint32_t supports_network; // non-zero if it can use the lobby
    double refresh_hz;
} whitty_game_info;

// An opaque game instance. Only the plugin knows what is behind it.
typedef struct whitty_game_instance whitty_game_instance;

// The plugin's function table. Every entry must be non-null.
typedef struct whitty_game_api {
    uint32_t abi_version; // must equal WHITTY_GAME_ABI_VERSION

    // Describes the game without starting it.
    void (*describe)(whitty_game_info* out_info);

    // Creates an instance. `bundle_path` is the directory holding the game's
    // data, the way a ROM path names a ROM set. Returns null on failure.
    whitty_game_instance* (*create)(const char* bundle_path);
    void (*destroy)(whitty_game_instance* instance);

    // Advances exactly one frame and returns the picture. `inputs` has
    // `player_count` entries. The returned frame's pixels stay valid until the
    // next call.
    void (*run_frame)(whitty_game_instance* instance,
                      const whitty_game_input* inputs,
                      uint32_t player_count,
                      whitty_game_frame* out_frame);

    // Optional-but-required-to-exist hooks: implement them as no-ops rather
    // than leaving them null, so the host never has to null-check per frame.
    void (*reset)(whitty_game_instance* instance);
    void (*set_paused)(whitty_game_instance* instance, uint32_t paused);

    // Current score, for the host's high-score table. A game with no score
    // returns 0.
    uint64_t (*score)(whitty_game_instance* instance);

    // A hash of everything the simulation depends on, for NETWORKED PLAY.
    //
    // Four instances each rendering their own view cannot work by streaming one
    // machine's picture; they each simulate the whole game from the same inputs
    // and draw it locally. That only holds while every instance stays bit-for-
    // bit identical, and the failure mode when it does not is silent - two
    // players see different arenas and neither is told. Peers therefore compare
    // this value each frame: the moment it differs, they have desynced, and the
    // host can say so instead of letting the match quietly become two games.
    //
    // It must cover every value the simulation reads, including the random
    // number generator's state, and must NOT cover anything cosmetic that a
    // machine could legitimately differ on.
    //
    // A game that does not support network play returns 0.
    uint64_t (*state_checksum)(whitty_game_instance* instance);
} whitty_game_api;

// The exported entry point. Returns a pointer to a table with static storage
// duration - the host does not free it.
//
//   const whitty_game_api* whitty_game_entry(void);
typedef const whitty_game_api* (*whitty_game_entry_fn)(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WHITTY_GAME_PLUGIN_H
