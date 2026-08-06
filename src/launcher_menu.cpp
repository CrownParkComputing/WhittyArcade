#include "launcher_menu.h"
#include "menu_stick.h"
#include "platform_paths.h"
#include "manx_fmv.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// Same as draw_text but stretched about its top-left. Used only where the
// size is under the player's control (the cover-flow designer): re-rendering
// the face at a new point size every frame would be the sharper answer, but
// the text block is three lines of an animated page, not body copy.
void draw_text_scaled(SDL_Renderer* renderer, const rendered_text& text,
                      int x, int y, float scale,
                      const SDL_Rect* clip = nullptr) {
    if (!text.texture) return;
    const SDL_FRect destination = frect(
        x, y, static_cast<int>(text.width * scale),
        static_cast<int>(text.height * scale));
    SDL_SetRenderClipRect(renderer, clip);
    SDL_RenderTexture(renderer, text.texture, nullptr, &destination);
    SDL_SetRenderClipRect(renderer, nullptr);
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
    // Book-style italic face for the game synopsis on the cover-flow page;
    // null falls back to the UI font.
    TTF_Font* desc_font{};
    // Shown in the corner of the browsing screens: whether another machine
    // is out there. A player deciding what to play should not have to open a
    // game to find out that two-player across machines is unavailable.
    std::string status_text;
    bool status_good{};
    // Rows the current list draws greyed out and refuses to open.
    std::vector<int> unavailable_rows;

#ifdef MANX_HAVE_FFMPEG
    // Video state for the cover-flow background video snap.
    struct coverflow_video_state {
        manx_fmv* fmv{};
        SDL_Texture* texture{};
        SDL_AudioStream* audio{};
        std::string path;
        void close() {
            // The audio stream goes first: destroying it stops the device
            // pulling before the decoder it reads from disappears.
            if (audio) SDL_DestroyAudioStream(audio);
            if (fmv) manx_fmv_close(fmv);
            if (texture) SDL_DestroyTexture(texture);
            audio = nullptr;
            fmv = nullptr;
            texture = nullptr;
            path.clear();
        }
    };
    coverflow_video_state cf_video;

    // Opens (or reopens, for looping) the selected game's video snap and,
    // when the snap has a soundtrack, a matching device audio stream.
    void open_coverflow_video(const std::string& path) {
        cf_video.path = path;
        cf_video.fmv = manx_fmv_open(path.c_str(), logical_width,
                                     logical_height, manx_fmv_format_rgba);
        if (!cf_video.fmv || !manx_fmv_has_audio(cf_video.fmv)) return;
        if (!SDL_WasInit(SDL_INIT_AUDIO) &&
            !SDL_InitSubSystem(SDL_INIT_AUDIO))
            return;
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
        cf_video.audio = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!cf_video.audio) return;
        // Background level: the snap is an attract loop behind the page,
        // not the main event.
        SDL_SetAudioStreamGain(cf_video.audio, 0.6f);
        SDL_ResumeAudioStreamDevice(cf_video.audio);
    }
