// launcher_menu.h - shared vertical menu used before an arcade board starts.
#pragma once

#include "input_mapping.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
    // Distinct from Back: the OS close button must terminate the launcher,
    // not reopen the menu that was underneath the current screen.
    static constexpr int exit_requested = -7;
    // The status corner was pressed: whoever is signed in, who is about, and
    // the lobbies. It opens over whatever screen asked for it rather than
    // being somewhere to navigate to, because it is a thing you glance at
    // and then act on, not a place you go.
    static constexpr int shortcut = -8;
    // Compact utility mode is used for menus shown over a running game.
    explicit launcher_menu(bool compact_utility_window = false);
    ~launcher_menu();

    launcher_menu(const launcher_menu&) = delete;
    launcher_menu& operator=(const launcher_menu&) = delete;

    // Returns the selected item index, -1 for Back, or exit_requested for the
    // OS window close button.
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

    // Graphical launcher choice used for play topology. Unlike the utility
    // list above, these are large animated tiles whose picture carries the
    // meaning: players, screens, linked cabinets or a network. `value` is
    // used by the cabinet icon to show the chosen number of instances.
    enum class mode_icon : uint8_t {
        solo,
        local_players,
        split_screen,
        linked_cabinets,
        network,
        cabinet_count,
        display_wall,
        settings,
        controls,
        storage,
        scores,
        folder,
        audit,
        information,
        refresh,
        cover_front,
        cover_3d,
        cover_back,
        title_art,
        screenshot_art,
        marquee_art,
        fan_art,
        thumbnail_art,
        exit,
    };
    struct mode_card {
        std::string title;
        std::string subtitle;
        mode_icon icon{mode_icon::solo};
        int value{};
        bool unavailable{};
        std::string status;
        // Optional logo supplied by the caller. Pixels remain caller-owned
        // for the duration of select_modes; null keeps the drawn icon.
        const uint8_t* artwork{};
        int artwork_width{};
        int artwork_height{};
        // Publisher/platform wordmark used while an online logo is still
        // arriving, or when Commons has no image for an obscure maker.
        // Empty preserves the normal graphical mode icon.
        std::string artwork_fallback;

        mode_card() = default;
        mode_card(std::string title_in, std::string subtitle_in,
                  mode_icon icon_in, int value_in = 0,
                  bool unavailable_in = false,
                  std::string status_in = {})
            : title(std::move(title_in)), subtitle(std::move(subtitle_in)),
              icon(icon_in), value(value_in),
              unavailable(unavailable_in), status(std::move(status_in)) {}
    };

    // Returns a card index, -1 for Back, exit_requested for window close, or
    // interrupted when the supplied network condition changes. Keyboard,
    // controller and mouse are all accepted; unavailable cards stay visible
    // but cannot be launched.
    int select_modes(const std::string& title,
                     const std::string& description,
                     const std::vector<mode_card>& cards,
                     const std::string& back_label = "Back",
                     int initial_selection = 0,
                     std::function<bool()> interrupt = {},
                     std::function<bool()> tick = {});

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

    // All the artwork and metadata for one game, used by the game-detail
    // (cover-flow) view to show a rich, single-game page.
    struct game_media {
        const uint8_t* box_pixels{};
        int box_w{}, box_h{};
        const uint8_t* marquee_pixels{};
        int marquee_w{}, marquee_h{};
        const uint8_t* snap_pixels{};
        int snap_w{}, snap_h{};
        std::string publisher;
        std::string year;
        std::string players;
        std::string board_name;
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
                    bool info = false,
                    // True renders the page as a single-column list with
                    // small thumbnails instead of the cover-art card grid -
                    // forty games scan faster as rows than as posters.
                    bool list_view = false,
                    // A single horizontal strip for wide cabinet marquees.
                    // Left/right moves one game and artwork keeps its native
                    // aspect ratio instead of being stretched like a cover.
                    bool marquee_view = false,
                    // Full-screen cover-flow carousel: large 2D box art with
                    // game details, description, and optional video background.
                    // Cards are laid out in a 3D-ish arc with the selected
                    // game centred and largest; neighbours recede to the sides.
                    bool coverflow_view = false,
                    // Optional video snap path for the currently selected game.
                    // Called each frame with the item index; return empty
                    // string when no video is available. The video is decoded
                    // and played back in the background of the cover-flow view.
                    std::function<std::string(int)> video_for = {},
                    // All artwork and metadata for one game index. Used by
                    // cover-flow view to compose box, marquee, screenshot,
                    // publisher, year, players and board name on one page.
                    std::function<game_media(int)> media_for = {},
                    // Optional synopsis aligned with `items`. The selected
                    // game's text replaces the static header detail and
                    // scrolls automatically when it is wider than the page.
                    const std::vector<std::string>* item_descriptions =
                        nullptr);

    // Opens the next cover-flow page with the layout designer already on,
    // so arranging the page can be reached from the View menu rather than
    // only from the F2 key. Ignored by every other view.
    void arrange_coverflow_next();

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

    // Per-game loading screen, shown between the launcher picking a game and
    // the emulator window taking over. `title` is the game's display name
    // (large, centred), `subtitle` is the cabinet form (smaller, e.g. "Twin
    // cabinet"), and `step` is the current asset-warm message. A spinner
    // rotates beside the title; the progress bar fills as `progress` advances
    // from 0 to 1. The screen fades in from black on entry and fades out on
    // exit.
    //
    // `step_for(progress)` lets the caller drive both the step text and the
    // progress number from a single callback. It is called repeatedly while
    // loading; when it returns false the screen fades out and returns.
    //
    // Designed for the brief 100ms - 2s window during which the emulator is
    // spinning up its renderer and audio thread, so the screen is short-lived
    // but visible.
    void show_loading_screen(
        const std::string& title,
        const std::string& subtitle,
        const std::function<bool(float&)>& step_for);

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

    // Reads a line of text. An empty optional means cancel.
    //
    // The launcher had no text input at all before this: every other screen
    // is a list somebody points at. Signing in needs an email address and a
    // password, and neither can be pointed at - so this is a real field,
    // typed on a keyboard, with a character wheel on the d-pad for a cabinet
    // that has no keyboard attached.
    std::optional<std::string> prompt_text(
        const std::string& title, const std::string& description,
        const std::string& initial = {}, bool password = false,
        std::size_t max_length = 128);

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
