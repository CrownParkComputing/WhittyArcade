#include "launcher_menu.h"
#include "menu_stick.h"
#include "platform_paths.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cmath>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <utility>

namespace {
// The drawing space is described in these units and scaled to the window, so
// 720 is "one screen tall" rather than a pixel count. The width is not fixed:
// a fixed one letterboxes an ultrawide display into a strip down the middle
// with everything drawn twice the size it should be. It is set from the
// display's own shape when the window opens.
int logical_width = 800;
constexpr int logical_height = 720;
constexpr int compact_window_width = 560;
constexpr int compact_window_height = 510;
constexpr int horizontal_margin = 30;
constexpr int list_bottom = 652;
constexpr int row_height = 48;
constexpr std::size_t sdl_guid_text_size = 33;

constexpr SDL_Color white{238, 245, 250, 255};
constexpr SDL_Color muted{159, 177, 190, 255};
constexpr SDL_Color accent{70, 208, 255, 255};

// SDL3's render path is float-based; the launcher lays out with integers, so
// this converts a pixel rect to the SDL_FRect the renderer now expects.
inline SDL_FRect frect(int x, int y, int w, int h) {
    return SDL_FRect{static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(w), static_cast<float>(h)};
}

struct rendered_text {
    SDL_Texture* texture{};
    int width{};
    int height{};
};

rendered_text make_text(SDL_Renderer* renderer, TTF_Font* font,
                        const std::string& text, SDL_Color color,
                        int wrap_width = 0) {
    if (text.empty()) return {};
    SDL_Surface* surface = wrap_width ?
        TTF_RenderText_Blended_Wrapped(font, text.c_str(), text.size(), color,
                                       wrap_width) :
        TTF_RenderText_Blended(font, text.c_str(), text.size(), color);
    if (!surface) return {};
    rendered_text result;
    result.texture = SDL_CreateTextureFromSurface(renderer, surface);
    result.width = surface->w;
    result.height = surface->h;
    SDL_DestroySurface(surface);
    return result;
}

void destroy_text(rendered_text& text) {
    if (text.texture) SDL_DestroyTexture(text.texture);
    text = {};
}

void draw_text(SDL_Renderer* renderer, const rendered_text& text,
               int x, int y, const SDL_Rect* clip = nullptr) {
    if (!text.texture) return;
    const SDL_FRect destination = frect(x, y, text.width, text.height);
    SDL_SetRenderClipRect(renderer, clip);
    SDL_RenderTexture(renderer, text.texture, nullptr, &destination);
    SDL_SetRenderClipRect(renderer, nullptr);
}

int fallback_select(const std::string& title, const std::string& description,
                    const std::vector<std::string>& items,
                    const std::string& back_label, int initial_selection) {
    std::vector<SDL_MessageBoxButtonData> buttons;
    buttons.reserve(items.size() + 1);
    for (std::size_t index = 0; index < items.size(); ++index) {
        const Uint32 flags = initial_selection >= 0 &&
                             static_cast<int>(index) == initial_selection ?
            SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
        buttons.push_back({flags, static_cast<int>(index),
                           items[index].c_str()});
    }
    const Uint32 back_flags =
        static_cast<Uint32>(SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT) |
        (initial_selection < 0 ?
             static_cast<Uint32>(SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) : 0u);
    buttons.push_back({back_flags,
                       -1,
                       back_label.c_str()});
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, nullptr, title.c_str(),
        description.c_str(), static_cast<int>(buttons.size()), buttons.data(),
        nullptr,
    };
    int selected = -1;
    if (!SDL_ShowMessageBox(&box, &selected)) {
        std::fprintf(stderr, "Could not show launcher menu: %s\n",
                     SDL_GetError());
        return -1;
    }
    return selected >= 0 && selected < static_cast<int>(items.size()) ?
        selected : -1;
}

// One step of menu movement asked for by an analogue stick.
struct stick_step {
    int horizontal{};
    int vertical{};
};

// Maps SDL's axes onto the latch. Both sticks drive the menu, because which
// one a cabinet's controller reports as is not something a player should have
// to know.
stick_step stick_movement(menu_stick& latch, const SDL_Event& event) {
    if (event.type != SDL_EVENT_GAMEPAD_AXIS_MOTION) return {};
    const int value = static_cast<int>(event.gaxis.value);
    switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
    case SDL_GAMEPAD_AXIS_RIGHTX:
        return {latch.step(menu_stick::horizontal, value), 0};
    case SDL_GAMEPAD_AXIS_LEFTY:
    case SDL_GAMEPAD_AXIS_RIGHTY:
        return {0, latch.step(menu_stick::vertical, value)};
    default:
        return {};
    }
}

} // namespace

struct launcher_menu::implementation {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    TTF_Font* font{};
    TTF_Font* title_font{};
    TTF_Font* hint_font{};
    // Shown in the corner of the browsing screens: whether another machine
    // is out there. A player deciding what to play should not have to open a
    // game to find out that two-player across machines is unavailable.
    std::string status_text;
    bool status_good{};
    bool sdl_initialized{};
    bool ttf_initialized{};
    std::vector<SDL_Gamepad*> controllers;

