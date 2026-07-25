#include "high_scores.h"
#include "platform_paths.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace {

struct memory_block {
    std::uint32_t address;
    std::size_t size;
    std::uint8_t ready_first;
    std::uint8_t ready_last;
};

using decoder = bool (*)(const std::vector<std::uint8_t>&,
                         high_score_table&, std::string&);

struct score_spec {
    const char* game_name;
    const memory_block* blocks;
    std::size_t block_count;
    decoder decode;
};

// Address/guard definitions follow MAME's hiscore.dat. The decoders below
// were independently checked against the corresponding public hi2txt layouts
// and reference output; no unverified EEPROM offsets are exposed.
constexpr std::array<memory_block, 21> phoenix_blocks{{
    {0x4389, 3, 0x00, 0x00},
    {0x41e1, 1, 0x20, 0x20}, {0x41c1, 1, 0x20, 0x20},
    {0x41a1, 1, 0x20, 0x20}, {0x4181, 1, 0x20, 0x20},
    {0x4161, 1, 0x20, 0x20}, {0x4141, 1, 0x20, 0x20},
    {0x4381, 3, 0x00, 0x00},
    {0x4301, 1, 0x20, 0x20}, {0x42e1, 1, 0x20, 0x20},
    {0x42c1, 1, 0x20, 0x20}, {0x42a1, 1, 0x20, 0x20},
    {0x4281, 1, 0x20, 0x20}, {0x4261, 1, 0x20, 0x20},
    {0x4385, 3, 0x00, 0x00},
    {0x40c1, 1, 0x20, 0x20}, {0x40a1, 1, 0x20, 0x20},
    {0x4081, 1, 0x20, 0x20}, {0x4061, 1, 0x20, 0x20},
    {0x4041, 1, 0x20, 0x20}, {0x4021, 1, 0x20, 0x20},
}};

constexpr std::array<memory_block, 1> mooncrst_blocks{{
    {0x8042, 0x54, 0x00, 0x24},
}};

constexpr std::array<memory_block, 2> shinobi_blocks{{
    {0xfffc00, 0x142, 0x00, 0x54},
    {0xfff010, 4, 0x00, 0x00},
}};

// MAME plugins/hiscore/hiscore.dat (capcom/gng.cpp): ten rank pointers and
// ten 7-byte score/name records, followed by the four-byte live top score.
constexpr std::array<memory_block, 2> gng_blocks{{
    {0x1518, 0x5a, 0x15, 0x72},
    {0x00d0, 4, 0x00, 0x00},
}};

bool bcd_value(const std::uint8_t* bytes, std::size_t size,
               std::uint64_t& value) {
    value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned high = bytes[index] >> 4;
        const unsigned low = bytes[index] & 0x0f;
        if (high > 9 || low > 9) return false;
        value = value * 100 + high * 10 + low;
    }
    return true;
}

