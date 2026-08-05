// Reading a game's audio bank, and wrapping its sounds so a decoder accepts
// them.
//
// Both halves fail silently when they are wrong. A misread bank yields a
// payload offset that still points somewhere, so the "sound" decodes to noise
// or to nothing rather than to an error; a wrong RIFF header makes the decoder
// reject packets with a message about the data rather than about the header.
// Each check here pins a mistake that was actually made getting this working.

#include "xbox360_asset_import.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void put_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

void put_tag(std::vector<uint8_t>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
}

uint32_t read_le32(const std::vector<uint8_t>& d, std::size_t at) {
    return static_cast<uint32_t>(d[at]) |
           (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) |
           (static_cast<uint32_t>(d[at + 3]) << 24);
}

uint16_t read_le16(const std::vector<uint8_t>& d, std::size_t at) {
    return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}

// Builds a WAVE record of `record_size` bytes describing one sound.
void append_record(std::vector<uint8_t>& bank, const std::string& name,
                   uint32_t offset, uint32_t size, uint32_t rate,
                   uint32_t channels, uint32_t record_size = 0x5c) {
    const std::size_t start = bank.size();
    bank.resize(start + record_size, 0);
    std::memcpy(&bank[start], "WAVE", 4);
    bank[start + 4] = static_cast<uint8_t>((record_size >> 24) & 0xff);
    bank[start + 5] = static_cast<uint8_t>((record_size >> 16) & 0xff);
    bank[start + 6] = static_cast<uint8_t>((record_size >> 8) & 0xff);
    bank[start + 7] = static_cast<uint8_t>(record_size & 0xff);
    std::memcpy(&bank[start + 0x0c], name.data(),
                std::min<std::size_t>(name.size(), 0x1f));
    const auto be_at = [&](std::size_t at, uint32_t value) {
        bank[start + at + 0] = static_cast<uint8_t>((value >> 24) & 0xff);
        bank[start + at + 1] = static_cast<uint8_t>((value >> 16) & 0xff);
        bank[start + at + 2] = static_cast<uint8_t>((value >> 8) & 0xff);
        bank[start + at + 3] = static_cast<uint8_t>(value & 0xff);
    };
    if (record_size >= 0x5c) {
        be_at(0x2c, offset);
        be_at(0x30, size);
        be_at(0x3c, rate);
        be_at(0x40, 16);
        be_at(0x44, channels);
    }
}

std::vector<uint8_t> build_bank() {
    std::vector<uint8_t> bank;
    put_tag(bank, "BANK");
    put_be32(bank, 12); // header size: the table starts here
    put_be32(bank, 0);
    // Payloads are appended after the table; offsets are patched below.
    append_record(bank, "Fire_normal", 0, 4096, 48000, 1);
    // A short record between two good ones. This is the shape that truncated a
    // real bank from 104 sounds to 6: stopping here loses everything after.
    append_record(bank, "Odd_one_out", 0, 0, 0, 0, 0x50);
    append_record(bank, "Fire_smartbomb", 0, 8192, 48000, 2);
    // A record pointing outside the file: it must not become a sound.
    append_record(bank, "Truncated", 0x7fffff00, 4096, 48000, 1);

    const uint32_t payload_start = static_cast<uint32_t>(bank.size());
    bank.resize(payload_start + 4096 + 8192, 0x5a);
    // Patch the two good offsets now that the payloads have somewhere to live.
    const auto patch = [&](std::size_t record_index, uint32_t value) {
        // Records are laid out in order from offset 12.
        std::size_t at = 12;
        for (std::size_t i = 0; i < record_index; ++i) {
            const uint32_t size = (static_cast<uint32_t>(bank[at + 4]) << 24) |
                                  (static_cast<uint32_t>(bank[at + 5]) << 16) |
                                  (static_cast<uint32_t>(bank[at + 6]) << 8) |
                                  static_cast<uint32_t>(bank[at + 7]);
            at += size;
        }
        bank[at + 0x2c] = static_cast<uint8_t>((value >> 24) & 0xff);
        bank[at + 0x2d] = static_cast<uint8_t>((value >> 16) & 0xff);
        bank[at + 0x2e] = static_cast<uint8_t>((value >> 8) & 0xff);
        bank[at + 0x2f] = static_cast<uint8_t>(value & 0xff);
    };
    patch(0, payload_start);
    patch(2, payload_start + 4096);
    return bank;
}