    explicit implementation(bool compact_utility_window) {
        SDL_SetHint("SDL_APP_ID", "WhittyArcade");
        SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "WhittyArcade");
        SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "WhittyArcade");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS |
                               SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "Launcher SDL initialization failed: %s\n",
                         SDL_GetError());
            return;
        }
        sdl_initialized = true;
        if (!TTF_WasInit()) {
            if (!TTF_Init()) {
                std::fprintf(stderr, "Launcher font initialization failed: %s\n",
                             SDL_GetError());
                return;
            }
            ttf_initialized = true;
        }

        // A cabinet front end takes the whole screen. Windowed is still
        // available for working on it, where a fullscreen menu that steals
        // focus every launch is a nuisance.
        const bool windowed_menu =
            compact_utility_window ||
            (std::getenv("WHITTY_MENU_WINDOWED") &&
             *std::getenv("WHITTY_MENU_WINDOWED") == '1');
        if (!compact_utility_window) {
            const SDL_DisplayMode* desktop =
                SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
            if (desktop && desktop->w > 0 && desktop->h > 0) {
                logical_width = std::clamp(
                    logical_height * desktop->w / desktop->h, 800, 2400);
            }
        }
        const SDL_WindowFlags window_flags =
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY |
            (windowed_menu ? 0 : SDL_WINDOW_FULLSCREEN) |
            (compact_utility_window ?
                 (SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_UTILITY) : 0);
        window = SDL_CreateWindow(
            "WhittyArcade",
            compact_utility_window ? compact_window_width : logical_width,
            compact_utility_window ? compact_window_height : logical_height,
            window_flags);
        if (!window) {
            std::fprintf(stderr, "Launcher window creation failed: %s\n",
                         SDL_GetError());
            return;
        }
        SDL_SetWindowMinimumSize(window,
                                 compact_utility_window ? 480 : 560,
                                 compact_utility_window ? 438 : 500);
        if (!compact_utility_window) {
            renderer = SDL_CreateRenderer(window, nullptr);
            if (renderer) SDL_SetRenderVSync(renderer, 1);
        }
        if (!renderer)
            renderer = SDL_CreateRenderer(window, "software");
        if (!renderer) {
            std::fprintf(stderr, "Launcher renderer creation failed: %s\n",
                         SDL_GetError());
            return;
        }
        SDL_SetRenderLogicalPresentation(renderer, logical_width, logical_height,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        if (compact_utility_window) {
            SDL_SetWindowAlwaysOnTop(window, true);
            SDL_RaiseWindow(window);
        }

        for (const std::filesystem::path& path : whitty_platform::font_paths()) {
            font = TTF_OpenFont(path.string().c_str(), 21);
            if (font) {
                // The banner pages draw their board or publisher name large.
                title_font = TTF_OpenFont(path.string().c_str(), 44);
                // Control hints are a glance, not a sentence: small, and
                // paired with the button they name.
                hint_font = TTF_OpenFont(path.string().c_str(), 15);
                break;
            }
        }
        if (!font)
            std::fprintf(stderr, "Could not open a launcher font: %s\n",
                         SDL_GetError());

        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
            for (int index = 0; index < count; ++index)
                open_controller(ids[index]);
            SDL_free(ids);
        }
    }

    ~implementation() {
        for (SDL_Gamepad* controller : controllers)
            SDL_CloseGamepad(controller);
        if (hint_font) TTF_CloseFont(hint_font);
        if (title_font) TTF_CloseFont(title_font);
        if (font) TTF_CloseFont(font);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (ttf_initialized) TTF_Quit();
        if (sdl_initialized)
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS |
                              SDL_INIT_GAMEPAD);
    }

    bool ready() const { return window && renderer && font; }

    SDL_Gamepad* controller_for_instance(SDL_JoystickID instance_id) const {
        const auto found = std::find_if(
            controllers.begin(), controllers.end(),
            [instance_id](SDL_Gamepad* controller) {
                return controller &&
                    SDL_GetJoystickID(
                        SDL_GetGamepadJoystick(controller)) ==
                        instance_id;
            });
        return found == controllers.end() ? nullptr : *found;
    }

    void remove_disconnected_controllers() {
        controllers.erase(
            std::remove_if(
                controllers.begin(), controllers.end(),
                [](SDL_Gamepad* controller) {
                    if (controller &&
                        SDL_GamepadConnected(controller))
                        return false;
                    if (controller) SDL_CloseGamepad(controller);
                    return true;
                }),
            controllers.end());
    }

    void open_controller(SDL_JoystickID instance) {
        if (!SDL_IsGamepad(instance)) return;
        if (controller_for_instance(instance)) return;
        if (SDL_Gamepad* controller = SDL_OpenGamepad(instance))
            controllers.push_back(controller);
    }

    void refresh_controllers() {
        remove_disconnected_controllers();
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
            for (int index = 0; index < count; ++index)
                open_controller(ids[index]);
            SDL_free(ids);
        }
    }

    std::vector<launcher_controller_info> controller_list() {
        refresh_controllers();
        std::vector<launcher_controller_info> result;
        result.reserve(controllers.size());
        for (SDL_Gamepad* controller : controllers) {
            SDL_Joystick* joystick =
                SDL_GetGamepadJoystick(controller);
            if (!joystick) continue;
            std::array<char, sdl_guid_text_size> guid{};
            SDL_GUIDToString(SDL_GetJoystickGUID(joystick),
                                      guid.data(),
                                      static_cast<int>(guid.size()));
            const char* name = SDL_GetGamepadName(controller);
            result.push_back({guid.data(), name && *name ? name : "Controller",
                              static_cast<int32_t>(
                                  SDL_GetJoystickID(joystick))});
        }
        return result;
    }

    int hit_row(float mouse_x, float mouse_y, int top, int first, int visible,
                int total) const {
        float logical_x = 0.0f;
        float logical_y = 0.0f;
        SDL_RenderCoordinatesFromWindow(renderer, mouse_x, mouse_y,
                                        &logical_x, &logical_y);
        const float top_y = static_cast<float>(top);
        const float bottom_y =
            static_cast<float>(top + visible * row_height);
        if (logical_x < horizontal_margin ||
            logical_x >= logical_width - horizontal_margin ||
            logical_y < top_y || logical_y >= bottom_y)
            return -1;
        const int row = first +
            static_cast<int>((logical_y - static_cast<float>(top)) / row_height);
        return row >= 0 && row < total ? row : -1;
    }


    // ---- On-screen controls ----------------------------------------------
    //
    // A row of prose along the bottom naming every key is the first thing to
    // read and the last thing anyone needs. These draw the button instead:
    // the pad's own coloured faces and a d-pad, each with one word.

    void fill_disc(int centre_x, int centre_y, int radius, SDL_Color colour) {
        SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b,
                               colour.a);
        for (int dy = -radius; dy <= radius; ++dy) {
            const int half = static_cast<int>(
                std::sqrt(static_cast<double>(radius * radius - dy * dy)));
            const SDL_FRect span = frect(centre_x - half, centre_y + dy,
                                         half * 2 + 1, 1);
            SDL_RenderFillRect(renderer, &span);
        }
    }

    // One hint: either a face button carrying a letter, or the d-pad.
    struct hint {
        std::string letter;   // empty for the d-pad
        std::string label;
    };

    // Xbox-pad colours, because that is what is plugged in and the colour is
    // recognised before the letter is read.
    static SDL_Color face_colour(const std::string& letter) {
        if (letter == "A") return SDL_Color{104, 187, 89, 255};
        if (letter == "B") return SDL_Color{214, 84, 78, 255};
        if (letter == "X") return SDL_Color{74, 150, 226, 255};
        if (letter == "Y") return SDL_Color{226, 182, 66, 255};
        return SDL_Color{110, 128, 142, 255};
    }

    void draw_dpad(int x, int y, int size) {
        SDL_SetRenderDrawColor(renderer, 150, 168, 182, 255);
        const float arm = size / 3.0f;
        const SDL_FRect vertical = frect(x + arm, y, arm, size);
        const SDL_FRect horizontal = frect(x, y + arm, size, arm);
        SDL_RenderFillRect(renderer, &vertical);
        SDL_RenderFillRect(renderer, &horizontal);
    }

    // Draws the hints along the bottom, centred. Returns nothing: hints are
    // decoration, and a screen too narrow for them simply gets fewer.
    void draw_hints(const std::vector<hint>& hints, int y) {
        TTF_Font* small = hint_font ? hint_font : font;
        if (!small) return;
        constexpr int icon = 20;
        constexpr int inner_gap = 7;
        constexpr int between = 22;

        std::vector<rendered_text> labels;
        int total = 0;
        for (const hint& item : hints) {
            labels.push_back(make_text(renderer, small, item.label, muted));
            total += icon + inner_gap + labels.back().width + between;
        }
        int x = std::max(horizontal_margin, (logical_width - total) / 2);
        for (std::size_t index = 0; index < hints.size(); ++index) {
            const hint& item = hints[index];
            if (item.letter.empty()) {
                draw_dpad(x, y, icon);
            } else {
                fill_disc(x + icon / 2, y + icon / 2, icon / 2,
                          face_colour(item.letter));
                rendered_text mark = make_text(renderer, small, item.letter,
                                               SDL_Color{12, 18, 26, 255});
                draw_text(renderer, mark, x + icon / 2 - mark.width / 2,
                          y + icon / 2 - mark.height / 2);
                destroy_text(mark);
            }
            draw_text(renderer, labels[index], x + icon + inner_gap,
                      y + (icon - labels[index].height) / 2);
            x += icon + inner_gap + labels[index].width + between;
            destroy_text(labels[index]);
        }
    }

    // The network state, top right, on every browsing screen.
    void draw_status_chip() {
        if (status_text.empty()) return;
        TTF_Font* small = hint_font ? hint_font : font;
        if (!small) return;
        rendered_text text = make_text(
            renderer, small, status_text,
            status_good ? SDL_Color{150, 226, 170, 255}
                        : SDL_Color{168, 184, 198, 255});
        const int width = text.width + 34;
        const int x = logical_width - horizontal_margin - width;
        SDL_SetRenderDrawColor(renderer, 18, 28, 38, 235);
        const SDL_FRect plate = frect(x, 18, width, 28);
        SDL_RenderFillRect(renderer, &plate);
        fill_disc(x + 15, 32, 5,
                  status_good ? SDL_Color{104, 214, 132, 255}
                              : SDL_Color{120, 136, 150, 255});
        draw_text(renderer, text, x + 26, 32 - text.height / 2);
        destroy_text(text);
    }

    // ---- Cover-art grid ------------------------------------------------
    //
    // Cards are laid out on the artwork's own 3:4 proportions so a cover fills
    // its card exactly and a card with no cover yet is the same size, which
    // stops the grid reflowing as artwork trickles in.
    struct grid_metrics {
        int columns;
        int card_width;
        int card_height;
        int cover_height;
        int left;
        int top;
        int rows_visible;
    };

    static grid_metrics measure_grid(int top) {
        constexpr int gap = 18;
        constexpr int preferred_card = 150;
        const int usable = logical_width - horizontal_margin * 2;
        const int columns =
            std::max(1, (usable + gap) / (preferred_card + gap));
        const int card_width = (usable - gap * (columns - 1)) / columns;
        // IGDB covers are 3:4 and carry the game's name themselves, so the
        // card is nothing but artwork - no caption repeating what the box
        // already says. A card with no cover yet shows its title inside the
        // placeholder instead, at the same size so the grid never reflows.
        const int cover_height = card_width * 4 / 3;
        const int card_height = cover_height;
        const int rows_visible =
            std::max(1, (list_bottom - top + gap) / (card_height + gap));
        return {columns, card_width, card_height, cover_height,
                horizontal_margin, top, rows_visible};
    }

    // Uploaded cover textures, keyed by item index. The source pointer is kept
    // so a cover that arrives later replaces the placeholder rather than being
    // ignored for the life of the menu.
    struct grid_texture {
        SDL_Texture* texture{};
        const uint8_t* source{};
    };

    void destroy_grid_textures(std::map<int, grid_texture>& textures) {
        for (auto& entry : textures)
            if (entry.second.texture) SDL_DestroyTexture(entry.second.texture);
        textures.clear();
    }

    SDL_Texture* grid_texture_for(std::map<int, grid_texture>& textures,
                                  int index,
                                  const launcher_menu::cover& art) {
        auto& entry = textures[index];
        if (entry.texture && entry.source == art.pixels) return entry.texture;
        if (!art.pixels || art.width <= 0 || art.height <= 0) return nullptr;
        if (entry.texture) SDL_DestroyTexture(entry.texture);
        entry.texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
            art.width, art.height);
        entry.source = art.pixels;
        if (!entry.texture) return nullptr;
        SDL_SetTextureBlendMode(entry.texture, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(entry.texture, nullptr, art.pixels, art.width * 4);
        return entry.texture;
    }

    // wanted == 0 picks one item and returns it; otherwise cards are held
    // until that many are chosen and all of them are returned.
    std::vector<int> run_grid(
            const std::string& title, const std::string& description,
            const std::vector<std::string>& items,
            const std::string& back_label, int initial_selection,
            const std::function<launcher_menu::cover(int)>& cover_for,
            const std::function<bool()>& tick, int wanted,
            const std::function<bool()>& interrupt = {},
            bool paging = false,
            const launcher_menu::cover* banner = nullptr,
            bool info = false) {
        const int total = static_cast<int>(items.size());
        std::vector<int> chosen;
        if (total == 0) return chosen;
        int selected = std::clamp(initial_selection, 0, total - 1);

        rendered_text detail = make_text(
            renderer, font, description, muted,
            logical_width - horizontal_margin * 2);
        // A banner header carries a large title, the chip row and the
        // description, so the grid starts lower than on a plain page.
        const int grid_top = banner ?
            std::max(178, 120 + detail.height + 14) :
            std::max(116, 58 + detail.height + 18);
        destroy_text(detail);

        std::map<int, grid_texture> textures;
        int first_row = 0;
        // Lives across events: an analogue stick's position only means
        // something relative to where it was.
        menu_stick sticks;
        bool redraw = true;
        for (;;) {
            const grid_metrics metrics = measure_grid(grid_top);
            const int row = selected / metrics.columns;
            if (row < first_row) first_row = row;
            if (row >= first_row + metrics.rows_visible)
                first_row = row - metrics.rows_visible + 1;
            const int total_rows =
                (total + metrics.columns - 1) / metrics.columns;
            first_row = std::clamp(
                first_row, 0, std::max(0, total_rows - metrics.rows_visible));

            if (redraw) {
                draw_grid(title, description, items, selected, first_row,
                          metrics, textures, cover_for, back_label, chosen,
                          wanted, paging, banner, info);
                redraw = false;
            }

            SDL_Event event{};
            if (interrupt && interrupt()) {
                chosen.assign(1, launcher_menu::interrupted);
                break;
            }
            if (!SDL_WaitEventTimeout(
                    &event, (tick || interrupt) ? 100 : -1)) {
                // Timed out: artwork may have arrived while nothing happened.
                if (tick && tick()) redraw = true;
                continue;
            }
            if (tick && tick()) redraw = true;
            if (event.type == SDL_EVENT_QUIT) break;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                open_controller(event.gdevice.which);
                continue;
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                remove_disconnected_controllers();
                continue;
            }
            if (event.type >= SDL_EVENT_WINDOW_FIRST &&
                event.type <= SDL_EVENT_WINDOW_LAST) {
                redraw = true;
                continue;
            }

            int horizontal = 0;
            int vertical = 0;
            bool activate = false;
            bool back = false;
            bool view = false;
            bool want_info = false;
            int page = 0;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                switch (event.key.key) {
                case SDLK_LEFT: horizontal = -1; break;
                case SDLK_RIGHT: horizontal = 1; break;
                case SDLK_UP: vertical = -1; break;
                case SDLK_DOWN: vertical = 1; break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE: activate = true; break;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE: back = true; break;
                case SDLK_TAB: view = wanted <= 0; break;
                case SDLK_PAGEUP: page = paging ? -1 : 0; break;
                case SDLK_PAGEDOWN: page = paging ? 1 : 0; break;
                case SDLK_I: want_info = info; break;
                default: break;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: horizontal = -1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: horizontal = 1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_UP: vertical = -1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: vertical = 1; break;
                case SDL_GAMEPAD_BUTTON_SOUTH: activate = true; break;
                case SDL_GAMEPAD_BUTTON_EAST: back = true; break;
                case SDL_GAMEPAD_BUTTON_NORTH: view = wanted <= 0; break;
                case SDL_GAMEPAD_BUTTON_WEST: want_info = info; break;
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                    page = paging ? -1 : 0;
                    break;
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                    page = paging ? 1 : 0;
                    break;
                default: break;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                const stick_step step = stick_movement(sticks, event);
                horizontal = step.horizontal;
                vertical = step.vertical;
            }

            if (want_info) {
                chosen.assign(1, launcher_menu::info_request);
                break;
            }
            if (view) {
                chosen.assign(1, launcher_menu::view_change);
                break;
            }
            if (page != 0) {
                chosen.assign(1, page < 0 ? launcher_menu::page_back
                                          : launcher_menu::page_forward);
                break;
            }
            if (back) { chosen.clear(); break; }
            if (activate) {
                if (wanted <= 0) { chosen.assign(1, selected); break; }
                const auto held =
                    std::find(chosen.begin(), chosen.end(), selected);
                // Choosing a held card releases it, so a mistake is undone
                // with the same button that made it.
                if (held != chosen.end()) chosen.erase(held);
                else if (static_cast<int>(chosen.size()) < wanted)
                    chosen.push_back(selected);
                if (static_cast<int>(chosen.size()) == wanted) break;
                redraw = true;
            }
            if (horizontal) {
                selected = std::clamp(selected + horizontal, 0, total - 1);
                redraw = true;
            }
            if (vertical) {
                const int moved = selected + vertical * metrics.columns;
                if (moved >= 0 && moved < total) selected = moved;
                redraw = true;
            }
        }
        destroy_grid_textures(textures);
        return chosen;
    }

    void draw_grid(const std::string& title, const std::string& description,
                   const std::vector<std::string>& items, int selected,
                   int first_row, const grid_metrics& metrics,
                   std::map<int, grid_texture>& textures,
                   const std::function<launcher_menu::cover(int)>& cover_for,
                   const std::string& back_label,
                   const std::vector<int>& chosen, int wanted,
                   bool paging = false,
                   const launcher_menu::cover* banner = nullptr,
                   bool info = false) {
        constexpr int gap = 18;
        SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
        SDL_RenderClear(renderer);

        if (banner) {
            // A banner page: the board photo or publisher logo occupies a
            // reserved column on the right, and the title, the "i" chip and
            // the description are laid out inside the column that is left.
            // Text and image never share pixels - measuring the artwork
            // first is what stops it landing on top of the words.
            const int art_height = 96;
            int art_width = 0;
            if (banner->pixels && banner->width > 0 && banner->height > 0) {
                art_width = art_height * banner->width /
                            std::max(banner->height, 1);
                art_width = std::min(art_width,
                                     static_cast<int>(logical_width * 0.34f));
            }
            const int text_width =
                logical_width - horizontal_margin * 2 -
                (art_width > 0 ? art_width + 28 : 0);
            if (art_width > 0) {
                SDL_Texture* art = SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_RGBA32,
                    SDL_TEXTUREACCESS_STATIC, banner->width, banner->height);
                if (art) {
                    SDL_UpdateTexture(art, nullptr, banner->pixels,
                                      banner->width * 4);
                    SDL_SetTextureScaleMode(art, SDL_SCALEMODE_LINEAR);
                    // Logos arrive with transparent backgrounds.
                    SDL_SetTextureBlendMode(art, SDL_BLENDMODE_BLEND);
                    const SDL_FRect where = frect(
                        logical_width - horizontal_margin - art_width, 14,
                        static_cast<float>(art_width),
                        static_cast<float>(art_height));
                    SDL_RenderTexture(renderer, art, nullptr, &where);
                    SDL_DestroyTexture(art);
                }
            }
            rendered_text big = make_text(
                renderer, title_font ? title_font : font, title, accent,
                text_width);
            draw_text(renderer, big, horizontal_margin, 16);
            const int title_bottom = 16 + big.height;
            destroy_text(big);
            // The chip sits on its own line under the title, at a fixed
            // place: hanging it off the end of wrapped text put it wherever
            // the words happened to stop.
            int detail_x = horizontal_margin;
            if (info) {
                SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
                const SDL_FRect chip = frect(horizontal_margin,
                                             title_bottom + 6, 26, 26);
                SDL_RenderFillRect(renderer, &chip);
                rendered_text mark =
                    make_text(renderer, font, "i", SDL_Color{8, 13, 20, 255});
                draw_text(renderer, mark,
                          horizontal_margin + 13 - mark.width / 2,
                          title_bottom + 8);
                destroy_text(mark);
                detail_x += 36;
            }
            rendered_text detail = make_text(
                renderer, font, description, muted,
                text_width - (detail_x - horizontal_margin));
            draw_text(renderer, detail, detail_x, title_bottom + 10);
            destroy_text(detail);
        } else {
            rendered_text heading = make_text(renderer, font, title, accent);
            draw_text(renderer, heading, horizontal_margin, 22);
            destroy_text(heading);
            rendered_text detail = make_text(
                renderer, font, description, muted,
                logical_width - horizontal_margin * 2);
            draw_text(renderer, detail, horizontal_margin, 58);
            destroy_text(detail);
        }

        const int total = static_cast<int>(items.size());
        for (int row = 0; row < metrics.rows_visible; ++row) {
            for (int column = 0; column < metrics.columns; ++column) {
                const int index =
                    (first_row + row) * metrics.columns + column;
                if (index >= total) break;
                const int x = metrics.left +
                    column * (metrics.card_width + gap);
                const int y = metrics.top +
                    row * (metrics.card_height + gap);
                const bool active = index == selected;
                const auto held =
                    std::find(chosen.begin(), chosen.end(), index);
                const bool taken = held != chosen.end();

                const SDL_FRect card{
                    static_cast<float>(x - 4), static_cast<float>(y - 4),
                    static_cast<float>(metrics.card_width + 8),
                    static_cast<float>(metrics.card_height + 8)};
                if (active || taken) {
                    // A held card keeps a visible frame even once the cursor
                    // has moved on, which is the whole point of picking
                    // several before starting.
                    if (active)
                        SDL_SetRenderDrawColor(renderer, accent.r, accent.g,
                                               accent.b, 255);
                    else
                        SDL_SetRenderDrawColor(renderer, 34, 128, 158, 255);
                    SDL_RenderFillRect(renderer, &card);
                }
                const SDL_FRect art{
                    static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(metrics.card_width),
                    static_cast<float>(metrics.cover_height)};
                SDL_Texture* texture = grid_texture_for(
                    textures, index, cover_for ? cover_for(index)
                                               : launcher_menu::cover{});
                if (texture) {
                    SDL_RenderTexture(renderer, texture, nullptr, &art);
                } else {
                    // No artwork yet: a plain plate so the grid keeps its
                    // shape and the title still reads.
                    SDL_SetRenderDrawColor(renderer, 19, 30, 41, 255);
                    SDL_RenderFillRect(renderer, &art);
                }
                if (taken) {
                    // The number is the screen it will run on.
                    const int slot = static_cast<int>(
                        std::distance(chosen.begin(), held)) + 1;
                    rendered_text badge = make_text(
                        renderer, font, std::to_string(slot), white);
                    const SDL_FRect plate{
                        static_cast<float>(x + 6), static_cast<float>(y + 6),
                        28.0f, 28.0f};
                    SDL_SetRenderDrawColor(renderer, 8, 13, 20, 220);
                    SDL_RenderFillRect(renderer, &plate);
                    draw_text(renderer, badge, x + 16, y + 8);
                    destroy_text(badge);
                }
                if (!texture) {
                    // No artwork: the title has to carry the card.
                    rendered_text label = make_text(
                        renderer, font,
                        items[static_cast<std::size_t>(index)],
                        active || taken ? white : muted,
                        metrics.card_width - 16);
                    draw_text(renderer, label, x + 8, y + 10);
                    destroy_text(label);
                }
            }
        }

        std::vector<hint> hints{{"", "Move"}};
        hints.push_back({"A", wanted > 0 ?
            "Hold (" + std::to_string(chosen.size()) + " of " +
                std::to_string(wanted) + ")" : std::string("Play")});
        if (wanted <= 0) hints.push_back({"Y", "View"});
        if (info) hints.push_back({"X", "Board Info"});
        if (paging) hints.push_back({"", "Page"});
        hints.push_back({"B", back_label});
        draw_hints(hints, list_bottom + 18);
        draw_status_chip();
        SDL_RenderPresent(renderer);
    }

    void draw_frame(const std::string& title,
                    const std::string& description,
                    const std::vector<std::string>& rows, int selected,
                    int first, int visible, int list_top,
                    // Empty means the ordinary three, drawn as buttons.
                    // Screens with something unusual to say still pass prose.
                    const std::string& footer = {}) {
        SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
        const SDL_FRect top_border = frect(0, 0, logical_width, 5);
        const SDL_FRect bottom_border =
            frect(0, logical_height - 5, logical_width, 5);
        SDL_RenderFillRect(renderer, &top_border);
        SDL_RenderFillRect(renderer, &bottom_border);

        rendered_text heading = make_text(renderer, font, title, accent);
        rendered_text detail = make_text(
            renderer, font, description, muted,
            logical_width - horizontal_margin * 2);
        draw_text(renderer, heading, horizontal_margin, 20);
        draw_text(renderer, detail, horizontal_margin, 58);

        const SDL_Rect label_clip{horizontal_margin + 24, list_top,
                                  logical_width - horizontal_margin * 2 - 36,
                                  visible * row_height};
        const int end = std::min(static_cast<int>(rows.size()), first + visible);
        for (int index = first; index < end; ++index) {
            const int y = list_top + (index - first) * row_height;
            if (index == selected) {
                SDL_SetRenderDrawColor(renderer, 19, 75, 101, 245);
                const SDL_FRect highlight =
                    frect(horizontal_margin, y + 2,
                          logical_width - horizontal_margin * 2,
                          row_height - 4);
                SDL_RenderFillRect(renderer, &highlight);
            }
            rendered_text label = make_text(renderer, font,
                (index == selected ? ">  " : "   ") + rows[index],
                index == selected ? accent : white);
            draw_text(renderer, label, horizontal_margin + 12, y + 11,
                      &label_clip);
            destroy_text(label);
        }
        if (first > 0) {
            rendered_text more = make_text(renderer, font, "MORE ABOVE", muted);
            draw_text(renderer, more, logical_width - more.width - 34,
                      list_top - 27);
            destroy_text(more);
        }
        if (end < static_cast<int>(rows.size())) {
            rendered_text more = make_text(renderer, font, "MORE BELOW", muted);
            draw_text(renderer, more, logical_width - more.width - 34,
                      list_bottom + 4);
            destroy_text(more);
        }
        if (footer.empty()) {
            draw_hints({{"", "Move"}, {"A", "Open"}, {"B", "Back"}}, 676);
        } else {
            rendered_text controls = make_text(
                renderer, hint_font ? hint_font : font, footer, muted);
            draw_text(renderer, controls, horizontal_margin, 680);
            destroy_text(controls);
        }
        draw_status_chip();
        destroy_text(detail);
        destroy_text(heading);
        SDL_RenderPresent(renderer);
    }

    // The boot screen: the name, what is being loaded, and a bar. Drawn and
    // presented immediately - it exists to fill the wait, so it must not wait
    // on anything itself.
    void splash(const std::string& step, float progress) {
        SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
        SDL_RenderClear(renderer);
        // A slab of colour behind the name, so the boot screen reads as a
        // cabinet warming up rather than a progress dialog.
        SDL_SetRenderDrawColor(renderer, 14, 26, 38, 255);
        const SDL_FRect band = frect(0, logical_height / 2 - 150,
                                     logical_width, 210);
        SDL_RenderFillRect(renderer, &band);
        SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
        const SDL_FRect rule = frect(0, logical_height / 2 - 150,
                                     logical_width, 4);
        SDL_RenderFillRect(renderer, &rule);

        rendered_text name = make_text(
            renderer, title_font ? title_font : font, "WHITTY ARCADE", accent);
        draw_text(renderer, name, (logical_width - name.width) / 2,
                  logical_height / 2 - 118);
        destroy_text(name);
        rendered_text tag = make_text(
            renderer, font, "INSERT COIN", muted);
        draw_text(renderer, tag, (logical_width - tag.width) / 2,
                  logical_height / 2 - 58);
        destroy_text(tag);

        const int bar_width = std::min(560, logical_width - 120);
        const int bar_x = (logical_width - bar_width) / 2;
        const int bar_y = logical_height / 2 + 96;
        SDL_SetRenderDrawColor(renderer, 26, 40, 54, 255);
        const SDL_FRect trough = frect(bar_x, bar_y, bar_width, 14);
        SDL_RenderFillRect(renderer, &trough);
        SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
        const SDL_FRect fill = frect(
            bar_x, bar_y,
            bar_width * std::clamp(progress, 0.0f, 1.0f), 14);
        SDL_RenderFillRect(renderer, &fill);

        if (!step.empty()) {
            rendered_text what = make_text(
                renderer, hint_font ? hint_font : font, step, muted);
            draw_text(renderer, what, (logical_width - what.width) / 2,
                      bar_y + 28);
            destroy_text(what);
        }
        SDL_RenderPresent(renderer);
        // Keeps the window alive with the compositor while a long load runs;
        // without it the boot screen is shown but reported as unresponsive.
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {}
    }

    int select(const std::string& title, const std::string& description,
               const std::vector<std::string>& items,
               const std::string& back_label, int initial_selection,
               const std::function<bool()>& interrupt = {}) {
        std::vector<std::string> rows = items;
        rows.push_back(back_label);
        const int total = static_cast<int>(rows.size());
        int selected = items.empty() || initial_selection < 0 ? total - 1 :
            std::clamp(initial_selection, 0,
                       static_cast<int>(items.size()) - 1);

        rendered_text detail = make_text(
            renderer, font, description, muted,
            logical_width - horizontal_margin * 2);
        const int list_top = std::max(116, 58 + detail.height + 18);
        destroy_text(detail);
        const int visible = std::max(1, (list_bottom - list_top) / row_height);
        int first = 0;
        // Lives across events: an analogue stick's position only means
        // something relative to where it was.
        menu_stick sticks;
        bool redraw = true;

        for (;;) {
            if (selected < first) first = selected;
            if (selected >= first + visible) first = selected - visible + 1;
            first = std::clamp(first, 0, std::max(0, total - visible));
            if (redraw) {
                draw_frame(title, description, rows, selected, first, visible,
                           list_top);
                redraw = false;
            }

            if (interrupt && interrupt())
                return launcher_menu::interrupted;
            SDL_Event event{};
            if (!SDL_WaitEventTimeout(&event, interrupt ? 100 : -1))
                continue;
            if (event.type == SDL_EVENT_QUIT) return -1;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                open_controller(event.gdevice.which);
                continue;
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                remove_disconnected_controllers();
                continue;
            }
            if (event.type >= SDL_EVENT_WINDOW_FIRST &&
                event.type <= SDL_EVENT_WINDOW_LAST) {
                redraw = true;
                continue;
            }
            int movement = 0;
            bool activate = false;
            bool back = false;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                switch (event.key.key) {
                case SDLK_UP: movement = -1; break;
                case SDLK_DOWN: movement = 1; break;
                case SDLK_PAGEUP: movement = -visible; break;
                case SDLK_PAGEDOWN: movement = visible; break;
                case SDLK_HOME: selected = 0; redraw = true; break;
                case SDLK_END: selected = total - 1; redraw = true; break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE: activate = true; break;
                case SDLK_ESCAPE: back = true; break;
                default: break;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: movement = -1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: movement = 1; break;
                case SDL_GAMEPAD_BUTTON_SOUTH:
                case SDL_GAMEPAD_BUTTON_START: activate = true; break;
                case SDL_GAMEPAD_BUTTON_EAST: back = true; break;
                default: break;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                movement = stick_movement(sticks, event).vertical;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                movement = event.wheel.y > 0 ? -1 : event.wheel.y < 0 ? 1 : 0;
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                const int row = hit_row(event.motion.x, event.motion.y,
                                        list_top, first, visible, total);
                if (row >= 0 && row != selected) {
                    selected = row;
                    redraw = true;
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                const int row = hit_row(event.button.x, event.button.y,
                                        list_top, first, visible, total);
                if (row >= 0) {
                    selected = row;
                    activate = true;
                }
            }
            if (back) return -1;
            if (movement != 0) {
                selected = (selected + movement) % total;
                if (selected < 0) selected += total;
                redraw = true;
            }
            if (activate)
                return selected < static_cast<int>(items.size()) ? selected : -1;
        }
    }

    std::optional<input_binding> capture_binding(
        const std::string& title, const std::string& description,
        bool keyboard, int32_t controller_instance, bool allow_inherit) {
        SDL_Gamepad* target = keyboard ? nullptr :
            controller_for_instance(controller_instance);
        if (!keyboard && !target) return std::nullopt;

        std::array<int, SDL_GAMEPAD_AXIS_COUNT> baseline{};
        if (target) {
            for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
                baseline[static_cast<std::size_t>(axis)] =
                    SDL_GetGamepadAxis(
                        target, static_cast<SDL_GamepadAxis>(axis));
            }
        }

        const std::vector<std::string> rows{
            keyboard ? "Press the key now" :
                       "Press a button or move an axis now"};
        bool redraw = true;
        for (;;) {
            if (redraw) {
                draw_frame(
                    title, description, rows, 0, 0, 1, 152,
                    allow_inherit ?
                        "ESC: CANCEL    DELETE: CLEAR    BACKSPACE: USE GENERAL" :
                        "ESC: CANCEL    DELETE/BACKSPACE: CLEAR MAPPING");
                redraw = false;
            }

            SDL_Event event{};
            if (!SDL_WaitEvent(&event)) continue;
            if (event.type == SDL_EVENT_QUIT) return std::nullopt;
            if (event.type >= SDL_EVENT_WINDOW_FIRST &&
                event.type <= SDL_EVENT_WINDOW_LAST) {
                redraw = true;
                continue;
            }
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                open_controller(event.gdevice.which);
                continue;
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                remove_disconnected_controllers();
                if (!keyboard &&
                    event.gdevice.which ==
                        static_cast<SDL_JoystickID>(controller_instance))
                    return std::nullopt;
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE)
                    return std::nullopt;
                if (event.key.scancode == SDL_SCANCODE_DELETE)
                    return input_binding{};
                if (event.key.scancode == SDL_SCANCODE_BACKSPACE)
                    return allow_inherit ?
                        input_binding{input_binding_type::inherit, -1, 0} :
                        input_binding{};
                if (keyboard) {
                    return input_binding{
                        input_binding_type::keyboard,
                        static_cast<int>(event.key.scancode), 0};
                }
                continue;
            }
            if (keyboard ||
                (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                 event.gbutton.which !=
                     static_cast<SDL_JoystickID>(controller_instance)))
                continue;
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                return input_binding{
                    input_binding_type::controller_button,
                    static_cast<int>(event.gbutton.button), 0};
            }
            if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                event.gaxis.which ==
                    static_cast<SDL_JoystickID>(controller_instance) &&
                event.gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
                const int axis = static_cast<int>(event.gaxis.axis);
                const int delta = static_cast<int>(event.gaxis.value) -
                    baseline[static_cast<std::size_t>(axis)];
                constexpr int capture_threshold = 16000;
                if (std::abs(delta) >= capture_threshold) {
                    return input_binding{
                        input_binding_type::controller_axis, axis,
                        delta < 0 ? -1 : 1};
                }
            }
        }
    }

    void show_text(const std::string& title, const std::string& body,
                   const std::string& back_label) {
        rendered_text heading = make_text(renderer, font, title, accent);
        rendered_text text = make_text(
            renderer, font, body, white,
            logical_width - horizontal_margin * 2 - 20);
        rendered_text back = make_text(renderer, font, ">  " + back_label,
                                       accent);
        constexpr int body_top = 70;
        constexpr int body_bottom = 624;
        constexpr int body_height = body_bottom - body_top;
        int scroll = 0;
        // Lives across events: an analogue stick's position only means
        // something relative to where it was.
        menu_stick sticks;
        bool redraw = true;
        for (;;) {
            if (redraw) {
                SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
                SDL_RenderClear(renderer);
                SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
                const SDL_FRect top_border = frect(0, 0, logical_width, 5);
                const SDL_FRect bottom_border =
                    frect(0, logical_height - 5, logical_width, 5);
                SDL_RenderFillRect(renderer, &top_border);
                SDL_RenderFillRect(renderer, &bottom_border);
                draw_text(renderer, heading, horizontal_margin, 20);
                if (text.texture) {
                    const int shown = std::min(body_height, text.height - scroll);
                    if (shown > 0) {
                        const SDL_FRect source =
                            frect(0, scroll, text.width, shown);
                        const SDL_FRect destination =
                            frect(horizontal_margin + 10, body_top,
                                  text.width, shown);
                        SDL_RenderTexture(renderer, text.texture, &source,
                                          &destination);
                    }
                }
                SDL_SetRenderDrawColor(renderer, 19, 75, 101, 245);
                const SDL_FRect highlight =
                    frect(horizontal_margin, 636,
                          logical_width - horizontal_margin * 2, 42);
                SDL_RenderFillRect(renderer, &highlight);
                draw_text(renderer, back, horizontal_margin + 12, 643);
                if (text.height > body_height) {
                    rendered_text hint = make_text(
                        renderer, font, "UP/DOWN: SCROLL", muted);
                    draw_text(renderer, hint, logical_width - hint.width - 34,
                              28);
                    destroy_text(hint);
                }
                SDL_RenderPresent(renderer);
                redraw = false;
            }

            SDL_Event event{};
            if (!SDL_WaitEvent(&event)) continue;
            if (event.type == SDL_EVENT_QUIT) break;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                open_controller(event.gdevice.which);
                continue;
            }
            if (event.type >= SDL_EVENT_WINDOW_FIRST &&
                event.type <= SDL_EVENT_WINDOW_LAST) {
                redraw = true;
                continue;
            }
            int movement = 0;
            bool close = false;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                switch (event.key.key) {
                case SDLK_UP: movement = -32; break;
                case SDLK_DOWN: movement = 32; break;
                case SDLK_PAGEUP: movement = -body_height; break;
                case SDLK_PAGEDOWN: movement = body_height; break;
                case SDLK_HOME: scroll = 0; redraw = true; break;
                case SDLK_END: scroll = std::max(0, text.height - body_height);
                               redraw = true; break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                case SDLK_ESCAPE: close = true; break;
                default: break;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP)
                    movement = -32;
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                    movement = 32;
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                         event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
                         event.gbutton.button == SDL_GAMEPAD_BUTTON_START)
                    close = true;
            } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                movement = stick_movement(sticks, event).vertical * 32;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                movement = event.wheel.y > 0 ? -64 : event.wheel.y < 0 ? 64 : 0;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                float logical_x = 0.0f;
                float logical_y = 0.0f;
                SDL_RenderCoordinatesFromWindow(renderer, event.button.x,
                                                event.button.y, &logical_x,
                                                &logical_y);
                close = logical_y >= 636 && logical_y < 678 &&
                        logical_x >= horizontal_margin &&
                        logical_x < logical_width - horizontal_margin;
            }
            if (close) break;
            if (movement) {
                scroll = std::clamp(scroll + movement, 0,
                                    std::max(0, text.height - body_height));
                redraw = true;
            }
        }
        destroy_text(back);
        destroy_text(text);
        destroy_text(heading);
    }

    // The high-score board, drawn like the arcade ranking screens it
    // celebrates: rows sweep in one after another, the champion's medal
    // pulses, and left/right pages between games. Text is rendered once per
    // page and animated as textures, so the motion costs almost nothing.
    int show_scoreboard(
        const std::string& back_label, const std::string& description,
        int count,
        const std::function<launcher_menu::scoreboard_page(int)>&
            page_provider,
        int current) {
        if (count < 1) return -1;
        current = std::clamp(current, 0, count - 1);
        constexpr SDL_Color gold{255, 203, 70, 255};
        constexpr SDL_Color silver{200, 207, 218, 255};
        constexpr SDL_Color bronze{212, 138, 82, 255};
        constexpr SDL_Color chip_text{16, 22, 30, 255};

        struct row_art {
            rendered_text rank;
            rendered_text name;
            rendered_text score;
            int rank_value{};
        };
        launcher_menu::scoreboard_page page;
        rendered_text heading{};
        rendered_text subtitle{};
        rendered_text message{};
        rendered_text pager{};
        std::vector<row_art> rows;
        std::vector<std::pair<rendered_text, rendered_text>> extras;
        const int card_width = 720;
        const int card_x = (logical_width - card_width) / 2;

        const auto destroy_page = [&] {
            destroy_text(heading);
            destroy_text(subtitle);
            destroy_text(message);
            destroy_text(pager);
            for (row_art& row : rows) {
                destroy_text(row.rank);
                destroy_text(row.name);
                destroy_text(row.score);
            }
            rows.clear();
            for (auto& extra : extras) {
                destroy_text(extra.first);
                destroy_text(extra.second);
            }
            extras.clear();
        };
        const auto build_page = [&] {
            destroy_page();
            page = page_provider(current);
            heading = make_text(renderer, font, page.title, accent);
            if (!page.subtitle.empty())
                subtitle = make_text(renderer, font, page.subtitle, muted);
            if (!page.message.empty())
                message = make_text(renderer, font, page.message, white,
                                    card_width - 40);
            const std::string counter = count > 1 ?
                description + "  \u00b7  " + std::to_string(current + 1) +
                    " / " + std::to_string(count) +
                    "  \u00b7  \u2190 \u2192 next game" :
                description;
            pager = make_text(renderer, font, counter, muted,
                              logical_width - horizontal_margin * 2);
            const int shown =
                std::min<int>(static_cast<int>(page.rows.size()), 10);
            for (int index = 0; index < shown; ++index) {
                const auto& entry =
                    page.rows[static_cast<std::size_t>(index)];
                row_art art;
                art.rank_value = entry.rank;
                const SDL_Color* medal =
                    entry.rank == 1 ? &gold :
                    entry.rank == 2 ? &silver :
                    entry.rank == 3 ? &bronze : nullptr;
                art.rank = make_text(renderer, font,
                                     std::to_string(entry.rank),
                                     medal ? chip_text : muted);
                if (!entry.name.empty())
                    art.name = make_text(renderer, font, entry.name, white);
                art.score = make_text(renderer, font, entry.score,
                                      medal ? *medal : white);
                rows.push_back(art);
            }
            for (const auto& extra : page.extras)
                extras.emplace_back(
                    make_text(renderer, font, extra.first, muted),
                    make_text(renderer, font, extra.second, accent));
        };
        build_page();
        uint64_t animation_start = SDL_GetTicks();
        menu_stick sticks;

        const auto draw_with_alpha = [&](rendered_text& text, int x, int y,
                                         uint8_t alpha) {
            if (!text.texture) return;
            SDL_SetTextureAlphaMod(text.texture, alpha);
            draw_text(renderer, text, x, y);
            SDL_SetTextureAlphaMod(text.texture, 255);
        };

        int result = -1;
        for (bool running = true; running;) {
            const uint64_t now = SDL_GetTicks();
            const float elapsed =
                static_cast<float>(now - animation_start);

            SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
            const SDL_FRect top_border = frect(0, 0, logical_width, 5);
            const SDL_FRect bottom_border =
                frect(0, logical_height - 5, logical_width, 5);
            SDL_RenderFillRect(renderer, &top_border);
            SDL_RenderFillRect(renderer, &bottom_border);
            draw_text(renderer, heading, horizontal_margin, 20);
            draw_text(renderer, pager, horizontal_margin, 58);

            // A sweep of light runs under the title once per page.
            const float sweep = elapsed / 700.0f;
            if (sweep < 1.0f) {
                SDL_SetRenderDrawColor(renderer, 56, 198, 255, 220);
                const SDL_FRect underline = frect(
                    horizontal_margin, 48,
                    (logical_width - horizontal_margin * 2) * sweep, 3);
                SDL_RenderFillRect(renderer, &underline);
            }

            int y = 108;
            SDL_SetRenderDrawColor(renderer, 13, 21, 32, 255);
            const SDL_FRect card =
                frect(card_x - 18, y - 8, card_width + 36, 520);
            SDL_RenderFillRect(renderer, &card);
            SDL_SetRenderDrawColor(renderer, 34, 84, 110, 255);
            const SDL_FRect card_top =
                frect(card_x - 18, y - 8, card_width + 36, 2);
            SDL_RenderFillRect(renderer, &card_top);
            if (subtitle.texture) {
                draw_text(renderer, subtitle,
                          card_x + (card_width - subtitle.width) / 2, y);
                y += 40;
            }
            if (message.texture) {
                draw_with_alpha(message, card_x + 20, y + 16,
                                static_cast<uint8_t>(std::min(
                                    255.0f, elapsed * 0.5f)));
            }
            const int row_height = 40;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                // Each rank sweeps in from the right, one after another,
                // the way the arcade ranking screens built their tables.
                const float begin = 140.0f + 90.0f * index;
                if (elapsed < begin) break;
                const float progress =
                    std::min(1.0f, (elapsed - begin) / 260.0f);
                const float ease = 1.0f - (1.0f - progress) *
                                              (1.0f - progress);
                const int slide = static_cast<int>((1.0f - ease) * 260.0f);
                const uint8_t alpha =
                    static_cast<uint8_t>(255.0f * ease);
                const int row_y =
                    y + static_cast<int>(index) * row_height;
                row_art& art = rows[index];
                if ((index & 1) != 0) {
                    SDL_SetRenderDrawColor(renderer, 17, 27, 40, 255);
                    const SDL_FRect stripe =
                        frect(card_x - 6, row_y - 3, card_width + 12,
                              row_height - 4);
                    SDL_RenderFillRect(renderer, &stripe);
                }
                const SDL_Color* medal =
                    art.rank_value == 1 ? &gold :
                    art.rank_value == 2 ? &silver :
                    art.rank_value == 3 ? &bronze : nullptr;
                if (medal) {
                    // The champion's medal breathes; the others hold steady.
                    float pulse = 1.0f;
                    if (art.rank_value == 1)
                        pulse = 0.82f + 0.18f * (0.5f + 0.5f * std::sin(
                            static_cast<float>(now) / 260.0f));
                    SDL_SetRenderDrawColor(
                        renderer,
                        static_cast<uint8_t>(medal->r * pulse),
                        static_cast<uint8_t>(medal->g * pulse),
                        static_cast<uint8_t>(medal->b * pulse), alpha);
                    const SDL_FRect chip = frect(card_x + slide, row_y - 1,
                                                 34, row_height - 8);
                    SDL_RenderFillRect(renderer, &chip);
                }
                draw_with_alpha(art.rank,
                                card_x + slide + (34 - art.rank.width) / 2,
                                row_y + 1, alpha);
                if (art.name.texture)
                    draw_with_alpha(art.name, card_x + slide + 52,
                                    row_y + 1, alpha);
                draw_with_alpha(art.score,
                                card_x + slide + card_width -
                                    art.score.width,
                                row_y + 1, alpha);
            }
            {
                const int extras_begin =
                    y + static_cast<int>(rows.size()) * row_height;
                const float extras_at =
                    140.0f + 90.0f * rows.size() + 120.0f;
                if (elapsed >= extras_at) {
                    int extra_y = extras_begin + 6;
                    for (auto& extra : extras) {
                        draw_text(renderer, extra.first, card_x + 52,
                                  extra_y);
                        draw_text(renderer, extra.second,
                                  card_x + card_width - extra.second.width,
                                  extra_y);
                        extra_y += 34;
                    }
                }
            }

            SDL_SetRenderDrawColor(renderer, 19, 75, 101, 245);
            const SDL_FRect back_highlight =
                frect(horizontal_margin, 636,
                      logical_width - horizontal_margin * 2, 42);
            SDL_RenderFillRect(renderer, &back_highlight);
            rendered_text back =
                make_text(renderer, font, ">  " + back_label, accent);
            draw_text(renderer, back, horizontal_margin + 12, 643);
            destroy_text(back);
            rendered_text controls = make_text(
                renderer, font,
                "LEFT/RIGHT: GAME    ESC/ENTER: BACK", muted);
            draw_text(renderer, controls, horizontal_margin, 680);
            destroy_text(controls);
            SDL_RenderPresent(renderer);

            SDL_Event event{};
            // The table keeps breathing, so the loop ticks rather than
            // blocking; 30ms keeps the pulse smooth and the CPU asleep.
            if (!SDL_WaitEventTimeout(&event, 30)) continue;
            do {
                if (event.type == SDL_EVENT_QUIT) { running = false; break; }
                if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                    open_controller(event.gdevice.which);
                    continue;
                }
                int direction = 0;
                bool closed = false;
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    switch (event.key.key) {
                    case SDLK_LEFT: direction = -1; break;
                    case SDLK_RIGHT: direction = 1; break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                    case SDLK_ESCAPE:
                    case SDLK_BACKSPACE: closed = true; break;
                    default: break;
                    }
                } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    switch (event.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: direction = -1; break;
                    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: direction = 1; break;
                    case SDL_GAMEPAD_BUTTON_SOUTH:
                    case SDL_GAMEPAD_BUTTON_EAST:
                    case SDL_GAMEPAD_BUTTON_START: closed = true; break;
                    default: break;
                    }
                } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                    direction = stick_movement(sticks, event).horizontal;
                }
                if (closed) { running = false; break; }
                if (direction != 0) {
                    const int next =
                        std::clamp(current + direction, 0, count - 1);
                    if (next != current) {
                        current = next;
                        build_page();
                        animation_start = SDL_GetTicks();
                    }
                }
            } while (SDL_PollEvent(&event));
        }
        destroy_page();
        // Backing out is the only way this view ends, so the answer is
        // always "done": the caller must not reopen it. (Returning the page
        // index here is what made Escape appear to do nothing.)
        return result;
    }

    int show_pages(
        const std::string& back_label, const std::string& description,
        int count,
        const std::function<std::pair<std::string, std::string>(int)>&
            page_provider,
        int current) {
        if (count < 1) return -1;
        current = std::clamp(current, 0, count - 1);
        auto [title, body] = page_provider(current);
        rendered_text heading = make_text(renderer, font, title, accent);
        rendered_text text = make_text(
            renderer, font, body, white,
            logical_width - horizontal_margin * 2 - 20);
        rendered_text back = make_text(renderer, font, ">  " + back_label,
                                       accent);
        constexpr int body_top = 106;
        constexpr int body_bottom = 620;
        constexpr int body_height = body_bottom - body_top;
        int scroll = 0;
        // Lives across events: an analogue stick's position only means
        // something relative to where it was.
        menu_stick sticks;
        bool redraw = true;
        bool closed = false;
        for (;;) {
            if (redraw) {
                SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
                SDL_RenderClear(renderer);
                SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
                const SDL_FRect top_border = frect(0, 0, logical_width, 5);
                const SDL_FRect bottom_border =
                    frect(0, logical_height - 5, logical_width, 5);
                SDL_RenderFillRect(renderer, &top_border);
                SDL_RenderFillRect(renderer, &bottom_border);
                draw_text(renderer, heading, horizontal_margin, 20);

                // Description line (like the launcher select menu)
                rendered_text desc = make_text(
                    renderer, font, description, muted,
                    logical_width - horizontal_margin * 2);
                draw_text(renderer, desc, horizontal_margin, 58);
                if (text.height > body_height) {
                    rendered_text scroll_hint = make_text(
                        renderer, font, "SCROLL", muted);
                    draw_text(renderer, scroll_hint,
                              logical_width - scroll_hint.width - 34, 28);
                    destroy_text(scroll_hint);
                }
                destroy_text(desc);

                if (text.texture) {
                    const int shown = std::min(body_height, text.height - scroll);
                    if (shown > 0) {
                        const SDL_FRect source =
                            frect(0, scroll, text.width,
                                  static_cast<float>(shown));
                        const SDL_FRect destination =
                            frect(horizontal_margin + 10, body_top, text.width,
                                  static_cast<float>(shown));
                        const SDL_Rect clip{horizontal_margin, body_top - 4,
                            logical_width - horizontal_margin * 2,
                            body_height + 8};
                        SDL_SetRenderClipRect(renderer, &clip);
                        SDL_RenderTexture(renderer, text.texture, &source,
                                          &destination);
                        SDL_SetRenderClipRect(renderer, nullptr);
                    }
                }
                SDL_SetRenderDrawColor(renderer, 19, 75, 101, 245);
                const SDL_FRect back_highlight =
                    frect(horizontal_margin, 636,
                          logical_width - horizontal_margin * 2, 42);
                SDL_RenderFillRect(renderer, &back_highlight);
                draw_text(renderer, back, horizontal_margin + 12, 643);
                // Footer with controls hint (like the launcher select menu)
                rendered_text controls = make_text(
                    renderer, font,
                    "UP/DOWN: SCROLL    LEFT/RIGHT: GAME    ENTER: BACK",
                    muted);
                draw_text(renderer, controls, horizontal_margin, 680);
                destroy_text(controls);
                SDL_RenderPresent(renderer);
                redraw = false;
            }

            SDL_Event event{};
            if (!SDL_WaitEvent(&event)) continue;
            if (event.type == SDL_EVENT_QUIT) { current = -1; break; }
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                open_controller(event.gdevice.which);
                continue;
            }
            if (event.type >= SDL_EVENT_WINDOW_FIRST &&
                event.type <= SDL_EVENT_WINDOW_LAST) {
                redraw = true;
                continue;
            }
            int movement = 0;
            closed = false;
            bool navigate = false;
            int direction = 0;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                switch (event.key.key) {
                case SDLK_UP: movement = -32; break;
                case SDLK_DOWN: movement = 32; break;
                case SDLK_PAGEUP: movement = -body_height; break;
                case SDLK_PAGEDOWN: movement = body_height; break;
                case SDLK_HOME: scroll = 0; redraw = true; break;
                case SDLK_END: scroll = std::max(0, text.height - body_height);
                               redraw = true; break;
                case SDLK_LEFT:
                    if (current > 0) { navigate = true; direction = -1; }
                    break;
                case SDLK_RIGHT:
                    if (current + 1 < count) { navigate = true; direction = 1; }
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                case SDLK_ESCAPE: closed = true; break;
                default: break;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP)
                    movement = -32;
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                    movement = 32;
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                         event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
                         event.gbutton.button == SDL_GAMEPAD_BUTTON_START)
                    closed = true;
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT &&
                         current > 0) {
                    navigate = true; direction = -1;
                } else if (event.gbutton.button ==
                               SDL_GAMEPAD_BUTTON_DPAD_RIGHT &&
                           current + 1 < count) {
                    navigate = true; direction = 1;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                const stick_step step = stick_movement(sticks, event);
                movement = step.vertical * 32;
                if ((step.horizontal < 0 && current > 0) ||
                    (step.horizontal > 0 && current + 1 < count)) {
                    navigate = true;
                    direction = step.horizontal;
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                movement = event.wheel.y > 0 ? -64 : event.wheel.y < 0 ? 64 : 0;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                float logical_x = 0.0f;
                float logical_y = 0.0f;
                SDL_RenderCoordinatesFromWindow(renderer, event.button.x,
                                                event.button.y, &logical_x,
                                                &logical_y);
                closed = logical_y >= 636 && logical_y < 678 &&
                        logical_x >= horizontal_margin &&
                        logical_x < logical_width - horizontal_margin;
            }
            if (navigate) {
                current += direction;
                current = std::clamp(current, 0, count - 1);
                destroy_text(text);
                destroy_text(heading);
                auto [new_title, new_body] = page_provider(current);
                heading = make_text(renderer, font, new_title, accent);
                text = make_text(renderer, font, new_body, white,
                                 logical_width - horizontal_margin * 2 - 20);
                scroll = 0;
                redraw = true;
                continue;
            }
            if (closed) break;
            if (movement) {
                scroll = std::clamp(scroll + movement, 0,
                                    std::max(0, text.height - body_height));
                redraw = true;
            }
        }
        destroy_text(back);
        destroy_text(text);
        destroy_text(heading);
        return closed ? current : -1;
    }
};