bool low_nibble_digits(const std::uint8_t* bytes, std::size_t size,
                       std::uint64_t& value) {
    value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned digit = bytes[index] & 0x0f;
        if (digit > 9) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool decode_phoenix(const std::vector<std::uint8_t>& bytes,
                    high_score_table& table, std::string& error) {
    if (bytes.size() != 27) {
        error = "Phoenix score data must be exactly 27 bytes.";
        return false;
    }
    std::uint64_t top = 0;
    std::uint64_t player1 = 0;
    std::uint64_t player2 = 0;
    std::uint64_t top_display = 0;
    std::uint64_t player1_display = 0;
    std::uint64_t player2_display = 0;
    if (!bcd_value(bytes.data(), 3, top) ||
        !bcd_value(bytes.data() + 9, 3, player1) ||
        !bcd_value(bytes.data() + 18, 3, player2) ||
        !low_nibble_digits(bytes.data() + 3, 6, top_display) ||
        !low_nibble_digits(bytes.data() + 12, 6, player1_display) ||
        !low_nibble_digits(bytes.data() + 21, 6, player2_display)) {
        error = "Phoenix score data contains an invalid BCD digit.";
        return false;
    }
    top = std::max(top, top_display);
    player1 = std::max(player1, player1_display);
    player2 = std::max(player2, player2_display);
    table.game_name = "Phoenix";
    table.entries = {{std::max({top, player1, player2}), {}}};
    table.extra_scores = {{"Player 1", player1}, {"Player 2", player2}};
    return true;
}

char mooncrst_character(std::uint8_t value) {
    if (value == 0x24) return ' ';
    if (value == 0x2c) return '.';
    if (value == 0xff) return '\0';
    const unsigned translated = static_cast<unsigned>(value) + 55;
    return translated >= 0x20 && translated <= 0x7e ?
        static_cast<char>(translated) : '?';
}

std::string decode_mooncrst_name(const std::uint8_t* bytes) {
    std::string result;
    for (std::size_t index = 0; index < 10; ++index) {
        const char value = mooncrst_character(bytes[index]);
        if (!value) break;
        result.push_back(value);
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    if (result == "usqomkig") result = "Nichibutsu";
    return result;
}

bool decode_mooncrst(const std::vector<std::uint8_t>& bytes,
                     high_score_table& table, std::string& error) {
    if (bytes.size() != 0x54) {
        error = "Moon Cresta score data must be exactly 84 bytes.";
        return false;
    }
    table.game_name = "Moon Cresta";
    for (std::size_t rank = 0; rank < 5; ++rank) {
        std::uint64_t score = 0;
        if (!bcd_value(bytes.data() + rank * 3, 3, score)) {
            error = "Moon Cresta score data contains an invalid BCD digit.";
            return false;
        }
        const std::size_t name_offset = rank < 4 ? 18 + rank * 14 : 74;
        table.entries.push_back(
            {score, decode_mooncrst_name(bytes.data() + name_offset)});
    }
    return true;
}

std::string decode_shinobi_name(const std::uint8_t* bytes) {
    std::string result;
    for (std::size_t index = 0; index < 3; ++index) {
        const std::uint8_t value = bytes[index];
        switch (value) {
        case 0x22: result.push_back('.'); break;
        case 0x23: result.push_back('\''); break;
        case 0x25: result.push_back('/'); break;
        case 0x26: result.push_back('('); break;
        case 0x27: result.push_back(')'); break;
        case 0x28: result.push_back('-'); break;
        case 0x29: result += "M"; break;
        case 0x2a: result += "F"; break;
        case 0x2b: result += "*"; break;
        case 0x2e: result.push_back('='); break;
        default:
            result.push_back(value >= 0x20 && value <= 0x7e ?
                             static_cast<char>(value) : '?');
            break;
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

bool decode_shinobi(const std::vector<std::uint8_t>& bytes,
                    high_score_table& table, std::string& error) {
    if (bytes.size() != 0x146) {
        error = "Shinobi score data must be exactly 326 bytes.";
        return false;
    }
    table.game_name = "Shinobi";
    for (std::size_t rank = 0; rank < 20; ++rank) {
        const std::size_t offset = rank * 8;
        std::uint64_t score = 0;
        std::uint64_t mirrored_score = 0;
        if (!bcd_value(bytes.data() + offset, 4, score)) {
            error = "Shinobi score data contains an invalid BCD digit.";
            return false;
        }
        const std::size_t mirrored_offset = 0xa2 + rank * 8;
        if (!bcd_value(bytes.data() + mirrored_offset, 4, mirrored_score)) {
            error = "Shinobi's mirrored score table contains an invalid "
                    "BCD digit.";
            return false;
        }
        table.entries.push_back(
            {std::max(score, mirrored_score),
             decode_shinobi_name(bytes.data() + offset + 5)});
    }
    std::uint64_t recorded_top = 0;
    if (!bcd_value(bytes.data() + 0x142, 4, recorded_top)) {
        error = "Shinobi's top-score record contains an invalid BCD digit.";
        return false;
    }
    table.extra_scores = {{"Top score record", recorded_top}};
    return true;
}

std::string decode_gng_name(const std::uint8_t* bytes) {
    std::string result;
    for (std::size_t index = 0; index < 3; ++index) {
        const std::uint8_t value = bytes[index];
        if (value == 0x1d) result.push_back('.');
        else if (value >= 0x20 && value <= 0x7e)
            result.push_back(static_cast<char>(value));
        else if (value != 0)
            result.push_back('?');
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

bool decode_gng(const std::vector<std::uint8_t>& bytes,
                high_score_table& table, std::string& error) {
    if (bytes.size() != 0x5e) {
        error = "Ghosts'n Goblins score data must be exactly 94 bytes.";
        return false;
    }
    std::array<std::size_t, 10> record_for_rank{};
    std::array<bool, 10> used{};
    for (std::size_t rank = 0; rank < 10; ++rank) {
        // The saved table stores a page byte (0x15) followed by the record
        // offset. hi2txt's byte-skip="odd" selects that second byte.
        const int pointer = bytes[rank * 2 + 1];
        if (pointer < 44 || (pointer - 44) % 7 != 0) {
            error = "Ghosts'n Goblins contains an invalid rank pointer.";
            return false;
        }
        const std::size_t record = static_cast<std::size_t>(pointer - 44) / 7;
        if (record >= 10 || used[record]) {
            error = "Ghosts'n Goblins contains duplicate rank pointers.";
            return false;
        }
        used[record] = true;
        record_for_rank[rank] = record;
    }
    table.game_name = "Ghosts'n Goblins";
    constexpr std::size_t records_offset = 20;
    for (std::size_t rank = 0; rank < 10; ++rank) {
        const std::size_t offset = records_offset + record_for_rank[rank] * 7;
        std::uint64_t score = 0;
        if (!bcd_value(bytes.data() + offset, 4, score)) {
            error = "Ghosts'n Goblins score data contains an invalid BCD digit.";
            return false;
        }
        table.entries.push_back(
            {score, decode_gng_name(bytes.data() + offset + 4)});
    }
    std::uint64_t top_score = 0;
    if (!bcd_value(bytes.data() + 0x5a, 4, top_score)) {
        error = "Ghosts'n Goblins top score contains an invalid BCD digit.";
        return false;
    }
    table.extra_scores = {{"Top score record", top_score}};
    return true;
}

const score_spec* find_spec(const std::string& short_name) {
    static const score_spec phoenix{
        "Phoenix", phoenix_blocks.data(), phoenix_blocks.size(), decode_phoenix};
    static const score_spec mooncrst{
        "Moon Cresta", mooncrst_blocks.data(), mooncrst_blocks.size(),
        decode_mooncrst};
    static const score_spec shinobi{
        "Shinobi", shinobi_blocks.data(), shinobi_blocks.size(), decode_shinobi};
    static const score_spec gng{
        "Ghosts'n Goblins", gng_blocks.data(), gng_blocks.size(), decode_gng};
    if (short_name == "phoenix") return &phoenix;
    if (short_name == "mooncrst") return &mooncrst;
    if (short_name == "shinobi" || short_name == "shinobi4") return &shinobi;
    if (short_name == "gng") return &gng;
    return nullptr;
}

std::size_t expected_size(const score_spec& spec) {
    std::size_t result = 0;
    for (std::size_t index = 0; index < spec.block_count; ++index)
        result += spec.blocks[index].size;
    return result;
}

fs::path config_root() {
    const fs::path root = whitty_platform::config_root();
    return root.empty() ? fs::current_path() : root;
}

fs::path score_root() {
    if (const char* override_path = std::getenv("WHITTYARCADE_HISCORE_PATH"))
        return fs::path(override_path);
    return config_root() / "WhittyArcade" / "hi";
}

fs::path own_score_path(const std::string& short_name) {
    return score_root() /
        (whitty_platform::cabinet_scoped_name(short_name) + ".hi");
}

std::vector<fs::path> score_candidates(const std::string& short_name) {
    std::vector<std::string> names{short_name};
    if (short_name == "shinobi4") names.push_back("shinobi");
    std::vector<fs::path> result;
    result.push_back(own_score_path(short_name));
    if (const fs::path home = whitty_platform::home_root(); !home.empty()) {
        for (const std::string& name : names) {
            result.push_back(home / ".mame" / "hi" / (name + ".hi"));
            result.push_back(home / ".local" / "share" / "mame" / "hi" /
                             (name + ".hi"));
        }
    }
    return result;
}

bool read_file(const fs::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), size);
        return input.gcount() == size;
    }
    return true;
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        if (!bytes.empty())
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        if (!output.good()) return false;
    }
    fs::rename(temporary, path, error);
    if (!error) return true;
    fs::remove(path, error);
    error.clear();
    fs::rename(temporary, path, error);
    return !error;
}

bool locate_score_file(const std::string& short_name, fs::path& path,
                       std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    for (const fs::path& candidate : score_candidates(short_name)) {
        if (!fs::is_regular_file(candidate, error)) continue;
        path = candidate;
        return read_file(candidate, bytes);
    }
    return false;
}

bool memory_ready(const score_spec& spec,
                  const high_score_runtime::read_callback& read) {
    for (std::size_t index = 0; index < spec.block_count; ++index) {
        const memory_block& block = spec.blocks[index];
        if (read(block.address) != block.ready_first ||
            read(block.address + static_cast<std::uint32_t>(block.size - 1)) !=
                block.ready_last)
            return false;
    }
    return true;
}

std::vector<std::uint8_t> capture(
    const score_spec& spec, const high_score_runtime::read_callback& read) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(expected_size(spec));
    for (std::size_t block_index = 0; block_index < spec.block_count;
         ++block_index) {
        const memory_block& block = spec.blocks[block_index];
        for (std::size_t index = 0; index < block.size; ++index)
            bytes.push_back(read(block.address +
                                 static_cast<std::uint32_t>(index)));
    }
    return bytes;
}

void restore(const score_spec& spec, const std::vector<std::uint8_t>& bytes,
             const high_score_runtime::write_callback& write) {
    std::size_t source = 0;
    for (std::size_t block_index = 0; block_index < spec.block_count;
         ++block_index) {
        const memory_block& block = spec.blocks[block_index];
        for (std::size_t index = 0; index < block.size; ++index)
            write(block.address + static_cast<std::uint32_t>(index),
                  bytes[source++]);
    }
}

std::string format_score(std::uint64_t score) {
    std::string digits = std::to_string(score);
    for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(digits.size()) - 3;
         position > 0; position -= 3)
        digits.insert(static_cast<std::size_t>(position), 1, ',');
    return digits;
}

} // namespace

