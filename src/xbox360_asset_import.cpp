#include "xbox360_asset_import.h"

#include "xbox360/stfs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

uint32_t be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void put_le32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

void put_le16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void put_tag(std::vector<uint8_t>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return value;
}

// Quotes an argument for the shell. Every path here comes from a package or a
// user-chosen directory and habitually contains spaces and apostrophes; getting
// this wrong turns one file into several arguments.
std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
}

bool write_file(const fs::path& path, const uint8_t* data, std::size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

} // namespace

std::vector<bank_sound> read_audio_bank(const std::vector<uint8_t>& bank,
                                        std::string& error) {
    std::vector<bank_sound> sounds;
    if (bank.size() < 8 || std::memcmp(bank.data(), "BANK", 4) != 0) {
        error = "not an audio bank";
        return sounds;
    }
    std::size_t offset = be32(bank.data() + 4);
    while (offset + 8 <= bank.size() &&
           std::memcmp(bank.data() + offset, "WAVE", 4) == 0) {
        const uint32_t record = be32(bank.data() + offset + 4);
        // Records are NOT all the same length - most are 92 bytes but some are
        // 80, and requiring the longer form stopped the walk at the seventh
        // sound of a hundred and four, silently losing everything after it.
        // 0x48 is where the last field this reads ends.
        constexpr uint32_t full_record = 0x5c;
        if (record < 0x48 || offset + record > bank.size()) break;
        // Short records exist and must be STEPPED OVER, not stopped at: doing
        // the latter lost 98 of 104 sounds. Their fields are laid out
        // differently, so they are skipped rather than misread.
        if (record < full_record) {
            offset += record;
            continue;
        }
        const uint8_t* p = bank.data() + offset;

        bank_sound sound;
        // The name is a fixed 32-byte field, null padded.
        const char* name = reinterpret_cast<const char*>(p + 0x0c);
        const std::size_t length = ::strnlen(name, 0x20);
        sound.name.assign(name, length);
        sound.offset = be32(p + 0x2c);
        sound.size = be32(p + 0x30);
        sound.sample_rate = be32(p + 0x3c);
        sound.bits = be32(p + 0x40);
        sound.channels = be32(p + 0x44);
        if (sound.sample_rate == 0) sound.sample_rate = 48000;
        if (sound.channels == 0 || sound.channels > 2) sound.channels = 1;

        // A payload that does not lie inside the file is a sound that cannot be
        // decoded; keeping it would produce a silent or corrupt WAV later, far
        // from the cause.
        const bool inside = sound.size > 0 &&
                            static_cast<std::size_t>(sound.offset) +
                                    sound.size <= bank.size();
        if (!sound.name.empty() && inside) sounds.push_back(std::move(sound));
        offset += record;
    }
    if (sounds.empty()) error = "bank contains no readable sounds";
    return sounds;
}

