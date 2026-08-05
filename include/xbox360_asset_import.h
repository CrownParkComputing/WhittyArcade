// xbox360_asset_import.h - taking the assets out of an owned Xbox 360 game.
//
// A converted title's code is recompiled ahead of time, but its artwork and
// audio are not: they stay inside the signed package and are read at runtime.
// That is what ties a converted game to the retail file forever. This turns the
// package into a self-contained bundle instead - import once, and the game runs
// from its own folder with nothing to mount.
//
// WHAT THE AUDIO ACTUALLY IS, because none of it is guessable from the file
// names. The package holds `audio/*.baf` banks in a chunked big-endian format:
//
//   'BANK' <u32 size> ... "GW3"          bank header, named
//   'WAVE' <u32 size> <u32> <32-byte name> ... one per sound, 104 of them
//                     +0x2c u32 file offset of the sound's payload
//                     +0x30 u32 payload size
//                     +0x3c u32 sample rate      +0x40 u32 bits
//                     +0x44 u32 channel count (1 mono, 2 stereo)
//
// Records are not a fixed length: most are 92 bytes, some are 80, and the short
// form lays its fields out differently. Only the long form is trusted here -
// reading a short one with these offsets yields payload sizes that are not a
// whole number of packets, which would decode to noise rather than to nothing.
//   'DATA' <u32 size> <payload>          one per sound, payload is XMA2
//
// The payloads are XMA2 packet streams, 2048 bytes per packet, always a single
// stream carrying one or two channels. They are decoded by wrapping each one in a RIFF header that
// declares format 0x166 and handing it to ffmpeg - verified against an
// independently extracted copy of the same sound, matching to within one step
// of a 16-bit sample.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// How a sound's bytes are stored. Two games from the same studio, two formats:
// the sequel compresses everything, the original does not compress at all.
enum class sound_codec {
    xma2,    // compressed; needs an external decoder
    pcm_be,  // 16-bit big-endian samples, needing only a byte swap
};

// One sound as the bank describes it. `name` is the game's own name for it,
// which is what makes a bank worth reading rather than dumping by index.
struct bank_sound {
    std::string name;
    uint32_t offset{};      // of the payload, from the start of the bank file
    uint32_t size{};
    uint32_t sample_rate{48000};
    uint32_t bits{16};
    // The field at +0x44. It reads like a stream count but is the CHANNEL
    // count: 1 for mono, 2 for stereo. Passing it to the decoder as a stream
    // count makes every stereo sound fail with "requires channel layout to be
    // set", while mono ones decode perfectly - so the mistake looks like a
    // problem with particular sounds rather than with the field.
    uint32_t channels{1};
    sound_codec codec{sound_codec::xma2};
};

// Reads an XACT wave bank (.xwb). Retro Evolved stores its effects this way -
// uncompressed 16-bit big-endian PCM - so nothing external is needed to read
// them, only a byte swap. The bank carries NO names (its name segment is
// empty), so the returned sounds are unnamed and must be named separately.
std::vector<bank_sound> read_wave_bank(const std::vector<uint8_t>& bank,
                                       std::string& error);

// Names unnamed wave-bank entries from a companion string table, in order.
//
// The names live in a separate file next to the bank, mixed in with every other
// string the game kept. The run belonging to the bank is found by looking for
// the window of exactly `entries` consecutive strings containing the most names
// this importer recognises - so a wrong window scores zero and is refused,
// rather than silently naming every sound after something else.
bool name_bank_entries(const std::vector<uint8_t>& string_table,
                       std::vector<bank_sound>& entries);

// Turns one sound's stored bytes into a finished WAV, ready to write. Only
// valid for pcm_be; compressed sounds go through wrap_xma2 and a decoder.
std::vector<uint8_t> pcm_be_to_wav(const uint8_t* payload, uint32_t size,
                                   uint32_t channels, uint32_t sample_rate);

// Reads a .baf bank's table of contents. Returns empty and sets `error` when
// the file is not a bank; a bank whose table walks off the end is truncated
// rather than rejected, so a partly readable one still yields what it has.
std::vector<bank_sound> read_audio_bank(const std::vector<uint8_t>& bank,
                                        std::string& error);

// Wraps one XMA2 payload as a RIFF file that a decoder will accept. Exposed
// because this is the part with no margin for error - a wrong channel mask
// decodes to silence or to noise, with nothing said either way.
std::vector<uint8_t> wrap_xma2(const uint8_t* payload, uint32_t size,
                               uint32_t channels, uint32_t sample_rate);

// What one import produced.
struct import_report {
    std::vector<std::string> extracted;  // bundle-relative paths written
    std::vector<std::string> problems;
    bool ok() const noexcept { return !extracted.empty(); }
};

// Reads `package_path` and writes what a native bundle needs into
// `bundle_path`: `sfx/<cue>.wav` for every cue the game's own names can be
// mapped onto, and `art/` for the title's artwork. Existing files are
// overwritten - an import is the authority on what it produces.
//
// `cue_names` are the cues to fill, in index order. Pass an EMPTY list to
// import the game's whole effects bank instead, each sound written under the
// game's own name - which is how a title is imported before anything exists to
// render it, and how you find out what a game actually contains.
//
// `cue_names` are the plugin's cues, in index order. `decoder` is the command
// used to turn a wrapped XMA file into a WAV, "ffmpeg" by default; import
// reports a problem rather than failing when it is not installed.
import_report import_xbox360_assets(const std::string& package_path,
                                    const std::string& bundle_path,
                                    const std::vector<std::string>& cue_names,
                                    const std::string& decoder = "ffmpeg");

// Unpacks the WHOLE package into `directory`, preserving its internal layout.
//
// This is what standalone needs, and it is a different job from importing the
// pieces a bundle can use. A recompiled title is handed either a signed package
// or a `default.xex` plus the directory its data sits in - so unpacking a
// package produces the second form, and the title then runs from ordinary files
// with nothing signed to mount. It is also the shape an Android build packages,
// where there is no package to mount at all.
//
// Everything is written, not just the art and audio, because the title reads
// its own data by name and there is no way to know from outside which files it
// will want.
import_report extract_xbox360_package(const std::string& package_path,
                                      const std::string& directory);

// The game's own sound name that best serves a plugin cue, or empty. Kept
// separate from the import so the mapping can be tested and argued about
// without a package to hand.
std::string sound_for_cue(const std::string& cue,
                          const std::vector<bank_sound>& sounds);
