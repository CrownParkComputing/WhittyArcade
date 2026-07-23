#include "saves/McDumpReader.h"

#include "StdStreamUtils.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void extract_directory(CMcDumpReader& reader,
                       const CMcDumpReader::Directory& directory,
                       const fs::path& output_path) {
    fs::create_directories(output_path);
    for (const auto& entry : directory) {
        if (!std::strcmp(entry.name, ".") ||
            !std::strcmp(entry.name, ".."))
            continue;

        const fs::path destination = output_path / entry.name;
        std::cout << entry.name << " size=" << entry.length
                  << " cluster=" << entry.cluster
                  << ((entry.mode & CMcDumpReader::DF_DIRECTORY) ?
                          " directory" : " file")
                  << '\n';
        if (entry.mode & CMcDumpReader::DF_DIRECTORY) {
            extract_directory(reader,
                              reader.ReadDirectory(entry.cluster,
                                                   entry.length),
                              destination);
            continue;
        }

        const auto contents = reader.ReadFile(entry.cluster, entry.length);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Couldn't create " +
                                     destination.string());
        output.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: rrv_mc_extract <memory-card dump> <output dir>\n";
        return 2;
    }

    auto input = Framework::CreateInputStdStream(std::string(argv[1]));
    CMcDumpReader reader(input);
    extract_directory(reader, reader.ReadRootDirectory(), argv[2]);
    return 0;
}
