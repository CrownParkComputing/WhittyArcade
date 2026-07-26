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

// A linked cabinet has the same problem a wall column does, for the same
// reason: the second cabinet is a spawned process whose stderr lands in the
// parent's terminal mixed with the parent's own, if it is seen at all. It
// gets a file of its own, named for the cabinet rather than the column.
inline void begin_cabinet(int node) {
    if (sink() || node < 1 || node > 8) return;
    const char* home = std::getenv("HOME");
    std::string root = home && *home ? home : "/tmp";
    root += "/.local/share/WhittyArcade/cabinet-" + std::to_string(node) +
            ".log";
    sink() = std::fopen(root.c_str(), "w");
    if (!sink()) return;
    std::fprintf(sink(), "cabinet %d of 2\n", node);
    std::fflush(sink());
}

// An ordinary single launch keeps a file too. It has neither a column nor a
// cabinet number, so it wrote nothing at all - which left the one
// configuration worth comparing against as the one nobody could see.
inline void begin_session() {
    if (sink()) return;
    const char* home = std::getenv("HOME");
    std::string root = home && *home ? home : "/tmp";
    root += "/.local/share/WhittyArcade/session.log";
    sink() = std::fopen(root.c_str(), "w");
    if (!sink()) return;
    std::fprintf(sink(), "single cabinet\n");
    std::fflush(sink());
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
