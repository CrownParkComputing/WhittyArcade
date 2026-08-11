#include "rr6_cloud_save.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void check(bool condition, const char* description) {
    if (condition) return;
    std::printf("FAIL: %s\n", description);
    ++failures;
}

void set_data_root(const fs::path& root) {
#if defined(_WIN32)
    _putenv_s("XDG_DATA_HOME", root.string().c_str());
#else
    setenv("XDG_DATA_HOME", root.string().c_str(), 1);
#endif
}

void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}
} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                          ("rr6-cloud-save-test-" + std::to_string(stamp));
    set_data_root(root);

    std::vector<uint8_t> bytes(rr6_cloud_save::payload_size);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 37u) & 0xffu);
    const std::string name = "UY^V#]    $lllllllllllllli";
    const fs::path seed = root / "seed" / "Game_Data";
    write_file(seed / name, bytes);
    write_file(seed / "__thumbnail.png", {1, 2, 3});
    write_file(seed / ".thumbnail.png", {4, 5, 6});

    std::string error;
    check(rr6_cloud_save::install_seed(seed, error), "known-good seed installs");
    auto live = rr6_cloud_save::capture_live(&error);
    check(live.has_value(), "installed seed captures as a valid save");
    check(live && live->payload == bytes, "installed bytes are unchanged");
    check(live && live->checksum == rr6_cloud_save::checksum(bytes),
          "save checksum is deterministic");
    check(fs::is_regular_file(rr6_cloud_save::live_directory() /
                              "__thumbnail.png"),
          "seed thumbnail follows the save");
    check(fs::is_regular_file(rr6_cloud_save::live_directory() /
                              ".thumbnail.png"),
          "alternate seed thumbnail follows the save");

    if (live) {
        check(rr6_cloud_save::preserve_last_good(*live, error),
              "valid save is retained locally");
        check(rr6_cloud_save::preserve_last_good(*live, error),
              "replacing an existing last-good snapshot is atomic");
        write_file(rr6_cloud_save::live_directory() / name, {9, 9, 9});
        check(!rr6_cloud_save::capture_live(&error),
              "a short interrupted write is rejected");
        check(rr6_cloud_save::restore_last_good(error),
              "last-good save repairs interrupted local data");
        check(rr6_cloud_save::capture_live(&error).has_value(),
              "repaired local data is valid");

        online_cloud_save cloud = rr6_cloud_save::upload(*live);
        check(rr6_cloud_save::from_cloud(cloud, error).has_value(),
              "valid cloud snapshot is accepted");
        cloud.payload.pop_back();
        check(!rr6_cloud_save::from_cloud(cloud, error),
              "wrong-sized cloud payload is rejected");
        cloud = rr6_cloud_save::upload(*live);
        cloud.title_id = 1;
        check(!rr6_cloud_save::from_cloud(cloud, error),
              "wrong-title cloud payload is rejected");
        cloud = rr6_cloud_save::upload(*live);
        cloud.file_name = "../escape";
        check(!rr6_cloud_save::from_cloud(cloud, error),
              "unsafe cloud filename is rejected");
        cloud = rr6_cloud_save::upload(*live);
        cloud.checksum = "bad";
        check(!rr6_cloud_save::from_cloud(cloud, error),
              "malformed cloud checksum is rejected");
        cloud = rr6_cloud_save::upload(*live);
        cloud.checksum[0] = cloud.checksum[0] == '0' ? '1' : '0';
        check(!rr6_cloud_save::from_cloud(cloud, error),
              "mismatched cloud checksum is rejected");

        rr6_cloud_save::snapshot bad = *live;
        bad.checksum = "not-a-checksum";
        check(!rr6_cloud_save::install(bad, {}, error),
              "invalid replacement is not installed");
        check(!rr6_cloud_save::preserve_last_good(bad, error),
              "invalid replacement cannot poison last-good data");
    }
    check(!rr6_cloud_save::capture(root / "missing", nullptr),
          "missing save directory is rejected without an error sink");
    const fs::path multiple = root / "multiple";
    write_file(multiple / "one", bytes);
    write_file(multiple / "two", bytes);
    check(!rr6_cloud_save::capture(multiple, &error),
          "multiple payload files are rejected");
    const fs::path empty = root / "empty";
    fs::create_directories(empty);
    check(!rr6_cloud_save::capture(empty, &error),
          "a directory without a payload is rejected");

    check(!rr6_cloud_save::valid_file_name("../save"),
          "path traversal filename is rejected");
    check(!rr6_cloud_save::valid_file_name(""), "empty filename is rejected");
    check(!rr6_cloud_save::valid_file_name("."), "dot filename is rejected");
    check(!rr6_cloud_save::valid_file_name(".."), "dot-dot filename is rejected");
    check(!rr6_cloud_save::valid_file_name("a/b"), "slash is rejected");
    check(!rr6_cloud_save::valid_file_name("a\\b"), "backslash is rejected");
    check(!rr6_cloud_save::valid_file_name(std::string("a\0b", 3)),
          "embedded NUL is rejected");
    check(!rr6_cloud_save::valid_file_name(std::string(43, 'x')),
          "overlong filename is rejected");

    fs::create_directories(rr6_cloud_save::marker_path().parent_path());
    std::ofstream(rr6_cloud_save::marker_path()) << "0123456789abcdef\n";
    check(rr6_cloud_save::read_marker() == "0123456789abcdef",
          "valid checksum marker is read");
    std::ofstream(rr6_cloud_save::marker_path()) << "0123456789abcdeg\n";
    check(rr6_cloud_save::read_marker().empty(),
          "non-hex checksum marker is rejected");

    // --- divergence resolution ------------------------------------------
    // The whole point of the marker is that it is the COMMON ANCESTOR. These
    // cases are what separates a fast-forward from a real conflict; local and
    // cloud alone cannot tell them apart.
    {
        using rr6_cloud_save::resolution;
        auto make = [&](uint8_t fill) {
            rr6_cloud_save::snapshot s;
            s.file_name = name;
            s.payload.assign(rr6_cloud_save::payload_size, fill);
            s.checksum = rr6_cloud_save::checksum(s.payload);
            return s;
        };
        const auto base_save = make(1);
        const auto mine = make(2);
        const auto theirs = make(3);
        const auto factory = make(4);
        const std::string base = base_save.checksum;
        const std::optional<rr6_cloud_save::snapshot> none;

        auto verdict = [&](const std::optional<rr6_cloud_save::snapshot>& l,
                           const std::optional<rr6_cloud_save::snapshot>& c,
                           const std::string& b, const std::string& seed_sum) {
            return rr6_cloud_save::resolve(l, c, b, seed_sum).what;
        };

        check(verdict(none, none, {}, {}) == resolution::nothing,
              "no save anywhere resolves to nothing");
        check(verdict(mine, none, base, {}) == resolution::publish_local,
              "a missing cloud document is published to, never merged");
        check(verdict(none, theirs, base, {}) == resolution::take_cloud,
              "with no local save the cloud is taken");
        check(verdict(mine, mine, base, {}) == resolution::already_synced,
              "identical bytes need no transfer");

        // Local still holds exactly what we last uploaded: the other machine
        // has played since, so its save is the descendant.
        check(verdict(base_save, theirs, base, {}) == resolution::take_cloud,
              "an unplayed local save yields to the cloud");
        // First run, never uploaded, local is untouched factory bytes.
        check(verdict(factory, theirs, {}, factory.checksum) ==
                  resolution::take_cloud,
              "an untouched factory save yields to the cloud");

        // We played; the cloud is still where we left it. Safe fast-forward.
        check(verdict(mine, base_save, base, {}) == resolution::publish_local,
              "local progress publishes when the cloud is still at the base");

        // THE REGRESSION THIS GUARDS: both sides moved off the base. The old
        // code published local here and destroyed the other machine's progress.
        check(verdict(mine, theirs, base, {}) == resolution::conflict,
              "two diverged saves are a conflict, not an overwrite");
        // Never synced, and local is NOT the factory save, so it carries
        // progress of its own while a cloud save already exists.
        check(verdict(mine, theirs, {}, factory.checksum) ==
                  resolution::conflict,
              "an unsynced machine with progress does not clobber the cloud");
        check(verdict(mine, theirs, {}, {}) == resolution::conflict,
              "with no base and no seed the safe answer is conflict");

        // A conflict must cost nothing: the side we do not keep is retained.
        check(rr6_cloud_save::preserve_conflict(theirs, error),
              "the conflicting save is retained");
        check(rr6_cloud_save::capture(
                  rr6_cloud_save::conflict_directory() / theirs.checksum,
                  &error).has_value(),
              "the retained conflict copy reads back intact");
        check(rr6_cloud_save::preserve_conflict(theirs, error),
              "re-meeting the same conflict rewrites one copy");
        rr6_cloud_save::snapshot corrupt = theirs;
        corrupt.checksum = "not-a-checksum";
        check(!rr6_cloud_save::preserve_conflict(corrupt, error),
              "an invalid save is not retained as a conflict copy");
    }

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures == 0) std::printf("rr6 cloud save tests passed\n");
    return failures == 0 ? 0 : 1;
}