std::vector<bank_sound> read_wave_bank(const std::vector<uint8_t>& bank,
                                       std::string& error) {
    std::vector<bank_sound> sounds;
    // "DNBW" is "WBND" with the bytes swapped: an Xbox 360 bank is big-endian
    // throughout, which is also why every field below is read that way.
    if (bank.size() < 0x30 || std::memcmp(bank.data(), "DNBW", 4) != 0) {
        error = "not an XACT wave bank";
        return sounds;
    }
    // Five segments follow the header: bank data, entry metadata, seek tables,
    // entry names, wave data. WHERE they start depends on the bank's version -
    // later ones carry a header version too, pushing the table on by four
    // bytes. Rather than keep a table of version numbers, both positions are
    // tried and the one whose wave segment ends exactly at the end of the file
    // is the right one. A wrong guess otherwise yields an entry count in the
    // hundreds of millions and a metadata element size of zero.
    std::size_t segment_table = 0;
    for (const std::size_t candidate : {std::size_t{8}, std::size_t{12}}) {
        if (candidate + 5 * 8 > bank.size()) continue;
        const uint32_t wave_at = be32(bank.data() + candidate + 4 * 8);
        const uint32_t wave_length = be32(bank.data() + candidate + 4 * 8 + 4);
        if (static_cast<std::size_t>(wave_at) + wave_length == bank.size()) {
            segment_table = candidate;
            break;
        }
    }
    if (segment_table == 0) {
        error = "wave bank header is not a layout this understands";
        return sounds;
    }
    const uint32_t data_offset = be32(bank.data() + segment_table + 0 * 8);
    const uint32_t meta_offset = be32(bank.data() + segment_table + 1 * 8);
    const uint32_t meta_length = be32(bank.data() + segment_table + 1 * 8 + 4);
    const uint32_t wave_offset = be32(bank.data() + segment_table + 4 * 8);

    if (data_offset + 88 > bank.size()) {
        error = "wave bank header is truncated";
        return sounds;
    }
    const uint32_t count = be32(bank.data() + data_offset + 4);
    const uint32_t element = be32(bank.data() + data_offset + 72);
    if (count == 0 || element < 24 ||
        static_cast<std::size_t>(meta_offset) + meta_length > bank.size() ||
        meta_length < static_cast<uint64_t>(count) * element) {
        error = "wave bank entry table is truncated";
        return sounds;
    }

    // The packed format word's TAG IS NOT A FIXED WIDTH. Early banks give it
    // one bit, later ones two, and every other field shifts with it - so
    // reading a v45 bank with the v23 layout reports 4 channels at 88200 Hz
    // where the truth is 2 at 44100, and reading a v23 bank the other way turns
    // PCM into "ADPCM" at half the rate. Neither is an error; both produce
    // audio that plays and is wrong.
    //
    // The BANK'S VERSION decides, because guessing cannot. The two layouts
    // differ by one bit, so every rate one of them reports is exactly double or
    // half what the other does - and 48000/24000, 44100/22050, 32000/16000 and
    // 16000/8000 are all rates a console really uses. A "pick whichever looks
    // plausible" rule is therefore ambiguous for every bank in existence, and
    // wrong half the time without ever looking wrong.
    //
    // The version does NOT decide it either: Jetpac Refuelled's bank is
    // version 42 and needs two bits, while Space Giraffe's is 43 and needs one.
    // So the whole table is scored both ways and the reading that describes
    // more entries as real audio wins - a channel count of 1, 2 or 6, and a
    // sample rate a console actually uses. A wrong layout shifts BOTH fields,
    // so it shows up as four-channel 88200 Hz where the truth is stereo 44100.
    //
    // The rate alone cannot settle it, because the two layouts always differ by
    // exactly one bit and so always report rates a factor of two apart - and
    // 48000/24000, 44100/22050 and 16000/8000 are all real rates. The channel
    // count is what breaks the tie in practice.
    const auto score_layout = [&](uint32_t width) {
        int score = 0;
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            const uint32_t format = be32(bank.data() + meta_offset +
                                         i * element + 4);
            const uint32_t channels = (format >> width) & 0x7u;
            const uint32_t rate = (format >> (width + 3)) & 0x3FFFFu;
            if (channels == 1 || channels == 2 || channels == 6) ++score;
            if (rate == 48000 || rate == 44100 || rate == 32000 ||
                rate == 24000 || rate == 22050 || rate == 16000 ||
                rate == 11025 || rate == 8000)
                ++score;
        }
        return score;
    };
    // A tie goes to one bit: that is the older and more common form, and the
    // only bank that ties (Space Giraffe) is one of them.
    const uint32_t tag_bits = score_layout(2u) > score_layout(1u) ? 2u : 1u;
    const auto decode_format = [tag_bits](uint32_t format, uint32_t& tag,
                                          uint32_t& channels, uint32_t& rate) {
        tag = format & ((1u << tag_bits) - 1u);
        channels = (format >> tag_bits) & 0x7u;
        rate = (format >> (tag_bits + 3)) & 0x3FFFFu;
        return rate != 0;
    };

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = bank.data() + meta_offset + i * element;
        const uint32_t format = be32(entry + 4);
        const uint32_t play_offset = be32(entry + 8);
        const uint32_t play_length = be32(entry + 12);

        uint32_t tag = 0;
        uint32_t channels = 0;
        uint32_t rate = 0;
        if (!decode_format(format, tag, channels, rate)) continue;
        if (channels == 0) channels = 1;
        // 0 is uncompressed, 1 is XMA. Anything else (ADPCM, WMA) would need a
        // decoder this does not have, and is skipped rather than mis-decoded.
        if (tag != 0 && tag != 1) continue;

        bank_sound sound;
        sound.offset = wave_offset + play_offset;
        sound.size = play_length;
        sound.sample_rate = rate;
        sound.bits = 16;
        sound.channels = channels;
        sound.codec = tag == 0 ? sound_codec::pcm_be : sound_codec::xma2;
        if (sound.size == 0 ||
            static_cast<std::size_t>(sound.offset) + sound.size > bank.size())
            continue;
        sounds.push_back(std::move(sound));
    }
    if (sounds.empty()) error = "wave bank holds no readable entries";
    return sounds;
}