#endif
    // Cover-flow animation: target index, current float position (lerped),
    // and timestamp of the last selection change.
    int cf_target{};
    float cf_position{};
    int cf_video_index{-1};
    Uint64 cf_anim_start{};

    // ---- Cover-flow layout, user-adjustable via the F2 designer --------
    //
    // Every element is a centre point in screen fractions plus a scale, so
    // one layout works at any window size. The designer writes the file;
    // deleting it restores the defaults.
    struct cf_element {
        float x, y, scale;
    };
    struct cf_layout_t {
        cf_element box, snap, marquee, text;
    };
    static constexpr cf_layout_t cf_layout_defaults{
        {0.16f, 0.46f, 1.0f},   // box art, left
        {0.74f, 0.44f, 1.0f},   // screenshot, right
        {0.50f, 0.84f, 1.0f},   // marquee above the title
        {0.50f, 0.905f, 1.0f},  // title + metadata + synopsis block
    };
    cf_layout_t cf_layout{cf_layout_defaults};
    bool cf_layout_loaded{};
    bool cf_design{};
    // Set by arrange_coverflow_next() when the View menu asks for the
    // designer; consumed the next time a cover-flow page opens.
    bool cf_design_pending{};
    int cf_design_sel{};
    bool cf_dragging{};
    // Rects of the four elements as last drawn, indexed box/snap/marquee/
    // text - the designer's outlines and mouse hit-testing read these.
    SDL_FRect cf_rects[4]{};

    cf_element* cf_elements[4] = {&cf_layout.box, &cf_layout.snap,
                                  &cf_layout.marquee, &cf_layout.text};

    static std::filesystem::path cf_layout_path() {
        const std::filesystem::path root = manx_platform::config_root();
        return root.empty() ? std::filesystem::path("coverflow_layout.ini") :
                              root / "MANX" / "coverflow_layout.ini";
    }

    void load_cf_layout() {
        if (cf_layout_loaded) return;
        cf_layout_loaded = true;
        std::ifstream input(cf_layout_path());
        if (!input) return;
        std::string line;
        while (std::getline(input, line)) {
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            const std::string key = line.substr(0, equals);
            cf_element value{};
            if (std::sscanf(line.c_str() + equals + 1, "%f,%f,%f",
                            &value.x, &value.y, &value.scale) != 3)
                continue;
            value.x = std::clamp(value.x, 0.0f, 1.0f);
            value.y = std::clamp(value.y, 0.0f, 1.0f);
            value.scale = std::clamp(value.scale, 0.3f, 2.5f);
            if (key == "box") cf_layout.box = value;
            else if (key == "snap") cf_layout.snap = value;
            else if (key == "marquee") cf_layout.marquee = value;
            else if (key == "text") cf_layout.text = value;
        }
    }

    void save_cf_layout() const {
        const std::filesystem::path path = cf_layout_path();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::trunc);
        if (!output) return;
        const auto write = [&](const char* name, const cf_element& value) {
            char line[96];
            std::snprintf(line, sizeof line, "%s=%.4f,%.4f,%.4f\n", name,
                          value.x, value.y, value.scale);
            output << line;
        };
        write("box", cf_layout.box);
        write("snap", cf_layout.snap);
        write("marquee", cf_layout.marquee);
        write("text", cf_layout.text);
    }

    static const cf_element& cf_default_element(int index) {
        switch (index) {
        case 0: return cf_layout_defaults.box;
        case 1: return cf_layout_defaults.snap;
        case 2: return cf_layout_defaults.marquee;
        default: return cf_layout_defaults.text;
        }
    }

    // One designer event. Arrow keys move (Shift for fine steps), +/-
    // resizes, Tab cycles elements, R resets one element, F2/Escape leave
    // and save. The mouse drags an element directly and the wheel resizes.
    void design_event(const SDL_Event& event) {
        cf_element& element = *cf_elements[cf_design_sel];
        if (event.type == SDL_EVENT_KEY_DOWN) {
            const bool fine = SDL_GetModState() & SDL_KMOD_SHIFT;
            const float step = fine ? 0.002f : 0.01f;
            switch (event.key.key) {
            case SDLK_LEFT: element.x -= step; break;
            case SDLK_RIGHT: element.x += step; break;
            case SDLK_UP: element.y -= step; break;
            case SDLK_DOWN: element.y += step; break;
            case SDLK_EQUALS:
            case SDLK_KP_PLUS: element.scale += 0.05f; break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS: element.scale -= 0.05f; break;
            case SDLK_TAB:
                if (!event.key.repeat)
                    cf_design_sel = (cf_design_sel + 1) % 4;
                break;
            case SDLK_R:
                element = cf_default_element(cf_design_sel);
                break;
            case SDLK_ESCAPE:
            case SDLK_RETURN:
                if (!event.key.repeat) {
                    cf_design = false;
                    save_cf_layout();
                }
                break;
            default: break;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                   event.button.button == SDL_BUTTON_LEFT) {
            for (int index = 0; index < 4; ++index) {
                const SDL_FRect& rect = cf_rects[index];
                if (event.button.x >= rect.x &&
                    event.button.x <= rect.x + rect.w &&
                    event.button.y >= rect.y &&
                    event.button.y <= rect.y + rect.h) {
                    cf_design_sel = index;
                    cf_dragging = true;
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            cf_dragging = false;
        } else if (event.type == SDL_EVENT_MOUSE_MOTION && cf_dragging) {
            cf_element& dragged = *cf_elements[cf_design_sel];
            dragged.x = event.motion.x / static_cast<float>(logical_width);
            dragged.y = event.motion.y / static_cast<float>(logical_height);
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            element.scale += event.wheel.y * 0.05f;
        }
        cf_element& clamped = *cf_elements[cf_design_sel];
        clamped.x = std::clamp(clamped.x, 0.0f, 1.0f);
        clamped.y = std::clamp(clamped.y, 0.0f, 1.0f);
        clamped.scale = std::clamp(clamped.scale, 0.3f, 2.5f);
    }

    bool row_unavailable(int index) const {
        return std::find(unavailable_rows.begin(), unavailable_rows.end(),
                         index) != unavailable_rows.end();
    }
    bool sdl_initialized{};
    bool ttf_initialized{};
    std::vector<SDL_Gamepad*> controllers;

    explicit implementation(bool compact_utility_window) {
        SDL_SetHint("SDL_APP_ID", "MANX");
        SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "MANX");
        SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "MANX");
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
            (std::getenv("MANX_MENU_WINDOWED") &&
             *std::getenv("MANX_MENU_WINDOWED") == '1');
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
            "MANX",
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

        for (const std::filesystem::path& path : manx_platform::font_paths()) {
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

        // The synopsis reads as a sentence, not chrome: give it an italic
        // face a step larger than the UI font. First face found wins; null
        // falls back to the UI font at draw time.
        static constexpr const char* desc_faces[] = {
#if defined(_WIN32)
            "C:\\Windows\\Fonts\\segoeuii.ttf",
            "C:\\Windows\\Fonts\\ariali.ttf",
#else
            "/usr/share/fonts/Adwaita/AdwaitaSans-Italic.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Oblique.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Italic.ttf",
            "/usr/share/fonts/truetype/liberation/"
            "LiberationSans-Italic.ttf",
#endif
        };
        for (const char* face : desc_faces) {
            desc_font = TTF_OpenFont(face, 23);
            if (desc_font) break;
        }

        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
            for (int index = 0; index < count; ++index)
                open_controller(ids[index]);
            SDL_free(ids);
        }
    }

    ~implementation() {
#ifdef MANX_HAVE_FFMPEG
        cf_video.close();
#endif
        for (SDL_Gamepad* controller : controllers)
            SDL_CloseGamepad(controller);
        if (desc_font) TTF_CloseFont(desc_font);
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

    // One hint: a face button carrying a letter, a shoulder button, or the
    // d-pad.
    struct hint {
        std::string letter;   // empty for the d-pad
        std::string label;
        bool shoulder{};      // drawn as a bumper rather than a face button
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
        std::vector<int> icon_widths;
        int total = 0;
        for (const hint& item : hints) {
            labels.push_back(make_text(renderer, small, item.label, muted));
            // A bumper is a wider, flatter shape than a face button, and
            // carries two letters.
            icon_widths.push_back(item.shoulder ? icon * 2 : icon);
            total += icon_widths.back() + inner_gap +
                     labels.back().width + between;
        }
        int x = std::max(horizontal_margin, (logical_width - total) / 2);
        for (std::size_t index = 0; index < hints.size(); ++index) {
            const hint& item = hints[index];
            const int width = icon_widths[index];
            if (item.shoulder) {
                SDL_SetRenderDrawColor(renderer, 74, 88, 102, 255);
                const SDL_FRect bumper = frect(x, y + 3, width, icon - 6);
                SDL_RenderFillRect(renderer, &bumper);
                rendered_text mark = make_text(renderer, small, item.letter,
                                               SDL_Color{232, 240, 248, 255});
                draw_text(renderer, mark, x + width / 2 - mark.width / 2,
                          y + icon / 2 - mark.height / 2);
                destroy_text(mark);
            } else if (item.letter.empty()) {
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
            draw_text(renderer, labels[index], x + width + inner_gap,
                      y + (icon - labels[index].height) / 2);
            x += width + inner_gap + labels[index].width + between;
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
        int gap;
        bool list;
    };

    static grid_metrics measure_grid(int top, bool list) {
        const int gap = list ? 8 : 18;
        const int usable = logical_width - horizontal_margin * 2;
        if (list) {
            // One game per row: a small thumbnail and the title beside it.
            constexpr int row_height = 48;
            const int rows_visible = std::max(
                1, (list_bottom - top + gap) / (row_height + gap));
            return {1, usable, row_height, row_height,
                    horizontal_margin, top, rows_visible, gap, true};
        }
        constexpr int preferred_card = 150;
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
                horizontal_margin, top, rows_visible, gap, false};
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
            bool info = false,
            bool list = false,
            bool marquee = false,
            bool coverflow = false,
            std::function<std::string(int)> video_for = {},
            std::function<launcher_menu::game_media(int)> media_for = {},
            const std::vector<std::string>* item_descriptions = nullptr) {
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
        // Cover-flow initialisation
        Uint64 description_started = 0;
        if (coverflow) {
            cf_target = initial_selection;
            cf_position = static_cast<float>(initial_selection);
            cf_anim_start = 0;
            cf_video_index = -1;
            cf_dragging = false;
            load_cf_layout();
            // The View menu's "Arrange Page" hands the designer over here,
            // one page only: leaving and coming back is browsing again.
            cf_design = cf_design_pending;
            cf_design_pending = false;
#ifdef MANX_HAVE_FFMPEG
            cf_video.close();
#endif
        }
        for (;;) {
            const grid_metrics metrics = measure_grid(grid_top, list);
            if (!coverflow) {
            const int row = selected / metrics.columns;
            if (row < first_row) first_row = row;
            if (row >= first_row + metrics.rows_visible)
                first_row = row - metrics.rows_visible + 1;
            const int total_rows =
                (total + metrics.columns - 1) / metrics.columns;
            first_row = std::clamp(
                first_row, 0, std::max(0, total_rows - metrics.rows_visible));
            }

            if (redraw) {
                if (coverflow) {
                    // Smooth lerp toward current selection
                    constexpr float lerp_speed = 0.18f;
                    cf_position += (static_cast<float>(selected) -
                                    cf_position) * lerp_speed;
                    if (std::abs(cf_position -
                                 static_cast<float>(selected)) < 0.005f)
                        cf_position = static_cast<float>(selected);
                    // Selection changed: reset the description scroll and
                    // start the new game's video snap.
                    if (selected != cf_video_index) {
                        description_started = SDL_GetTicks();
#ifdef MANX_HAVE_FFMPEG
                        cf_video.close();
                        if (video_for) {
                            const std::string path = video_for(selected);
                            if (!path.empty()) open_coverflow_video(path);
                        }
#endif
                        cf_video_index = selected;
                    }
                    draw_coverflow(title, description, items, selected,
                                   textures, cover_for, back_label, chosen,
                                   wanted, item_descriptions,
                                   SDL_GetTicks() - description_started,
                                   media_for);
                } else {
                draw_grid(title, description, items, selected, first_row,
                          metrics, textures, cover_for, back_label, chosen,
                          wanted, paging, banner, info);
                }
                redraw = false;
            }

            SDL_Event event{};
            if (interrupt && interrupt()) {
                chosen.assign(1, launcher_menu::interrupted);
                break;
            }
            // Cover-flow is animated - the background video, the slide lerp
            // and the description scroll all advance with the wall clock -
            // so its loop runs at ~60Hz rather than sleeping until input.
            const Sint32 wait_ms = coverflow ? 16 :
                (tick || interrupt) ? 100 : -1;
            if (!SDL_WaitEventTimeout(&event, wait_ms)) {
                // Timed out: artwork may have arrived while nothing happened.
                if (tick && tick()) redraw = true;
                if (coverflow) redraw = true;
                continue;
            }
            if (tick && tick()) redraw = true;
            if (coverflow) redraw = true;
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

            // F2 opens the cover-flow designer. While it is open every
            // other event belongs to it - arrows move the element, they do
            // not change game - so the normal navigation below is skipped.
            if (coverflow && event.type == SDL_EVENT_KEY_DOWN &&
                !event.key.repeat && event.key.key == SDLK_F2) {
                load_cf_layout();
                cf_design = !cf_design;
                if (!cf_design) save_cf_layout();
                redraw = true;
                continue;
            }
            if (cf_design) {
                design_event(event);
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
                // On a paged view, pushing past the end turns the page - the
                // way you would expect a book of boards to work. Shoulders
                // and PgUp/PgDn still jump directly, but a stick and two
                // buttons is all an arcade panel has, and this needs no
                // instructions.
                const int moved = selected + horizontal;
                if (paging && (moved < 0 || moved >= total)) {
                    chosen.assign(1, horizontal < 0 ?
                                      launcher_menu::page_back :
                                      launcher_menu::page_forward);
                    break;
                }
                selected = std::clamp(moved, 0, total - 1);
                redraw = true;
            }
            if (vertical) {
                const int moved = selected + vertical * metrics.columns;
                if (moved >= 0 && moved < total) selected = moved;
                redraw = true;
            }
        }
        destroy_grid_textures(textures);
#ifdef MANX_HAVE_FFMPEG
        cf_video.close();
#endif
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
        const int gap = metrics.gap;
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

                if (metrics.list) {
                    // A row: selection bar, a small thumbnail letterboxed
                    // to its own aspect (title screens are 4:3, IGDB
                    // covers 3:4), the name beside it, the player badge at
                    // the right edge.
                    if (active || taken) {
                        if (active)
                            SDL_SetRenderDrawColor(renderer, 19, 75, 101,
                                                   245);
                        else
                            SDL_SetRenderDrawColor(renderer, 15, 52, 70,
                                                   245);
                        const SDL_FRect bar = frect(
                            x - 6, y - 3, metrics.card_width + 12,
                            metrics.card_height + 6);
                        SDL_RenderFillRect(renderer, &bar);
                    }
                    constexpr int thumb_slot = 64;
                    const launcher_menu::cover details =
                        cover_for ? cover_for(index)
                                  : launcher_menu::cover{};
                    SDL_Texture* texture =
                        grid_texture_for(textures, index, details);
                    if (texture && details.width > 0 &&
                        details.height > 0) {
                        int thumb_w = metrics.cover_height *
                                      details.width /
                                      std::max(details.height, 1);
                        thumb_w = std::min(thumb_w, thumb_slot);
                        const int thumb_h = metrics.cover_height;
                        const SDL_FRect art = frect(
                            x + (thumb_slot - thumb_w) / 2, y,
                            thumb_w, thumb_h);
                        SDL_RenderTexture(renderer, texture, nullptr, &art);
                    } else {
                        SDL_SetRenderDrawColor(renderer, 19, 30, 41, 255);
                        const SDL_FRect plate = frect(
                            x, y, thumb_slot, metrics.cover_height);
                        SDL_RenderFillRect(renderer, &plate);
                    }
                    rendered_text label = make_text(
                        renderer, font,
                        items[static_cast<std::size_t>(index)],
                        active ? accent : white,
                        metrics.card_width - thumb_slot - 140);
                    draw_text(renderer, label, x + thumb_slot + 18,
                              y + (metrics.card_height - label.height) / 2);
                    destroy_text(label);
                    if (taken) {
                        const int slot = static_cast<int>(
                            std::distance(chosen.begin(), held)) + 1;
                        rendered_text badge = make_text(
                            renderer, font, std::to_string(slot), white);
                        const SDL_FRect plate = frect(
                            x + metrics.card_width - 130, y + 8, 28, 28);
                        SDL_SetRenderDrawColor(renderer, 8, 13, 20, 220);
                        SDL_RenderFillRect(renderer, &plate);
                        draw_text(renderer, badge,
                                  x + metrics.card_width - 120, y + 10);
                        destroy_text(badge);
                    }
                    if (details.badge && *details.badge) {
                        TTF_Font* small = hint_font ? hint_font : font;
                        rendered_text mark = make_text(
                            renderer, small, details.badge,
                            SDL_Color{12, 18, 26, 255});
                        const SDL_Color tone =
                            details.badge_tone == 1 ?
                                SDL_Color{86, 206, 255, 255} :
                            details.badge_tone == 2 ?
                                SDL_Color{255, 190, 74, 255} :
                                SDL_Color{124, 214, 128, 255};
                        const int plate_w = mark.width + 12;
                        const int plate_h = mark.height + 6;
                        const int plate_x =
                            x + metrics.card_width - plate_w - 6;
                        const int plate_y =
                            y + (metrics.card_height - plate_h) / 2;
                        SDL_SetRenderDrawColor(renderer, tone.r, tone.g,
                                               tone.b,
                                               active ? 255 : 205);
                        const SDL_FRect plate =
                            frect(plate_x, plate_y, plate_w, plate_h);
                        SDL_RenderFillRect(renderer, &plate);
                        draw_text(renderer, mark, plate_x + 6, plate_y + 3);
                        destroy_text(mark);
                    }
                    continue;
                }

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
                const launcher_menu::cover details =
                    cover_for ? cover_for(index) : launcher_menu::cover{};
                SDL_Texture* texture =
                    grid_texture_for(textures, index, details);
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
                // How many can play, in the corner of the card, so a night
                // with two people does not mean opening games one at a time
                // to find out which of them take a second player.
                if (details.badge && *details.badge) {
                    TTF_Font* small = hint_font ? hint_font : font;
                    rendered_text mark = make_text(
                        renderer, small, details.badge,
                        SDL_Color{12, 18, 26, 255});
                    const SDL_Color tone =
                        details.badge_tone == 1 ?
                            SDL_Color{86, 206, 255, 255} :
                        details.badge_tone == 2 ?
                            SDL_Color{255, 190, 74, 255} :
                            SDL_Color{124, 214, 128, 255};
                    const int plate_w = mark.width + 12;
                    const int plate_h = mark.height + 6;
                    const int plate_x = x + metrics.card_width - plate_w - 6;
                    const int plate_y =
                        y + metrics.card_height - plate_h - 6;
                    SDL_SetRenderDrawColor(renderer, tone.r, tone.g, tone.b,
                                           active || taken ? 255 : 205);
                    const SDL_FRect plate =
                        frect(plate_x, plate_y, plate_w, plate_h);
                    SDL_RenderFillRect(renderer, &plate);
                    draw_text(renderer, mark, plate_x + 6, plate_y + 3);
                    destroy_text(mark);
                }
            }
        }

        std::vector<hint> hints{{"", "Move"}};
        hints.push_back({"A", wanted > 0 ?
            "Hold (" + std::to_string(chosen.size()) + " of " +
                std::to_string(wanted) + ")" : std::string("Play")});
        if (wanted <= 0) hints.push_back({"Y", "View"});
        if (info) hints.push_back({"X", "Board Info"});
        if (paging) hints.push_back({"LB RB", "Turn Page", true});
        hints.push_back({"B", back_label});
        draw_hints(hints, list_bottom + 18);
        draw_status_chip();
        SDL_RenderPresent(renderer);
    }

    // ---- Cover-flow carousel (Retrobat/EmulationStation style) ----------
    //
    // Single-game detail view: all artwork for the selected game composited
    // on one page with full-screen video background. Left/right slides the
    // entire view horizontally to the next/previous game with a smooth lerp.
    void draw_coverflow(
            const std::string&, const std::string&,
            const std::vector<std::string>& items, int selected,
            std::map<int, grid_texture>& textures,
            const std::function<launcher_menu::cover(int)>& cover_for,
            const std::string& back_label,
            const std::vector<int>& chosen, int wanted,
            const std::vector<std::string>* item_descriptions,
            Uint64 description_elapsed_ms,
            const std::function<launcher_menu::game_media(int)>& media_for) {

        const int total = static_cast<int>(items.size());

        // Background fill
        SDL_SetRenderDrawColor(renderer, 4, 8, 16, 255);
        SDL_RenderClear(renderer);

        // Decode and render video as full-screen dimmed background
#ifdef MANX_HAVE_FFMPEG
        if (cf_video.fmv) {
            int new_frame = 0;
            if (manx_fmv_update(cf_video.fmv, &new_frame)) {
                if (new_frame) {
                    const uint8_t* pixels = manx_fmv_frame(cf_video.fmv);
                    if (pixels) {
                        if (!cf_video.texture) {
                            cf_video.texture = SDL_CreateTexture(
                                renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING,
                                logical_width, logical_height);
                            if (cf_video.texture)
                                SDL_SetTextureBlendMode(cf_video.texture,
                                                        SDL_BLENDMODE_BLEND);
                        }
                        if (cf_video.texture) {
                            SDL_UpdateTexture(cf_video.texture, nullptr,
                                              pixels, logical_width * 4);
                        }
                    }
                }
                // Feed the snap's soundtrack to the device, keeping about
                // a quarter second queued ahead of the hardware.
                if (cf_video.audio) {
                    constexpr int frame_bytes = 2 * sizeof(int16_t);
                    if (SDL_GetAudioStreamQueued(cf_video.audio) <
                        12000 * frame_bytes) {
                        int16_t samples[4096 * 2];
                        const int got = manx_fmv_read_audio(
                            cf_video.fmv, samples, 4096);
                        if (got > 0)
                            SDL_PutAudioStreamData(
                                cf_video.audio, samples, got * frame_bytes);
                    }
                }
            } else {
                // End of video: loop by reopening
                const std::string path = cf_video.path;
                cf_video.close();
                open_coverflow_video(path);
            }
            // Video behind everything, near-full strength: it is the point
            // of the page, the wash below keeps the text readable.
            if (cf_video.texture) {
                SDL_SetTextureAlphaMod(cf_video.texture, 230);
                const SDL_FRect full{0, 0,
                    static_cast<float>(logical_width),
                    static_cast<float>(logical_height)};
                SDL_RenderTexture(renderer, cf_video.texture,
                                  nullptr, &full);
            }
        }
#endif

        // Light wash over video for legibility
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 55);
        const SDL_FRect wash{0, 0,
            static_cast<float>(logical_width),
            static_cast<float>(logical_height)};
        SDL_RenderFillRect(renderer, &wash);

        // ---- Gather media for the selected game ------------------------
        launcher_menu::game_media media;
        if (media_for) media = media_for(selected);

        // Box art texture
        SDL_Texture* box_tex = nullptr;
        float box_aspect = 3.0f / 4.0f;
        if (media.box_pixels && media.box_w > 0 && media.box_h > 0) {
            box_tex = grid_texture_for(
                textures, selected,
                launcher_menu::cover{media.box_pixels, media.box_w,
                                     media.box_h});
            box_aspect = static_cast<float>(media.box_w) /
                         static_cast<float>(media.box_h);
        } else if (cover_for) {
            launcher_menu::cover art = cover_for(selected);
            if (art.pixels && art.width > 0 && art.height > 0) {
                box_tex = grid_texture_for(textures, selected, art);
                box_aspect = static_cast<float>(art.width) /
                             static_cast<float>(art.height);
            }
        }

        // Marquee/logo texture
        SDL_Texture* marquee_tex = nullptr;
        if (media.marquee_pixels && media.marquee_w > 0 &&
            media.marquee_h > 0) {
            const int marquee_id = selected + 10000;
            marquee_tex = grid_texture_for(
                textures, marquee_id,
                launcher_menu::cover{media.marquee_pixels, media.marquee_w,
                                     media.marquee_h});
        }

        // Screenshot/fan art texture
        SDL_Texture* snap_tex = nullptr;
        if (media.snap_pixels && media.snap_w > 0 && media.snap_h > 0) {
            const int snap_id = selected + 20000;
            snap_tex = grid_texture_for(
                textures, snap_id,
                launcher_menu::cover{media.snap_pixels, media.snap_w,
                                     media.snap_h});
        }

        // ---- Slide animation offset ------------------------------------
        const float slide_offset =
            (cf_position - static_cast<float>(selected)) *
            static_cast<float>(logical_width);

        // ---- Layout (user-adjustable via the F2 designer) ---------------
        load_cf_layout();
        const cf_layout_t& lay = cf_layout;
        const float centre_x_pct = lay.text.x;
        const int box_max_h = static_cast<int>(
            (logical_height - 220) * lay.box.scale);
        const int snap_max_h = static_cast<int>(
            (logical_height - 280) * lay.snap.scale);
        const int marquee_max_w = static_cast<int>(
            logical_width * 0.38f * lay.marquee.scale);
        const int marquee_max_h = static_cast<int>(80 * lay.marquee.scale);

        // ---- Box art on the left ---------------------------------------
        {
            int box_w = box_max_h > 0 ?
                std::min(static_cast<int>(box_max_h * box_aspect),
                         static_cast<int>(logical_width * 0.32f *
                                          lay.box.scale)) :
                static_cast<int>(logical_width * 0.28f * lay.box.scale);
            int box_h = box_w > 0 ?
                static_cast<int>(box_w / box_aspect) : box_max_h;
            int box_x = static_cast<int>(
                logical_width * lay.box.x - box_w / 2 + slide_offset);
            int box_y = static_cast<int>(
                logical_height * lay.box.y - box_h / 2);
            cf_rects[0] = frect(box_x, box_y, box_w, box_h);
        }
        if (box_tex) {
            const int box_w = static_cast<int>(cf_rects[0].w);
            const int box_h = static_cast<int>(cf_rects[0].h);
            const int box_x = static_cast<int>(cf_rects[0].x);
            const int box_y = static_cast<int>(cf_rects[0].y);

            // Drop shadow
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
            const SDL_FRect box_shadow{
                static_cast<float>(box_x + 4),
                static_cast<float>(box_y + 4),
                static_cast<float>(box_w),
                static_cast<float>(box_h)};
            SDL_RenderFillRect(renderer, &box_shadow);

            // Box art
            SDL_SetTextureAlphaMod(box_tex, 255);
            const SDL_FRect box_dest{
                static_cast<float>(box_x), static_cast<float>(box_y),
                static_cast<float>(box_w), static_cast<float>(box_h)};
            SDL_RenderTexture(renderer, box_tex, nullptr, &box_dest);

            // Subtle border
            SDL_SetRenderDrawColor(renderer, 40, 50, 65, 180);
            SDL_RenderRect(renderer, &box_dest);
        }

        // ---- Screenshot/fan art on the right ----------------------------
        {
            const float snap_aspect = media.snap_w > 0 && media.snap_h > 0 ?
                static_cast<float>(media.snap_w) /
                    static_cast<float>(media.snap_h) :
                4.0f / 3.0f;
            int snap_w = snap_max_h > 0 ?
                static_cast<int>(snap_max_h * snap_aspect) :
                static_cast<int>(logical_width * 0.30f * lay.snap.scale);
            snap_w = std::min(snap_w,
                              static_cast<int>(logical_width * 0.35f *
                                               lay.snap.scale));
            const int snap_h = snap_w > 0 ?
                static_cast<int>(snap_w / snap_aspect) : snap_max_h;
            const int snap_x = static_cast<int>(
                logical_width * lay.snap.x - snap_w / 2 + slide_offset);
            const int snap_y = static_cast<int>(
                logical_height * lay.snap.y - snap_h / 2);
            cf_rects[1] = frect(snap_x, snap_y, snap_w, snap_h);
        }
        if (snap_tex && media.snap_w > 0) {
            const int snap_w = static_cast<int>(cf_rects[1].w);
            const int snap_h = static_cast<int>(cf_rects[1].h);
            const int snap_x = static_cast<int>(cf_rects[1].x);
            const int snap_y = static_cast<int>(cf_rects[1].y);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
            const SDL_FRect snap_shadow{
                static_cast<float>(snap_x + 4),
                static_cast<float>(snap_y + 4),
                static_cast<float>(snap_w),
                static_cast<float>(snap_h)};
            SDL_RenderFillRect(renderer, &snap_shadow);

            SDL_SetTextureAlphaMod(snap_tex, 230);
            const SDL_FRect snap_dest{
                static_cast<float>(snap_x), static_cast<float>(snap_y),
                static_cast<float>(snap_w), static_cast<float>(snap_h)};
            SDL_RenderTexture(renderer, snap_tex, nullptr, &snap_dest);

            SDL_SetRenderDrawColor(renderer, 40, 50, 65, 180);
            SDL_RenderRect(renderer, &snap_dest);
        }

        // ---- Marquee/logo centred above title --------------------------
        {
            const float mq_aspect = media.marquee_w > 0 &&
                    media.marquee_h > 0 ?
                static_cast<float>(media.marquee_w) /
                    static_cast<float>(media.marquee_h) :
                4.0f;
            int mq_w = marquee_max_w;
            int mq_h = static_cast<int>(mq_w / mq_aspect);
            if (mq_h > marquee_max_h) {
                mq_h = marquee_max_h;
                mq_w = static_cast<int>(mq_h * mq_aspect);
            }
            const int mq_x = static_cast<int>(
                logical_width * lay.marquee.x - mq_w / 2 + slide_offset);
            const int mq_y = static_cast<int>(
                logical_height * lay.marquee.y - mq_h / 2);
            cf_rects[2] = frect(mq_x, mq_y, mq_w, mq_h);
        }
        if (marquee_tex && media.marquee_w > 0) {
            SDL_SetTextureAlphaMod(marquee_tex, 240);
            SDL_RenderTexture(renderer, marquee_tex, nullptr, &cf_rects[2]);
        }

        // ---- Title (large, centred) ------------------------------------
        // The whole block - title, metadata row, synopsis - moves and
        // resizes as one under lay.text, and its bounds are recorded in
        // cf_rects[3] for the designer to outline and hit-test.
        const float text_scale = lay.text.scale;
        int text_left = logical_width;
        int text_right = 0;
        const std::string& game_name =
            items[static_cast<std::size_t>(selected)];
        rendered_text heading = make_text(
            renderer, title_font ? title_font : font,
            game_name, accent,
            static_cast<int>((logical_width - horizontal_margin * 2) /
                             text_scale));
        int title_y = static_cast<int>(logical_height * lay.text.y);
        const int heading_w = static_cast<int>(heading.width * text_scale);
        const int heading_x = static_cast<int>(
            logical_width * centre_x_pct - heading_w / 2 + slide_offset);
        draw_text_scaled(renderer, heading, heading_x, title_y, text_scale);
        int heading_h = static_cast<int>(heading.height * text_scale);
        text_left = std::min(text_left, heading_x);
        text_right = std::max(text_right, heading_x + heading_w);
        destroy_text(heading);

        // ---- Metadata row ----------------------------------------------
        std::string meta;
        if (!media.publisher.empty()) meta += media.publisher;
        if (!media.year.empty()) {
            if (!meta.empty()) meta += "  ·  ";
            meta += media.year;
        }
        if (!media.players.empty()) {
            if (!meta.empty()) meta += "  ·  ";
            meta += media.players;
        }
        if (!media.board_name.empty()) {
            if (!meta.empty()) meta += "  ·  ";
            meta += media.board_name;
        }
        if (!meta.empty()) {
            rendered_text meta_text = make_text(
                renderer, font, meta, muted,
                static_cast<int>((logical_width - horizontal_margin * 4) /
                                 text_scale));
            const int meta_w = static_cast<int>(meta_text.width * text_scale);
            const int meta_x = static_cast<int>(
                logical_width * centre_x_pct - meta_w / 2 + slide_offset);
            draw_text_scaled(renderer, meta_text, meta_x,
                             title_y + heading_h + 6, text_scale);
            heading_h = heading_h +
                static_cast<int>(meta_text.height * text_scale) + 6;
            text_left = std::min(text_left, meta_x);
            text_right = std::max(text_right, meta_x + meta_w);
            destroy_text(meta_text);
        }

        // ---- Description text ------------------------------------------
        std::string selected_description;
        if (item_descriptions && selected >= 0 &&
            selected < static_cast<int>(item_descriptions->size())) {
            bool spacing = false;
            for (unsigned char character :
                 (*item_descriptions)[static_cast<std::size_t>(selected)]) {
                if (std::isspace(character)) {
                    spacing = !selected_description.empty();
                    continue;
                }
                if (spacing) selected_description.push_back(' ');
                spacing = false;
                selected_description.push_back(static_cast<char>(character));
            }
        }
        const int desc_y = title_y + heading_h + 8;
        int desc_bottom = desc_y;
        if (!selected_description.empty()) {
            const int desc_width = static_cast<int>(
                logical_width * 0.45f * text_scale);
            rendered_text line = make_text(
                renderer, desc_font ? desc_font : font, selected_description,
                muted, static_cast<int>(desc_width / text_scale));
            const int line_w = static_cast<int>(line.width * text_scale);
            const int line_h = static_cast<int>(line.height * text_scale);
            int offset = 0;
            const int travel = std::max(0, line_w - desc_width);
            if (travel > 0 && description_elapsed_ms > 0) {
                constexpr Uint64 hold_ms = 2200;
                constexpr Uint64 end_hold_ms = 1100;
                constexpr Uint64 pixels_per_second = 36;
                const Uint64 travel_ms =
                    static_cast<Uint64>(travel) * 1000 / pixels_per_second;
                const Uint64 cycle = hold_ms + travel_ms + end_hold_ms;
                const Uint64 phase = description_elapsed_ms % cycle;
                if (phase > hold_ms)
                    offset = static_cast<int>(std::min<Uint64>(
                        travel,
                        (phase - hold_ms) * pixels_per_second / 1000));
            }
            const int desc_x = static_cast<int>(
                logical_width * centre_x_pct - desc_width / 2 + slide_offset);
            const SDL_Rect clip{desc_x, desc_y, desc_width,
                                std::max(line_h + 2, 24)};
            draw_text_scaled(renderer, line, desc_x - offset, desc_y,
                             text_scale, &clip);
            destroy_text(line);
            desc_bottom = desc_y + std::max(line_h, 24);
            text_left = std::min(text_left, desc_x);
            text_right = std::max(text_right, desc_x + desc_width);
        }
        cf_rects[3] = frect(text_left, title_y,
                            std::max(text_right - text_left, 40),
                            std::max(desc_bottom - title_y, 40));

        // ---- Navigation arrows (subtle, at sides) -----------------------
        SDL_SetRenderDrawColor(renderer, 70, 208, 255, 80);
        const int arrow_y = logical_height / 2;
        const int left_x = horizontal_margin;
        for (int i = 0; i < 10; ++i) {
            const int offset = 5 - i;
            SDL_RenderLine(renderer,
                static_cast<float>(left_x + i * 2),
                static_cast<float>(arrow_y + offset),
                static_cast<float>(left_x + i * 2),
                static_cast<float>(arrow_y - offset));
        }
        const int right_x = logical_width - horizontal_margin;
        for (int i = 0; i < 10; ++i) {
            const int offset = 5 - i;
            SDL_RenderLine(renderer,
                static_cast<float>(right_x - i * 2),
                static_cast<float>(arrow_y + offset),
                static_cast<float>(right_x - i * 2),
                static_cast<float>(arrow_y - offset));
        }

        // ---- Position indicator dots -----------------------------------
        {
            const int dot_count = std::min(total, 20);
            const int dot_spacing = 14;
            const int dot_y = logical_height - 42;
            const int dot_start = (logical_width -
                dot_count * dot_spacing) / 2;
            for (int dot = 0; dot < dot_count; ++dot) {
                const int game_index = dot * total / dot_count;
                const bool lit = game_index == selected;
                const int dot_x = dot_start + dot * dot_spacing;
                SDL_SetRenderDrawColor(renderer,
                    lit ? accent.r : 40,
                    lit ? accent.g : 46,
                    lit ? accent.b : 56,
                    255);
                fill_disc(dot_x, dot_y, lit ? 4 : 2,
                    lit ? accent :
                    SDL_Color{40, 46, 56, 255});
            }
        }

        // ---- Hints -----------------------------------------------------
        std::vector<hint> hints{{"", "Browse"}};
        hints.push_back({"A", wanted > 0 ?
            "Hold (" + std::to_string(chosen.size()) + " of " +
                std::to_string(wanted) + ")" : std::string("Play")});
        if (wanted <= 0) hints.push_back({"Y", "View"});
        hints.push_back({"B", back_label});
        hints.push_back({"F2", "Layout"});
        draw_hints(hints, list_bottom + 18);
        draw_status_chip();
        if (cf_design) draw_design_overlay();
        SDL_RenderPresent(renderer);
    }

    // The F2 designer, drawn over the finished page so the player is
    // arranging the real thing rather than a wireframe of it.
    void draw_design_overlay() {
        static constexpr const char* names[4] = {
            "Box art", "Screenshot", "Marquee", "Text"};
        for (int index = 0; index < 4; ++index) {
            const bool live = index == cf_design_sel;
            const SDL_FRect& rect = cf_rects[index];
            if (rect.w <= 0 || rect.h <= 0) continue;
            SDL_SetRenderDrawColor(renderer,
                                   live ? accent.r : 120,
                                   live ? accent.g : 132,
                                   live ? accent.b : 146,
                                   live ? 255 : 150);
            SDL_RenderRect(renderer, &rect);
            if (live) {
                // A second inset line so the selection reads at a glance
                // against busy box art.
                const SDL_FRect inner{rect.x + 2, rect.y + 2,
                                      rect.w - 4, rect.h - 4};
                SDL_RenderRect(renderer, &inner);
            }
            rendered_text tag = make_text(
                renderer, hint_font ? hint_font : font, names[index],
                live ? accent : muted);
            draw_text(renderer, tag, static_cast<int>(rect.x) + 4,
                      static_cast<int>(rect.y) - tag.height - 2);
            destroy_text(tag);
        }

        const cf_element& element = *cf_elements[cf_design_sel];
        char readout[128];
        std::snprintf(readout, sizeof readout,
                      "%s   x %.3f   y %.3f   size %.2f",
                      names[cf_design_sel], element.x, element.y,
                      element.scale);
        static constexpr const char* keys =
            "Drag or arrows to move (Shift = fine)   "
            "+/- or wheel to size   Tab next   R reset   "
            "Enter/Esc save and exit";

        rendered_text value = make_text(renderer, font, readout, accent);
        rendered_text help = make_text(renderer, hint_font ? hint_font : font,
                                       keys, muted);
        const int panel_h = value.height + help.height + 26;
        const SDL_FRect panel = frect(0, 0, logical_width, panel_h);
        SDL_SetRenderDrawColor(renderer, 8, 13, 20, 225);
        SDL_RenderFillRect(renderer, &panel);
        SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, 255);
        const SDL_FRect edge = frect(0, panel_h - 2, logical_width, 2);
        SDL_RenderFillRect(renderer, &edge);
        draw_text(renderer, value, horizontal_margin, 8);
        draw_text(renderer, help, horizontal_margin, 12 + value.height);
        destroy_text(value);
        destroy_text(help);
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
                if (row_unavailable(index))
                    SDL_SetRenderDrawColor(renderer, 38, 46, 55, 245);
                else
                    SDL_SetRenderDrawColor(renderer, 19, 75, 101, 245);
                const SDL_FRect highlight =
                    frect(horizontal_margin, y + 2,
                          logical_width - horizontal_margin * 2,
                          row_height - 4);
                SDL_RenderFillRect(renderer, &highlight);
            }
            const bool blocked = row_unavailable(index);
            rendered_text label = make_text(renderer, font,
                (index == selected ? ">  " : "   ") + rows[index],
                blocked ? SDL_Color{96, 110, 124, 255} :
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
            renderer, title_font ? title_font : font, "MANX", accent);
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

    // Per-game loading screen. The fade-in is short (200ms) so the screen
    // is readable even if the loader returns in 100ms total; the fade-out
    // is the same so a fast loader does not produce a jarring snap.
    //
    // The screen itself is centred text + a rotating arc "spinner" on the
    // left, plus a progress bar at the bottom. The arc is drawn as three
    // concentric rings: an outline ring (track), an active ring (progress),
    // and a small wedge rotating in the centre as the spinner.
    void loading_screen(const std::string& title,
                        const std::string& subtitle,
                        const std::function<bool(float&)>& step_for) {
        // Fade in from black. 200ms at 60fps = 12 frames.
        constexpr int FADE_FRAMES = 12;
        for (int i = 1; i <= FADE_FRAMES; ++i) {
            SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
            SDL_RenderClear(renderer);
            draw_loading_text_only(title, subtitle, "", 0.0f,
                                  i / static_cast<float>(FADE_FRAMES));
            SDL_RenderPresent(renderer);
            SDL_Delay(1000 / 60);
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {}
        }

        // Drive the loader. Each call to step_for receives a fresh
        // progress value; the caller can update both the step text and the
        // progress number together. The function returns false to fade out
        // and finish.
        float progress = 0.0f;
        std::string step_text;
        Uint64 last_present = SDL_GetTicks();
        while (true) {
            step_text.clear();
            const bool keep_going = step_for ? step_for(progress) : false;
            if (!keep_going) break;

            // Cap the framerate so a slow loader still has time to work.
            const Uint64 now = SDL_GetTicks();
            const Uint64 elapsed = now - last_present;
            constexpr Uint64 FRAME_MS = 1000 / 60;
            if (elapsed < FRAME_MS) SDL_Delay(static_cast<Uint32>(FRAME_MS - elapsed));
            last_present = SDL_GetTicks();

            // Render the loading frame, animating the spinner as time advances.
            const float spinner = (now % 1500ULL) / 1500.0f;
            SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
            SDL_RenderClear(renderer);
            draw_loading_screen(title, subtitle, step_text,
                                std::clamp(progress, 0.0f, 1.0f), spinner, 1.0f);
            SDL_RenderPresent(renderer);
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {}
        }

        // Fade out.
        for (int i = FADE_FRAMES - 1; i >= 0; --i) {
            SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
            SDL_RenderClear(renderer);
            draw_loading_screen(title, subtitle, "", 1.0f, 0.0f,
                                i / static_cast<float>(FADE_FRAMES));
            SDL_RenderPresent(renderer);
            SDL_Delay(1000 / 60);
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {}
        }
        SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }

    // Text-only render used during fade-in / fade-out (no spinner, dimmed).
    void draw_loading_text_only(const std::string& title,
                                const std::string& subtitle,
                                const std::string& step,
                                float progress,
                                float overlay_alpha) {
        // Same layout as draw_loading_screen but the spinner is hidden and
        // everything is darkened by overlay_alpha.
        const int cx = logical_width / 2;
        const int cy = logical_height / 2;
        const SDL_Color title_col = {
            static_cast<Uint8>(205 * overlay_alpha),
            static_cast<Uint8>(214 * overlay_alpha),
            static_cast<Uint8>(244 * overlay_alpha), 255};
        const SDL_Color sub_col = {
            static_cast<Uint8>(137 * overlay_alpha),
            static_cast<Uint8>(180 * overlay_alpha),
            static_cast<Uint8>(250 * overlay_alpha), 255};
        const SDL_Color muted_col = {
            static_cast<Uint8>(166 * overlay_alpha),
            static_cast<Uint8>(173 * overlay_alpha),
            static_cast<Uint8>(200 * overlay_alpha), 255};

        rendered_text t = make_text(renderer, title_font ? title_font : font,
                                   title.c_str(), title_col);
        draw_text(renderer, t, cx - t.width / 2, cy - 64);
        destroy_text(t);
        if (!subtitle.empty()) {
            rendered_text s = make_text(renderer, font, subtitle.c_str(),
                                       sub_col);
            draw_text(renderer, s, cx - s.width / 2, cy - 18);
            destroy_text(s);
        }
        if (!step.empty()) {
            rendered_text what = make_text(renderer, hint_font ? hint_font : font,
                                         step.c_str(), muted_col);
            draw_text(renderer, what, cx - what.width / 2, cy + 96);
            destroy_text(what);
        }
        const int bar_width = std::min(560, logical_width - 120);
        const int bar_x = (logical_width - bar_width) / 2;
        const int bar_y = cy + 124;
        SDL_SetRenderDrawColor(renderer, 26, 40, 54,
                               static_cast<Uint8>(255 * overlay_alpha));
        const SDL_FRect trough = frect(bar_x, bar_y, bar_width, 14);
        SDL_RenderFillRect(renderer, &trough);
        SDL_SetRenderDrawColor(renderer, 56, 198, 255,
                               static_cast<Uint8>(255 * overlay_alpha));
        const SDL_FRect fill = frect(bar_x, bar_y,
                                     bar_width * std::clamp(progress, 0.0f, 1.0f),
                                     14);
        SDL_RenderFillRect(renderer, &fill);
    }

    // Full render of the loading screen, with spinner.
    void draw_loading_screen(const std::string& title,
                             const std::string& subtitle,
                             const std::string& step,
                             float progress,
                             float spinner_phase,
                             float alpha) {
        draw_loading_text_only(title, subtitle, step, progress, alpha);
        const int cx = logical_width / 2;
        const int cy = logical_height / 2;
        // Spinner: three concentric rings + a wedge that rotates around
        // the centre. Positioned to the left of the title.
        const float ring_radius = 36.0f;
        const float cx_spin = cx - 240.0f;
        const float cy_spin = static_cast<float>(cy) - 30.0f;
        const Uint8 a = static_cast<Uint8>(255 * alpha);

        // Track ring.
        SDL_SetRenderDrawColor(renderer, 56, 198, 255,
                               static_cast<Uint8>(48 * alpha));
        draw_circle_outline(renderer, cx_spin, cy_spin, ring_radius, a);

        // Progress ring: an arc from 12 o'clock to (progress * 2*PI).
        SDL_SetRenderDrawColor(renderer, 56, 198, 255, a);
        draw_arc(renderer, cx_spin, cy_spin, ring_radius,
                 -static_cast<float>(M_PI_2),
                 -static_cast<float>(M_PI_2) +
                     2.0f * static_cast<float>(M_PI) * progress, a);

        // Spinner wedge: a small filled wedge that rotates every 1.5s.
        const float wedge_angle =
            -static_cast<float>(M_PI_2) +
            2.0f * static_cast<float>(M_PI) * spinner_phase;
        SDL_SetRenderDrawColor(renderer, 205, 214, 244, a);
        const float wedge_inner = ring_radius - 8.0f;
        const float wedge_outer = ring_radius - 1.0f;
        draw_arc_filled(renderer, cx_spin, cy_spin,
                         wedge_inner, wedge_outer,
                         wedge_angle - 0.5f, wedge_angle + 0.5f, a);

        // Centre dot.
        SDL_SetRenderDrawColor(renderer, 137, 180, 250, a);
        const SDL_FRect dot = frect(cx_spin - 4, cy_spin - 4, 8, 8);
        SDL_RenderFillRect(renderer, &dot);
    }

    // Helper: draw a circle outline by tracing pixels.
    void draw_circle_outline(SDL_Renderer* renderer, float cx, float cy,
                             float radius, Uint8 a) {
        const int steps = static_cast<int>(radius * 12);
        for (int i = 0; i < steps; ++i) {
            const float t0 = (static_cast<float>(i)     / steps) * 2.0f *
                              static_cast<float>(M_PI);
            const float t1 = (static_cast<float>(i + 1) / steps) * 2.0f *
                              static_cast<float>(M_PI);
            const float x0 = cx + radius * std::cos(t0);
            const float y0 = cy + radius * std::sin(t0);
            const float x1 = cx + radius * std::cos(t1);
            const float y1 = cy + radius * std::sin(t1);
            SDL_RenderLine(renderer, x0, y0, x1, y1);
        }
    }

    // Helper: draw an arc from angle a0 to a1 (radians, 0 = +x, increases
    // counter-clockwise).
    void draw_arc(SDL_Renderer* r, float cx, float cy, float radius,
                  float a0, float a1, Uint8 a) {
        if (a1 < a0) std::swap(a0, a1);
        const int steps = std::max(2, static_cast<int>(radius * (a1 - a0) * 6));
        for (int i = 0; i < steps; ++i) {
            const float t0 = a0 + (a1 - a0) *
                              (static_cast<float>(i)     / steps);
            const float t1 = a0 + (a1 - a0) *
                              (static_cast<float>(i + 1) / steps);
            const float x0 = cx + radius * std::cos(t0);
            const float y0 = cy + radius * std::sin(t0);
            const float x1 = cx + radius * std::cos(t1);
            const float y1 = cy + radius * std::sin(t1);
            SDL_RenderLine(r, x0, y0, x1, y1);
        }
    }

    // Helper: draw a filled ring segment between inner and outer radii.
    void draw_arc_filled(SDL_Renderer* r, float cx, float cy,
                         float inner, float outer, float a0, float a1,
                         Uint8 a) {
        if (a1 < a0) std::swap(a0, a1);
        const int steps = std::max(2, static_cast<int>(outer * (a1 - a0) * 6));
        for (int i = 0; i < steps; ++i) {
            const float t0 = a0 + (a1 - a0) *
                              (static_cast<float>(i)     / steps);
            const float t1 = a0 + (a1 - a0) *
                              (static_cast<float>(i + 1) / steps);
            for (float radius = inner; radius <= outer; radius += 1.5f) {
                const float x0 = cx + radius * std::cos(t0);
                const float y0 = cy + radius * std::sin(t0);
                const float x1 = cx + radius * std::cos(t1);
                const float y1 = cy + radius * std::sin(t1);
                SDL_RenderLine(r, x0, y0, x1, y1);
            }
        }
    }

    int select(const std::string& title, const std::string& description,
               const std::vector<std::string>& items,
               const std::string& back_label, int initial_selection,
               const std::function<bool()>& interrupt = {},
               const std::vector<int>& unavailable = {}) {
        // Scoped to this list: the next screen decides for itself.
        unavailable_rows = unavailable;
        struct clear_on_exit {
            std::vector<int>& rows;
            ~clear_on_exit() { rows.clear(); }
        } clear{unavailable_rows};
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
            if (activate && !row_unavailable(selected))
                return selected < static_cast<int>(items.size()) ? selected : -1;
        }
    }

    // The character wheel a cabinet with no keyboard uses: everything an
    // email address or a password is likely to contain, in an order somebody
    // can predict.
    static const std::string& wheel() {
        static const std::string characters =
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789"
            "@._-+"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "!#$%&*?/";
        return characters;
    }

    std::optional<std::string> prompt_text(
            const std::string& title, const std::string& description,
            const std::string& initial, bool password,
            std::size_t max_length) {
        std::string value = initial.substr(0, max_length);
        std::size_t cursor = value.size();
        std::size_t wheel_at = 0;
        bool redraw = true;

        SDL_StartTextInput(window);
        struct stop_text_input {
            SDL_Window* window;
            ~stop_text_input() { SDL_StopTextInput(window); }
        } const stopper{window};

        for (;;) {
            if (redraw) {
                // Masked, but the length is still shown: typing a password
                // blind with no feedback at all is how people give up.
                std::string shown = password ? std::string(value.size(), '*')
                                             : value;
                shown.insert(std::min(cursor, shown.size()), "_");
                const std::vector<std::string> rows{
                    shown.empty() ? std::string("_") : shown,
                    "",
                    "Type it, or use the stick: up and down pick a letter, "
                    "left and right move along."};
                draw_frame(title, description, rows, 0, 0,
                           static_cast<int>(rows.size()), 152,
                           "ENTER: DONE    ESC: CANCEL    BACKSPACE: DELETE");
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
                continue;
            }

            if (event.type == SDL_EVENT_TEXT_INPUT) {
                for (const char* at = event.text.text; at && *at; ++at) {
                    if (value.size() >= max_length) break;
                    // One line, so anything that would break it is dropped
                    // rather than silently mangling the field.
                    if (static_cast<unsigned char>(*at) < 0x20) continue;
                    value.insert(value.begin() +
                                 static_cast<std::ptrdiff_t>(cursor), *at);
                    ++cursor;
                }
                redraw = true;
                continue;
            }

            const auto insert_wheel = [&] {
                if (value.size() >= max_length) return;
                value.insert(value.begin() +
                             static_cast<std::ptrdiff_t>(cursor),
                             wheel()[wheel_at]);
                ++cursor;
            };
            const auto backspace = [&] {
                if (cursor == 0 || value.empty()) return;
                value.erase(value.begin() +
                            static_cast<std::ptrdiff_t>(cursor - 1));
                --cursor;
            };

            if (event.type == SDL_EVENT_KEY_DOWN) {
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    return std::nullopt;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                    return value;
                case SDL_SCANCODE_BACKSPACE: backspace(); break;
                case SDL_SCANCODE_DELETE:
                    if (cursor < value.size())
                        value.erase(value.begin() +
                                    static_cast<std::ptrdiff_t>(cursor));
                    break;
                case SDL_SCANCODE_LEFT:
                    if (cursor > 0) --cursor;
                    break;
                case SDL_SCANCODE_RIGHT:
                    if (cursor < value.size()) ++cursor;
                    break;
                case SDL_SCANCODE_HOME: cursor = 0; break;
                case SDL_SCANCODE_END: cursor = value.size(); break;
                default: break;
                }
                redraw = true;
                continue;
            }

            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_SOUTH: insert_wheel(); break;
                case SDL_GAMEPAD_BUTTON_EAST:  backspace(); break;
                case SDL_GAMEPAD_BUTTON_START: return value;
                case SDL_GAMEPAD_BUTTON_BACK:  return std::nullopt;
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    wheel_at = (wheel_at + wheel().size() - 1) % wheel().size();
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    wheel_at = (wheel_at + 1) % wheel().size();
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                    if (cursor > 0) --cursor;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                    if (cursor < value.size()) ++cursor;
                    break;
                default: break;
                }
                redraw = true;
                continue;
            }
        }
    }


    // The card grid. Every screen in the launcher chooses between a handful
    // of things, and until now all of them were drawn as a column of words
    // on the left - because select_modes threw the icon, the artwork, the
    // subtitle and the badge away and passed only the titles to the list.
    // The data was always there; nothing drew it.
    //
    // A tile shows its artwork if it has any. If it does not, it shows its
    // name set large on a field of its own colour, which is a tile rather
    // than a line of text - the fallback is a different rendering of the
    // same thing, not a different kind of screen.
    struct card_texture {
        SDL_Texture* texture{};
        const uint8_t* source{};
    };

    // A stable colour per tile, so a publisher keeps the same field every
    // time it is drawn and the shelf is recognisable at a glance.
    static SDL_Color field_colour(const std::string& name, bool dim) {
        uint32_t mixed = 2166136261u;
        for (const char character : name) {
            mixed ^= static_cast<unsigned char>(character);
            mixed *= 16777619u;
        }
        static const SDL_Color palette[] = {
            {46, 74, 112, 255},  {96, 52, 44, 255},   {48, 84, 66, 255},
            {84, 58, 96, 255},   {104, 82, 36, 255},  {40, 78, 96, 255},
            {90, 44, 62, 255},   {58, 66, 46, 255},
        };
        SDL_Color chosen = palette[mixed % (sizeof(palette) / sizeof(*palette))];
        if (dim) {
            chosen.r = static_cast<Uint8>(chosen.r / 2);
            chosen.g = static_cast<Uint8>(chosen.g / 2);
            chosen.b = static_cast<Uint8>(chosen.b / 2);
        }
        return chosen;
    }

    SDL_Texture* card_artwork(std::map<int, card_texture>& cache, int index,
                              const launcher_menu::mode_card& card) {
        if (!card.artwork || card.artwork_width <= 0 || card.artwork_height <= 0)
            return nullptr;
        auto& slot = cache[index];
        if (slot.texture && slot.source == card.artwork) return slot.texture;
        if (slot.texture) SDL_DestroyTexture(slot.texture);
        slot.texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
            card.artwork_width, card.artwork_height);
        if (slot.texture) {
            SDL_UpdateTexture(slot.texture, nullptr, card.artwork,
                              card.artwork_width * 4);
            SDL_SetTextureScaleMode(slot.texture, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(slot.texture, SDL_BLENDMODE_BLEND);
        }
        slot.source = card.artwork;
        return slot.texture;
    }

    int select_cards(const std::string& title, const std::string& description,
                     const std::vector<launcher_menu::mode_card>& cards,
                     const std::string& back_label, int initial_selection,
                     const std::function<bool()>& interrupt,
                     const std::function<bool()>& tick) {
        const int total = static_cast<int>(cards.size());
        int selected = std::clamp(initial_selection, 0, total - 1);
        int first_row = 0;
        std::map<int, card_texture> art;
        bool redraw = true;

        // Wider than a game cover: these are names and logos, not box art.
        const int gap = 16;
        const int usable = logical_width - horizontal_margin * 2;
        const int columns = std::max(1, (usable + gap) / (300 + gap));
        const int card_width = (usable - gap * (columns - 1)) / columns;
        const int card_height = card_width * 9 / 16;
        const int top = 150;
        const int rows_visible =
            std::max(1, (logical_height - 90 - top + gap) / (card_height + gap));

        const auto keep_visible = [&] {
            const int row = selected / columns;
            if (row < first_row) first_row = row;
            if (row >= first_row + rows_visible)
                first_row = row - rows_visible + 1;
        };

        for (;;) {
            if (redraw) {
                keep_visible();
                SDL_SetRenderDrawColor(renderer, 8, 13, 20, 255);
                SDL_RenderClear(renderer);
                SDL_SetRenderDrawColor(renderer, 56, 198, 255, 255);
                const SDL_FRect top_edge = frect(0, 0, logical_width, 5);
                const SDL_FRect bottom_edge =
                    frect(0, logical_height - 5, logical_width, 5);
                SDL_RenderFillRect(renderer, &top_edge);
                SDL_RenderFillRect(renderer, &bottom_edge);

                rendered_text heading = make_text(
                    renderer, title_font ? title_font : font, title,
                    SDL_Color{56, 198, 255, 255});
                draw_text(renderer, heading, horizontal_margin, 30);
                destroy_text(heading);
                if (!description.empty()) {
                    rendered_text prose = make_text(
                        renderer, desc_font ? desc_font : font, description,
                        SDL_Color{168, 180, 196, 255}, usable);
                    draw_text(renderer, prose, horizontal_margin, 74);
                    destroy_text(prose);
                }

                for (int row = 0; row < rows_visible; ++row) {
                    for (int column = 0; column < columns; ++column) {
                        const int index =
                            (first_row + row) * columns + column;
                        if (index >= total) break;
                        const launcher_menu::mode_card& card = cards[index];
                        const int x = horizontal_margin +
                                      column * (card_width + gap);
                        const int y = top + row * (card_height + gap);
                        const bool chosen = index == selected;

                        const SDL_Color field =
                            field_colour(card.title, card.unavailable);
                        SDL_SetRenderDrawColor(renderer, field.r, field.g,
                                               field.b, 255);
                        const SDL_FRect tile =
                            frect(x, y, card_width, card_height);
                        SDL_RenderFillRect(renderer, &tile);

                        if (SDL_Texture* picture =
                                card_artwork(art, index, card)) {
                            // Fitted, never stretched: a squashed logo looks
                            // worse than a smaller one.
                            float pw = 0, ph = 0;
                            SDL_GetTextureSize(picture, &pw, &ph);
                            const float scale = std::min(
                                (card_width - 24) / std::max(pw, 1.0f),
                                (card_height - 24) / std::max(ph, 1.0f));
                            const SDL_FRect where = frect(
                                x + (card_width - static_cast<int>(pw * scale)) / 2,
                                y + (card_height - static_cast<int>(ph * scale)) / 2,
                                static_cast<int>(pw * scale),
                                static_cast<int>(ph * scale));
                            SDL_SetTextureAlphaMod(
                                picture, card.unavailable ? 110 : 255);
                            SDL_RenderTexture(renderer, picture, nullptr, &where);
                        } else {
                            // No media, so the name becomes the tile.
                            rendered_text name = make_text(
                                renderer, title_font ? title_font : font,
                                card.title,
                                SDL_Color{240, 244, 250,
                                          static_cast<Uint8>(
                                              card.unavailable ? 130 : 255)});
                            const float scale = std::min(
                                1.6f, (card_width - 32) /
                                          std::max(1, name.width) * 1.0f);
                            draw_text_scaled(
                                renderer, name,
                                x + (card_width -
                                     static_cast<int>(name.width * scale)) / 2,
                                y + (card_height -
                                     static_cast<int>(name.height * scale)) / 2 - 8,
                                scale);
                            destroy_text(name);
                        }

                        TTF_Font* small = hint_font ? hint_font : font;
                        if (!card.status.empty()) {
                            rendered_text badge = make_text(
                                renderer, small, card.status,
                                SDL_Color{16, 22, 30, 255});
                            SDL_SetRenderDrawColor(renderer, 56, 198, 255, 235);
                            const SDL_FRect badge_field =
                                frect(x + card_width - badge.width - 18, y + 8,
                                      badge.width + 12, badge.height + 6);
                            SDL_RenderFillRect(renderer, &badge_field);
                            draw_text(renderer, badge,
                                      x + card_width - badge.width - 12, y + 11);
                            destroy_text(badge);
                        }
                        if (!card.subtitle.empty()) {
                            rendered_text note = make_text(
                                renderer, small, card.subtitle,
                                SDL_Color{206, 214, 226, 255},
                                card_width - 24);
                            draw_text(renderer, note, x + 12,
                                      y + card_height - note.height - 10);
                            destroy_text(note);
                        }

                        SDL_SetRenderDrawColor(
                            renderer, chosen ? 56 : 40, chosen ? 198 : 52,
                            chosen ? 255 : 66, 255);
                        for (int ring = 0; ring < (chosen ? 4 : 1); ++ring) {
                            const SDL_FRect edge =
                                frect(x - ring, y - ring,
                                      card_width + ring * 2,
                                      card_height + ring * 2);
                            SDL_RenderRect(renderer, &edge);
                        }
                    }
                }

                rendered_text footer = make_text(
                    renderer, hint_font ? hint_font : font,
                    "MOVE: ARROWS / STICK    SELECT: ENTER / A    " +
                        (back_label.empty() ? std::string("BACK: ESC / B")
                                            : back_label + ": ESC / B"),
                    SDL_Color{130, 142, 158, 255});
                draw_text(renderer, footer, horizontal_margin,
                          logical_height - 44);
                destroy_text(footer);

                SDL_RenderPresent(renderer);
                redraw = false;
            }

            if (tick && tick()) redraw = true;
            if (interrupt && interrupt()) {
                for (auto& entry : art)
                    if (entry.second.texture)
                        SDL_DestroyTexture(entry.second.texture);
                return launcher_menu::interrupted;
            }

            SDL_Event event{};
            if (!SDL_WaitEventTimeout(&event, 60)) continue;

            const auto finish = [&](int result) {
                for (auto& entry : art)
                    if (entry.second.texture)
                        SDL_DestroyTexture(entry.second.texture);
                return result;
            };
            const auto move = [&](int by) {
                const int wanted = std::clamp(selected + by, 0, total - 1);
                selected = wanted;
                redraw = true;
            };

            if (event.type == SDL_EVENT_QUIT)
                return finish(launcher_menu::exit_requested);
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
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE: return finish(-1);
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                case SDL_SCANCODE_SPACE:
                    if (!cards[static_cast<std::size_t>(selected)].unavailable)
                        return finish(selected);
                    break;
                case SDL_SCANCODE_LEFT:  move(-1); break;
                case SDL_SCANCODE_RIGHT: move(1); break;
                case SDL_SCANCODE_UP:    move(-columns); break;
                case SDL_SCANCODE_DOWN:  move(columns); break;
                case SDL_SCANCODE_HOME:  selected = 0; redraw = true; break;
                case SDL_SCANCODE_END:   selected = total - 1; redraw = true; break;
                default: break;
                }
                continue;
            }
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_SOUTH:
                    if (!cards[static_cast<std::size_t>(selected)].unavailable)
                        return finish(selected);
                    break;
                case SDL_GAMEPAD_BUTTON_EAST: return finish(-1);
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  move(-1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: move(1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_UP:    move(-columns); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  move(columns); break;
                default: break;
                }
                continue;
            }
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
                          int initial_selection,
                          const std::vector<int>& unavailable) {
    if (!m_impl->ready())
        return fallback_select(title, description, items, back_label,
                               initial_selection);
    SDL_SetWindowTitle(m_impl->window, title.c_str());
    SDL_RaiseWindow(m_impl->window);
    return m_impl->select(title, description, items, back_label,
                          initial_selection, {}, unavailable);
}

