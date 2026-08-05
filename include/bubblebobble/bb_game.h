// bb_game.h - Bubble Bobble, rebuilt natively rather than emulated.
//
// The second native title in the arcade, and the first with a hardware
// ancestor: BUBBLE BOBBLE Neo! is an Xbox 360 remake whose Classic mode is the
// 1986 Taito arcade game. That recompiled build still exists and is the oracle
// for what this has to do - but not for how, so nothing here reproduces a
// console's limits. There is no tile RAM, no sprite budget and no 60 Hz the
// simulation is welded to; there is state that advances and quads that get
// drawn.
//
// WHY A REWRITE AND NOT THE RECOMPILE. The recompiled build renders its
// characters through an emulated Xenos: PM4 parsed at run time, shader
// microcode translated to SPIR-V per draw, vertex fetch reconstructed from
// fetch constants. Its characters currently come out as flat silhouettes
// because an interpolated texture coordinate collapses somewhere in that
// chain. None of those layers exist here, so neither does that class of bug.
//
// The module knows nothing about the arcade: no window, no pad, no audio
// device. It is handed intent and hands back quads and cues, which is what lets
// it be a plugin (bb_plugin.cpp) rather than a compiled-in board.
#pragma once

#include <cstdint>
#include <vector>

namespace bb {

// The playfield is a 32x25 grid of tiles, which is the arcade original's own
// shape. Keeping the world in tile units rather than pixels means the
// simulation does not care what resolution it is presented at - a 4x scale and
// a 1x scale are the same game, and the only place a pixel is mentioned is the
// rasteriser.
inline constexpr int tiles_wide = 32;
inline constexpr int tiles_high = 25;
inline constexpr float tile_size = 8.0f;
inline constexpr float world_width = tiles_wide * tile_size;
inline constexpr float world_height = tiles_high * tile_size;

// What a player is asking for this frame, already normalised. The plugin fills
// this in, so the game logic never sees a gamepad, a key or a mapping.
struct player_intent {
    float move_x{}; // -1 .. 1
    bool jump{};
    bool fire{};
    bool start{};
    // Whether a pad is actually in this seat. Bub and Bob are separate players
    // and a seat joins when its pad appears, so a one-pad session must not put
    // a motionless second dragon in the level to be killed.
    bool connected{};
};

// What a quad is for. The simulation names the THING, never a texture or a
// colour, so the presentation can change - flat fills now, extracted artwork
// later - without the game logic knowing. This is the seam that keeps a
// renderer swap from being a rewrite.
enum class sprite_kind : uint8_t {
    block,        // level geometry
    wall,         // the enclosing frame
    player_green, // Bub
    player_blue,  // Bob
    enemy,        // a loose Zen-chan
    enemy_angry,  // a Zen-chan in a hurry-up level
    bubble,       // an empty bubble, drifting
    bubble_full,  // a bubble with an enemy in it
    fruit,        // what a popped enemy leaves behind
    pop,          // the flash where a bubble burst
};

// One drawn rectangle in world units, origin top-left. Everything visible is
// this: the level, the dragons, the bubbles and the fruit. A renderer that can
// fill an axis-aligned rectangle can draw Bubble Bobble, and one that can also
// sample a texture can draw it properly.
struct sprite_quad {
    float x{};
    float y{};
    float w{};
    float h{};
    sprite_kind kind{};
    // Which way the thing is looking, so a later renderer can mirror artwork
    // without the simulation having to hold two of everything.
    bool facing_left{};
    // 0..1 through whatever the thing is currently doing. Animation is the
    // renderer's business, but WHEN a cycle started is the simulation's, so
    // this is the one number that crosses.
    float phase{};
};

// Sounds are named events, never samples. The simulation says what happened;
// the host plays whatever the bundle supplies for it. Audio therefore cannot
// influence the simulation, which is what keeps two networked machines with
// different sound devices in step.
enum class cue : uint8_t {
    jump,
    shoot,
    pop,
    trap,
    fruit,
    death,
    level_clear,
    count,
};

// Bub in flight. Kept as an explicit state rather than a pile of booleans
// because the arcade's feel comes from the transitions: a jump that has been
// released early rises less, and a dragon walking off a ledge may not jump
// again until it lands.
enum class motion : uint8_t {
    standing,
    walking,
    rising,
    falling,
};

struct dragon {
    float x{};
    float y{};
    float vx{};
    float vy{};
    bool alive{};
    bool joined{};
    bool facing_left{};
    motion state{};
    int fire_cooldown{};
    int respawn_timer{};
    int invulnerable{};
    int lives{};
    float anim{};
};

// A bubble, empty or occupied. One type covers both because in this game they
// ARE one thing: a bubble that caught an enemy is a bubble that is now worth
// points, and it drifts, ages and pops on exactly the same rules.
struct bubble {
    float x{};
    float y{};
    float vx{};
    float vy{};
    int life{};
    bool active{};
    bool holds_enemy{};
    float anim{};
};

struct enemy {
    float x{};
    float y{};
    float vx{};
    float vy{};
    bool alive{};
    bool facing_left{};
    bool grounded{};
    int turn_cooldown{};
    float anim{};
};

struct fruit_item {
    float x{};
    float y{};
    float vy{};
    int life{};
    bool active{};
};

struct pop_effect {
    float x{};
    float y{};
    int life{};
    bool active{};
};

// Deterministic by construction. The lobby's lockstep rests on two machines
// advancing identically from identical inputs, so the simulation must not read
// a clock or the C library's rand(), and this is the only source of chance in
// it. xorshift64* is used rather than anything from <random>, whose engines are
// specified but whose DISTRIBUTIONS are not - the same distribution over the
// same engine can differ between standard libraries, which is exactly the
// silent divergence lockstep cannot survive.
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