bool name_bank_entries(const std::vector<uint8_t>& string_table,
                       std::vector<bank_sound>& entries) {
    if (entries.empty()) return false;

    // Every printable, name-shaped, NUL-terminated string in the file, in order.
    std::vector<std::string> strings;
    std::string current;
    for (const uint8_t byte : string_table) {
        if (byte >= 0x20 && byte < 0x7f) {
            current.push_back(static_cast<char>(byte));
            continue;
        }
        if (byte == 0 && current.size() >= 3 && current.size() <= 31) {
            const bool name_shaped =
                std::all_of(current.begin(), current.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) ||
                           c == '_';
                });
            if (name_shaped) strings.push_back(current);
        }
        current.clear();
    }
    if (strings.size() < entries.size()) return false;

    // Slide a window the size of the bank over them and keep the one holding
    // the most names this importer knows how to use. A window that recognises
    // nothing is not the sound table, and naming from it would attach real
    // names to the wrong sounds - worse than leaving them unnamed.
    static const char* known[] = {
        "Fire_normal", "Enemy_explode", "Ship_explode", "Fire_smartbomb",
        "Game_over",   "Game_start",    "bullet_hitwall", "Shield_on",
    };
    std::size_t best_start = 0;
    int best_score = 0;
    for (std::size_t start = 0; start + entries.size() <= strings.size();
         ++start) {
        int score = 0;
        for (std::size_t i = 0; i < entries.size(); ++i)
            for (const char* candidate : known)
                if (lower(strings[start + i]) == lower(candidate)) ++score;
        // The LATEST best window wins, not the first. The sound names sit in
        // the middle of a much larger table, so every window that starts a
        // little before the real run still contains all the recognised names
        // and scores identically - and taking the first of those shifted every
        // name earlier than the sound it belonged to. Here that put
        // "Ship_explode" on the entry holding silence, which imported as a
        // death sound that could not be heard. Shifting later always drops a
        // name and lowers the score, so the last best start is the real one.
        if (score >= best_score) {
            best_score = score;
            best_start = start;
        }
    }
    if (best_score == 0) return false;
    for (std::size_t i = 0; i < entries.size(); ++i)
        entries[i].name = strings[best_start + i];
    return true;
}

