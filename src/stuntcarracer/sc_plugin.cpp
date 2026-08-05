// Stunt Car Racer as a MANX plugin.
//
// Builds a MANX-compatible shared library: the host dlopens us,
// looks up `manx_game_entry`, and drives our run_frame() each tick
// with already-mapped input. We produce a tightly-packed RGBA8 frame
// for the host to upload to its texture.
//
// Rendering: the ported 3D engine (3d_engine_linux.cpp) runs the
// math engine, then a CPU rasterizer (added in the same TU) writes
// the resulting screen-space triangles to the host's frame buffer.
// No GPU, no OpenGL, no EGL, no Vulkan — works on any POSIX or
// Android target. The rasterizer matches the original D3D color
// encoding (0xAARRGGBB) and uses the same fixed-point orientation
// test the engine's Polygon() function uses.
//
// To build:
//   c++ -std=c++20 -fPIC -shared -I../include \
//       sc_plugin.cpp 3d_engine_linux.cpp \
//       -o libstuntcarracer.so
//   cp libstuntcarracer.so ~/.local/share/manx/games/stuntcarracer/

#include "manx_game_plugin.h"
#include "sc_rasterizer.h"  // brings DWORD, COORD_3D etc. via platform_linux.h

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

// 3D engine declarations. The engine file is compiled into the same
// shared library so we have direct access to its globals. We do NOT
// link against the standalone binary — that would create a second
// copy of the engine's static state and the two would desync.
namespace sc_engine {
struct instance {
    // Engine state. The original DX9 build keeps the state in
    // statics inside 3d_engine_linux.cpp; in the plugin we have
    // exactly one instance per plugin (single-player for now), so
    // we leave the statics alone. If the engine is ever extended to
    // multi-instance, this struct will own the per-instance fields
    // and the statics become a "current instance" pointer.
    uint32_t*    pixels = nullptr;
    int          width  = 0;
    int          height = 0;
};
}

// The engine's 3d_engine_linux.cpp declares Fill_Colour / Line_Colour
// as `extern DWORD` (C++ linkage). The same engine file is linked into
// the same .so, so the engine's definition is the one that ends up
// in the binary. We just declare them here for the plugin's use. The
// default color values (Catppuccin blue) come from the engine.
extern DWORD Fill_Colour;
extern DWORD Line_Colour;

// Engine entry points. C++ linkage (the engine file does NOT wrap
// them in extern "C", so the symbols are C++-mangled).
extern void CreateSinCosTable(void);
extern long CalcYXZTrigCoefficients(long x, long y, long z);
extern long TransformCoordinates(void* cptr, long size);
extern long Polygon(long* cptr, long sides);
extern void Line(long c1, long c2);
extern void DrawFilledRectangle(long x1, long y1, long x2, long y2, DWORD color);
extern void* Port_GetDefaultScreenCoords(int* out_size);
extern void* Port_GetDefaultTransformedCoords(int* out_size);

// GetScreenDimensions is implemented in 3d_engine_linux.cpp by
// calling Port3D_GetScreenW / Port3D_GetScreenH, which are defined
// in port3d_linux.cpp. We don't link port3d_linux.cpp into the
// plugin (it pulls in OpenGL). Provide a stub here that returns the
// actual frame size — the plugin sets it just before each run_frame.
extern "C" int Port3D_GetScreenW(void);
extern "C" int Port3D_GetScreenH(void);
static int g_plugin_w = 0, g_plugin_h = 0;
extern "C" int Port3D_GetScreenW(void) { return g_plugin_w; }
extern "C" int Port3D_GetScreenH(void) { return g_plugin_h; }
extern "C" void GetScreenDimensions(long* w, long* h) {
    if (w) *w = g_plugin_w;
    if (h) *h = g_plugin_h;
}
extern "C" void Port3D_SetScreenSize(int w, int h) {
    g_plugin_w = w; g_plugin_h = h;
}