launcher_menu::launcher_menu(bool compact_utility_window)
    : m_impl(std::make_unique<implementation>(compact_utility_window)) {}
launcher_menu::~launcher_menu() = default;

int launcher_menu::select(const std::string& title,
                          const std::string& description,
                          const std::vector<std::string>& items,
                          const std::string& back_label,
                          int initial_selection) {
    if (!m_impl->ready())
        return fallback_select(title, description, items, back_label,
                               initial_selection);
    SDL_SetWindowTitle(m_impl->window, title.c_str());
    SDL_RaiseWindow(m_impl->window);
    return m_impl->select(title, description, items, back_label,
                          initial_selection);
}

int launcher_menu::select_grid(
        const std::string& title, const std::string& description,
        const std::vector<std::string>& items,
        const std::string& back_label, int initial_selection,
        std::function<cover(int)> cover_for, std::function<bool()> tick,
        std::function<bool()> interrupt, bool paging, const cover* banner,
        bool info) {
    // No window means no artwork; the text selector is the honest fallback.
    if (!m_impl->ready())
        return fallback_select(title, description, items, back_label,
                               initial_selection);
    SDL_RaiseWindow(m_impl->window);
    const std::vector<int> chosen =
        m_impl->run_grid(title, description, items, back_label,
                         initial_selection, cover_for, tick, 0, interrupt,
                         paging, banner, info);
    return chosen.empty() ? -1 : chosen.front();
}