void test_a_bank_yields_its_named_sounds() {
    std::string error;
    const std::vector<bank_sound> sounds = read_audio_bank(build_bank(), error);
    // Two good sounds. The short record is stepped over, and the one whose
    // payload lies outside the file is dropped.
    assert(sounds.size() == 2);
    assert(sounds[0].name == "Fire_normal");
    assert(sounds[0].size == 4096);
    assert(sounds[0].channels == 1);
    assert(sounds[1].name == "Fire_smartbomb" &&
           "a short record must be stepped over, not stopped at - doing the "
           "latter lost 98 of 104 sounds in a real bank");
    assert(sounds[1].channels == 2);
    assert(sounds[1].sample_rate == 48000);
}

void test_rubbish_is_not_a_bank() {
    std::string error;
    std::vector<uint8_t> nonsense(64, 0x11);
    assert(read_audio_bank(nonsense, error).empty());
    assert(!error.empty());
    std::vector<uint8_t> empty;
    assert(read_audio_bank(empty, error).empty());
}

void test_the_wrapper_declares_one_stream_whatever_the_channels() {
    // The mistake that cost the most: passing the channel count as the STREAM
    // count. Mono sounds still decoded perfectly, so it looked like particular
    // sounds were corrupt rather than like the header being wrong.
    const std::vector<uint8_t> payload(4096, 0x33);
    for (const uint32_t channels : {1u, 2u}) {
        const std::vector<uint8_t> riff =
            wrap_xma2(payload.data(), static_cast<uint32_t>(payload.size()),
                      channels, 48000);
        assert(std::memcmp(riff.data(), "RIFF", 4) == 0);
        assert(std::memcmp(riff.data() + 8, "WAVE", 4) == 0);
        assert(std::memcmp(riff.data() + 12, "fmt ", 4) == 0);

        const std::size_t fmt = 20; // after RIFF/size/WAVE/'fmt '/size
        assert(read_le16(riff, fmt + 0) == 0x166 && "must declare XMA2");
        assert(read_le16(riff, fmt + 2) == channels);
        assert(read_le32(riff, fmt + 4) == 48000);
        assert(read_le16(riff, fmt + 12) == 2048 && "XMA2 packets are 2048");

        const std::size_t extra = fmt + 18;
        assert(read_le16(riff, extra + 0) == 1 &&
               "NumStreams is always 1 - a stream carries up to two channels");
        // And the channel mask has to agree with the channel count, or the
        // decoder reports "requires channel layout to be set".
        assert(read_le32(riff, extra + 2) == (channels == 1 ? 0x4u : 0x3u));

        // The payload must survive intact; a wrapper that dropped or padded it
        // would decode to noise from the first packet.
        const std::size_t data_tag = riff.size() - payload.size() - 8;
        assert(std::memcmp(&riff[data_tag], "data", 4) == 0);
        assert(read_le32(riff, data_tag + 4) == payload.size());
        assert(std::memcmp(&riff[data_tag + 8], payload.data(),
                           payload.size()) == 0);
    }
}