    // Half-open [0, bound). Rejection-free and biased by at most 2^-64 per
    // draw, which is immaterial here and, unlike a modulo of a distribution
    // object, identical everywhere.
    uint32_t below(uint32_t bound) noexcept {
        return bound == 0 ? 0
                          : static_cast<uint32_t>((next() >> 32) % bound);
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

    // Advances exactly one frame. `intents` has `player_count` entries.
    void step(const player_intent* intents, int player_count);

    void reset();

    // The quads to draw this frame, rebuilt each step. Back-to-front, so a
    // renderer can blit them in order without sorting.
    const std::vector<sprite_quad>& quads() const noexcept { return m_quads; }

    // Sounds the last step asked for, oldest first. Draining is the caller's
    // job: a cue is an event, and a host that skipped a frame must not hear it
    // twice.
    std::vector<cue> take_cues();

    uint64_t score() const noexcept { return m_score; }
    int level() const noexcept { return m_level; }
    int lives(int seat) const noexcept;
    bool game_over() const noexcept { return m_game_over; }

    // Covers every value the simulation reads, and nothing cosmetic. Two peers
    // compare this each frame; the moment it differs they have desynced, which
    // is the only way to catch a divergence that otherwise just looks like two
    // people playing different games.
    uint64_t checksum() const noexcept;

    // True where the level has solid geometry. Public because the tests assert
    // on the collision rules directly rather than inferring them from motion.
    bool solid(int column, int row) const noexcept;
    bool solid_at(float x, float y) const noexcept;

    static constexpr int max_players = 2;

private:
    void load_level(int index);
    void step_dragon(dragon& who, const player_intent& intent, int seat);
    void step_bubbles();
    void step_enemies();
    void step_fruit();
    void spawn_bubble(const dragon& who);
    void emit(cue what);
    void build_quads();
    bool feet_supported(float x, float y) const noexcept;
    // Where a body moving from `prev_y` to `next_y` comes to rest, if it
    // crossed the top of a solid tile on the way. Returns false when it did
    // not, which is the case that matters: a body that is already INSIDE a
    // ledge is passing through it, not landing on it.
    bool land_from_above(float x, float prev_y, float next_y,
                         float& out_y) const noexcept;

    std::vector<uint8_t> m_tiles;
    dragon m_dragons[max_players]{};
    std::vector<bubble> m_bubbles;
    std::vector<enemy> m_enemies;
    std::vector<fruit_item> m_fruit;
    std::vector<pop_effect> m_pops;
    std::vector<sprite_quad> m_quads;
    std::vector<cue> m_cues;

    rng m_rng;
    uint64_t m_score{};
    int m_level{};
    int m_frame{};
    int m_clear_timer{};
    bool m_game_over{};
};

} // namespace bb
