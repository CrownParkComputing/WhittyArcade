#include "arcade_video_worker.h"

#include "namco/system22/system22_config.h"
#include "arcade_renderer.h"

#include <algorithm>
#include <chrono>
#include <future>

arcade_video_worker::arcade_video_worker() = default;
arcade_video_worker::~arcade_video_worker() { shutdown(); }

bool arcade_video_worker::initialize(const emulator_settings& settings) {
    if (m_thread.joinable()) return m_start_success;
    m_stop.store(false);
    m_alive.store(true);
    m_host_action.store(arcade_host_action::continue_running);
    m_start_complete = false;
    m_start_success = false;
    m_thread = std::thread(&arcade_video_worker::worker_loop, this, settings);
    std::unique_lock<std::mutex> lock(m_start_mutex);
    m_started.wait(lock, [this] { return m_start_complete; });
    if (!m_start_success) {
        lock.unlock();
        shutdown();
    }
    return m_start_success;
}

void arcade_video_worker::shutdown() {
    m_stop.store(true);
    m_alive.store(false);
    m_task_ready.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lock(m_task_mutex);
    m_tasks.clear();
    m_pending_rgba_frame.reset();
    m_rgba_task_queued = false;
    m_pending_model2_frame.reset();
    m_model2_task_queued = false;
}

arcade_host_action arcade_video_worker::process_events() const {
    const arcade_host_action action = m_host_action.load();
    if (action != arcade_host_action::continue_running) return action;
    return m_alive.load() ? arcade_host_action::continue_running :
                            arcade_host_action::exit_application;
}

void arcade_video_worker::reset_session() {
    if (!m_alive.load(std::memory_order_acquire)) return;
    auto completed = std::make_shared<std::promise<void>>();
    std::future<void> result = completed->get_future();
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        m_tasks.clear();
        m_pending_rgba_frame.reset();
        m_rgba_task_queued = false;
        m_pending_model2_frame.reset();
        m_model2_task_queued = false;
        m_tasks.emplace_back([completed](polygon_renderer_gpu& renderer) {
            renderer.set_lightgun_cursor(false);
            renderer.set_single_screen_only(false);
            renderer.set_cabinet_status({});
            renderer.reset_session_ui();
            completed->set_value();
        });
    }
    m_task_ready.notify_one();
    // The window may have been closed between the alive check and the fence.
    // Do not deadlock teardown if the worker exits without consuming it.
    result.wait_for(std::chrono::seconds(2));
    {
        std::lock_guard<std::mutex> lock(m_frontend_mutex);
        m_operator_actions.clear();
        m_controls_pending = false;
    }
    m_settings_visible.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
}

void arcade_video_worker::run_modal(std::function<void()> action) {
    if (!action) return;
    if (!m_alive.load(std::memory_order_acquire)) {
        action();
        return;
    }
    auto completed = std::make_shared<std::promise<void>>();
    std::future<void> result = completed->get_future();
    enqueue([action = std::move(action), completed](polygon_renderer_gpu& renderer) {
        const bool restore_lightgun = renderer.lightgun_cursor_enabled();
        const uint8_t restore_player = renderer.lightgun_cursor_player();
        renderer.set_lightgun_cursor(false);
        action();
        renderer.set_lightgun_cursor(restore_lightgun, restore_player);
        completed->set_value();
    });
    result.wait();
}

void arcade_video_worker::enqueue(task work) {
    if (!m_alive.load()) return;
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        m_tasks.emplace_back(std::move(work));
    }
    m_task_ready.notify_one();
}