// The engine's inline DXUTGetD3DDevice() in platform_linux.h calls
// Port3D_GetDevice() to get the active "device". For the plugin
// we don't have a real device — return nullptr. The engine's GL
// backend paths check for nullptr and skip, but in the MANX_PLUGIN
// build the GL paths are #ifdef'd out anyway, so nullptr is fine.
void* Port3D_GetDevice() { return nullptr; }

DWORD Fill_Colour = 0xFF89B4FAu;  // ARGB Catppuccin blue (default)
DWORD Line_Colour = 0xFFCDD6F4u;  // ARGB Catppuccin text

// CPU rasterizer (compiled in the same TU as 3d_engine_linux.cpp
// when -DMANX_PLUGIN=1 is passed). Provides:
//   namespace sc_raster { struct frame; void fill_triangle(...);
//   void draw_line(...); void fill_rect(...); }
// The engine's render path (DrawPolygon, Line, DrawFilledRectangle)
// is replaced with CPU calls when MANX_PLUGIN is set.
#include "sc_rasterizer.h"  // CPU rasterizer (header from the port repo)

namespace {

// Clear the frame to Catppuccin base.
void clear_frame(sc_raster::frame& f) {
    for (int i = 0; i < f.width * f.height; i++) {
        f.pixels[i] = 0xFF1E1E2Eu;  // RGBA: opaque Catppuccin base
    }
}

// Run one frame of the engine. The math + draw list is identical to
// the standalone binary; the only difference is that DrawPolygon /
// Line / DrawFilledRectangle call the CPU rasterizer instead of GL.
void render_frame(sc_raster::frame& f) {
    clear_frame(f);
    CreateSinCosTable();

    // The original game's main render path is in stuntcarracer.cpp's
    // FrameRender(), which the upstream wires through IDirect3D9
    // device calls. We don't have that file ported yet (session 5
    // work). For the session-2 deliverable we drive the engine
    // manually with a simple test scene: a single rotating triangle
    // on a black panel. This proves the rasterizer works and gives
    // the user a visible result. Sessions 3+ replace this with the
    // real game render.
    long w, h;
    GetScreenDimensions(&w, &h);
    int n;
    auto* sc = (long(*)[2])Port_GetDefaultScreenCoords(&n);
    auto* tc = (long(*)[3])Port_GetDefaultTransformedCoords(&n);
    (void)tc;  // unused in the test scene

    // Set a viewport center for the test scene.
    int cx = (int)w / 2;
    int cy = (int)h / 2;
    int r = std::min(cx, cy) - 40;

    // A 6-vertex hexagon (fan) in the screen, with a Catppuccin
    // gradient from top to bottom. This exercises fill_triangle()
    // and shows the user the render path works.
    Fill_Colour = 0xFF89B4FAu;  // blue
    Line_Colour = 0xFFCDD6F4u;  // off-white
    long cptr[6];
    for (int i = 0; i < 6; i++) {
        // Use a 6-vertex polyon with the standard angle increments
        cptr[i] = i;
        sc[i][0] = cx + (int)(r * 0.95 * std::cos(i * 3.14159 / 3.0));
        sc[i][1] = cy + (int)(r * 0.95 * std::sin(i * 3.14159 / 3.0));
    }
    // The engine's Polygon() does its own orientation test + draws.
    // Our build of 3d_engine_linux.cpp with -DMANX_PLUGIN=1 routes
    // DrawPolygon() to the CPU rasterizer (via the function-pointer
    // backend), so this call rasterizes the hexagon into `f`.
    //
    // 6-sided fan = 4 triangles, all of the same color (Fill_Colour).
    // For a gradient we'd vary Fill_Colour per vertex, but the
    // engine's flat-shaded Polygon() uses a single color.
    Polygon(cptr, 6);

    // Draw a small star polygon (different color) to show line draws
    // work too.
    Fill_Colour = 0xFFF38BA8u;  // pink
    long star[5];
    for (int i = 0; i < 5; i++) {
        star[i] = i;
        sc[i][0] = cx + (int)(r * 0.4 * std::cos(i * 1.2566));
        sc[i][1] = cy + (int)(r * 0.4 * std::sin(i * 1.2566));
    }
    Polygon(star, 5);

    // Outlines to show Line() works.
    Line_Colour = 0xFFCDD6F4u;
    Line(0, 3);
    Line(3, 1);
    Line(1, 4);
    Line(4, 2);
    Line(2, 0);
}

}  // namespace