void test_cues_map_onto_the_games_own_names() {
    std::string error;
    const std::vector<bank_sound> sounds = read_audio_bank(build_bank(), error);
    assert(sound_for_cue("shot", sounds) == "Fire_normal");
    assert(sound_for_cue("bomb", sounds) == "Fire_smartbomb");
    // A cue with nothing to match must say so rather than pick something
    // arbitrary - a wrong sound is harder to notice than a missing one.
    assert(sound_for_cue("black_hole_collapse", sounds).empty());
    assert(sound_for_cue("nonsense_cue", sounds).empty());

    // And a cue named exactly as the game names a sound resolves without
    // needing an entry in the table.
    assert(sound_for_cue("Fire_normal", sounds) == "Fire_normal");
}

// Builds an XACT wave bank: header, five segment pointers, bank data, entry
// metadata, then the samples.
std::vector<uint8_t> build_wave_bank(uint32_t count, uint32_t bytes_each) {
    constexpr uint32_t data_offset = 0x30;
    constexpr uint32_t meta_offset = 0x90;
    const uint32_t meta_length = count * 24;
    const uint32_t wave_offset = meta_offset + meta_length;

    std::vector<uint8_t> bank(wave_offset + count * bytes_each, 0);
    std::memcpy(&bank[0], "DNBW", 4); // "WBND" byte-swapped: big-endian
    const auto be_at = [&](std::size_t at, uint32_t v) {
        bank[at + 0] = static_cast<uint8_t>((v >> 24) & 0xff);
        bank[at + 1] = static_cast<uint8_t>((v >> 16) & 0xff);
        bank[at + 2] = static_cast<uint8_t>((v >> 8) & 0xff);
        bank[at + 3] = static_cast<uint8_t>(v & 0xff);
    };
    be_at(4, 23); // version
    be_at(8 + 0 * 8, data_offset);
    be_at(8 + 0 * 8 + 4, 96);
    be_at(8 + 1 * 8, meta_offset);
    be_at(8 + 1 * 8 + 4, meta_length);
    be_at(8 + 4 * 8, wave_offset);
    be_at(8 + 4 * 8 + 4, count * bytes_each);
    be_at(data_offset + 4, count);       // entry count
    be_at(data_offset + 72, 24);         // metadata element size

    // Packed format: tag 1 bit (0 = PCM), channels 3, rate 18.
    const uint32_t format = (1u << 1) | (48000u << 4);
    for (uint32_t i = 0; i < count; ++i) {
        const std::size_t at = meta_offset + i * 24;
        be_at(at + 4, format);
        be_at(at + 8, i * bytes_each); // play offset, from the wave segment
        be_at(at + 12, bytes_each);
    }
    // Distinct big-endian sample values so a misread is visible.
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t b = 0; b < bytes_each; b += 2) {
            bank[wave_offset + i * bytes_each + b] = 0x12;
            bank[wave_offset + i * bytes_each + b + 1] =
                static_cast<uint8_t>(i + 1);
        }
    return bank;
}

std::vector<uint8_t> build_string_table(
    const std::vector<std::string>& before,
    const std::vector<std::string>& sounds,
    const std::vector<std::string>& after) {
    std::vector<uint8_t> table;
    const auto add = [&](const std::string& text) {
        table.insert(table.end(), text.begin(), text.end());
        table.push_back(0);
    };
    for (const std::string& text : before) add(text);
    for (const std::string& text : sounds) add(text);
    for (const std::string& text : after) add(text);
    return table;
}

void test_a_wave_bank_yields_pcm_entries() {
    std::string error;
    const std::vector<bank_sound> sounds =
        read_wave_bank(build_wave_bank(4, 64), error);
    assert(sounds.size() == 4);
    for (const bank_sound& sound : sounds) {
        // Reading the format tag as two bits - which later XACT versions use -
        // turns PCM into "ADPCM" and halves the rate, giving audio that plays
        // but sounds wrong rather than an error.
        assert(sound.codec == sound_codec::pcm_be);
        assert(sound.sample_rate == 48000);
        assert(sound.channels == 1);
        assert(sound.size == 64);
    }
    // Offsets must be absolute, not relative to the wave segment, or every
    // sound is read from the wrong place.
    assert(sounds[1].offset == sounds[0].offset + 64);

    std::string bad;
    std::vector<uint8_t> nonsense(64, 0x22);
    assert(read_wave_bank(nonsense, bad).empty());
}

