// Where a LAN regression would come from, so it is pinned here rather than
// discovered on a second machine: LAN discovery and internet seeding share
// one send pass, and the rule is that adding the second must not take
// anything away from the first.

#include "lobby_targets.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

lobby_link::hello_target answering(uint32_t ipv4, uint16_t port) {
    return lobby_link::hello_target{ipv4, port, true};
}

lobby_link::remote_peer seed(uint32_t ipv4, uint16_t port) {
    return lobby_link::remote_peer{ipv4, port};
}

bool has(const std::vector<lobby_link::hello_target>& targets, uint32_t ipv4,
         uint16_t port) {
    for (const auto& at : targets)
        if (at.ipv4 == ipv4 && at.port == port) return true;
    return false;
}

std::size_t count(const std::vector<lobby_link::hello_target>& targets,
                  uint32_t ipv4, uint16_t port) {
    std::size_t found = 0;
    for (const auto& at : targets)
        if (at.ipv4 == ipv4 && at.port == port) ++found;
    return found;
}

constexpr uint32_t lan_a = 0x0101a8c0;      // 192.168.1.1, network order
constexpr uint32_t lan_b = 0xbd01a8c0;      // 192.168.1.189
constexpr uint32_t far_away = 0x0708090a;
constexpr uint32_t mine = 0x01020304;

} // namespace

int main() {
    // --- LAN only: byte-for-byte what the lobby did before seeding -----
    {
        const std::vector<lobby_link::hello_target> lan = {
            answering(lan_a, 35109), answering(lan_b, 35109)};
        const auto targets = lobby_link::merge_hello_targets(lan, {}, 0, 0);
        check(targets.size() == 2, "no seeds means no change at all");
        check(targets[0].relay_logs && targets[1].relay_logs,
              "machines that answered still get the log relay");
    }

    // --- the sweep ------------------------------------------------------
    check(lobby_link::should_sweep(false),
          "the sweep runs while nothing has answered");
    check(!lobby_link::should_sweep(true),
          "the sweep stops the moment a machine answers");
    check(lobby_link::search_is_exhausted(true, false),
          "a completed sweep with no seeds is an exhausted search");
    check(!lobby_link::search_is_exhausted(true, true),
          "a punch in flight means the search is not over, whatever the "
          "sweep found");
    check(!lobby_link::search_is_exhausted(false, false),
          "a sweep still running is not an exhausted search");

    // --- seeding does not disturb the LAN -------------------------------
    {
        const std::vector<lobby_link::hello_target> lan = {
            answering(lan_a, 35109)};
        const auto targets = lobby_link::merge_hello_targets(
            lan, {seed(far_away, 40000)}, mine, 35109);
        check(has(targets, lan_a, 35109),
              "the machine on this network is still a target");
        check(has(targets, far_away, 40000), "the seeded machine is a target");
        check(targets.size() == 2, "and nothing else is");
        for (const auto& at : targets)
            if (at.ipv4 == far_away)
                check(!at.relay_logs,
                      "a seed gets no log relay: it has no nonce yet");
    }

    // --- dedupe once the seed answers -----------------------------------
    {
        // The punch worked, so the peer is now in the answering list at the
        // very address it was seeded with. Sending twice would only double
        // the rate for no benefit.
        const std::vector<lobby_link::hello_target> lan = {
            answering(far_away, 40000)};
        const auto targets = lobby_link::merge_hello_targets(
            lan, {seed(far_away, 40000)}, mine, 35109);
        check(targets.size() == 1, "a seed that has answered appears once");
        check(count(targets, far_away, 40000) == 1, "and only once");
        check(targets[0].relay_logs,
              "having answered, it gets the log relay like any other machine");
    }
    {
        // Same address, different port is a different machine behind the
        // same router, and must not be deduplicated away.
        const std::vector<lobby_link::hello_target> lan = {
            answering(far_away, 40000)};
        const auto targets = lobby_link::merge_hello_targets(
            lan, {seed(far_away, 40001)}, mine, 35109);
        check(targets.size() == 2,
              "a different port behind the same address is a different peer");
    }

    // --- never punch at ourselves ---------------------------------------
    {
        const auto targets = lobby_link::merge_hello_targets(
            {}, {seed(mine, 35109), seed(far_away, 40000)}, mine, 35109);
        check(!has(targets, mine, 35109),
              "our own row in the member list is not a target");
        check(has(targets, far_away, 40000), "but everybody else's is");
    }
    {
        // Our public address with a different port is another machine
        // behind our own router - the case that matters at a LAN party
        // where one person joined online.
        const auto targets = lobby_link::merge_hello_targets(
            {}, {seed(mine, 41000)}, mine, 35109);
        check(has(targets, mine, 41000),
              "another machine behind our own router is still a target");
    }
    {
        // Before STUN has answered we do not know our own address. Not
        // knowing must not mean discarding everything.
        const auto targets = lobby_link::merge_hello_targets(
            {}, {seed(far_away, 40000)}, 0, 0);
        check(targets.size() == 1,
              "seeding works before the public address is known");
    }

    // --- rubbish in the member list -------------------------------------
    {
        const auto targets = lobby_link::merge_hello_targets(
            {}, {seed(0, 40000), seed(far_away, 0), seed(far_away, 40000)}, 0,
            0);
        check(targets.size() == 1,
              "a member with no address or no port is skipped, not punched at");
    }

    // --- timeouts --------------------------------------------------------
    check(lobby_link::timeout_for(false) == lobby_link::lan_peer_timeout,
          "a machine on this network keeps the timeout it always had");
    check(lobby_link::timeout_for(true) > lobby_link::timeout_for(false),
          "a machine across the internet is allowed to go quiet for longer");
    check(lobby_link::remote_peer_timeout <= std::chrono::seconds(10),
          "but not so long that a dead session hangs about");

    if (failures == 0) std::printf("lobby_targets_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
