// native_title_library.h - the converted Xbox 360 titles, and the games they
// were converted from.
//
// These are not emulated and they are not plugins. Each one is a real native
// executable produced by the manx_xenon recompilation pipeline, and each still
// needs the ORIGINAL game it was converted from: the recompiler turns the
// title's PowerPC code into native code, but the artwork, audio and level data
// are never extracted, so the binary mounts the retail package at runtime. A
// converted title without its package starts and then dies without saying why,
// which is exactly the failure this file exists to make impossible - the pair
// is discovered together or the title is refused together.
//
// The pairing is not guessed. The recompilation workbench already records, for
// every title it has converted, where the binary is and which owned package it
// came from; that record is the source of truth and is read rather than
// duplicated. Retail data stays where it is and is never copied into MANX.
#pragma once

#include <string>
#include <string_view>
#include <vector>

// One converted title that is actually runnable right now.
struct native_title {
    std::string short_name;   // key for scores, artwork and the catalogue
    std::string display_name;
    std::string title_id;     // the Xbox 360 title id, as hex
    std::string binary_path;  // the converted native executable
    // The original game. Exactly one of these is set: a signed STFS package is
    // handed to the runtime whole, while an extracted title is handed its
    // default.xex and the directory its data sits in.
    std::string package_path;
    std::string xex_path;
    std::string game_root;
    // What the workbench thinks of it - "completed", "revisit", "queued". Kept
    // rather than reduced to a flag because "runnable" and "finished" are not
    // the same claim, and the launcher should not imply the stronger one.
    std::string status;

    bool packaged() const noexcept { return !package_path.empty(); }
};

// A title that could not be offered, and the reason in words a person can act
// on. Reported rather than skipped: a converted game that silently fails to
// appear cannot be told from one that was never converted.
struct rejected_title {
    std::string short_name;
    std::string reason;
};

class native_title_library {
public:
    // Reads a workbench titles.json. Missing file is not an error - it means no
    // conversions are installed - but a malformed one is reported.
    void scan(const std::string& catalog_path);

    const std::vector<native_title>& titles() const noexcept { return m_titles; }
    const std::vector<rejected_title>& rejected() const noexcept {
        return m_rejected;
    }

    const native_title* find(std::string_view short_name) const noexcept;

private:
    void consider(const std::string& slug, const std::string& name,
                  const std::string& title_id, const std::string& binary,
                  const std::string& source, const std::string& status);

    std::vector<native_title> m_titles;
    std::vector<rejected_title> m_rejected;
};

// What has been taken out of a title's own game and written beside it.
//
// A recompiled title runs from the owned package, so an import is not needed to
// PLAY it - which is exactly why it needs saying somewhere. Otherwise there is
// no way to tell a title whose assets have been pulled out from one that has
// never been imported: both play identically.
struct imported_assets {
    std::size_t sounds{};
    std::size_t artwork{};

    bool any() const noexcept { return sounds != 0 || artwork != 0; }
};

// Counts what is in <data root>/MANX/games/<short name>. Cheap enough
// to call while building a menu.
imported_assets imported_assets_for(const std::string& short_name);

// Where the workbench's title catalogue is looked for, most specific first, so
// a failure can name every path that was tried. MANX_XBOX_TITLES overrides.
std::vector<std::string> native_title_catalog_candidates();

// The first candidate that exists, or empty.
std::string find_native_title_catalog();
