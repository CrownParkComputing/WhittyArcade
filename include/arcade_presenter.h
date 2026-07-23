// arcade_presenter.h - Vulkan and software host presentation backends
#pragma once

#include "arcade_settings.h"

#include <cstdint>
#include <memory>

// Visible host-window output used when the board renderer is not presenting
// directly through its OpenGL context. Vulkan is a native swapchain transfer
// path; software uses SDL's CPU renderer. Both consume a finished RGBA frame.
class alternate_presenter {
public:
    alternate_presenter();
    ~alternate_presenter();

    alternate_presenter(const alternate_presenter&) = delete;
    alternate_presenter& operator=(const alternate_presenter&) = delete;

    bool initialize(renderer_backend backend, int width, int height,
                    const emulator_settings& settings);
    void shutdown();
    void apply_settings(const emulator_settings& settings);
    void drawable_size(int& width, int& height) const;
    void* window() const;
    renderer_backend backend() const;

    // OpenGL readback begins at the lower-left. The presenter flips it while
    // copying into its top-left host output.
    bool present_rgba_bottom_up(const uint8_t* pixels, int width, int height,
                                const uint8_t* overlay_pixels = nullptr,
                                int overlay_width = 0,
                                int overlay_height = 0,
                                bool menu_visible = false,
                                int display_width = 0,
                                int display_height = 0);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};