std::vector<int> launcher_menu::select_grid_multiple(
        const std::string& title, const std::string& description,
        const std::vector<std::string>& items, const std::string& back_label,
        int wanted, std::function<cover(int)> cover_for,
        std::function<bool()> tick) {
    if (!m_impl->ready() || wanted <= 0) return {};
    SDL_RaiseWindow(m_impl->window);
    return m_impl->run_grid(title, description, items, back_label, 0,
                            cover_for, tick, wanted);
}

int launcher_menu::select_interruptible(
        const std::string& title, const std::string& description,
        const std::vector<std::string>& items, const std::string& back_label,
        int initial_selection, std::function<bool()> interrupt) {
    if (!m_impl->ready())
        return fallback_select(title, description, items, back_label,
                               initial_selection);
    SDL_SetWindowTitle(m_impl->window, title.c_str());
    SDL_RaiseWindow(m_impl->window);
    return m_impl->select(title, description, items, back_label,
                          initial_selection, interrupt);
}

void launcher_menu::show_splash(const std::string& step, float progress) {
    if (!m_impl || !m_impl->ready()) return;
    SDL_SetWindowTitle(m_impl->window, "WhittyArcade");
    m_impl->splash(step, progress);
}

void launcher_menu::set_status(const std::string& text, bool good) {
    if (!m_impl) return;
    m_impl->status_text = text;
    m_impl->status_good = good;
}

