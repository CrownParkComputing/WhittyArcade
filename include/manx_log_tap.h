// manx_log_tap.h - keeping this machine's own console output in memory so it
// can be sent to the other machine.
//
// Debugging two computers is otherwise a matter of walking between them. The
// tap captures everything MANX and its libraries print, keeps the last few
// hundred lines, and hands them to the lobby, which relays them to the
// partner machine. Because the relay is continuous, the lines a cabinet
// printed just before it died are already on the other machine by the time
// it stops answering - which is exactly the output that is otherwise lost.
//
// Output still reaches the real console unchanged; this only duplicates it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace manx_log_tap {

// Redirects stdout and stderr through a reader thread. Safe to call more
// than once; only the first call does anything. Call it early - lines
// printed before this are not captured.
void start();

// Sequence number one past the newest captured line. Line numbers never
// restart, so both machines can talk about "line 412" and mean it.
uint32_t next_sequence();

// Captured lines from `first` onwards, newest last, up to `max_lines`. Lines
// older than the retained window are gone; `first_returned` reports the
// sequence number actually delivered so a caller can tell it lost some.
std::vector<std::string> lines_from(uint32_t first, std::size_t max_lines,
                                    uint32_t* first_returned = nullptr);

} // namespace manx_log_tap