void test_names_align_with_entries_not_with_the_table() {
    // THE bug this exists for. The sound names sit in the middle of a much
    // larger string table. Several windows that start before the real run
    // contain every recognised name and score identically, so choosing the
    // first shifts every name earlier than the sound it belongs to - which
    // silently gave "Ship_explode" the entry holding silence.
    const std::vector<std::string> sounds = {
        "bullet_hitwall", "Enemy_explode", "Fire_normal", "Fire_smartbomb",
        "Game_over",      "Shield_on",     "Ship_explode", "silence"};
    const std::vector<uint8_t> table = build_string_table(
        {"ONEX", "Object", "GW1_GameObject", "Data", "Components", "Constant"},
        sounds, {"Variable", "Zero", "Half", "Quarter"});

    std::string error;
    std::vector<bank_sound> entries =
        read_wave_bank(build_wave_bank(static_cast<uint32_t>(sounds.size()), 32),
                       error);
    assert(entries.size() == sounds.size());
    assert(name_bank_entries(table, entries));
    for (std::size_t i = 0; i < sounds.size(); ++i)
        assert(entries[i].name == sounds[i] &&
               "names shifted: every sound would be imported under the wrong "
               "name, and the last one is silence");
    assert(sound_for_cue("player_death", entries) == "Ship_explode");
}

void test_a_table_with_no_recognisable_names_is_refused() {
    // Naming from the wrong window attaches real names to the wrong sounds,
    // which is worse than leaving them unnamed - so no match means refuse.
    const std::vector<uint8_t> table = build_string_table(
        {}, {"alpha_one", "beta_two", "gamma_three", "delta_four"}, {});
    std::string error;
    std::vector<bank_sound> entries = read_wave_bank(build_wave_bank(4, 32), error);
    assert(!name_bank_entries(table, entries));
    for (const bank_sound& entry : entries) assert(entry.name.empty());
}

void test_pcm_is_byte_swapped_into_a_playable_wav() {
    // The console stores samples big-endian. Writing them through unswapped
    // produces a file that plays at full volume as noise - loud, and obviously
    // wrong only once you hear it.
    const uint8_t payload[8] = {0x12, 0x34, 0x00, 0x01, 0xff, 0xfe, 0x7f, 0xff};
    const std::vector<uint8_t> wav = pcm_be_to_wav(payload, sizeof(payload), 1,
                                                   48000);
    assert(std::memcmp(wav.data(), "RIFF", 4) == 0);
    const std::size_t fmt = 20;
    assert(read_le16(wav, fmt + 0) == 1 && "must declare plain PCM");
    assert(read_le16(wav, fmt + 2) == 1);
    assert(read_le32(wav, fmt + 4) == 48000);
    assert(read_le16(wav, fmt + 14) == 16);

    const std::size_t data = wav.size() - sizeof(payload);
    assert(wav[data + 0] == 0x34 && wav[data + 1] == 0x12);
    assert(wav[data + 2] == 0x01 && wav[data + 3] == 0x00);
    assert(wav[data + 6] == 0xff && wav[data + 7] == 0x7f);
}

} // namespace

int main() {
    test_a_bank_yields_its_named_sounds();
    test_rubbish_is_not_a_bank();
    test_the_wrapper_declares_one_stream_whatever_the_channels();
    test_cues_map_onto_the_games_own_names();
    test_a_wave_bank_yields_pcm_entries();
    test_names_align_with_entries_not_with_the_table();
    test_a_table_with_no_recognisable_names_is_refused();
    test_pcm_is_byte_swapped_into_a_playable_wav();
    std::printf("xbox360_asset_import_test: all checks passed\n");
    return 0;
}