bool decode_high_score_table(const std::string& short_name,
                             const std::vector<std::uint8_t>& bytes,
                             high_score_table& table, std::string* error) {
    table = {};
    const score_spec* spec = find_spec(short_name);
    if (!spec) {
        if (error) *error = "No verified decoder is available for this game.";
        return false;
    }
    std::string reason;
    const bool valid = spec->decode(bytes, table, reason);
    if (!valid && error) *error = std::move(reason);
    return valid;
}

std::string high_score_root_path() {
    return score_root().string();
}

bool has_high_score_decoder(const std::string& short_name) {
    return find_spec(short_name) != nullptr;
}

bool high_score_view(const std::string& short_name, high_score_table& table,
                     std::string& message) {
    const score_spec* spec = find_spec(short_name);
    if (!spec) {
        message = "No verified high-score decoder is available for this game "
                  "yet.";
        return false;
    }
    fs::path source;
    std::vector<std::uint8_t> bytes;
    if (!locate_score_file(short_name, source, bytes)) {
        message = std::string("No saved score table yet. Play ") +
                  spec->game_name +
                  " once and the board's own table appears here; existing "
                  "MAME .hi files are picked up automatically.";
        return false;
    }
    std::string error;
    if (!decode_high_score_table(short_name, bytes, table, &error)) {
        message = "The saved score file could not be decoded safely.\n" +
                  error;
        return false;
    }
    return true;
}

