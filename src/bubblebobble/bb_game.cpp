#include "bubblebobble/bb_game.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace bb {
namespace {

// Tile codes. The distinction between a block and a wall is not decoration: in
// this game you jump UP THROUGH a platform and land on top of it, which is what
// makes its levels readable as a single screen instead of a maze. So a block is
// solid only to something falling onto it, and a wall - the frame around the
// playfield - is solid from every side.
constexpr uint8_t tile_empty = 0;
constexpr uint8_t tile_block = 1;
constexpr uint8_t tile_wall = 2;

// Level 1, read off the real thing. The recompiled build was driven to the
// first Classic-mode screen and the layout taken from that frame, so this is
// the game's own geometry rather than an approximation of a memory of it.
//
// The floor has gaps at both ends ON PURPOSE. Falling through them is how a
// dragon wraps to the top of the screen, which is a mechanic and not an
// accident - a solid floor would quietly delete it.
const char* const level_one[tiles_high] = {
    "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..........##########..........W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..######.....#######....######W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..###########.....############W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W...######...#######...#######.W",
    "W..............................W",
    "W..............................W",
    "W..............................W",
    "W..WWWWWWWWWWWWWWWWWWWWWWWWWW..W",
};

// Body sizes in world units. A dragon is a tile wide, which is what makes the
// gaps in the level readable at a glance: if it fits visually, it fits.
constexpr float body_w = 8.0f;
constexpr float body_h = 8.0f;
constexpr float bubble_size = 8.0f;

constexpr float walk_speed = 0.85f;
constexpr float gravity = 0.28f;
constexpr float terminal_velocity = 4.2f;
constexpr float jump_velocity = -3.7f;

constexpr int fire_interval = 14;
constexpr int bubble_drift_delay = 22;   // frames travelling before it rises
constexpr int bubble_life = 300;
constexpr float bubble_speed = 2.4f;
constexpr float bubble_rise = -0.55f;

constexpr float enemy_speed = 0.42f;
constexpr int respawn_frames = 90;
constexpr int invulnerable_frames = 120;
constexpr int clear_delay = 120;

constexpr int fruit_life = 600;
constexpr uint64_t score_trap = 10;
constexpr uint64_t score_pop = 500;
constexpr uint64_t score_fruit = 1000;

bool overlaps(float ax, float ay, float aw, float ah, float bx, float by,
              float bw, float bh) noexcept {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

} // namespace

game::game() { reset(); }

void game::reset() {
    m_rng = rng(0x5BB1E80B1EULL);
    m_score = 0;
    m_level = 0;
    m_frame = 0;
    m_clear_timer = 0;
    m_game_over = false;
    for (dragon& who : m_dragons) {
        who = dragon{};
        who.lives = 3;
    }
    load_level(0);
}

void game::load_level(int index) {
    m_level = index;
    m_tiles.assign(static_cast<std::size_t>(tiles_wide) * tiles_high,
                   tile_empty);
    for (int row = 0; row < tiles_high; ++row) {
        const char* line = level_one[row];
        for (int col = 0; col < tiles_wide; ++col) {
            const char c = line[col];
            // Later screens reuse level one's frame and mirror its interior.
            // This is a STAND-IN and is labelled as one: the real 100-screen
            // table still has to come out of the game, and a mirrored screen is
            // an honest placeholder in a way a randomly generated one is not.
            const char mirrored = (index % 2 == 1)
                                      ? level_one[row][tiles_wide - 1 - col]
                                      : c;
            const char use = mirrored;
            m_tiles[static_cast<std::size_t>(row) * tiles_wide + col] =
                use == 'W' ? tile_wall : (use == '#' ? tile_block : tile_empty);
        }
    }

    m_bubbles.clear();
    m_enemies.clear();
    m_fruit.clear();
    m_pops.clear();

    // Dragons start on the floor, a little in from each end, the way the
    // arcade opens a screen.
    const float floor_y = 23.0f * tile_size;
    m_dragons[0].x = 4.0f * tile_size;
    m_dragons[0].y = floor_y;
    m_dragons[1].x = 26.0f * tile_size;
    m_dragons[1].y = floor_y;
    for (dragon& who : m_dragons) {
        who.vx = 0.0f;
        who.vy = 0.0f;
        who.state = motion::standing;
        who.fire_cooldown = 0;
        who.respawn_timer = 0;
        who.invulnerable = invulnerable_frames;
        if (who.joined) who.alive = true;
    }
    m_dragons[0].facing_left = false;
    m_dragons[1].facing_left = true;

    // Enemies stand on the level's own ledges. Placing them by TILE rather than
    // by pixel means a screen edit cannot leave one hanging in the air.
    const int spawn_col[] = {5, 16, 26, 8, 15};
    const int spawn_row[] = {9, 9, 9, 14, 19};
    const int count = 3 + std::min(2, index);
    for (int i = 0; i < count; ++i) {
        enemy foe;
        foe.x = static_cast<float>(spawn_col[i]) * tile_size;
        foe.y = static_cast<float>(spawn_row[i]) * tile_size;
        foe.vx = (i % 2 == 0) ? enemy_speed : -enemy_speed;
        foe.facing_left = foe.vx < 0.0f;
        foe.alive = true;
        m_enemies.push_back(foe);
    }
    m_clear_timer = 0;
}

bool game::solid(int column, int row) const noexcept {
    if (column < 0 || column >= tiles_wide || row < 0 || row >= tiles_high)
        return false;
    return m_tiles[static_cast<std::size_t>(row) * tiles_wide + column] !=
           tile_empty;
}

bool game::solid_at(float x, float y) const noexcept {
    const int col = static_cast<int>(std::floor(x / tile_size));
    const int row = static_cast<int>(std::floor(y / tile_size));
    return solid(col, row);
}

namespace {

// Whether a box overlaps any tile the predicate calls solid. Sampling the four
// corners is enough because nothing here is smaller than a tile.
template <typename Fn>
bool box_hits(float x, float y, float w, float h, Fn&& is_solid) {
    const float eps = 0.01f;
    return is_solid(x + eps, y + eps) || is_solid(x + w - eps, y + eps) ||
           is_solid(x + eps, y + h - eps) ||
           is_solid(x + w - eps, y + h - eps);
}

} // namespace

bool game::feet_supported(float x, float y) const noexcept {
    const float below = y + body_h + 0.6f;
    return solid_at(x + 1.0f, below) || solid_at(x + body_w - 1.0f, below);
}

bool game::land_from_above(float x, float prev_y, float next_y,
                           float& out_y) const noexcept {
    // Only a tile top CROSSED during this move can stop the fall. Testing
    // overlap instead - which is the obvious way to write this - catches a
    // dragon that jumped up into the underside of a ledge and leaves it stuck
    // there, standing on nothing with the platform around its head. The jump
    // test found exactly that.
    const float prev_bottom = prev_y + body_h;
    const float next_bottom = next_y + body_h;
    const int first = static_cast<int>(std::floor(prev_bottom / tile_size));
    const int last = static_cast<int>(std::floor(next_bottom / tile_size));
    const int left = static_cast<int>(std::floor((x + 1.0f) / tile_size));
    const int right =
        static_cast<int>(std::floor((x + body_w - 1.0f) / tile_size));
    for (int row = std::max(0, first); row <= last; ++row) {
        const float top = static_cast<float>(row) * tile_size;
        if (prev_bottom > top + 0.001f) continue; // started below this top
        if (!solid(left, row) && !solid(right, row)) continue;
        out_y = top - body_h;
        return true;
    }
    return false;
}

void game::emit(cue what) { m_cues.push_back(what); }

std::vector<cue> game::take_cues() {
    std::vector<cue> drained;
    drained.swap(m_cues);
    return drained;
}

int game::lives(int seat) const noexcept {
    if (seat < 0 || seat >= max_players) return 0;
    return m_dragons[seat].lives;
}

void game::spawn_bubble(const dragon& who) {
    bubble made;
    made.x = who.x + (who.facing_left ? -bubble_size : body_w);
    made.y = who.y;
    made.vx = who.facing_left ? -bubble_speed : bubble_speed;
    made.vy = 0.0f;
    made.life = bubble_life;
    made.active = true;
    made.holds_enemy = false;
    m_bubbles.push_back(made);
    emit(cue::shoot);
}

void game::step_dragon(dragon& who, const player_intent& intent, int seat) {
    if (!who.joined) {
        if (intent.connected && (intent.start || seat == 0)) {
            who.joined = true;
            who.alive = true;
            who.invulnerable = invulnerable_frames;
        } else {
            return;
        }
    }

    if (!who.alive) {
        if (who.respawn_timer > 0) {
            --who.respawn_timer;
            if (who.respawn_timer == 0 && who.lives > 0) {
                who.alive = true;
                who.x = (seat == 0 ? 4.0f : 26.0f) * tile_size;
                who.y = 23.0f * tile_size;
                who.vx = 0.0f;
                who.vy = 0.0f;
                who.invulnerable = invulnerable_frames;
            }
        }
        return;
    }

    if (who.invulnerable > 0) --who.invulnerable;
    if (who.fire_cooldown > 0) --who.fire_cooldown;

    who.vx = intent.move_x * walk_speed;
    if (who.vx < 0.0f) who.facing_left = true;
    if (who.vx > 0.0f) who.facing_left = false;

    const bool grounded = feet_supported(who.x, who.y) && who.vy >= 0.0f;
    if (intent.jump && grounded) {
        who.vy = jump_velocity;
        emit(cue::jump);
    }

    who.vy += gravity;
    who.vy = std::min(who.vy, terminal_velocity);

    // Horizontal first, then vertical. Resolving one axis at a time is what
    // stops a diagonal move from tunnelling a corner, and doing it in this
    // order is what lets a dragon run off a ledge rather than catching on it.
    const float next_x = who.x + who.vx;
    const auto wall_at = [this](float px, float py) {
        const int col = static_cast<int>(std::floor(px / tile_size));
        const int row = static_cast<int>(std::floor(py / tile_size));
        if (col < 0 || col >= tiles_wide || row < 0 || row >= tiles_high)
            return false;
        return m_tiles[static_cast<std::size_t>(row) * tiles_wide + col] ==
               tile_wall;
    };
    if (!box_hits(next_x, who.y, body_w, body_h, wall_at)) who.x = next_x;

    const float next_y = who.y + who.vy;
    if (who.vy > 0.0f) {
        // Falling: come to rest on the first tile top crossed on the way down.
        float rest_y = 0.0f;
        if (land_from_above(who.x, who.y, next_y, rest_y)) {
            who.y = rest_y;
            who.vy = 0.0f;
            who.state = motion::standing;
        } else {
            who.y = next_y;
            who.state = motion::falling;
        }
    } else {
        // Rising: walls stop a dragon, platforms do not.
        if (box_hits(who.x, next_y, body_w, body_h, wall_at)) {
            who.vy = 0.0f;
        } else {
            who.y = next_y;
            who.state = motion::rising;
        }
    }
    if (who.vy == 0.0f && who.vx != 0.0f) who.state = motion::walking;

    // Falling out of the bottom puts a dragon back at the top. This is the
    // reason the floor has holes in it.
    if (who.y > world_height) who.y -= world_height;
    if (who.x < tile_size) who.x = tile_size;
    if (who.x > world_width - tile_size - body_w)
        who.x = world_width - tile_size - body_w;

    if (intent.fire && who.fire_cooldown == 0) {
        spawn_bubble(who);
        who.fire_cooldown = fire_interval;
    }

    who.anim += 0.12f;
    if (who.anim >= 1.0f) who.anim -= 1.0f;
}

void game::step_bubbles() {
    for (bubble& one : m_bubbles) {
        if (!one.active) continue;
        --one.life;
        if (one.life <= 0) {
            one.active = false;
            pop_effect burst;
            burst.x = one.x;
            burst.y = one.y;
            burst.life = 12;
            burst.active = true;
            m_pops.push_back(burst);
            // A bubble that ages out with an enemy in it lets it go, which is
            // what makes leaving a trapped enemy alone a mistake rather than a
            // free kill.
            if (one.holds_enemy) {
                enemy freed;
                freed.x = one.x;
                freed.y = one.y;
                freed.vx = enemy_speed;
                freed.alive = true;
                m_enemies.push_back(freed);
            }
            emit(cue::pop);
            continue;
        }

        if (one.life < bubble_life - bubble_drift_delay) {
            one.vx *= 0.88f;
            one.vy = bubble_rise;
        }

        const float next_x = one.x + one.vx;
        if (!solid_at(next_x + 1.0f, one.y + bubble_size * 0.5f) &&
            !solid_at(next_x + bubble_size - 1.0f, one.y + bubble_size * 0.5f))
            one.x = next_x;
        else
            one.vx = 0.0f;

        const float next_y = one.y + one.vy;
        if (!solid_at(one.x + bubble_size * 0.5f, next_y + 1.0f))
            one.y = next_y;
        else
            one.vy = 0.0f;

        one.anim += 0.05f;
        if (one.anim >= 1.0f) one.anim -= 1.0f;

        if (one.holds_enemy) continue;
        for (enemy& foe : m_enemies) {
            if (!foe.alive) continue;
            if (!overlaps(one.x, one.y, bubble_size, bubble_size, foe.x, foe.y,
                          body_w, body_h))
                continue;
            foe.alive = false;
            one.holds_enemy = true;
            one.vx = 0.0f;
            one.life = std::max(one.life, 180);
            m_score += score_trap;
            emit(cue::trap);
            break;
        }
    }

    m_bubbles.erase(std::remove_if(m_bubbles.begin(), m_bubbles.end(),
                                   [](const bubble& b) { return !b.active; }),
                    m_bubbles.end());
}

void game::step_enemies() {
    for (enemy& foe : m_enemies) {
        if (!foe.alive) continue;

        foe.vy += gravity;
        foe.vy = std::min(foe.vy, terminal_velocity);

        const float next_x = foe.x + foe.vx;
        const bool blocked =
            solid_at(next_x + (foe.vx < 0.0f ? 0.5f : body_w - 0.5f),
                     foe.y + body_h * 0.5f);
        // Turn at a wall, and at the end of the ledge being walked along, so a
        // Zen-chan patrols its platform instead of walking off it every time.
        const bool ledge_ends =
            foe.vy == 0.0f &&
            !feet_supported(next_x, foe.y) && m_rng.below(4) != 0;
        if (blocked || ledge_ends) {
            foe.vx = -foe.vx;
            foe.facing_left = foe.vx < 0.0f;
        } else {
            foe.x = next_x;
        }

        const float next_y = foe.y + foe.vy;
        float rest_y = 0.0f;
        if (foe.vy > 0.0f && land_from_above(foe.x, foe.y, next_y, rest_y)) {
            foe.y = rest_y;
            foe.vy = 0.0f;
            foe.grounded = true;
        } else {
            foe.y = next_y;
            foe.grounded = false;
        }

        if (foe.y > world_height) foe.y -= world_height;

        foe.anim += 0.08f;
        if (foe.anim >= 1.0f) foe.anim -= 1.0f;
    }

    m_enemies.erase(std::remove_if(m_enemies.begin(), m_enemies.end(),
                                   [](const enemy& e) { return !e.alive; }),
                    m_enemies.end());
}

void game::step_fruit() {
    for (fruit_item& item : m_fruit) {
        if (!item.active) continue;
        --item.life;
        if (item.life <= 0) {
            item.active = false;
            continue;
        }
        item.vy = std::min(item.vy + gravity, terminal_velocity);
        const float next_y = item.y + item.vy;
        float rest_y = 0.0f;
        if (land_from_above(item.x, item.y, next_y, rest_y)) {
            item.y = rest_y;
            item.vy = 0.0f;
        } else {
            item.y = next_y;
        }
    }
    m_fruit.erase(std::remove_if(m_fruit.begin(), m_fruit.end(),
                                 [](const fruit_item& f) { return !f.active; }),
                  m_fruit.end());
}

void game::step(const player_intent* intents, int player_count) {
    m_cues.clear();
    ++m_frame;

    const int seats = std::min(player_count, max_players);
    for (int seat = 0; seat < seats; ++seat)
        step_dragon(m_dragons[seat], intents[seat], seat);

    step_bubbles();
    step_enemies();
    step_fruit();

    // Contact. Done after everything has moved so a frame's outcome does not
    // depend on the order things happened to be updated in.
    for (int seat = 0; seat < max_players; ++seat) {
        dragon& who = m_dragons[seat];
        if (!who.alive) continue;

        for (bubble& one : m_bubbles) {
            if (!one.active) continue;
            if (!overlaps(who.x, who.y, body_w, body_h, one.x, one.y,
                          bubble_size, bubble_size))
                continue;
            one.active = false;
            pop_effect burst;
            burst.x = one.x;
            burst.y = one.y;
            burst.life = 12;
            burst.active = true;
            m_pops.push_back(burst);
            if (one.holds_enemy) {
                fruit_item item;
                item.x = one.x;
                item.y = one.y;
                item.life = fruit_life;
                item.active = true;
                m_fruit.push_back(item);
                m_score += score_pop;
            }
            emit(cue::pop);
        }

        for (fruit_item& item : m_fruit) {
            if (!item.active) continue;
            if (!overlaps(who.x, who.y, body_w, body_h, item.x, item.y, body_w,
                          body_h))
                continue;
            item.active = false;
            m_score += score_fruit;
            emit(cue::fruit);
        }

        if (who.invulnerable == 0) {
            for (const enemy& foe : m_enemies) {
                if (!foe.alive) continue;
                if (!overlaps(who.x, who.y, body_w, body_h, foe.x, foe.y,
                              body_w, body_h))
                    continue;
                who.alive = false;
                who.lives = std::max(0, who.lives - 1);
                who.respawn_timer = respawn_frames;
                emit(cue::death);
                break;
            }
        }
    }

    m_bubbles.erase(std::remove_if(m_bubbles.begin(), m_bubbles.end(),
                                   [](const bubble& b) { return !b.active; }),
                    m_bubbles.end());
    m_fruit.erase(std::remove_if(m_fruit.begin(), m_fruit.end(),
                                 [](const fruit_item& f) { return !f.active; }),
                  m_fruit.end());

    for (pop_effect& burst : m_pops)
        if (burst.active && --burst.life <= 0) burst.active = false;
    m_pops.erase(std::remove_if(m_pops.begin(), m_pops.end(),
                                [](const pop_effect& p) { return !p.active; }),
                 m_pops.end());

    // A screen is finished when nothing hostile is left loose OR bubbled.
    // Counting bubbled enemies is the difference between clearing a screen and
    // clearing it early: a trapped enemy that ages out would otherwise appear
    // on the next one.
    bool any_enemy = !m_enemies.empty();
    for (const bubble& one : m_bubbles)
        if (one.active && one.holds_enemy) any_enemy = true;

    if (!any_enemy) {
        if (m_clear_timer == 0) emit(cue::level_clear);
        ++m_clear_timer;
        if (m_clear_timer >= clear_delay) load_level(m_level + 1);
    } else {
        m_clear_timer = 0;
    }

    bool anyone_playing = false;
    for (const dragon& who : m_dragons)
        if (who.joined && (who.alive || who.lives > 0)) anyone_playing = true;
    m_game_over = !anyone_playing;

    build_quads();
}

void game::build_quads() {
    m_quads.clear();

    for (int row = 0; row < tiles_high; ++row) {
        for (int col = 0; col < tiles_wide; ++col) {
            const uint8_t tile =
                m_tiles[static_cast<std::size_t>(row) * tiles_wide + col];
            if (tile == tile_empty) continue;
            sprite_quad quad;
            quad.x = static_cast<float>(col) * tile_size;
            quad.y = static_cast<float>(row) * tile_size;
            quad.w = tile_size;
            quad.h = tile_size;
            quad.kind = tile == tile_wall ? sprite_kind::wall
                                          : sprite_kind::block;
            m_quads.push_back(quad);
        }
    }

    for (const fruit_item& item : m_fruit) {
        if (!item.active) continue;
        m_quads.push_back(
            {item.x, item.y, body_w, body_h, sprite_kind::fruit, false, 0.0f});
    }

    for (const enemy& foe : m_enemies) {
        if (!foe.alive) continue;
        m_quads.push_back({foe.x, foe.y, body_w, body_h, sprite_kind::enemy,
                           foe.facing_left, foe.anim});
    }

    for (int seat = 0; seat < max_players; ++seat) {
        const dragon& who = m_dragons[seat];
        if (!who.joined || !who.alive) continue;
        // Blink as protection RUNS OUT, not while it is fresh. Blinking from
        // the moment of spawn hides the dragon on the very frames a player is
        // looking for it, and says nothing they can act on; blinking over the
        // second half says "this is about to end", which they can. Cosmetic
        // and derived rather than stored, so it cannot desync anything.
        const bool ending = who.invulnerable > 0 &&
                            who.invulnerable < invulnerable_frames / 2;
        if (ending && (who.invulnerable / 4) % 2 == 1) continue;
        m_quads.push_back({who.x, who.y, body_w, body_h,
                           seat == 0 ? sprite_kind::player_green
                                     : sprite_kind::player_blue,
                           who.facing_left, who.anim});
    }

    for (const bubble& one : m_bubbles) {
        if (!one.active) continue;
        m_quads.push_back({one.x, one.y, bubble_size, bubble_size,
                           one.holds_enemy ? sprite_kind::bubble_full
                                           : sprite_kind::bubble,
                           false, one.anim});
    }

    for (const pop_effect& burst : m_pops) {
        if (!burst.active) continue;
        m_quads.push_back({burst.x, burst.y, bubble_size, bubble_size,
                           sprite_kind::pop, false,
                           static_cast<float>(burst.life) / 12.0f});
    }
}

uint64_t game::checksum() const noexcept {
    // FNV-1a over every value the simulation reads. Floats go in by their bit
    // pattern, not their printed value: two machines that agree to six decimal
    // places and differ in the last bit are already desynced, and rounding here
    // would hide exactly that.
    uint64_t hash = 0xCBF29CE484222325ull;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 0x100000001B3ull;
    };
    const auto mix_float = [&mix](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };

