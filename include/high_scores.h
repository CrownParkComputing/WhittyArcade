// MAME-compatible high-score persistence and verified score-table decoders.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct high_score_entry {
    std::uint64_t score{};
    std::string name;
};

struct high_score_table {
    std::string game_name;
    std::vector<high_score_entry> entries;
    std::vector<std::pair<std::string, std::uint64_t>> extra_scores;
};

// Decoders are deliberately available only for tables whose MAME memory
// blocks and binary representation have been verified.
bool decode_high_score_table(const std::string& short_name,
                             const std::vector<std::uint8_t>& bytes,
                             high_score_table& table,
                             std::string* error = nullptr);

std::string high_score_root_path();
std::string high_score_report(const std::string& short_name);

// Structured variant of high_score_report for the scoreboard view: fills the
// table and returns true when a verified decode exists, otherwise leaves an
// explanation (no file yet, undecodable) in message. format_high_score gives
// the same thousands-grouped rendering the text report uses.
bool high_score_view(const std::string& short_name, high_score_table& table,
                     std::string& message);
std::string format_high_score(std::uint64_t score);

// Returns true when a verified score-table decoder exists for the given game.
bool has_high_score_decoder(const std::string& short_name);

// A small board-facing helper implementing MAME's hiscore.dat lifecycle:
// wait until the game's default table is initialized, restore a compatible
// .hi file once, then persist changed score bytes. The callbacks access the
// emulated CPU's byte address space.
class high_score_runtime {
public:
    using read_callback = std::function<std::uint8_t(std::uint32_t)>;
    using write_callback =
        std::function<void(std::uint32_t, std::uint8_t)>;

    explicit high_score_runtime(std::string short_name);

    void reset();
    void update(const read_callback& read, const write_callback& write);
    void flush(const read_callback& read);

private:
    std::string m_short_name;
    bool m_initialized{};
    bool m_disabled{};
    unsigned m_frames_since_check{};
    std::vector<std::uint8_t> m_last_snapshot;
};