std::string format_high_score(std::uint64_t score) {
    return format_score(score);
}

std::string high_score_report(const std::string& short_name) {
    const score_spec* spec = find_spec(short_name);
    if (!spec)
        return "No verified high-score decoder is available for this game yet.\n\n"
               "Its operator EEPROM/NVRAM remains available in the data "
               "manager; WhittyArcade will not display guessed values.";

    fs::path source;
    std::vector<std::uint8_t> bytes;
    if (!locate_score_file(short_name, source, bytes)) {
        return std::string("No saved high-score table exists yet.\n\n") +
            "Play " + spec->game_name +
            " once and WhittyArcade will create one after the game's "
            "score table is initialized. Existing MAME .hi files are "
            "detected automatically.";
    }

    high_score_table table;
    std::string error;
    if (!decode_high_score_table(short_name, bytes, table, &error)) {
        std::ostringstream message;
        message << "The saved score file could not be decoded safely.\n\n"
                << error << "\nExpected " << expected_size(*spec)
                << " bytes; found " << bytes.size() << ".\n\nSource: "
                << source.string();
        return message.str();
    }

    std::ostringstream report;
    report << table.game_name << " high scores\n\n";
    const std::size_t shown = std::min<std::size_t>(table.entries.size(), 10);
    for (std::size_t rank = 0; rank < shown; ++rank) {
        report << std::setw(2) << rank + 1 << ".  "
               << std::setw(10) << format_score(table.entries[rank].score);
        if (!table.entries[rank].name.empty())
            report << "   " << table.entries[rank].name;
        report << '\n';
    }
    for (const auto& extra : table.extra_scores)
        report << extra.first << ": " << format_score(extra.second) << '\n';
    report << "\nSource: " << source.string();
    return report.str();
}

