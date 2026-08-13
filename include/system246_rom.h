// Namco System 246 ROM/media discovery for Ridge Racer V: Arcade Battle.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class system246_rom_set : uint8_t {
    unknown,
    ridge_racer_v_arcade_battle,
    motogp,
};

enum class system246_disc_container : uint8_t {
    unknown,
    hard_disk_chd,
    cdrom_chd,
};

struct system246_disc_info {
    system246_disc_container container{system246_disc_container::unknown};
    uint64_t logical_bytes{};
    uint32_t hunk_bytes{};
    uint32_t unit_bytes{};
    std::string sha1;
    std::string metadata_tag;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

struct system246_rom_load_result {
    system246_rom_set set{system246_rom_set::unknown};
    std::string dongle_archive;
    std::string disc_path;
    std::vector<uint8_t> dongle;
    system246_disc_info disc;
    std::string error;

    explicit operator bool() const {
        return error.empty() && set != system246_rom_set::unknown &&
               !dongle.empty() && static_cast<bool>(disc);
    }
};

class system246_rom_loader {
public:
    // Identification uses the MAME entry's exact basename, size and CRC.
    // Archive filenames and internal directories are deliberately ignored.
    static system246_rom_set identify_set(const std::string& path);
    static const char* set_short_name(system246_rom_set set);
    static const char* set_display_name(system246_rom_set set);

    // Finds rrv1-a.chd either beside the selected ZIP or in the conventional
    // MAME software directory (rrvac/rrv1-a.chd).
    static std::string find_disc_path(
        const std::string& selected_path,
        const std::string& configured_chd_directory = {});
    static system246_disc_info inspect_disc(const std::string& path);
    static system246_rom_load_result load(
        const std::string& path,
        const std::string& configured_chd_directory = {});

    // System 246/256 games boot the PCSX2 arcade core through a small
    // "<name>.acgame" manifest that names the ELF loader, dongle image and
    // disc/HDD media in the same directory. A game is "ready" when that
    // manifest and every file it references exist beside it; this lets both
    // RRV and MotoGP be marked ready without hard-coding a single game's
    // file names. When it returns false and missing is non-null, missing
    // names the first absent item.
    static bool acgame_ready(const std::string& path,
                             std::string* missing = nullptr);

    // Like acgame_ready, but checks only the in-pack files that determine a
    // squashfs cache can boot: the elf and the media (mediasrc/card). The
    // dongle is centralized in the memcards directory (never beside a squashfs
    // manifest), so it is excluded. A squashfs cache is only "ready" when these
    // exist, so a partial unpack (missing the big media CHD) re-extracts.
    static bool acgame_pack_ready(const std::string& path,
                                  std::string* missing = nullptr);

    // The game's short name: the lowercased basename of its ".acgame" manifest
    // (e.g. "tekken5"), resolving a directory selection to the manifest inside
    // it. Empty when the selection has no manifest. This is what lets ANY
    // collection title be identified as a System 246/256 game without a
    // built-in enum entry -- identify_set only knows the curated sets, but the
    // board boots any manifest, so routing keys off this instead.
    static std::string acgame_short_name(const std::string& path);

    // True when a collection game seats two players at once (Time Crisis'
    // second gun, a fighting game's second panel). These games carry no
    // catalog entry - the board boots any manifest, which is what lets the
    // whole collection work with no per-title code - so the two-player launch
    // options have to be answered from here instead of from a manifest.
    static bool acgame_two_player(const std::string& short_name);

    // ---- Squashfs (packed collection) support ---------------------------
    //
    // The Batocera/Namco System 246/256 packs ship each game as a single
    // .squashfs image containing its <name>.acgame manifest plus the ELF
    // loader, dongle image and disc/HDD media. MANX keeps these images
    // squashed on disk (30 GB across the set) and only unpacks one to a
    // per-game cache directory on game selection.

    // True when a path names a .squashfs image (case-insensitive).
    static bool is_squashfs(const std::string& path);

    // The display name of a squashed game, read from its <name>.acgame
    // manifest's "name =" line via `unsquashfs -cat` (no full extraction).
    // Falls back to the image stem when the manifest cannot be read.
    static std::string squashfs_game_name(const std::string& squashfs_path);

    // True when the squashed game has a <name>.acgame manifest and is a
    // System 246/256 title, so a pack that is not the target platform can be
    // skipped during discovery.
    static bool squashfs_is_system246(const std::string& squashfs_path);

    // Deterministic per-game cache directory for an unpacked image
    // (e.g. <data_root>/MANX/squashfs/<image-stem>/). Empty on failure.
    static std::string squashfs_cache_dir(const std::string& squashfs_path);

    // Extract one squashed game into its cache directory (unsquashfs -f).
    // Returns the path of the unpacked <name>.acgame manifest, or empty if
    // the image could not be unpacked or has no manifest. Idempotent: if the
    // cached copy is already present it is reused. When `progress` is given it
    // is called with a fraction in [0,1] as extraction advances, so a caller
    // can show a real progress bar instead of a blank wait.
    using squashfs_progress_fn =
        void (*)(float fraction, void* user);
    static std::string squashfs_unpack(const std::string& squashfs_path,
                                       std::string* error = nullptr,
                                       squashfs_progress_fn progress = nullptr,
                                       void* progress_user = nullptr);

    // Path of an already-unpacked game's <name>.acgame manifest in the cache,
    // or empty when the game is not yet unpacked (so a caller can avoid a
    // redundant extract when the boot path already resolves).
    static std::string squashfs_cached_acgame(const std::string& squashfs_path);
};
