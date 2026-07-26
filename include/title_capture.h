// title_capture.h - the launcher's game icons, captured from the games.
//
// Downloaded artwork came in every shape a shop shelf knows; a browser row
// wants one shape. So the icon for every game is a frame the emulator
// itself presented, saved once about thirty seconds into a session - long
// enough for any of these boards to be showing its title or attract. The
// files are plain 24-bit BMPs (stb_image reads them back) keyed by the
// game's short name, and deleting one simply means it is recaptured the
// next time that game runs.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

inline std::string title_capture_directory() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string root;
    if (xdg && *xdg) {
        root = xdg;
    } else {
        const char* home = std::getenv("HOME");
        root = std::string(home && *home ? home : ".") + "/.local/share";
    }
    return root + "/WhittyArcade/titles";
}

inline std::string title_capture_path(const std::string& short_name) {
    return title_capture_directory() + "/" + short_name + ".bmp";
}

inline bool title_capture_exists(const std::string& short_name) {
    std::error_code error;
    return std::filesystem::exists(title_capture_path(short_name), error);
}

// Writes RGBA pixels as a bottom-up 24-bit BMP. top_down says how the
// incoming rows are ordered.
inline bool title_capture_write(const std::string& short_name,
                                const uint8_t* rgba, int width, int height,
                                bool top_down) {
    if (!rgba || width <= 0 || height <= 0) return false;
    std::error_code error;
    std::filesystem::create_directories(title_capture_directory(), error);

    const int row_bytes = (width * 3 + 3) & ~3;
    const uint32_t image_bytes =
        static_cast<uint32_t>(row_bytes) * static_cast<uint32_t>(height);
    const uint32_t file_bytes = 54 + image_bytes;

    std::FILE* out =
        std::fopen(title_capture_path(short_name).c_str(), "wb");
    if (!out) return false;

    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    header[2] = static_cast<uint8_t>(file_bytes);
    header[3] = static_cast<uint8_t>(file_bytes >> 8);
    header[4] = static_cast<uint8_t>(file_bytes >> 16);
    header[5] = static_cast<uint8_t>(file_bytes >> 24);
    header[10] = 54;                        // pixel data offset
    header[14] = 40;                        // BITMAPINFOHEADER
    header[18] = static_cast<uint8_t>(width);
    header[19] = static_cast<uint8_t>(width >> 8);
    header[20] = static_cast<uint8_t>(width >> 16);
    header[21] = static_cast<uint8_t>(width >> 24);
    header[22] = static_cast<uint8_t>(height);
    header[23] = static_cast<uint8_t>(height >> 8);
    header[24] = static_cast<uint8_t>(height >> 16);
    header[25] = static_cast<uint8_t>(height >> 24);
    header[26] = 1;                         // planes
    header[28] = 24;                        // bits per pixel
    header[34] = static_cast<uint8_t>(image_bytes);
    header[35] = static_cast<uint8_t>(image_bytes >> 8);
    header[36] = static_cast<uint8_t>(image_bytes >> 16);
    header[37] = static_cast<uint8_t>(image_bytes >> 24);
    if (std::fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        std::fclose(out);
        return false;
    }

    std::vector<uint8_t> row(static_cast<std::size_t>(row_bytes), 0);
    for (int y = height - 1; y >= 0; --y) {
        const int source_y = top_down ? y : height - 1 - y;
        const uint8_t* src =
            rgba + static_cast<std::size_t>(source_y) * width * 4;
        for (int x = 0; x < width; ++x) {
            row[static_cast<std::size_t>(x) * 3 + 0] = src[x * 4 + 2];
            row[static_cast<std::size_t>(x) * 3 + 1] = src[x * 4 + 1];
            row[static_cast<std::size_t>(x) * 3 + 2] = src[x * 4 + 0];
        }
        if (std::fwrite(row.data(), 1, row.size(), out) != row.size()) {
            std::fclose(out);
            return false;
        }
    }
    std::fclose(out);
    return true;
}