void launcher_menu::show_text(const std::string& title,
                              const std::string& text,
                              const std::string& back_label) {
    if (!m_impl->ready()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title.c_str(),
                                 text.c_str(), nullptr);
        return;
    }
    SDL_SetWindowTitle(m_impl->window, title.c_str());
    SDL_RaiseWindow(m_impl->window);
    m_impl->show_text(title, text, back_label);
}

std::vector<launcher_controller_info> launcher_menu::controllers() {
    return m_impl->ready() ? m_impl->controller_list() :
                             std::vector<launcher_controller_info>{};
}

std::optional<input_binding> launcher_menu::capture_binding(
    const std::string& title, const std::string& description,
    bool keyboard, int32_t controller_instance, bool allow_inherit) {
    if (!m_impl->ready()) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_INFORMATION, title.c_str(),
            "Interactive input capture requires the WhittyArcade launcher "
            "window.", nullptr);
        return std::nullopt;
    }
    SDL_SetWindowTitle(m_impl->window, title.c_str());
    SDL_RaiseWindow(m_impl->window);
    return m_impl->capture_binding(title, description, keyboard,
                                   controller_instance, allow_inherit);
}


int launcher_menu::show_scoreboard(
    const std::string& back_label, const std::string& description, int count,
    std::function<scoreboard_page(int)> page_provider, int initial_page) {
    if (!m_impl) return -1;
    return m_impl->show_scoreboard(back_label, description, count,
                                   page_provider, initial_page);
}

int launcher_menu::show_pages(
    const std::string& back_label, const std::string& description,
    int count,
    std::function<std::pair<std::string, std::string>(int)> page_provider,
    int initial_page) {
    if (!m_impl->ready()) {
        if (count > 0) {
            auto [title, body] = page_provider(0);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title.c_str(),
                                     body.c_str(), nullptr);
        }
        return -1;
    }
    SDL_RaiseWindow(m_impl->window);
    return m_impl->show_pages(back_label, description, count,
                              std::move(page_provider), initial_page);
}
