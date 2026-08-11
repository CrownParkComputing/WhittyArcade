#pragma once

#include "online_link.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rr6_cloud_save {

constexpr uint32_t title_id = 0x4E4D07D3u;
constexpr std::size_t payload_size = 72192;

struct snapshot {
    std::string file_name;
    std::vector<uint8_t> payload;
    std::string checksum;
};

// What to do when the local save and the cloud document disagree.
//
// The marker file holds the checksum this machine last saw Firestore accept,
// so it is the common ancestor of both sides - a parent revision expressed as
// a hash rather than a counter. Comparing BOTH sides against it is what tells
// a fast-forward apart from a genuine divergence. Comparing only local against
// cloud cannot: the two look identical in both cases, which is why publishing
// local unconditionally used to discard whichever side happened to be newer.
enum class resolution : uint8_t {
    nothing,        // no cloud document and nothing local worth publishing
    already_synced, // both sides hold the same bytes
    take_cloud,     // local is an unmodified ancestor; the cloud is authoritative
    publish_local,  // the cloud is still at the base; local descends from it
    conflict,       // both moved off the base - keep both, overwrite neither
};

struct decision {
    resolution what{resolution::nothing};
    std::string reason;
};

// Pure over checksums so every branch is testable without a network or a disk.
// `base` is read_marker() and may be empty on a machine that has never
// uploaded; `seed_checksum` is the untouched default save, which lets a first
// run recognise "local is still the factory save" without a marker.
decision resolve(const std::optional<snapshot>& local,
                 const std::optional<snapshot>& cloud, const std::string& base,
                 const std::string& seed_checksum);

const char* describe(resolution what);

std::filesystem::path live_directory();
std::filesystem::path marker_path();
std::filesystem::path last_good_directory();
std::filesystem::path conflict_directory();

std::string checksum(const std::vector<uint8_t>& payload);
bool valid_file_name(const std::string& name);
std::optional<snapshot> capture(const std::filesystem::path& directory,
                                std::string* error = nullptr);
std::optional<snapshot> capture_live(std::string* error = nullptr);

// Replace the live container as one directory operation. The previous valid
// directory is retained as the local last-good copy, so an interrupted title
// write can be repaired without asking the network.
bool install(const snapshot& save, const std::filesystem::path& seed_directory,
             std::string& error);
bool restore_last_good(std::string& error);
bool install_seed(const std::filesystem::path& seed_directory,
                  std::string& error);
bool preserve_last_good(const snapshot& save, std::string& error);

// Retains the side we are NOT keeping, under a name carrying its checksum, so
// a divergence costs the player nothing while they decide. Repeated conflicts
// with the same bytes collapse onto one copy rather than growing without
// bound.
bool preserve_conflict(const snapshot& save, std::string& error);

std::string read_marker();
online_cloud_save upload(const snapshot& save);
std::optional<snapshot> from_cloud(const online_cloud_save& save,
                                   std::string& error);

} // namespace rr6_cloud_save
