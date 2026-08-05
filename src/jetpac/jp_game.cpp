#include "jetpac/jp_game.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jp {
namespace {

constexpr uint8_t tile_empty = 0;
constexpr uint8_t tile_platform = 1;
constexpr uint8_t tile_ground = 2;

// One screen, four ledges and a floor. Jetpac never scrolls and never has more
// geometry than this - the game is what happens in the air between the ledges,
// so they are placed to make crossing the screen a decision rather than a
// straight line.
const char* const screen_one[tiles_high] = {
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "......PPPPPP..........PPPPPP....",
    "................................",
    "................................",
    "................................",
    "................................",
    "..PPPPPP..................PPPPPP",
    "................................",
    "................................",
    "................................",
    "................................",
    "..........PPPPPPPPPPPP..........",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG",
};

constexpr float body_w = 8.0f;
constexpr float body_h = 8.0f;
constexpr float item_size = 8.0f;

// A jetpack is a THRUST, not a jump: it accelerates while held and the fall
// resumes the moment it is let go. That difference is the whole feel of the
// game, so thrust is an acceleration here rather than a velocity being set.
constexpr float gravity = 0.16f;
constexpr float thrust_accel = -0.34f;
constexpr float move_accel = 0.22f;
constexpr float drag = 0.86f;
constexpr float max_vx = 1.6f;
constexpr float max_fall = 2.6f;
constexpr float max_rise = -2.2f;

constexpr int fire_interval = 12;
constexpr float laser_speed = 4.2f;
constexpr int max_lasers = 4;

constexpr float alien_speed = 0.55f;
constexpr int alien_interval = 150;
constexpr int max_aliens = 4;

constexpr int respawn_frames = 100;
constexpr int invulnerable_frames = 120;
constexpr int launch_frames = 150;
constexpr int treasure_life = 480;

constexpr uint64_t score_alien = 25;
constexpr uint64_t score_deliver = 100;
constexpr uint64_t score_treasure = 250;
constexpr uint64_t score_launch = 1000;

bool overlaps(float ax, float ay, float aw, float ah, float bx, float by,
              float bw, float bh) noexcept {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// Horizontal wrap. The screen has no left or right edge - fly off one side and
// you arrive at the other - which is what makes a single screen feel like a
// planet rather than a box.
float wrap_x(float x) noexcept {
    if (x < -body_w) return x + world_width;
    if (x > world_width) return x - world_width;
    return x;
}

} // namespace

game::game() { reset(); }

void game::reset() {
    m_rng = rng(0x3E7FAC01ull);
    m_score = 0;
    m_level = 0;
    m_frame = 0;
    m_game_over = false;
    for (jetman& who : m_pilots) {
        who = jetman{};
        who.lives = 3;
    }
    load_level(0);
}

void game::load_level(int index) {
    m_level = index;
    m_tiles.assign(static_cast<std::size_t>(tiles_wide) * tiles_high,
                   tile_empty);
    for (int row = 0; row < tiles_high; ++row) {
        for (int col = 0; col < tiles_wide; ++col) {
            const char c = screen_one[row][col];
            m_tiles[static_cast<std::size_t>(row) * tiles_wide + col] =
                c == 'G' ? tile_ground
                         : (c == 'P' ? tile_platform : tile_empty);
        }
    }

    m_items.clear();
    m_aliens.clear();
    m_lasers.clear();
    m_blasts.clear();

    m_parts_fitted = 0;
    m_fuel_loaded = 0;
    m_rocket_state = rocket_state::needs_parts;
    m_payload_out = false;
    m_payload_timer = 0;
    m_alien_timer = alien_interval;
    m_launch_timer = 0;

    // The rocket sits on the ground, off centre so the trip to it is never
    // symmetrical from both sides of the screen.
    m_rocket_x = 6.0f * tile_size;
    m_rocket_ground_y = 23.0f * tile_size;

    for (int seat = 0; seat < max_players; ++seat) {
        jetman& who = m_pilots[seat];
        who.x = (seat == 0 ? 20.0f : 26.0f) * tile_size;
        who.y = m_rocket_ground_y - body_h;
        who.vx = 0.0f;
        who.vy = 0.0f;
        who.carrying = -1;
        who.thrusting = false;
        who.fire_cooldown = 0;
        who.respawn_timer = 0;
        who.invulnerable = invulnerable_frames;
        if (who.joined) who.alive = true;
    }

    spawn_next_payload();
}

bool game::solid(int column, int row) const noexcept {
    if (column < 0 || column >= tiles_wide || row < 0 || row >= tiles_high)
        return false;
    return m_tiles[static_cast<std::size_t>(row) * tiles_wide + column] !=
           tile_empty;
}

bool game::solid_at(float x, float y) const noexcept {
    return solid(static_cast<int>(std::floor(x / tile_size)),
                 static_cast<int>(std::floor(y / tile_size)));
}

bool game::land_from_above(float x, float prev_y, float next_y,
                           float& out_y) const noexcept {
    // Only a tile top CROSSED this move stops a fall. Testing overlap instead
    // catches something that rose into a ledge from underneath and pins it
    // there - every platform here is one-way, because a jetpack that could be
    // trapped under a ledge would be unplayable.
    const float prev_bottom = prev_y + body_h;
    const float next_bottom = next_y + body_h;
    const int first = static_cast<int>(std::floor(prev_bottom / tile_size));
    const int last = static_cast<int>(std::floor(next_bottom / tile_size));
    const int left = static_cast<int>(std::floor((x + 1.0f) / tile_size));
    const int right =
        static_cast<int>(std::floor((x + body_w - 1.0f) / tile_size));
    for (int row = std::max(0, first); row <= last; ++row) {
        const float top = static_cast<float>(row) * tile_size;
        if (prev_bottom > top + 0.001f) continue;
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

bool game::carrying(int seat) const noexcept {
    if (seat < 0 || seat >= max_players) return false;
    return m_pilots[seat].carrying >= 0;
}

int game::lives(int seat) const noexcept {
    if (seat < 0 || seat >= max_players) return 0;
    return m_pilots[seat].lives;
}

void game::spawn_next_payload() {
    // One piece at a time, dropped from the top. Releasing them all at once
    // would turn the game into a collection sweep; one at a time is what makes
    // each trip a decision about the aliens in the way.
    if (m_payload_out) return;
    if (m_rocket_state == rocket_state::ready ||
        m_rocket_state == rocket_state::launching)
        return;

    item dropped;
    dropped.kind = m_rocket_state == rocket_state::needs_parts
                       ? item_kind::rocket_part
                       : item_kind::fuel_pod;
    dropped.x = static_cast<float>(4 + m_rng.below(24)) * tile_size;
    dropped.y = -item_size;
    dropped.vy = 0.0f;
    dropped.active = true;
    dropped.carried = false;
    dropped.life = 0;
    m_items.push_back(dropped);
    m_payload_out = true;
}

void game::spawn_alien() {
    if (static_cast<int>(m_aliens.size()) >= max_aliens) return;
    alien foe;
    const bool from_left = m_rng.below(2) == 0;
    foe.x = from_left ? -body_w : world_width;
    foe.vx = from_left ? alien_speed : -alien_speed;
    // Never at the very top or on the floor: an alien that spawns inside the
    // ground would be unshootable, and one at the ceiling is never in the way.
    foe.y = static_cast<float>(3 + m_rng.below(16)) * tile_size;
    foe.vy = 0.0f;
    foe.is_ufo = m_rng.below(3) == 0;
    foe.alive = true;
    m_aliens.push_back(foe);
}

void game::deliver(jetman& who, item& what) {
    what.active = false;
    who.carrying = -1;
    m_payload_out = false;
    m_score += score_deliver;
    emit(cue::deliver);

    if (what.kind == item_kind::rocket_part) {
        ++m_parts_fitted;
        if (m_parts_fitted >= parts_needed)
            m_rocket_state = rocket_state::needs_fuel;
    } else if (what.kind == item_kind::fuel_pod) {
        ++m_fuel_loaded;
        if (m_fuel_loaded >= fuel_needed)
            m_rocket_state = rocket_state::ready;
    }
    m_payload_timer = 45;
}

void game::step_pilot(jetman& who, const player_intent& intent, int seat) {
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
        if (who.respawn_timer > 0 && --who.respawn_timer == 0 &&
            who.lives > 0) {
            who.alive = true;
            who.x = (seat == 0 ? 20.0f : 26.0f) * tile_size;
            who.y = m_rocket_ground_y - body_h;
            who.vx = 0.0f;
            who.vy = 0.0f;
            who.invulnerable = invulnerable_frames;
        }
        return;
    }

    if (who.invulnerable > 0) --who.invulnerable;
    if (who.fire_cooldown > 0) --who.fire_cooldown;

    who.vx += intent.move_x * move_accel;
    who.vx *= drag;
    who.vx = std::clamp(who.vx, -max_vx, max_vx);
    if (intent.move_x < 0.0f) who.facing_left = true;
    if (intent.move_x > 0.0f) who.facing_left = false;

    who.thrusting = intent.thrust;
    who.vy += intent.thrust ? (gravity + thrust_accel) : gravity;
    who.vy = std::clamp(who.vy, max_rise, max_fall);
    if (intent.thrust && (m_frame % 9) == 0) emit(cue::thrust);

    who.x = wrap_x(who.x + who.vx);

    const float next_y = who.y + who.vy;
    float rest_y = 0.0f;
    if (who.vy > 0.0f && land_from_above(who.x, who.y, next_y, rest_y)) {
        who.y = rest_y;
        who.vy = 0.0f;
    } else {
        who.y = next_y;
    }
    // The ceiling is a wall, not a killer: bumping it stops the climb.
    if (who.y < 0.0f) {
        who.y = 0.0f;
        who.vy = 0.0f;
    }

    if (intent.fire && who.fire_cooldown == 0 &&
        static_cast<int>(m_lasers.size()) < max_lasers) {
        laser_bolt shot;
        shot.x = who.x + (who.facing_left ? -4.0f : body_w);
        shot.y = who.y + body_h * 0.35f;
        shot.vx = who.facing_left ? -laser_speed : laser_speed;
        shot.active = true;
        m_lasers.push_back(shot);
        who.fire_cooldown = fire_interval;
        emit(cue::fire);
    }

    // Picking up is touching, and only when empty-handed. There is no button
    // for it: a game about ferrying things should never ask which of two
    // adjacent objects you meant.
    if (who.carrying < 0) {
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            item& what = m_items[i];
            if (!what.active || what.carried) continue;
            if (!overlaps(who.x, who.y, body_w, body_h, what.x, what.y,
                          item_size, item_size))
                continue;
            if (what.kind == item_kind::treasure) {
                what.active = false;
                m_score += score_treasure;
                emit(cue::collect);
                break;
            }
            what.carried = true;
            who.carrying = static_cast<int>(i);
            emit(cue::collect);
            break;
        }
    }

    if (who.carrying >= 0 &&
        who.carrying < static_cast<int>(m_items.size())) {
        item& held = m_items[static_cast<std::size_t>(who.carrying)];
        held.x = who.x;
        held.y = who.y + body_h;
        held.vy = 0.0f;
        // Over the rocket is delivered. The rocket is two tiles wide, and the
        // check is horizontal only, so a delivery can be made from any height -
        // dropping down the shaft is the shot, not landing on a pad.
        const bool over_rocket =
            held.x + item_size > m_rocket_x &&
            held.x < m_rocket_x + tile_size * 2.0f;
        if (over_rocket) deliver(who, held);
    }

    who.anim += 0.14f;
    if (who.anim >= 1.0f) who.anim -= 1.0f;
}

void game::step_items() {
    for (item& what : m_items) {
        if (!what.active || what.carried) continue;
        if (what.kind == item_kind::treasure && --what.life <= 0) {
            what.active = false;
            continue;
        }
        what.vy = std::min(what.vy + gravity, max_fall);
        const float next_y = what.y + what.vy;
        float rest_y = 0.0f;
        if (land_from_above(what.x, what.y, next_y, rest_y)) {
            what.y = rest_y;
            what.vy = 0.0f;
        } else {
            what.y = next_y;
        }
    }
}

void game::step_aliens() {
    if (--m_alien_timer <= 0) {
        spawn_alien();
        m_alien_timer = alien_interval - std::min(90, m_level * 12);
    }

    for (alien& foe : m_aliens) {
        if (!foe.alive) continue;
        foe.x += foe.vx;
        // A UFO weaves; a drone flies straight. Two behaviours is enough to
        // make the screen feel populated rather than patterned.
        if (foe.is_ufo)
            foe.y += std::sin(static_cast<float>(m_frame) * 0.05f) * 0.8f;
        foe.y = std::clamp(foe.y, tile_size, world_height - tile_size * 2.0f);
        foe.anim += 0.1f;
        if (foe.anim >= 1.0f) foe.anim -= 1.0f;
        if (foe.x < -tile_size * 2.0f || foe.x > world_width + tile_size * 2.0f)
            foe.alive = false;
    }
    m_aliens.erase(std::remove_if(m_aliens.begin(), m_aliens.end(),
                                  [](const alien& a) { return !a.alive; }),
                   m_aliens.end());
}

void game::step_lasers() {
    for (laser_bolt& shot : m_lasers) {
        if (!shot.active) continue;
        shot.x += shot.vx;
        if (shot.x < 0.0f || shot.x > world_width) {
            shot.active = false;
            continue;
        }
        for (alien& foe : m_aliens) {
            if (!foe.alive) continue;
            if (!overlaps(shot.x, shot.y, 4.0f, 2.0f, foe.x, foe.y, body_w,
                          body_h))
                continue;
            foe.alive = false;
            shot.active = false;
            m_score += score_alien;
            blast burst;
            burst.x = foe.x;
            burst.y = foe.y;
            burst.life = 14;
            burst.active = true;
            m_blasts.push_back(burst);
            emit(cue::alien_hit);
            // A shot alien sometimes leaves treasure, which is the only reason
            // to fly INTO the danger rather than around it.
            if (m_rng.below(4) == 0) {
                item prize;
                prize.kind = item_kind::treasure;
                prize.x = foe.x;
                prize.y = foe.y;
                prize.active = true;
                prize.life = treasure_life;
                m_items.push_back(prize);
            }
            break;
        }
    }
    m_lasers.erase(std::remove_if(m_lasers.begin(), m_lasers.end(),
                                  [](const laser_bolt& l) { return !l.active; }),
                   m_lasers.end());
}

void game::step(const player_intent* intents, int player_count) {
    m_cues.clear();
    ++m_frame;

    const int seats = std::min(player_count, max_players);
    for (int seat = 0; seat < seats; ++seat)
        step_pilot(m_pilots[seat], intents[seat], seat);

    step_items();
    step_aliens();
    step_lasers();

    if (m_payload_timer > 0 && --m_payload_timer == 0) spawn_next_payload();

    for (blast& burst : m_blasts)
        if (burst.active && --burst.life <= 0) burst.active = false;
    m_blasts.erase(std::remove_if(m_blasts.begin(), m_blasts.end(),
                                  [](const blast& b) { return !b.active; }),
                   m_blasts.end());

    // Aliens kill on contact. Checked after everything has moved so the
    // outcome does not depend on update order.
    for (int seat = 0; seat < max_players; ++seat) {
        jetman& who = m_pilots[seat];
        if (!who.alive || who.invulnerable > 0) continue;
        for (const alien& foe : m_aliens) {
            if (!foe.alive) continue;
            if (!overlaps(who.x, who.y, body_w, body_h, foe.x, foe.y, body_w,
                          body_h))
                continue;
            who.alive = false;
            who.lives = std::max(0, who.lives - 1);
            who.respawn_timer = respawn_frames;
            // Whatever he was carrying falls where he died.
            if (who.carrying >= 0 &&
                who.carrying < static_cast<int>(m_items.size())) {
                m_items[static_cast<std::size_t>(who.carrying)].carried = false;
                who.carrying = -1;
            }
            blast burst;
            burst.x = who.x;
            burst.y = who.y;
            burst.life = 20;
            burst.active = true;
            m_blasts.push_back(burst);
            emit(cue::death);
            break;
        }
    }

    // A fuelled rocket needs a pilot in it. Flying into it is the launch.
    if (m_rocket_state == rocket_state::ready) {
        for (jetman& who : m_pilots) {
            if (!who.alive) continue;
            if (who.x + body_w < m_rocket_x ||
                who.x > m_rocket_x + tile_size * 2.0f)
                continue;
            m_rocket_state = rocket_state::launching;
            m_launch_timer = launch_frames;
            m_score += score_launch;
            emit(cue::launch);
            break;
        }
    } else if (m_rocket_state == rocket_state::launching) {
        if (--m_launch_timer <= 0) load_level(m_level + 1);
    }

    m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                 [](const item& i) { return !i.active; }),
                  m_items.end());
    // Erasing invalidates the carry indices, so they are rebuilt rather than
    // patched: an index into a vector that shrinks under it is how a carried
    // fuel pod becomes a different object mid-flight.
    for (jetman& who : m_pilots) who.carrying = -1;
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        if (!m_items[i].carried) continue;
        for (jetman& who : m_pilots) {
            if (!who.alive || who.carrying >= 0) continue;
            who.carrying = static_cast<int>(i);
            break;
        }
    }

    bool anyone_playing = false;
    for (const jetman& who : m_pilots)
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
            m_quads.push_back({static_cast<float>(col) * tile_size,
                               static_cast<float>(row) * tile_size, tile_size,
                               tile_size,
                               tile == tile_ground ? sprite_kind::ground
                                                   : sprite_kind::platform,
                               false, 0.0f});
        }
    }

    // The rocket, as much of it as has been built. Drawn bottom-up so the
    // stack reads as progress at a glance - which is the only status display
    // this game has or needs.
    const float rocket_w = tile_size * 2.0f;
    m_quads.push_back({m_rocket_x, m_rocket_ground_y - tile_size, rocket_w,
                       tile_size, sprite_kind::rocket_base, false, 0.0f});
    if (m_parts_fitted >= 1)
        m_quads.push_back({m_rocket_x, m_rocket_ground_y - tile_size * 2.0f,
                           rocket_w, tile_size, sprite_kind::rocket_mid, false,
                           0.0f});
    if (m_parts_fitted >= 2)
        m_quads.push_back({m_rocket_x, m_rocket_ground_y - tile_size * 3.0f,
                           rocket_w, tile_size, sprite_kind::rocket_mid, false,
                           0.0f});
    if (m_parts_fitted >= 3)
        m_quads.push_back({m_rocket_x, m_rocket_ground_y - tile_size * 4.0f,
                           rocket_w, tile_size, sprite_kind::rocket_top, false,
                           0.0f});

    for (const item& what : m_items) {
        if (!what.active) continue;
        sprite_kind kind = sprite_kind::treasure;
        if (what.kind == item_kind::rocket_part) kind = sprite_kind::rocket_part;
        if (what.kind == item_kind::fuel_pod) kind = sprite_kind::fuel_pod;
        m_quads.push_back(
            {what.x, what.y, item_size, item_size, kind, false, 0.0f});
    }

    for (const alien& foe : m_aliens) {
        if (!foe.alive) continue;
        m_quads.push_back({foe.x, foe.y, body_w, body_h,
                           foe.is_ufo ? sprite_kind::alien_ufo
                                      : sprite_kind::alien_drone,
                           foe.vx < 0.0f, foe.anim});
    }

    for (const laser_bolt& shot : m_lasers) {
        if (!shot.active) continue;
        m_quads.push_back({shot.x, shot.y, 4.0f, 2.0f, sprite_kind::laser,
                           shot.vx < 0.0f, 0.0f});
    }

    for (int seat = 0; seat < max_players; ++seat) {
        const jetman& who = m_pilots[seat];
        if (!who.joined || !who.alive) continue;
        // Blink as protection runs out rather than while it is fresh - a pilot
        // hidden on the frames a player is looking for him says nothing they
        // can act on.
        const bool ending = who.invulnerable > 0 &&
                            who.invulnerable < invulnerable_frames / 2;
        if (ending && (who.invulnerable / 4) % 2 == 1) continue;
        if (who.thrusting)
            m_quads.push_back({who.x, who.y + body_h, body_w, tile_size * 0.5f,
                               sprite_kind::jetman_flame, who.facing_left,
                               who.anim});
        m_quads.push_back({who.x, who.y, body_w, body_h, sprite_kind::jetman,
                           who.facing_left, who.anim});
    }

    for (const blast& burst : m_blasts) {
        if (!burst.active) continue;
        m_quads.push_back({burst.x, burst.y, body_w, body_h,
                           sprite_kind::explosion, false,
                           static_cast<float>(burst.life) / 20.0f});
    }
}

uint64_t game::checksum() const noexcept {
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
    mix(static_cast<uint64_t>(m_parts_fitted));
    mix(static_cast<uint64_t>(m_fuel_loaded));
    mix(static_cast<uint64_t>(m_rocket_state));
    mix(static_cast<uint64_t>(m_alien_timer));
    mix(static_cast<uint64_t>(m_payload_timer));
    mix(static_cast<uint64_t>(m_launch_timer));

    for (const jetman& who : m_pilots) {
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
        mix(static_cast<uint64_t>(who.carrying + 1));
    }
    for (const item& what : m_items) {
        mix_float(what.x);
        mix_float(what.y);
        mix_float(what.vy);
        mix(static_cast<uint64_t>(what.kind));
        mix(static_cast<uint64_t>(what.carried));
        mix(static_cast<uint64_t>(what.life));
    }
    for (const alien& foe : m_aliens) {
        mix_float(foe.x);
        mix_float(foe.y);
        mix_float(foe.vx);
        mix(static_cast<uint64_t>(foe.is_ufo));
    }
    for (const laser_bolt& shot : m_lasers) {
        mix_float(shot.x);
        mix_float(shot.y);
        mix_float(shot.vx);
    }
    return hash;
}

} // namespace jp
