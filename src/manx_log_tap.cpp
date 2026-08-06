#include "manx_log_tap.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace manx_log_tap {
namespace {

// Enough to hold the whole of a launch and a crash, and small enough that
// keeping it costs nothing worth measuring.
constexpr std::size_t retained_lines = 400;
constexpr std::size_t max_line_length = 400;

std::mutex g_mutex;
std::deque<std::string> g_lines;
uint32_t g_first_sequence = 0;   // sequence number of g_lines.front()
std::atomic_uint32_t g_next_sequence{0};
std::atomic_bool g_started{false};

#if defined(_WIN32)
int read_fd(int fd, char* buffer, unsigned count) {
    return _read(fd, buffer, count);
}
int write_fd(int fd, const char* buffer, unsigned count) {
    return _write(fd, buffer, count);
}
#else
int read_fd(int fd, char* buffer, unsigned count) {
    return static_cast<int>(::read(fd, buffer, count));
}
int write_fd(int fd, const char* buffer, unsigned count) {
    return static_cast<int>(::write(fd, buffer, count));
}
#endif

void store_line(std::string line) {
    // Terminal control sequences and stray carriage returns make a mess of a
    // menu that is going to draw this as plain text.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.size() > max_line_length) line.resize(max_line_length);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.push_back(std::move(line));
    g_next_sequence.fetch_add(1, std::memory_order_acq_rel);
    while (g_lines.size() > retained_lines) {
        g_lines.pop_front();
        ++g_first_sequence;
    }
}

// Reads the duplicated output, passes it straight through to the real
// console, and files it a line at a time.
void reader(int pipe_read, int console) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        const int got = read_fd(pipe_read, buffer, sizeof(buffer));
        if (got <= 0) break;
        write_fd(console, buffer, static_cast<unsigned>(got));
        for (int index = 0; index < got; ++index) {
            if (buffer[index] == '\n') {
                store_line(std::move(pending));
                pending.clear();
            } else {
                // A single runaway line must not grow without bound.
                if (pending.size() < max_line_length * 4)
                    pending.push_back(buffer[index]);
            }
        }
    }
}

} // namespace

void start() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    int ends[2]{-1, -1};
#if defined(_WIN32)
    if (_pipe(ends, 1 << 16, _O_BINARY) != 0) return;
#else
    if (pipe(ends) != 0) return;
#endif

    // Keep the real console to write through to, then point stdout and
    // stderr at the pipe.
#if defined(_WIN32)
    const int console = _dup(1);
    if (console < 0) return;
    _dup2(ends[1], 1);
    _dup2(ends[1], 2);
    _close(ends[1]);
#else
    const int console = dup(1);
    if (console < 0) return;
    dup2(ends[1], 1);
    dup2(ends[1], 2);
    close(ends[1]);
#endif

    // Line buffering, so a line reaches the tap when it is printed rather
    // than when four kilobytes have accumulated. Without this the output
    // from a crash sits in a buffer that is never flushed - which is the
    // whole problem this exists to solve.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::thread(reader, ends[0], console).detach();
}

uint32_t next_sequence() {
    return g_next_sequence.load(std::memory_order_acquire);
}

std::vector<std::string> lines_from(uint32_t first, std::size_t max_lines,
                                    uint32_t* first_returned) {
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t start = first;
    if (static_cast<int32_t>(start - g_first_sequence) < 0)
        start = g_first_sequence;   // caller fell behind the retained window
    std::vector<std::string> out;
    const uint32_t end = g_first_sequence +
        static_cast<uint32_t>(g_lines.size());
    if (first_returned) *first_returned = start;
    for (uint32_t index = start;
         index < end && out.size() < max_lines; ++index)
        out.push_back(g_lines[index - g_first_sequence]);
    return out;
}

} // namespace manx_log_tap
