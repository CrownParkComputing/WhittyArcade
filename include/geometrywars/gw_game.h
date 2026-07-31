// gw_game.h - Geometry Wars, rebuilt natively rather than emulated.
//
// This is the first title in the arcade that is not a machine. Every other
// board here loads ROMs and runs the original hardware's code; this one runs
// game logic written from scratch, so there is no CPU, no ROM map and no
// framebuffer being scanned out - only state that advances and geometry that
// gets drawn.
//
// The module knows nothing about the arcade: no window, no pad, no audio
// device. It is handed intent and hands back line geometry, which is what lets
// it be a plugin (gw_plugin.cpp) rather than a compiled-in board.
#pragma once

#include <cstdint>
#include <vector>

namespace gw {

// Which of the two games a session is running. They share this module because
// they share their look, their playfield and most of their behaviour - the
// same reason galaxian and phoenix share one session in this codebase.
enum class edition : uint8_t {
    retro_evolved,   // Geometry Wars: Retro Evolved. Single player.
    retro_evolved_2, // Geometry Wars: Retro Evolved 2. Up to four players.
};

// The playfield is a fixed arena the camera never leaves, so world units and
// the drawn picture only differ by a scale. Keeping the arena in its own units
// means the game does not have to care what resolution it is presented at -
// which is the whole point of rebuilding rather than emulating a 720p console.
inline constexpr float arena_half_width = 800.0f;
inline constexpr float arena_half_height = 450.0f;

// What a player is asking for this frame, already normalised. The plugin fills
// this in, so the game logic never sees a gamepad, a key or a mapping.
struct player_intent {
    float move_x{};  // -1 .. 1
    float move_y{};  // -1 .. 1
    float aim_x{};   // -1 .. 1
    float aim_y{};   // -1 .. 1
    bool fire{};
    bool bomb{};
    bool start{};
    // Whether a pad is actually in this seat. A four-player game with one pad
    // plugged in must not put three motionless ships in the arena to be killed
    // - so a seat joins when its pad appears rather than because the edition
    // could hold it.
    bool connected{};
};

// One end of a line segment, in world units, with its colour. The ships, the
// enemies, the bullets, the particles and the arena grid are all this: a
// renderer that can draw a coloured line additively can draw Geometry Wars.
struct line_vertex {
    float x{};
    float y{};
    float r{};
    float g{};
    float b{};
    float a{};
};

// The enemies of Retro Evolved, kept as behaviours rather than as art. Each
// one is a different answer to "where do I go", which is what makes the arena
// read as a crowd rather than a swarm of one thing.
enum class enemy_kind : uint8_t {
    wanderer, // drifts, turns at the walls, ignores the player
    seeker,   // steers straight at the nearest ship
    weaver,   // seeks, but slides sideways so it is hard to lead
    splitter, // seeks slowly; breaks into seekers when killed
};

class game {
public:
    explicit game(edition which) noexcept : m_edition(which) { reset(); }

    // Starts a fresh session.
    void reset() noexcept;

    // Advances by one fixed step. Fixed rather than delta-timed on purpose:
    // this game is tuned around a constant step, and a variable one changes
    // how it feels, which is the single thing a rewrite must not do.
    void advance(const player_intent* intents, std::size_t player_count) noexcept;

    // The frame's geometry, as line pairs: [0]-[1] is one segment, [2]-[3] the
    // next. Valid until the next advance().
    const std::vector<line_vertex>& lines() const noexcept { return m_lines; }

    edition which() const noexcept { return m_edition; }
    uint64_t score() const noexcept { return m_score; }
    // Seat one's lives, which is the whole story in a single-player game and
    // the HUD's headline in a shared one.
    uint32_t lives() const noexcept { return m_ships[0].lives; }
    uint32_t lives_for(std::size_t seat) const noexcept {
        return seat < 4 ? m_ships[seat].lives : 0u;
    }
    uint32_t multiplier() const noexcept { return m_multiplier; }
    bool attract() const noexcept { return m_attract; }
    // Over only when nobody left in the arena has a life. A four-player game
    // does not end because one seat ran out.
    bool game_over() const noexcept {
        for (const ship& s : m_ships)
            if (s.active && s.lives > 0) return false;
        return true;
    }
    std::size_t enemy_count() const noexcept { return m_enemies.size(); }

