// jp_game.h - Jetpac, rebuilt natively rather than emulated.
//
// The third native title in the arcade. Jetpac Refuelled is Rare's Xbox 360
// remake of Ultimate Play The Game's 1983 original, and its Retro mode is that
// original. The recompiled build is the oracle for what this has to do - its
// Jetman is the one whose animation frames never advance - but not for how, so
// nothing here reproduces a Spectrum's limits beyond the ones that ARE the
// game: one screen, no scrolling, and a rocket you build a piece at a time.
//
// The module knows nothing about the arcade: no window, no pad, no audio
// device. It is handed intent and hands back quads and cues, which is what lets
// it be a plugin (jp_plugin.cpp) rather than a compiled-in board.
#pragma once

#include <cstdint>
#include <vector>

namespace jp {

// 32x24 tiles of 8 units - 256x192, the screen the game was designed on. The
// simulation works in these units and never in pixels, so the resolution it is
// presented at is the renderer's choice and not the game's.
inline constexpr int tiles_wide = 32;
inline constexpr int tiles_high = 24;
inline constexpr float tile_size = 8.0f;
inline constexpr float world_width = tiles_wide * tile_size;
inline constexpr float world_height = tiles_high * tile_size;

// What a player is asking for this frame, already normalised.
struct player_intent {
    float move_x{}; // -1 .. 1
    bool thrust{};  // the jetpack, held rather than tapped
    bool fire{};
    bool start{};
    bool connected{};
};

// What a quad is for. The simulation names the THING, never a colour or a
// texture, so artwork can replace flat fills without the game logic changing.
enum class sprite_kind : uint8_t {
    ground,
    platform,
    jetman,
    jetman_flame, // the jetpack burn, drawn under him while thrusting
    laser,
    alien_drone,
    alien_ufo,
    rocket_base,
    rocket_mid,
    rocket_part, // a loose piece, not yet fitted - never the built stack
    rocket_top,
    fuel_pod,
    treasure,
    explosion,
};

// One drawn rectangle in world units, origin top-left.
struct sprite_quad {
    float x{};
    float y{};
    float w{};
    float h{};
    sprite_kind kind{};
    bool facing_left{};
    float phase{}; // 0..1 through whatever the thing is doing
};

// Sounds are named events, never samples: the simulation says what happened and
// the host plays whatever the bundle has for it. Audio therefore cannot feed
// back into the simulation, which is what keeps two networked machines in step.
enum class cue : uint8_t {
    thrust,
    fire,
    alien_hit,
    collect,
    deliver,
    launch,
    death,
    count,
};

// What the rocket still wants. The order is the game: three pieces stacked,
// then six pods of fuel, then a pilot.
enum class rocket_state : uint8_t {
    needs_parts,
    needs_fuel,
    ready,
    launching,
};

struct jetman {
    float x{};
    float y{};
    float vx{};
    float vy{};
    bool alive{};
    bool joined{};
    bool facing_left{};
    bool thrusting{};
    int fire_cooldown{};
    int respawn_timer{};
    int invulnerable{};
    int lives{};
    // What he is carrying, as an index into the item list, or -1. One thing at
    // a time: the whole rhythm of the game is the trip back to the rocket.
    int carrying{-1};
    float anim{};
};

// A rocket part, a fuel pod or a piece of treasure. One type covers all three
// because they behave identically - they fall, they can be picked up, and they
// are worth something - and differ only in what happens when they arrive.
enum class item_kind : uint8_t { rocket_part, fuel_pod, treasure };

struct item {
    float x{};
    float y{};
    float vy{};
    item_kind kind{};
    bool active{};
    bool carried{};
    int life{}; // treasure only; parts and pods wait for ever
};

struct alien {
    float x{};
    float y{};
    float vx{};
    float vy{};
    bool alive{};
    bool is_ufo{};
    float anim{};
};

struct laser_bolt {
    float x{};
    float y{};
    float vx{};
    bool active{};
};

struct blast {
    float x{};
    float y{};
    int life{};
    bool active{};
};

// Deterministic by construction - see the note in bb_game.h. xorshift64* rather
// than anything from <random>, whose distributions are not specified to produce
// the same values across standard libraries.
class rng {
public:
    explicit rng(uint64_t seed = 0x9E3779B97F4A7C15ull) noexcept
        : m_state(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

    uint64_t next() noexcept {
        m_state ^= m_state >> 12;
        m_state ^= m_state << 25;
        m_state ^= m_state >> 27;
        return m_state * 0x2545F4914F6CDD1Dull;
    }
    uint32_t below(uint32_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<uint32_t>((next() >> 32) % bound);
    }
    float unit() noexcept {
        return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f);
    }
    uint64_t state() const noexcept { return m_state; }

private:
    uint64_t m_state;
};

class game {
public:
    game();

    void step(const player_intent* intents, int player_count);
    void reset();

    const std::vector<sprite_quad>& quads() const noexcept { return m_quads; }
    std::vector<cue> take_cues();

    uint64_t score() const noexcept { return m_score; }
    int level() const noexcept { return m_level; }
    int lives(int seat) const noexcept;
    bool game_over() const noexcept { return m_game_over; }
    rocket_state rocket() const noexcept { return m_rocket_state; }
    int parts_fitted() const noexcept { return m_parts_fitted; }
    int fuel_loaded() const noexcept { return m_fuel_loaded; }
    // Whether a seat has something in its hands, and where the pad is. Both
    // are things a player can see, and both are needed by anything driving the
    // game without eyes on it - a test, a demo attract loop, a bot.
    bool carrying(int seat) const noexcept;
    float rocket_x() const noexcept { return m_rocket_x; }
    uint64_t checksum() const noexcept;

    bool solid(int column, int row) const noexcept;
    bool solid_at(float x, float y) const noexcept;

    static constexpr int max_players = 2;
    static constexpr int parts_needed = 3;
    static constexpr int fuel_needed = 6;

private:
    void load_level(int index);
    void step_pilot(jetman& who, const player_intent& intent, int seat);
    void step_items();
    void step_aliens();
    void step_lasers();
    void spawn_next_payload();
    void spawn_alien();
    void deliver(jetman& who, item& what);
    void emit(cue what);
    void build_quads();
    bool land_from_above(float x, float prev_y, float next_y,
                         float& out_y) const noexcept;

    std::vector<uint8_t> m_tiles;
    jetman m_pilots[max_players]{};
    std::vector<item> m_items;
    std::vector<alien> m_aliens;
    std::vector<laser_bolt> m_lasers;
    std::vector<blast> m_blasts;
    std::vector<sprite_quad> m_quads;
    std::vector<cue> m_cues;

    rng m_rng;
    uint64_t m_score{};
    int m_level{};
    int m_frame{};
    int m_parts_fitted{};
    int m_fuel_loaded{};
    int m_alien_timer{};
    int m_payload_timer{};
    int m_launch_timer{};
    float m_rocket_x{};
    float m_rocket_ground_y{};
    rocket_state m_rocket_state{rocket_state::needs_parts};
    bool m_payload_out{};
    bool m_game_over{};
};

} // namespace jp