int launcher_menu::select_grid(
        const std::string& title, const std::string& description,
        const std::vector<std::string>& items,
        const std::string& back_label, int initial_selection,
        std::function<cover(int)> cover_for, std::function<bool()> tick,
        std::function<bool()> interrupt, bool paging, const cover* banner,
        bool info, bool list_view, bool marquee_view,
        bool coverflow_view,
        std::function<std::string(int)> video_for,
        std::function<game_media(int)> media_for,
        const std::vector<std::string>* item_descriptions) {
    // No window means no artwork; the text selector is the honest fallback.
    if (!m_impl->ready())
        return fallback_select(title, description, items, back_label,
                               initial_selection);
    SDL_RaiseWindow(m_impl->window);
    const std::vector<int> chosen =
        m_impl->run_grid(title, description, items, back_label,
                         initial_selection, cover_for, tick, 0, interrupt,
                         paging, banner, info, list_view, marquee_view,
                         coverflow_view, video_for, media_for,
                         item_descriptions);
    return chosen.empty() ? -1 : chosen.front();
}

void launcher_menu::arrange_coverflow_next() {
    if (!m_impl->ready()) return;
    m_impl->cf_design_pending = true;
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
    SDL_SetWindowTitle(m_impl->window, "MANX");
    m_impl->splash(step, progress);
}