    // Everything the simulation depends on, hashed. Two machines playing the
    // same match must agree on this every frame; the instant they do not, they
    // have desynced. Cosmetics are deliberately excluded - particles are
    // spawned from the shared RNG but never influence play, so including them
    // would report a desync where none affects the game.
    uint64_t state_checksum() const noexcept;

    // How many players this edition supports at once. Retro Evolved is a
    // single-player game; its sequel is where the lobby earns its keep.
    std::size_t max_players() const noexcept {
        return m_edition == edition::retro_evolved ? 1u : 4u;
    }

    static constexpr double refresh_hz = 60.0;

private:
    struct ship {
        float x{}, y{};
        float facing{};
        float fire_cooldown{};
        float respawn{};   // >0 while dead and waiting to come back
        uint32_t bombs{3};
        // Lives are PER SEAT, not shared. Sharing them meant three players
        // drained one pool three times as fast and every run ended almost at
        // once - measurable as a three-player game scoring less than a solo
        // one. The arena is shared; the credit is not.
        uint32_t lives{3};
        bool alive{};
        bool active{};     // this seat is in play at all
    };
    struct bullet {
        float x{}, y{}, vx{}, vy{};
        float life{};
        uint8_t owner{};
    };
    struct enemy {
        float x{}, y{}, vx{}, vy{};
        float spawn{};     // >0 while materialising and harmless
        float phase{};     // per-enemy offset so a wave does not move as one
        enemy_kind kind{};
    };
    struct particle {
        float x{}, y{}, vx{}, vy{};
        float life{}, life_max{};
        float r{}, g{}, b{};
    };
    // Retro Evolved 2's signature hazard, and the reason its arena plays
    // differently: it drags everything towards itself, eats the enemies it
    // catches and grows on them, and pays out in a spray of seekers when it is
    // finally destroyed. Absent from Retro Evolved.
    struct black_hole {
        float x{}, y{};
        float radius{};
        float health{};
        float spin{};
        bool collapsing{};
    };

    // Deterministic on purpose: the same seed gives the same waves, which is
    // what makes a bug reproducible and is a precondition for ever running
    // this over the network.
    uint32_t random() noexcept;
    float random_unit() noexcept;
    float random_range(float low, float high) noexcept;

    void update_ships(const player_intent* intents,
                      std::size_t player_count) noexcept;
    void update_bullets() noexcept;
    void update_enemies() noexcept;
    void update_particles() noexcept;
    void spawn_wave() noexcept;
    void spawn_enemy(enemy_kind kind, float x, float y) noexcept;
    void update_black_holes() noexcept;
    void collapse_black_hole(std::size_t index) noexcept;
    void draw_black_hole(const black_hole& hole) noexcept;
    bool has_black_holes() const noexcept {
        return m_edition == edition::retro_evolved_2;
    }
    void kill_enemy(std::size_t index) noexcept;
    void kill_ship(ship& s) noexcept;
    void detonate_bomb(ship& s) noexcept;
    void burst(float x, float y, float r, float g, float b,
               int count, float speed) noexcept;
    const ship* nearest_ship(float x, float y) const noexcept;

    void emit_line(float x0, float y0, float x1, float y1,
                   float r, float g, float b, float a) noexcept;
    void draw_arena() noexcept;
    void draw_ship(const ship& s, std::size_t index) noexcept;
    void draw_enemy(const enemy& e) noexcept;
    void draw_polygon(float x, float y, float radius, int sides, float rotation,
                      float r, float g, float b, float a) noexcept;

    edition m_edition;
    std::vector<line_vertex> m_lines;
    std::vector<bullet> m_bullets;
    std::vector<enemy> m_enemies;
    std::vector<particle> m_particles;
    std::vector<black_hole> m_black_holes;
    ship m_ships[4]{};
    uint64_t m_score{};
    uint32_t m_lives{3}; // legacy mirror; per-seat lives are authoritative
    uint32_t m_multiplier{1};
    uint32_t m_kills{};
    uint32_t m_rng{0x13579BDFu};
    uint64_t m_step{};
    float m_spawn_timer{};
    float m_difficulty{};
    float m_over_hold{}; // steps held on the game-over screen before attract
    bool m_attract{true};
};

} // namespace gw
