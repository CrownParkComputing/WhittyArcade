// system16_data.h - locally harvested system16.com board specs and game art.
//
// system16.com sits behind a Cloudflare challenge that only a real browser
// passes, so nothing here fetches: the data is harvested once with the user's
// browser clearance and read from disk. A game with no local artwork simply
// has none, and the caller falls back to its other source.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace system16 {

// "Main CPU" -> "Motorola 68EC020 32-bit @ 24.576 MHz", in page order.
using spec_list = std::vector<std::pair<std::string, std::string>>;

// Hardware description for one of our boards, keyed by the board id in
// arcade_catalog (system22, model2, ...). Empty when nothing was harvested.
const spec_list& board_specs(const std::string& board_id);

// Absolute path to a game's best local artwork - its arcade title screen
// where one exists - keyed by MAME short name. Empty when there is none.
std::string game_art_path(const std::string& short_name);

} // namespace system16