void arcade_video_worker::worker_loop(emulator_settings settings) {
    m_renderer = std::make_unique<polygon_renderer_gpu>();
    const bool initialized = m_renderer->initialize(settings);
    {
        std::lock_guard<std::mutex> lock(m_start_mutex);
        m_start_success = initialized;
        m_start_complete = true;
    }
    m_started.notify_all();
    if (!initialized) {
        m_alive.store(false);
        m_renderer.reset();
        return;
    }

    while (!m_stop.load()) {
        task work;
        {
            std::unique_lock<std::mutex> lock(m_task_mutex);
            m_task_ready.wait_for(lock, std::chrono::milliseconds(2),
                [this] { return m_stop.load() || !m_tasks.empty(); });
            if (m_stop.load()) break;
            if (!m_tasks.empty()) {
                work = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
        }
        if (work) work(*m_renderer);
        const arcade_host_action host_action = m_renderer->process_events();
        if (host_action == arcade_host_action::return_to_menu) {
            m_host_action.store(host_action);
        } else if (host_action == arcade_host_action::exit_application) {
            m_host_action.store(host_action);
            m_alive.store(false);
            break;
        }
        harvest_frontend(*m_renderer);
    }
    m_alive.store(false);
    m_renderer->shutdown();
    m_renderer.reset();
}

void arcade_video_worker::harvest_frontend(polygon_renderer_gpu& renderer) {
    m_settings_visible.store(renderer.settings_visible());
    m_paused.store(renderer.paused());
    std::string selected;
    emulator_settings changed;
    operator_menu_action operator_action;
    const bool has_rom = renderer.take_rom_selection(selected);
    const bool has_operator = renderer.take_operator_action(operator_action);
    const bool has_controls = renderer.take_controls_request();
    const bool has_settings = renderer.take_settings_change(changed);
    if (!has_rom && !has_operator && !has_controls && !has_settings) return;
    std::lock_guard<std::mutex> lock(m_frontend_mutex);
    if (has_rom) {
        m_selected_rom = std::move(selected);
        m_rom_pending = true;
    }
    if (has_operator) m_operator_actions.push_back(operator_action);
    if (has_controls) m_controls_pending = true;
    if (has_settings) {
        m_changed_settings = changed;
        m_settings_pending = true;
    }
}

void arcade_video_worker::refresh_output() {
    enqueue([](polygon_renderer_gpu& renderer) {
        renderer.refresh_output();
    });
}

void arcade_video_worker::set_single_screen_only(bool enabled) {
    enqueue([enabled](polygon_renderer_gpu& renderer) {
        renderer.set_single_screen_only(enabled);
    });
}

void arcade_video_worker::set_cabinet_status(std::string status) {
    enqueue([status = std::move(status)](polygon_renderer_gpu& renderer) mutable {
        renderer.set_cabinet_status(std::move(status));
    });
}

void arcade_video_worker::set_rom_choices(
    std::vector<rom_choice> choices) {
    enqueue([choices = std::move(choices)](polygon_renderer_gpu& renderer) mutable {
        renderer.set_rom_choices(std::move(choices));
    });
}

void arcade_video_worker::set_operator_menu(operator_menu_definition menu) {
    enqueue([menu = std::move(menu)](polygon_renderer_gpu& renderer) mutable {
        renderer.set_operator_menu(std::move(menu));
    });
}

bool arcade_video_worker::take_rom_selection(std::string& path) {
    std::lock_guard<std::mutex> lock(m_frontend_mutex);
    if (!m_rom_pending) return false;
    path = std::move(m_selected_rom);
    m_rom_pending = false;
    return true;
}

bool arcade_video_worker::take_operator_action(operator_menu_action& action) {
    std::lock_guard<std::mutex> lock(m_frontend_mutex);
    if (m_operator_actions.empty()) return false;
    action = m_operator_actions.front();
    m_operator_actions.pop_front();
    return true;
}

bool arcade_video_worker::take_controls_request() {
    std::lock_guard<std::mutex> lock(m_frontend_mutex);
    const bool pending = m_controls_pending;
    m_controls_pending = false;
    return pending;
}

void arcade_video_worker::snapshot_picture_rect(polygon_renderer_gpu& renderer) {
    int x = 0, y = 0, w = 0, h = 0, dw = 0, dh = 0;
    if (!renderer.picture_rect(x, y, w, h, dw, dh)) return;
    std::lock_guard<std::mutex> lock(m_picture_rect_mutex);
    m_picture_rect = {x, y, w, h, dw, dh};
}

bool arcade_video_worker::picture_rect(int& x, int& y, int& width, int& height,
                                       int& drawable_width,
                                       int& drawable_height) const {
    std::lock_guard<std::mutex> lock(m_picture_rect_mutex);
    if (m_picture_rect.width <= 0 || m_picture_rect.height <= 0) return false;
    x = m_picture_rect.x;
    y = m_picture_rect.y;
    width = m_picture_rect.width;
    height = m_picture_rect.height;
    drawable_width = m_picture_rect.drawable_width;
    drawable_height = m_picture_rect.drawable_height;
    return true;
}

void arcade_video_worker::set_lightgun_cursor(bool enabled, uint8_t player) {
    enqueue([enabled, player](polygon_renderer_gpu& renderer) {
        renderer.set_lightgun_cursor(enabled, player);
    });
}

bool arcade_video_worker::take_settings_change(emulator_settings& settings) {
    std::lock_guard<std::mutex> lock(m_frontend_mutex);
    if (!m_settings_pending) return false;
    settings = m_changed_settings;
    m_settings_pending = false;
    return true;
}

void arcade_video_worker::apply_display_settings(
    const emulator_settings& settings) {
    enqueue([settings](polygon_renderer_gpu& renderer) {
        renderer.apply_display_settings(settings);
    });
}

void arcade_video_worker::submit_polygons(const polygon_object* polygons,
                                          int count) {
    if (!polygons || count <= 0) return;
    std::vector<polygon_object> copy(polygons, polygons + count);
    enqueue([copy = std::move(copy)](polygon_renderer_gpu& renderer) {
        renderer.submit_polygons(copy.data(), static_cast<int>(copy.size()));
    });
}

void arcade_video_worker::submit_textures(
    const uint8_t* texture_rom, std::size_t texture_size,
    const uint8_t* tilemap_rom, std::size_t tilemap_size,
    std::size_t region_offset, std::size_t bank_count,
    bool tile_high_bit_from_attr) {
    if (!texture_rom || !tilemap_rom) return;
    std::vector<uint8_t> textures(texture_rom, texture_rom + texture_size);
    std::vector<uint8_t> tilemap(tilemap_rom, tilemap_rom + tilemap_size);
    enqueue([textures = std::move(textures), tilemap = std::move(tilemap),
             region_offset, bank_count, tile_high_bit_from_attr]
            (polygon_renderer_gpu& renderer) {
        renderer.submit_textures(textures.data(), textures.size(),
                                 tilemap.data(), tilemap.size(), region_offset,
                                 bank_count, tile_high_bit_from_attr);
    });
}

void arcade_video_worker::submit_gamma(const uint8_t* gamma_proms,
                                       std::size_t size) {
    if (!gamma_proms || !size) return;
    std::vector<uint8_t> copy(gamma_proms, gamma_proms + size);
    enqueue([copy = std::move(copy)](polygon_renderer_gpu& renderer) {
        renderer.submit_gamma(copy.data(), copy.size());
    });
}

void arcade_video_worker::submit_sprites(const uint8_t* sprite_rom,
                                         std::size_t size) {
    if (!sprite_rom || !size) return;
    std::vector<uint8_t> copy(sprite_rom, sprite_rom + size);
    enqueue([copy = std::move(copy)](polygon_renderer_gpu& renderer) {
        renderer.submit_sprites(copy.data(), copy.size());
    });
}

void arcade_video_worker::set_system22_layer_mask(uint8_t mask) {
    enqueue([mask](polygon_renderer_gpu& renderer) {
        renderer.set_system22_layer_mask(mask);
    });
}

void arcade_video_worker::submit_palette(const uint8_t* palette_ram,
                                         std::size_t size) {
    if (!palette_ram || !size) return;
    std::vector<uint8_t> copy(palette_ram, palette_ram + size);
    enqueue([copy = std::move(copy)](polygon_renderer_gpu& renderer) {
        renderer.submit_palette(copy.data(), copy.size());
    });
}

void arcade_video_worker::submit_text_layer(
    const uint8_t* character_ram, std::size_t character_size,
    const uint8_t* text_ram, std::size_t text_size,
    const uint8_t* text_attributes, const uint8_t* mixer,
    std::size_t mixer_size, bool super_system22) {
    if (!character_ram || !text_ram || !text_attributes || !mixer) return;
    std::vector<uint8_t> characters(character_ram,
                                    character_ram + character_size);
    std::vector<uint8_t> text(text_ram, text_ram + text_size);
    std::vector<uint8_t> attributes(text_attributes,
                                    text_attributes + 0x10);
    std::vector<uint8_t> mixer_copy(mixer, mixer + mixer_size);
    enqueue([characters = std::move(characters), text = std::move(text),
             attributes = std::move(attributes), mixer_copy = std::move(mixer_copy),
             super_system22]
            (polygon_renderer_gpu& renderer) {
        renderer.submit_text_layer(
            characters.data(), characters.size(), text.data(), text.size(),
            attributes.data(), mixer_copy.data(), mixer_copy.size(),
            super_system22);
    });
}

void arcade_video_worker::render_scene(const view_matrix& view,
                                       const rgba_color& fog_color) {
    enqueue([view, fog_color](polygon_renderer_gpu& renderer) {
        renderer.render_scene(view, fog_color);
    });
}

void arcade_video_worker::present_rgba_frame(const uint8_t* pixels,
                                             int width, int height,
                                             int display_width,
                                             int display_height) {
    if (!pixels || width <= 0 || height <= 0) return;
    const std::size_t size = static_cast<std::size_t>(width) * height * 4;
    rgba_frame frame{{pixels, pixels + size}, width, height,
                     display_width, display_height};
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        if (!m_alive.load()) return;
        // A board may produce frames faster than the host can present them.
        // Keep only the newest complete RGBA frame so latency remains bounded
        // instead of turning a temporary slowdown into a growing input queue.
        m_pending_rgba_frame = std::move(frame);
        if (m_rgba_task_queued) return;
        m_rgba_task_queued = true;
        m_tasks.emplace_back([this](polygon_renderer_gpu& renderer) {
            rgba_frame newest;
            {
                std::lock_guard<std::mutex> pending_lock(m_task_mutex);
                if (!m_pending_rgba_frame) {
                    m_rgba_task_queued = false;
                    return;
                }
                newest = std::move(*m_pending_rgba_frame);
                m_pending_rgba_frame.reset();
                m_rgba_task_queued = false;
            }
            renderer.present_rgba_frame(newest.pixels.data(), newest.width,
                                        newest.height, newest.display_width,
                                        newest.display_height);
            snapshot_picture_rect(renderer);
        });
    }
    m_task_ready.notify_one();
}