high_score_runtime::high_score_runtime(std::string short_name)
    : m_short_name(std::move(short_name)) {}

void high_score_runtime::reset() {
    m_initialized = false;
    m_disabled = false;
    m_frames_since_check = 0;
    m_last_snapshot.clear();
}

void high_score_runtime::update(const read_callback& read,
                                const write_callback& write) {
    const score_spec* spec = find_spec(m_short_name);
    if (!spec || m_disabled) return;

    if (!m_initialized) {
        if (!memory_ready(*spec, read)) return;
        fs::path source;
        std::vector<std::uint8_t> stored;
        const bool found = locate_score_file(m_short_name, source, stored);
        if (found && stored.size() != expected_size(*spec)) {
            // Never replace a malformed user file with the game's defaults.
            m_disabled = true;
            return;
        }
        if (found) {
            high_score_table decoded;
            std::string error;
            if (!spec->decode(stored, decoded, error)) {
                m_disabled = true;
                return;
            }
        }
        if (found) restore(*spec, stored, write);
        m_last_snapshot = capture(*spec, read);
        m_initialized = true;

        const fs::path own = own_score_path(m_short_name);
        // Create a local file for a new table, or copy a compatible MAME file
        // into WhittyArcade's own store without modifying the MAME original.
        if (!found || source != own) write_file(own, m_last_snapshot);
        return;
    }

    if (++m_frames_since_check < 60) return;
    m_frames_since_check = 0;
    const std::vector<std::uint8_t> current = capture(*spec, read);
    if (current == m_last_snapshot) return;
    if (write_file(own_score_path(m_short_name), current))
        m_last_snapshot = current;
}

void high_score_runtime::flush(const read_callback& read) {
    const score_spec* spec = find_spec(m_short_name);
    if (!spec || !m_initialized || m_disabled) return;
    const std::vector<std::uint8_t> current = capture(*spec, read);
    if (current != m_last_snapshot &&
        write_file(own_score_path(m_short_name), current))
        m_last_snapshot = current;
}