// ── MANX plugin vtable ───────────────────────────────────────────
//
// One global instance for the plugin (single-player only). MANX's
// plugin API is single-threaded — all callbacks happen on the host's
// main thread, sequentially — so a global is safe and matches the
// upstream engine's single-instance model.

static sc_raster::frame g_frame = {nullptr, 0, 0};

// The MANX ABI requires this exact symbol name. -fvisibility=hidden
// is the right default (keeps game internals from leaking), so we mark
// just this one function as exported.
#define MANX_GAME_EXPORT __attribute__((visibility("default")))

extern "C" MANX_GAME_EXPORT const manx_game_api* manx_game_entry(void) {
    static manx_game_api api{};
    api.abi_version    = MANX_GAME_ABI_VERSION;
    api.describe       = [] (manx_game_info* out) {
        out->short_name       = "stuntcarracer";
        out->display_name     = "Stunt Car Racer (Linux Port)";
        out->publisher        = "Crown Park Computing / MANX Framework";
        out->max_players      = 1;
        out->supports_network = 0;
        out->refresh_hz       = 60.0;
    };
    api.create          = [] (const char* bundle_path) -> manx_game_instance* {
        (void)bundle_path;  // The plugin has no ROM data to load
        return reinterpret_cast<manx_game_instance*>(new sc_engine::instance);
    };
    api.destroy         = [] (manx_game_instance* instance) {
        delete reinterpret_cast<sc_engine::instance*>(instance);
    };
    api.reset           = [] (manx_game_instance* instance) {
        (void)instance;  // stateless; engine reset = recreate trig table
        CreateSinCosTable();
    };
    api.set_paused       = [] (manx_game_instance*, uint32_t) {
        // No pause logic in the test scene. Real game would suspend
        // the simulation step here.
    };
    api.run_frame       = [] (manx_game_instance* instance,
                               const manx_game_input* inputs,
                               uint32_t player_count,
                               manx_game_frame* out_frame) {
        (void)instance;
        (void)inputs;
        (void)player_count;
        // The MANX host sets the frame's width/height/pixels BEFORE
        // calling run_frame (the host allocates a fresh buffer each
        // frame). We just write to the buffer it's given.
        g_frame.pixels = const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(out_frame->pixels));
        g_frame.width  = (int)out_frame->width;
        g_frame.height = (int)out_frame->height;
        g_plugin_w = g_frame.width;
        g_plugin_h = g_frame.height;
        std::fprintf(stderr, "[stunt] run_frame: %dx%d pixels=%p\n",
                     g_frame.width, g_frame.height, (void*)g_frame.pixels);
        render_frame(g_frame);
    };
    api.score           = [] (manx_game_instance*) -> uint64_t {
        return 0;  // No scoring in the test scene
    };
    api.state_checksum  = [] (manx_game_instance*) -> uint64_t {
        return 0;  // Single-player, no network sync
    };
    api.take_audio_cues = [] (manx_game_instance*,
                               manx_game_audio_cue* out, uint32_t max_cues) -> uint32_t {
        (void)out; (void)max_cues;
        return 0;  // No audio in the test scene
    };
    api.describe_audio_cues = [] (const char** out_names, uint32_t max_names) -> uint32_t {
        if (out_names && max_names > 0) out_names[0] = nullptr;
        return 0;
    };
    return &api;
}
