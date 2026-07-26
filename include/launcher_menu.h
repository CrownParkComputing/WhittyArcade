// launcher_menu.h - shared vertical menu used before an arcade board starts.
#pragma once

#include "input_mapping.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct launcher_controller_info {
    std::string guid;
    std::string name;
    int32_t instance_id{-1};
};

class launcher_menu {
public:
    static constexpr int interrupted = -2;
    // Returned by select_grid when the player asks to change the view (TAB /
    // controller Y): the caller re-sorts or re-filters and shows the grid
    // again.
    static constexpr int view_change = -3;
    // Returned by a paging grid (PgUp/PgDn, controller shoulders): the
    // caller flips to the previous or next page - the next board, the next
    // publisher - and shows the grid again.
    static constexpr int page_back = -4;
    static constexpr int page_forward = -5;
    // Returned when the player asks for the page's information panel
    // (the "i" chip, I key or controller X).
    static constexpr int info_request = -6;
    // Compact utility mode is used for menus shown over a running game.
    explicit launcher_menu(bool compact_utility_window = false);
    ~launcher_menu();

    launcher_menu(const launcher_menu&) = delete;
    launcher_menu& operator=(const launcher_menu&) = delete;

    // Returns the selected item index, or -1 for Back/window close.
    // `unavailable` names items that are shown but cannot be chosen - a
    // second monitor on a machine that has only one. Hiding them instead
    // leaves a player wondering whether the feature exists; greying one out
    // says it does, and that this machine cannot do it today.
    int select(const std::string& title, const std::string& description,
               const std::vector<std::string>& items,
               const std::string& back_label = "Back",
               int initial_selection = 0,
               const std::vector<int>& unavailable = {});

    // As select(), but returns interrupted when the background condition
    // becomes true. Used by the multiplayer lobby so Player 2 launches
    // without touching the second app again.
    int select_interruptible(
        const std::string& title, const std::string& description,
        const std::vector<std::string>& items,
        const std::string& back_label, int initial_selection,
        std::function<bool()> interrupt);

    // Cover art for one grid card. The pixels are RGBA, owned by the caller
    // and only read during the call; a null pointer draws a title-only card,
    // which is what a game with no artwork yet gets.
    struct cover {
        const uint8_t* pixels{};
        int width{};
        int height{};
        // A short mark in the card's corner saying how many people can play
        // this one and how - "2P", "LINK", "NET". Null draws nothing, which
        // is what a one-player board gets. The text is not owned: pass a
        // string literal, since this is asked for while drawing.
        const char* badge{};
        // Colours the badge so the kind registers before the word is read:
        // 0 two players here, 1 two machines, 2 the cabinet link.
        int badge_tone{};
    };

    // Cover-art grid selector, the console-style counterpart to select().
    // cover_for(index) is asked for artwork as cards are drawn, and tick() is
    // called once per frame so the caller can collect covers that arrived in
    // the background - returning true when something changed and the grid
    // should be redrawn.
    // interrupt, when supplied, is polled alongside tick: returning true
    // abandons the grid with `interrupted` - how a network peer's launch
    // pulls the player out of browsing.
    int select_grid(const std::string& title, const std::string& description,
                    const std::vector<std::string>& items,
                    const std::string& back_label, int initial_selection,
                    std::function<cover(int)> cover_for,
                    std::function<bool()> tick = {},
                    std::function<bool()> interrupt = {},
                    bool paging = false,
                    // Non-null on a paged view: the page's board photo or
                    // publisher logo, drawn large in the header. Pixels may
                    // be null while the artwork is still arriving.
                    const cover* banner = nullptr,
                    // True when the page has an information panel to show;
                    // the grid then offers the "i" chip and returns
                    // info_request when it is asked for.
                    bool info = false);

    // Picks several games from one grid rather than asking repeatedly. Cards
    // already chosen are numbered in the order they were taken, and the grid
    // returns as soon as `wanted` of them are held. Empty when cancelled.
    std::vector<int> select_grid_multiple(
        const std::string& title, const std::string& description,
        const std::vector<std::string>& items, const std::string& back_label,
        int wanted, std::function<cover(int)> cover_for,
        std::function<bool()> tick = {});

    // The boot screen. Shown while the library is being read and artwork
    // gathered, so the front end is on screen from the first moment rather
    // than the desktop being empty until everything is ready. Call it as each
    // step begins; `progress` is 0..1 and drives the loading bar.
    void show_splash(const std::string& step, float progress);

    // A short line about the machine's network state, drawn in the corner of
    // every browsing screen ("Waiting for another machine", "2 machines
    // connected"). Empty hides it.
    void set_status(const std::string& text, bool good);

    // Displays scrollable text using the same launcher presentation.
    void show_text(const std::string& title, const std::string& text,
                   const std::string& back_label = "Back");

    // Displays scrollable text with left/right navigation across `count` pages.
    // `page_provider(page_index)` returns the (title, body) pair for that page.
    // Returns the index of the page shown when the user closed, or -1 if they
    // backed out entirely (only possible when count is 0).
    // One page of the high-score board: a ranked table drawn with medal
    // colours and aligned columns rather than a wall of text.
    struct scoreboard_row {
        int rank{};
        std::string name;
        std::string score;
    };
    struct scoreboard_page {
        std::string title;
        std::string subtitle;
        std::vector<scoreboard_row> rows;
        std::vector<std::pair<std::string, std::string>> extras;
        // Shown instead of the table when there is nothing to decode.
        std::string message;
    };

    // Paged like show_pages: left/right moves between games.
    int show_scoreboard(
        const std::string& back_label, const std::string& description,
        int count, std::function<scoreboard_page(int)> page_provider,
        int initial_page = 0);

    int show_pages(
        const std::string& back_label, const std::string& description,
        int count,
        std::function<std::pair<std::string, std::string>(int)> page_provider,
        int initial_page = 0);

    // Returns controllers currently recognised by SDL's GameController API.
    std::vector<launcher_controller_info> controllers();

    // Waits for one key/button/axis movement. An empty optional means cancel;
    // an input_binding of type none means Clear. When allow_inherit is true,
    // Backspace returns an inherit binding for a per-game profile.
    std::optional<input_binding> capture_binding(
        const std::string& title, const std::string& description,
        bool keyboard, int32_t controller_instance = -1,
        bool allow_inherit = false);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};
