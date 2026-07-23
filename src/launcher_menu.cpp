#include "launcher_menu.h"
#include "platform_paths.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <utility>

namespace {
constexpr int logical_width = 800;
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
} // namespace

struct launcher_menu::implementation {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    TTF_Font* font{};
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

        const SDL_WindowFlags window_flags =
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY |
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
            if (font) break;
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

    void draw_frame(const std::string& title,
                    const std::string& description,
                    const std::vector<std::string>& rows, int selected,
                    int first, int visible, int list_top,
                    const std::string& footer =
                        "UP/DOWN: SELECT    ENTER/A: OPEN    ESC/B: BACK") {
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
        rendered_text controls = make_text(
            renderer, font, footer, muted);
        draw_text(renderer, controls, horizontal_margin, 680);
        destroy_text(controls);
        destroy_text(detail);
        destroy_text(heading);
        SDL_RenderPresent(renderer);
    }

    int select(const std::string& title, const std::string& description,
               const std::vector<std::string>& items,
               const std::string& back_label, int initial_selection) {
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

            SDL_Event event{};
            if (!SDL_WaitEvent(&event)) continue;
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