void launcher_menu::show_loading_screen(
    const std::string& title,
    const std::string& subtitle,
    const std::function<bool(float&)>& step_for) {
    if (!m_impl || !m_impl->ready()) return;
    SDL_SetWindowTitle(m_impl->window, "MANX");
    m_impl->loading_screen(title, subtitle, step_for);
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

std::optional<std::string> launcher_menu::prompt_text(
        const std::string& title, const std::string& description,
        const std::string& initial, bool password, std::size_t max_length) {
    return m_impl->prompt_text(title, description, initial, password,
                               max_length);
}

std::optional<input_binding> launcher_menu::capture_binding(
    const std::string& title, const std::string& description,
    bool keyboard, int32_t controller_instance, bool allow_inherit) {
    if (!m_impl->ready()) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_INFORMATION, title.c_str(),
            "Interactive input capture requires the MANX launcher "
            "window.", nullptr);
        return std::nullopt;
    }
    SDL_SetWindowTitle(m_impl->window, title.c_str());
    SDL_RaiseWindow(m_impl->window);
    return m_impl->capture_binding(title, description, keyboard,
                                   controller_instance, allow_inherit);
}


int launcher_menu::select_modes(const std::string& title,
                              const std::string& description,
                              const std::vector<mode_card>& cards,
                              const std::string& back_label,
                              int initial_selection,
                              std::function<bool()> interrupt,
                              std::function<bool()> tick) {
    if (cards.empty()) return -1;
    // Debug-only: drive straight through mode pages to reach the game grid
    // headlessly.
    if (std::getenv("MANX_AUTOPILOT"))
        return std::clamp(initial_selection, 0,
                          static_cast<int>(cards.size()) - 1);

    std::vector<std::string> items;
    std::vector<int> unavailable;
    for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
        items.push_back(cards[i].title);
        if (cards[i].unavailable) unavailable.push_back(i);
    }

    if (!m_impl->ready()) {
        return fallback_select(title, description, items, back_label,
                               initial_selection);
    }
    SDL_SetWindowTitle(m_impl->window, title.c_str());
    SDL_RaiseWindow(m_impl->window);
    return m_impl->select_cards(title, description, cards, back_label,
                                initial_selection, interrupt, tick);
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