void arcade_video_worker::present_model2_frame(model2_gpu_frame frame) {
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        if (!m_alive.load()) return;
        // Resource packets are sparse. Preserve an unconsumed upload when a
        // newer display list replaces the pending frame, otherwise the GPU
        // could permanently miss the only packet carrying a changed sheet.
        if (m_pending_model2_frame) {
            if (frame.texture_sheet_0.empty()) {
                frame.texture_sheet_0 = std::move(
                    m_pending_model2_frame->texture_sheet_0);
                frame.texture_sheet_1 = std::move(
                    m_pending_model2_frame->texture_sheet_1);
            }
            if (frame.luma.empty()) {
                frame.luma = std::move(m_pending_model2_frame->luma);
                frame.palette = std::move(m_pending_model2_frame->palette);
                frame.color_translation = std::move(
                    m_pending_model2_frame->color_translation);
            }
        }
        m_pending_model2_frame = std::move(frame);
        if (m_model2_task_queued) return;
        m_model2_task_queued = true;
        m_tasks.emplace_back([this](polygon_renderer_gpu& renderer) {
            model2_gpu_frame newest;
            {
                std::lock_guard<std::mutex> pending_lock(m_task_mutex);
                if (!m_pending_model2_frame) {
                    m_model2_task_queued = false;
                    return;
                }
                newest = std::move(*m_pending_model2_frame);
                m_pending_model2_frame.reset();
                m_model2_task_queued = false;
            }
            renderer.present_model2_frame(std::move(newest));
        });
    }
    m_task_ready.notify_one();
}

void arcade_video_worker::read_framebuffer(uint32_t* output) {
    if (!output || !m_alive.load()) return;
    auto completed = std::make_shared<std::promise<std::vector<uint32_t>>>();
    std::future<std::vector<uint32_t>> result = completed->get_future();
    enqueue([completed](polygon_renderer_gpu& renderer) {
        std::vector<uint32_t> pixels(
            std::size_t{SYSTEM22_SCREEN_WIDTH} * SYSTEM22_SCREEN_HEIGHT);
        renderer.read_framebuffer(pixels.data());
        completed->set_value(std::move(pixels));
    });
    const std::vector<uint32_t> pixels = result.get();
    std::copy(pixels.begin(), pixels.end(), output);
}
