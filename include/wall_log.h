// wall_log.h - per-column diagnostics for an arcade wall.
//
// A wall column that never appears is the hardest thing in this program to
// diagnose: its window is created hidden and only shown after the first
// successful present, so "rendering failed" and "no window" look identical
// from the outside, and the column that fails is usually not the one whose
// terminal you are looking at. Each column writes its own file, so the whole
// wall can be read back afterwards without anyone copying anything.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace whitty_wall_log {

inline std::string log_path(int slot) {
    const char* home = std::getenv("HOME");
    std::string root = home && *home ? home : "/tmp";
    root += "/.local/share/WhittyArcade";
    return root + "/wall-column-" + std::to_string(slot) + ".log";
}

// Non-null only for a wall column, so a single cabinet writes nothing at all.
inline std::FILE*& sink() {
    static std::FILE* file = nullptr;
    return file;
}

inline void begin(int slot, int count) {
    if (sink() || count <= 1) return;
    // Truncated per run: the interesting question is always what this launch
    // did, never what the last one did.
    sink() = std::fopen(log_path(slot).c_str(), "w");
    if (!sink()) return;
    std::fprintf(sink(), "column %d of %d\n", slot, count);
    std::fflush(sink());
}

inline void note(const char* format, ...) {
    if (!sink()) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(sink(), format, arguments);
    va_end(arguments);
    std::fputc('\n', sink());
    // Flushed every line: the failure being chased is one where the process
    // is still running, so nothing may ever close this file.
    std::fflush(sink());
}

// True only the first few times for a given counter: notes on per-frame
// paths say what happened without filling the file.
inline bool first(int& counter, int limit = 3) { return ++counter <= limit; }

} // namespace whitty_wall_log