    mix(m_rng.state());
    mix(m_score);
    mix(static_cast<uint64_t>(m_level));
    mix(static_cast<uint64_t>(m_frame));
    mix(static_cast<uint64_t>(m_clear_timer));

    for (const dragon& who : m_dragons) {
        mix_float(who.x);
        mix_float(who.y);
        mix_float(who.vx);
        mix_float(who.vy);
        mix(static_cast<uint64_t>(who.alive) | (uint64_t(who.joined) << 1) |
            (uint64_t(who.facing_left) << 2));
        mix(static_cast<uint64_t>(who.fire_cooldown));
        mix(static_cast<uint64_t>(who.respawn_timer));
        mix(static_cast<uint64_t>(who.invulnerable));
        mix(static_cast<uint64_t>(who.lives));
    }
    for (const bubble& one : m_bubbles) {
        mix_float(one.x);
        mix_float(one.y);
        mix_float(one.vx);
        mix_float(one.vy);
        mix(static_cast<uint64_t>(one.life));
        mix(static_cast<uint64_t>(one.holds_enemy));
    }
    for (const enemy& foe : m_enemies) {
        mix_float(foe.x);
        mix_float(foe.y);
        mix_float(foe.vx);
        mix_float(foe.vy);
        mix(static_cast<uint64_t>(foe.facing_left));
    }
    for (const fruit_item& item : m_fruit) {
        mix_float(item.x);
        mix_float(item.y);
        mix(static_cast<uint64_t>(item.life));
    }
    return hash;
}

} // namespace bb
