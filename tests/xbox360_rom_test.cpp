#include "test_platform.h"
#include "xbox360_rom.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void put_be32(std::array<std::uint8_t, 0x100>& bytes, std::size_t offset,
              std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void write_xex(const fs::path& path, std::uint32_t title_id) {
    std::array<std::uint8_t, 0x100> bytes{};
    bytes[0] = 'X';
    bytes[1] = 'E';
    bytes[2] = 'X';
    bytes[3] = '2';
    put_be32(bytes, 0x14, 1);
    put_be32(bytes, 0x18, 0x00040006);
    put_be32(bytes, 0x1c, 0x40);
    put_be32(bytes, 0x4c, title_id);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('\0');
}

// A stand-in STFS package: the signature at offset zero, the header size the
// file claims to have, and the title ID in the metadata. Nothing else about a
// package is read, and nothing about the filename is - the real ones are named
// after the SHA-1 of their content.
void write_package(const fs::path& path, const char* magic,
                   std::uint32_t title_id, std::uint32_t declared_header_size,
                   std::size_t size) {
    fs::create_directories(path.parent_path());
    std::vector<std::uint8_t> bytes(std::max<std::size_t>(size, 0x364));
    std::memcpy(bytes.data(), magic, 4);
    const auto put_be32_at = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value >> 24);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
        bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
        bytes[offset + 3] = static_cast<std::uint8_t>(value);
    };
    put_be32_at(0x340, declared_header_size);
    put_be32_at(0x360, title_id);
    bytes.resize(size);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main(int argc, char* argv[]) {
    const fs::path root = fs::temp_directory_path() /
        ("manx-xbox360-rom-test-" +
         std::to_string(test_process_id()));
    fs::create_directories(root);
    write_xex(root / "default.xex",
              xbox360_rom_loader::robotron_title_id);

    assert(xbox360_rom_loader::identify_set(root.string()) ==
           xbox360_rom_set::robotron_2084);
    assert(xbox360_rom_loader::identify_set(
               (root / "default.xex").string()) ==
           xbox360_rom_set::robotron_2084);
    assert(!xbox360_rom_loader::inspect(root.string()));

    touch(root / "classic" / "fw.sr");
    touch(root / "classic" / "robotron.sr");
    touch(root / "media" / "uiresource.xpr");
    const xbox360_rom_info complete =
        xbox360_rom_loader::inspect(root.string());
    assert(complete);
    assert(complete.title_id == xbox360_rom_loader::robotron_title_id);
    assert(complete.set == xbox360_rom_set::robotron_2084);

    // Geometry Wars keeps its data flat beside default.xex, so its readiness
    // check is a different list of files under the same rule: identified by
    // title ID from the moment the XEX is there, complete only once the data is.
    const fs::path geometry = root / "geometrywars";
    fs::create_directories(geometry);
    write_xex(geometry / "default.xex",
              xbox360_rom_loader::geometry_wars_title_id);
    assert(xbox360_rom_loader::identify_set(geometry.string()) ==
           xbox360_rom_set::geometry_wars);
    assert(!xbox360_rom_loader::inspect(geometry.string()));
    touch(geometry / "GeometryWars1.dat");
    assert(!xbox360_rom_loader::inspect(geometry.string()));
    touch(geometry / "GW1.xwb");
    const xbox360_rom_info geometry_complete =
        xbox360_rom_loader::inspect(geometry.string());
    assert(geometry_complete);
    assert(geometry_complete.set == xbox360_rom_set::geometry_wars);
    assert(geometry_complete.title_id ==
           xbox360_rom_loader::geometry_wars_title_id);
    // Robotron's data does not make Geometry Wars complete, and the two sets
    // must not share a short name.
    assert(std::string(xbox360_rom_loader::set_short_name(
               xbox360_rom_set::geometry_wars)) !=
           std::string(xbox360_rom_loader::set_short_name(
               xbox360_rom_set::robotron_2084)));

    // Geometry Wars 2 was never extracted: it is one signed STFS package
    // holding the executable and every data file, stored the way the console
    // stores it - <TITLE ID>/<content type>/<content hash>, under a directory
    // named after the store listing, so the path has spaces in it and the
    // filename says nothing at all. Only the package's own header identifies it.
    const fs::path package_directory =
        root / "Geometry Wars Evolved 2" / "584108FF" / "000D0000";
    const fs::path package =
        package_directory / "834312072F4985F9D33D0B9549FFAA32C505FDAD58";
    // 0xad0e is the header size a real Geometry Wars 2 package declares, and
    // 0xb000 is where STFS rounds that up to, so the fixture is short in the
    // same way an interrupted download is and whole in the same way a good one
    // is.
    write_package(package, "LIVE", xbox360_rom_loader::geometry_wars_2_title_id,
                  0xad0e, 0xb000);
    assert(xbox360_rom_loader::identify_set(package.string()) ==
           xbox360_rom_set::geometry_wars_2);
    // A package that is all there IS a ready title: there is no list of files
    // beside it to check, because there is nothing beside it.
    const xbox360_rom_info packaged =
        xbox360_rom_loader::inspect(package.string());
    assert(packaged);
    assert(packaged.set == xbox360_rom_set::geometry_wars_2);
    assert(packaged.packaged());
    assert(packaged.title_id == xbox360_rom_loader::geometry_wars_2_title_id);
    assert(packaged.package_path == package.lexically_normal().string());
    assert(packaged.xex_path.empty());
    assert(packaged.game_root ==
           package_directory.lexically_normal().string());
    assert(xbox360_rom_loader::set_preferred_shape(
               xbox360_rom_set::geometry_wars_2) ==
           xbox360_content_shape::package);
    assert(xbox360_rom_loader::set_preferred_shape(
               xbox360_rom_set::geometry_wars) ==
           xbox360_content_shape::extracted);
    // Neither Geometry Wars was dumped both ways, so neither plays from the
    // other shape - the mismatch checks further down depend on that.
    assert(!xbox360_rom_loader::set_plays_shape(
        xbox360_rom_set::geometry_wars_2, xbox360_content_shape::extracted));
    assert(!xbox360_rom_loader::set_plays_shape(
        xbox360_rom_set::geometry_wars, xbox360_content_shape::package));
    // Selecting the directory the package sits in finds it too - there is no
    // default.xex there, and the file cannot be found by name.
    assert(xbox360_rom_loader::inspect(package_directory.string()));

    // All three signature types are packages, and a file with none of them is
    // not one however plausible its path looks.
    for (const char* magic : {"CON ", "PIRS"}) {
        const fs::path variant = package_directory / magic;
        write_package(variant, magic,
                      xbox360_rom_loader::geometry_wars_2_title_id, 0xad0e,
                      0xb000);
        assert(xbox360_rom_loader::identify_set(variant.string()) ==
               xbox360_rom_set::geometry_wars_2);
        fs::remove(variant);
    }
    const fs::path imposter = package_directory / "notapackage";
    write_package(imposter, "ZIP\0",
                  xbox360_rom_loader::geometry_wars_2_title_id, 0xad0e, 0xb000);
    assert(xbox360_rom_loader::identify_set(imposter.string()) ==
           xbox360_rom_set::unknown);
    fs::remove(imposter);

    // A part-downloaded package still identifies - the browser should list the
    // game rather than silently omit it - but it is not ready, and it says which
    // file fell short instead of failing generically.
    const fs::path partial = root / "partial" / "584108FF" / "000D0000" /
                             "834312072F4985F9D33D0B9549FFAA32C505FDAD58";
    write_package(partial, "LIVE", xbox360_rom_loader::geometry_wars_2_title_id,
                  0xad0e, 0x1000);
    assert(xbox360_rom_loader::identify_set(partial.string()) ==
           xbox360_rom_set::geometry_wars_2);
    const xbox360_rom_info incomplete =
        xbox360_rom_loader::inspect(partial.string());
    assert(!incomplete);
    assert(incomplete.error.find(partial.filename().string()) !=
           std::string::npos);

    // A package holding some other title is no more supported than a XEX would
    // be, and the two shapes are not interchangeable: each set is played from
    // the one it was registered with, and finding the other is worth saying.
    const fs::path unsupported = root / "unsupported-package";
    write_package(unsupported, "LIVE", 0x12345678, 0xad0e, 0xb000);
    assert(xbox360_rom_loader::identify_set(unsupported.string()) ==
           xbox360_rom_set::unknown);
    const fs::path wrong_shape = root / "extracted-sequel";
    fs::create_directories(wrong_shape);
    write_xex(wrong_shape / "default.xex",
              xbox360_rom_loader::geometry_wars_2_title_id);
    const xbox360_rom_info as_directory =
        xbox360_rom_loader::inspect(wrong_shape.string());
    assert(!as_directory);
    assert(as_directory.error.find("package") != std::string::npos);
    const fs::path packaged_original = root / "packaged-original";
    write_package(packaged_original, "LIVE",
                  xbox360_rom_loader::geometry_wars_title_id, 0xad0e, 0xb000);
    const xbox360_rom_info as_package =
        xbox360_rom_loader::inspect(packaged_original.string());
    assert(!as_package);
    assert(as_package.error.find("default.xex") != std::string::npos);

    // Space Giraffe was dumped both ways, so both copies are real copies and
    // both must identify and be judged on their own terms. This is the case the
    // one-shape-per-set rule above cannot express.
    assert(std::string(xbox360_rom_loader::set_short_name(
               xbox360_rom_set::space_giraffe)) == "spacegiraffe");
    assert(xbox360_rom_loader::set_plays_shape(
        xbox360_rom_set::space_giraffe, xbox360_content_shape::extracted));
    assert(xbox360_rom_loader::set_plays_shape(
        xbox360_rom_set::space_giraffe, xbox360_content_shape::package));
    // The package is preferred: it is the shape that shipped, and the only one
    // whose completeness is a property of the file rather than of a guess about
    // which files the game reads.
    assert(xbox360_rom_loader::set_preferred_shape(
               xbox360_rom_set::space_giraffe) ==
           xbox360_content_shape::package);
    {
        // The trap this readiness list exists for: a real extraction that got
        // only the executable. The XEX is valid, the title ID is right, and the
        // game faults on the first asset it reads. It must identify - so the
        // browser can say what is wrong - and it must not be ready.
        const fs::path giraffe = root / "spacegiraffe";
        fs::create_directories(giraffe);
        write_xex(giraffe / "default.xex",
                  xbox360_rom_loader::space_giraffe_title_id);
        touch(giraffe / "image.bin");
        const xbox360_rom_info partial_extraction =
            xbox360_rom_loader::inspect(giraffe.string(), false);
        assert(partial_extraction);
        assert(partial_extraction.set == xbox360_rom_set::space_giraffe);
        assert(partial_extraction.shape == xbox360_content_shape::extracted);
        const xbox360_rom_info without_media =
            xbox360_rom_loader::inspect(giraffe.string());
        assert(!without_media);
        assert(without_media.error.find("Media/modules.ox") !=
               std::string::npos);
        assert(without_media.error.find("Media/sounds/GRsounds.xwb") !=
               std::string::npos);
        // An identified-but-incomplete copy still reports which shape it is, so
        // a caller can say "missing files" rather than "short package".
        assert(without_media.shape == xbox360_content_shape::extracted);

        // One marker is not the list: the wave bank is still missing.
        touch(giraffe / "Media" / "modules.ox");
        assert(!xbox360_rom_loader::inspect(giraffe.string()));
        touch(giraffe / "Media" / "sounds" / "GRsounds.xwb");
        const xbox360_rom_info extracted_ready =
            xbox360_rom_loader::inspect(giraffe.string());
        assert(extracted_ready);
        assert(extracted_ready.set == xbox360_rom_set::space_giraffe);
        assert(extracted_ready.shape == xbox360_content_shape::extracted);
        assert(!extracted_ready.packaged());
        assert(extracted_ready.xex_path ==
               (giraffe / "default.xex").lexically_normal().string());
        assert(extracted_ready.package_path.empty());
        // Media/clit.png is asked for by the title and is in neither the
        // extraction nor the package, so it is not what "complete" means. A copy
        // without it plays, and a check that demanded it would condemn every
        // copy there is.
        assert(!fs::exists(giraffe / "Media" / "clit.png"));

        // A dump that used different capitalisation is the same dump.
        const fs::path lower_case = root / "spacegiraffe-lowercase";
        fs::create_directories(lower_case);
        write_xex(lower_case / "default.xex",
                  xbox360_rom_loader::space_giraffe_title_id);
        touch(lower_case / "media" / "modules.ox");
        touch(lower_case / "media" / "sounds" / "grsounds.xwb");
        assert(xbox360_rom_loader::inspect(lower_case.string()));

        // And the packaged copy of the same title, stored the way the console
        // stores it, is ready on the strength of the package alone.
        const fs::path giraffe_package = root / "Space Giraffe" /
                                         "5841080C" / "000D0000" /
                                         "5B340CF9CCDDEC28B0810E99A01D0877D9"
                                         "60B42358";
        write_package(giraffe_package, "LIVE",
                      xbox360_rom_loader::space_giraffe_title_id, 0xad0e,
                      0xb000);
        const xbox360_rom_info packaged_giraffe =
            xbox360_rom_loader::inspect(giraffe_package.string());
        assert(packaged_giraffe);
        assert(packaged_giraffe.set == xbox360_rom_set::space_giraffe);
        assert(packaged_giraffe.shape == xbox360_content_shape::package);
        assert(packaged_giraffe.packaged());
        assert(packaged_giraffe.package_path ==
               giraffe_package.lexically_normal().string());
        assert(packaged_giraffe.xex_path.empty());
    }

    // A path with nothing at it names what it was looking for, because this is
    // reached with a path saved when the game was still there.
    const xbox360_rom_info absent =
        xbox360_rom_loader::inspect((root / "gone").string());
    assert(!absent);
    assert(absent.error.find((root / "gone").string()) != std::string::npos);

    const fs::path other = root / "other";
    fs::create_directories(other);
    write_xex(other / "default.xex", 0x12345678);
    assert(xbox360_rom_loader::identify_set(other.string()) ==
           xbox360_rom_set::unknown);

    if (argc > 1) {
        const xbox360_rom_info real = xbox360_rom_loader::inspect(argv[1]);
        assert(real);
        assert(real.title_id == xbox360_rom_loader::robotron_title_id);
    }
    fs::remove_all(root);
    return 0;
}