std::vector<uint8_t> pcm_be_to_wav(const uint8_t* payload, uint32_t size,
                                   uint32_t channels, uint32_t sample_rate) {
    if (channels == 0) channels = 1;
    const uint32_t samples = size & ~1u; // whole 16-bit samples only

    std::vector<uint8_t> fmt;
    put_le16(fmt, 1);             // WAVE_FORMAT_PCM
    put_le16(fmt, static_cast<uint16_t>(channels));
    put_le32(fmt, sample_rate);
    put_le32(fmt, sample_rate * channels * 2u);
    put_le16(fmt, static_cast<uint16_t>(channels * 2u));
    put_le16(fmt, 16);

    std::vector<uint8_t> body;
    put_tag(body, "WAVE");
    put_tag(body, "fmt ");
    put_le32(body, static_cast<uint32_t>(fmt.size()));
    body.insert(body.end(), fmt.begin(), fmt.end());
    put_tag(body, "data");
    put_le32(body, samples);
    // The console stores samples big-endian; every host that will play this
    // wants them the other way round.
    for (uint32_t i = 0; i + 1 < samples; i += 2) {
        body.push_back(payload[i + 1]);
        body.push_back(payload[i]);
    }

    std::vector<uint8_t> file;
    put_tag(file, "RIFF");
    put_le32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

std::vector<uint8_t> wrap_xma2(const uint8_t* payload, uint32_t size,
                               uint32_t channels, uint32_t sample_rate) {
    // XMA2 packets are always this size, independent of the sound.
    constexpr uint16_t packet_bytes = 2048;
    if (channels == 0) channels = 1;

    // The channel mask must agree with the channel count. A stereo mask on a
    // mono stream is the failure that costs the most time here: the decoder
    // accepts the file, emits one packet and then reports invalid data.
    const uint32_t mask = channels == 1 ? 0x4u   // front centre
                                        : 0x3u;  // front left + right
    const uint32_t samples = size * 4u; // an upper bound; the decoder stops early

    std::vector<uint8_t> extra;
    // Always ONE stream. A stream carries up to two channels, so a stereo sound
    // is one stream of two - not two streams. See the header.
    put_le16(extra, 1);           // NumStreams
    put_le32(extra, mask);
    put_le32(extra, samples);
    put_le32(extra, size);        // BytesPerBlock: the whole payload
    put_le32(extra, 0);           // PlayBegin
    put_le32(extra, samples);     // PlayLength
    put_le32(extra, 0);           // LoopBegin
    put_le32(extra, 0);           // LoopLength
    extra.push_back(0);           // LoopCount
    extra.push_back(4);           // EncoderVersion
    put_le16(extra, 1);           // BlockCount

    std::vector<uint8_t> fmt;
    put_le16(fmt, 0x166);         // WAVE_FORMAT_XMA2
    put_le16(fmt, static_cast<uint16_t>(channels));
    put_le32(fmt, sample_rate);
    put_le32(fmt, sample_rate * channels * 2u);
    put_le16(fmt, packet_bytes);
    put_le16(fmt, 16);
    put_le16(fmt, static_cast<uint16_t>(extra.size()));
    fmt.insert(fmt.end(), extra.begin(), extra.end());

    std::vector<uint8_t> body;
    put_tag(body, "WAVE");
    put_tag(body, "fmt ");
    put_le32(body, static_cast<uint32_t>(fmt.size()));
    body.insert(body.end(), fmt.begin(), fmt.end());
    put_tag(body, "data");
    put_le32(body, size);
    body.insert(body.end(), payload, payload + size);

    std::vector<uint8_t> file;
    put_tag(file, "RIFF");
    put_le32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

std::string sound_for_cue(const std::string& cue,
                          const std::vector<bank_sound>& sounds) {
    // The game's names and the plugin's cues were chosen independently, so the
    // mapping is stated rather than derived. "Gravity well" is what Retro
    // Evolved 2 calls a black hole - no amount of string matching finds that.
    struct wanted { const char* cue; const char* names[6]; };
    static const wanted table[] = {
        {"shot", {"Fire_normal", "Fire_2", nullptr}},
        {"enemy_death", {"Enemy_explode", "Enemy_explode_sub", nullptr}},
        {"player_death", {"Ship_explode", "Game_over", nullptr}},
        // Retro Evolved names a spawn after the enemy's colour rather than its
        // kind, so none of the sequel's names appear in it.
        {"spawn",
         {"Wanderer_spawn", "Grunt_spawn", "Enemy_spawn_blue",
          "Enemy_spawn_green", "Player_Spawn", nullptr}},
        {"bomb", {"Fire_smartbomb", "Fire_smartbomb_low", nullptr}},
        // Only the sequel has black holes ("gravity wells"), and only the
        // sequel has a multiplier sound. Finding nothing here is the right
        // answer for Retro Evolved, not a failure - the cue keeps its
        // synthesised placeholder.
        {"black_hole_collapse",
         {"Gravity_well_explode", "Gravity_well_die", nullptr}},
        {"multiplier_up", {"Multiplier", "Pickup_geom", nullptr}},
    };
    for (const wanted& entry : table) {
        if (cue != entry.cue) continue;
        for (const char* candidate : entry.names) {
            if (candidate == nullptr) break;
            for (const bank_sound& sound : sounds)
                if (lower(sound.name) == lower(candidate)) return sound.name;
        }
    }
    // Failing the table, a cue named exactly as the game names a sound.
    for (const bank_sound& sound : sounds)
        if (lower(sound.name) == lower(cue)) return sound.name;
    return {};
}

import_report extract_xbox360_package(const std::string& package_path,
                                      const std::string& directory) {
    import_report report;
    manx_xenon::stfs_package package;
    std::string error;
    if (!package.open(package_path, error)) {
        report.problems.push_back("cannot open " + package_path + ": " + error);
        return report;
    }
    for (const manx_xenon::stfs_entry& entry : package.entries()) {
        if (entry.directory) continue;
        // The package separates with backslashes; on disk that has to become a
        // real directory or the whole tree lands in one file with an odd name.
        std::string relative = entry.path;
        for (char& c : relative)
            if (c == '\\') c = '/';
        const fs::path out = fs::path(directory) / relative;

        std::vector<uint8_t> bytes(static_cast<std::size_t>(entry.size));
        const uint32_t got =
            bytes.empty() ? 0
                          : package.read(entry, bytes.data(),
                                         static_cast<uint32_t>(bytes.size()), 0);
        if (got != bytes.size()) {
            report.problems.push_back("short read on " + entry.path);
            continue;
        }
        if (!write_file(out, bytes.data(), bytes.size())) {
            report.problems.push_back("could not write " + out.string());
            continue;
        }
        report.extracted.push_back(relative);
    }
    return report;
}

import_report import_xbox360_assets(const std::string& package_path,
                                    const std::string& bundle_path,
                                    const std::vector<std::string>& cue_names,
                                    const std::string& decoder) {
    import_report report;
    manx_xenon::stfs_package package;
    std::string error;
    if (!package.open(package_path, error)) {
        report.problems.push_back("cannot open " + package_path + ": " + error);
        return report;
    }

    // Artwork first: it is already in a usable format, needs no decoding, and
    // is what the launcher shows before anyone presses anything.
    for (const manx_xenon::stfs_entry& entry : package.entries()) {
        if (entry.directory) continue;
        const std::string name = lower(entry.name);
        // By extension rather than by a list of known file names: every game
        // names its artwork differently, and a fixed list quietly extracted
        // nothing at all from a title that happened not to use those names.
        const std::string suffix =
            name.size() >= 4 ? name.substr(name.size() - 4) : std::string();
        const bool art = suffix == ".png" || suffix == ".jpg" ||
                         suffix == ".tga" || suffix == ".dds";
        if (!art || entry.size == 0 || entry.size > (32u << 20)) continue;
        std::vector<uint8_t> bytes(static_cast<std::size_t>(entry.size));
        const uint32_t got = package.read(entry, bytes.data(),
                                          static_cast<uint32_t>(bytes.size()), 0);
        const fs::path out = fs::path(bundle_path) / "art" / entry.name;
        if (got == bytes.size() && write_file(out, bytes.data(), got))
            report.extracted.push_back("art/" + entry.name);
        else
            report.problems.push_back("could not write " + out.string());
    }

    // Then the effects. EVERY bank is read and searched, not just the largest:
    // a game splits its sounds across banks by where they are used, so the one
    // with the most sounds is often the menu's, and picking it finds none of
    // the cues a game actually needs.
    struct loaded_bank {
        std::vector<uint8_t> bytes;
        std::vector<bank_sound> sounds;
    };
    std::vector<loaded_bank> banks;
    std::vector<bank_sound> sounds; // every sound, across every bank
    for (const manx_xenon::stfs_entry& entry : package.entries()) {
        if (entry.directory) continue;
        const std::string name = lower(entry.name);
        if (name.size() < 4 || name.substr(name.size() - 4) != ".baf") continue;
        std::vector<uint8_t> candidate(static_cast<std::size_t>(entry.size));
        if (package.read(entry, candidate.data(),
                         static_cast<uint32_t>(candidate.size()), 0) !=
            candidate.size())
            continue;
        std::string bank_error;
        std::vector<bank_sound> found = read_audio_bank(candidate, bank_error);
        if (found.empty()) continue;
        sounds.insert(sounds.end(), found.begin(), found.end());
        banks.push_back(loaded_bank{std::move(candidate), std::move(found)});
    }

    // Wave banks, the other format. Their entries carry no names, so each is
    // paired with a companion string table from the same package - without one
    // the sounds cannot be told apart and are not worth extracting.
    for (const manx_xenon::stfs_entry& entry : package.entries()) {
        if (entry.directory) continue;
        const std::string name = lower(entry.name);
        if (name.size() < 4 || name.substr(name.size() - 4) != ".xwb") continue;
        std::vector<uint8_t> candidate(static_cast<std::size_t>(entry.size));
        if (package.read(entry, candidate.data(),
                         static_cast<uint32_t>(candidate.size()), 0) !=
            candidate.size())
            continue;
        std::string bank_error;
        std::vector<bank_sound> found = read_wave_bank(candidate, bank_error);
        if (found.empty()) continue;

        bool named = false;
        for (const manx_xenon::stfs_entry& other : package.entries()) {
            if (other.directory || other.size == 0 ||
                other.size > (4u << 20))
                continue;
            // Games keep the names in different companions: Retro Evolved in
            // a .dat, a stock XACT project in the .xsb sound bank beside the
            // wave bank. Both are just string tables as far as this is
            // concerned, and a wrong one scores zero and is rejected, so
            // trying several costs nothing.
            const std::string other_name = lower(other.name);
            const std::string suffix = other_name.size() >= 4
                                           ? other_name.substr(
                                                 other_name.size() - 4)
                                           : std::string();
            if (suffix != ".dat" && suffix != ".xsb") continue;
            std::vector<uint8_t> table(static_cast<std::size_t>(other.size));
            if (package.read(other, table.data(),
                             static_cast<uint32_t>(table.size()), 0) !=
                table.size())
                continue;
            if (name_bank_entries(table, found)) {
                named = true;
                break;
            }
        }
        if (!named) {
            // The sounds are still worth having: a bundle full of correctly
            // decoded audio under index names can be listened to and mapped by
            // hand, whereas skipping the bank leaves nothing to work with.
            // Naming them WRONG would be worse than either, which is why the
            // matcher refused rather than guessed.
            for (std::size_t i = 0; i < found.size(); ++i) {
                char label[32];
                std::snprintf(label, sizeof(label), "sound_%03zu", i);
                found[i].name = label;
            }
            report.problems.push_back(
                "no names found for " + entry.name +
                "; its sounds are numbered, not named");
        }
        sounds.insert(sounds.end(), found.begin(), found.end());
        banks.push_back(loaded_bank{std::move(candidate), std::move(found)});
    }
    if (sounds.empty()) {
        report.problems.push_back(
            "no effects bank found in the package; artwork only");
        return report;
    }

    const fs::path staging = fs::path(bundle_path) / "sfx" / ".import";
    std::error_code ec;
    fs::create_directories(staging, ec);

    // With no cue list, every named sound is imported under its own name. That
    // makes an import useful before anything is written to play it: the bundle
    // ends up holding the game's whole effects bank, named as the game names
    // it, and a cue list can be mapped onto that later.
    std::vector<std::string> wanted = cue_names;
    const bool whole_bank = wanted.empty();
    if (whole_bank)
        for (const bank_sound& sound : sounds) wanted.push_back(sound.name);

    for (const std::string& cue : wanted) {
        const std::string chosen =
            whole_bank ? cue : sound_for_cue(cue, sounds);
        if (chosen.empty()) {
            report.problems.push_back("no sound in this game matches cue '" +
                                      cue + "'");
            continue;
        }
        // Find which bank actually holds it - the payload offset is relative
        // to its own bank file, so reading it out of the wrong one produces a
        // WAV of whatever happened to be at that offset.
        const bank_sound* sound = nullptr;
        const loaded_bank* owner = nullptr;
        for (const loaded_bank& candidate : banks) {
            for (const bank_sound& held : candidate.sounds)
                if (held.name == chosen) { sound = &held; owner = &candidate; }
            if (sound != nullptr) break;
        }
        if (sound == nullptr || owner == nullptr) continue;

        // A game's own name becomes a file name, so anything that would escape
        // the folder or upset a file system is replaced rather than trusted.
        std::string leaf;
        for (const char c : cue)
            leaf += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                     c == '-')
                        ? c
                        : '_';
        if (leaf.empty()) continue;
        const fs::path wav = fs::path(bundle_path) / "sfx" / (leaf + ".wav");

        // Uncompressed sounds need no decoder at all - a byte swap and a header
        // and they are finished. Going through the external decoder anyway
        // would make an import fail on a machine without one, for no reason.
        if (sound->codec == sound_codec::pcm_be) {
            const std::vector<uint8_t> finished =
                pcm_be_to_wav(owner->bytes.data() + sound->offset, sound->size,
                              sound->channels, sound->sample_rate);
            if (write_file(wav, finished.data(), finished.size()))
                report.extracted.push_back("sfx/" + leaf + ".wav");
            else
                report.problems.push_back("could not write " + wav.string());
            continue;
        }

        const std::vector<uint8_t> wrapped =
            wrap_xma2(owner->bytes.data() + sound->offset, sound->size,
                      sound->channels, sound->sample_rate);
        const fs::path xma = staging / (leaf + ".xma");
        if (!write_file(xma, wrapped.data(), wrapped.size())) {
            report.problems.push_back("could not stage " + xma.string());
            continue;
        }
        const std::string command = shell_quote(decoder) +
                                    " -y -v error -i " + shell_quote(xma.string()) +
                                    " -f wav -acodec pcm_s16le " +
                                    shell_quote(wav.string());
        const int status = std::system(command.c_str());
        if (status != 0 || !fs::is_regular_file(wav, ec)) {
            report.problems.push_back("could not decode '" + chosen +
                                      "' for cue '" + cue +
                                      "' - is " + decoder + " installed?");
            continue;
        }
        report.extracted.push_back("sfx/" + leaf + ".wav");
    }
    // The wrapped XMA is an intermediate, not an asset.
    fs::remove_all(staging, ec);
    return report;
}
