#include "play_video.h"

#include "video_deinterlace.h"

#include "gs/GSH_Vulkan/GSH_VulkanOffscreen.h"
#include "gs/GSHandler.h"
#include "gs/GsPixelFormats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>

namespace {

void dump_debug_image(const std::vector<uint8_t>& rgba, int width,
                      int height, uint64_t sequence, const char* stem,
                      uint64_t& last_sequence) {
    static const std::string capture_dir = [] {
        const char* value = std::getenv("WHITTYARCADE_CAPTURE_DIR");
        return value ? std::string(value) : std::string{};
    }();
    if (capture_dir.empty() || width <= 0 || height <= 0 ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4 ||
        (last_sequence != 0 && sequence < last_sequence + 15))
        return;
    last_sequence = sequence;

    std::error_code error;
    std::filesystem::create_directories(capture_dir, error);
    if (error) return;
    char name[64]{};
    std::snprintf(name, sizeof(name), "%s-%08llu.ppm", stem,
                  static_cast<unsigned long long>(sequence));
    const std::filesystem::path path =
        std::filesystem::path(capture_dir) / name;
    std::FILE* output = std::fopen(path.string().c_str(), "wb");
    if (!output) return;
    std::fprintf(output, "P6\n%d %d\n255\n", width, height);
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t source = 0, destination = 0;
         source < rgba.size(); source += 4, destination += 3) {
        rgb[destination + 0] = rgba[source + 0];
        rgb[destination + 1] = rgba[source + 1];
        rgb[destination + 2] = rgba[source + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), output);
    std::fclose(output);
}

void dump_debug_frame(const std::vector<uint8_t>& rgba, int width,
                      int height, uint64_t sequence) {
    static uint64_t last_sequence = 0;
    dump_debug_image(rgba, width, height, sequence, "frame", last_sequence);
}

void dump_world_probe_frame(const std::vector<uint8_t>& rgba, int width,
                            int height, uint64_t sequence) {
    static uint64_t last_sequence = 0;
    dump_debug_image(rgba, width, height, sequence, "probe", last_sequence);
}

void dump_raw_field_pair(const std::vector<uint8_t>& rgba, int width,
                         int height, uint64_t sequence, bool odd_field) {
    static std::vector<uint8_t> previous;
    static int previous_width = 0;
    static int previous_height = 0;
    static uint64_t previous_sequence = 0;
    static bool previous_odd = false;
    static uint64_t last_even_sequence = 0;
    static uint64_t last_odd_sequence = 0;

    // Save an adjacent pair once per second. This makes field-phase faults
    // reproducible without continuously writing 50+ MB/s of debug images.
    if (sequence % 60 == 0 && previous_sequence + 1 == sequence &&
        previous_width == width && previous_height == height &&
        previous.size() == rgba.size()) {
        uint64_t& previous_last = previous_odd ?
            last_odd_sequence : last_even_sequence;
        uint64_t& current_last = odd_field ?
            last_odd_sequence : last_even_sequence;
        dump_debug_image(previous, width, height, previous_sequence,
                         previous_odd ? "raw-odd" : "raw-even",
                         previous_last);
        dump_debug_image(rgba, width, height, sequence,
                         odd_field ? "raw-odd" : "raw-even",
                         current_last);
    }
    previous = rgba;
    previous_width = width;
    previous_height = height;
    previous_sequence = sequence;
    previous_odd = odd_field;
}

class system246_vulkan_handler final : public CGSH_VulkanOffscreen {
    struct pending_capture {
        CGSHandler::DISPLAY_INFO display{};
        CGSHandler::DISPLAY_INFO pcrtc_display{};
        std::array<std::uint64_t, 2> dispfb{};
        std::array<std::uint64_t, 2> display_reg{};
        std::uint64_t pmode{};
        MEMORY_READBACK_TOKEN readback{};
        bool interlaced{};
        bool frame_mode{};
        bool field{};
        uint64_t sequence{};
    };

public:
    bool copy_latest(std::vector<uint8_t>& rgba, int& width, int& height,
                     uint64_t& sequence, uint64_t consumed_sequence,
                     uint64_t& readback_microseconds) {
        std::vector<pending_capture> candidates;
        {
            std::lock_guard<std::mutex> lock(m_capture_mutex);
            while (!m_pending_captures.empty() &&
                   m_pending_captures.front().sequence <= consumed_sequence)
                m_pending_captures.pop_front();
            candidates.assign(m_pending_captures.begin(),
                              m_pending_captures.end());
        }
        if (candidates.empty()) return false;

        if (m_snapshot_ram.size() != CGSHandler::RAMSIZE)
            m_snapshot_ram.resize(CGSHandler::RAMSIZE);

        // Prefer the newest completed slot. If the GPU is still producing it,
        // an older completed slot is better than stalling the GS and SPU.
        for (auto candidate = candidates.rbegin();
             candidate != candidates.rend(); ++candidate) {
            const auto started = std::chrono::steady_clock::now();
            if (!TryCopyMemoryRangeReadback(
                    candidate->readback, m_snapshot_ram.data(),
                    static_cast<uint32>(m_snapshot_ram.size())))
                continue;

            std::vector<uint8_t> composed;
            int composed_width = 0;
            int composed_height = 0;
            if (!compose_capture(*candidate, composed,
                                 composed_width, composed_height))
                continue;
            const auto finished = std::chrono::steady_clock::now();

            rgba = std::move(composed);
            width = composed_width;
            height = composed_height;
            sequence = candidate->sequence;
            readback_microseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    finished - started).count());

            std::lock_guard<std::mutex> lock(m_capture_mutex);
            while (!m_pending_captures.empty() &&
                   m_pending_captures.front().sequence <= sequence)
                m_pending_captures.pop_front();
            return true;
        }
        return false;
    }

protected:
    void MarkNewFrame() override {
        if (!m_preclear_capture_queued)
            queue_display();
        m_preclear_capture_queued = false;

        // QueueMemoryRangeReadback only records a transfer. The normal frame
        // submission owns the fence and completes it asynchronously.
        CGSH_VulkanOffscreen::MarkNewFrame();
    }

    bool CaptureBeforePostWorldClear(
            uint32 buffer_ptr, uint32 buffer_width, uint32 psm,
            uint32 output_width, uint32 output_height) override {
        if (m_preclear_capture_queued) return true;
        m_preclear_capture_queued = queue_display(
            buffer_ptr, buffer_width, psm, output_width, output_height);
        return m_preclear_capture_queued;
    }

private:
    bool queue_display(
            uint32 buffer_ptr = CGSHandler::RAMSIZE,
            uint32 buffer_width = 0, uint32 psm = 0,
            uint32 output_width = 0, uint32 output_height = 0) {
        pending_capture capture;
        capture.pcrtc_display = GetCurrentDisplayInfo();
        capture.display = capture.pcrtc_display;
        capture.pmode = m_nPMODE;
        capture.dispfb = {m_nDISPFB1.value.q, m_nDISPFB2.value.q};
        capture.display_reg = {m_nDISPLAY1.value.q, m_nDISPLAY2.value.q};
        if (buffer_ptr < CGSHandler::RAMSIZE && buffer_width != 0 &&
            output_width != 0 && output_height != 0) {
            // At the post-world clear, DISPFB can still name the other half
            // of RRV's alternating pair. Capture the completed FRAME target
            // as the background while retaining the second PCRTC circuit,
            // which carries cars/HUD elements over that background.
            capture.display.width = output_width;
            capture.display.height = output_height;
            auto& layer = capture.display.layers[0];
            layer = {};
            layer.enabled = true;
            layer.width = output_width;
            layer.height = output_height;
            layer.bufPtr = buffer_ptr;
            layer.bufWidth = buffer_width;
            layer.psm = psm;
            layer.useConstantAlpha = true;
            layer.constantAlpha = 0x80;
        }
        static const bool debug_world_probe = [] {
            const char* value = std::getenv("WHITTYARCADE_DEBUG_WORLD_PROBE");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        capture.interlaced = GetCrtIsInterlaced();
        capture.frame_mode = GetCrtIsFrameMode();
        capture.sequence = ++m_queued_sequence;
        // Finish snapshots CSR.FIELD before ResetVBlank can toggle it. Do not
        // sample the live privileged register on this asynchronous GS thread:
        // that races the emulated CPU and occasionally assigns a framebuffer
        // to the following phase, which makes every edge jump by one line.
        capture.field = GetLatchedFrameField();

        uint32 range_begin = CGSHandler::RAMSIZE;
        uint32 range_end = 0;
        bool valid_ranges = true;
        unsigned enabled_layers = 0;
        for (const auto& layer : capture.display.layers) {
            if (!layer.enabled) continue;
            ++enabled_layers;
            uint32 layer_begin = 0;
            uint32 layer_end = 0;
            if (!get_layer_range(layer, layer_begin, layer_end)) {
                valid_ranges = false;
                break;
            }
            range_begin = std::min(range_begin, layer_begin);
            range_end = std::max(range_end, layer_end);
        }
		if (debug_world_probe && valid_ranges && enabled_layers != 0) {
			constexpr uint32_t debug_world_probe_fb = 0x300000;
			constexpr uint32_t debug_world_probe_size = 0x8C000;
			range_begin = std::min(range_begin, debug_world_probe_fb);
			range_end = std::max(
				range_end, debug_world_probe_fb + debug_world_probe_size);
		}

        if (enabled_layers != 0) {
            if (!valid_ranges || range_begin >= range_end) {
                range_begin = 0;
                range_end = CGSHandler::RAMSIZE;
            }
            capture.readback = QueueMemoryRangeReadback(
                range_begin, range_end - range_begin);
            if (capture.readback.IsValid()) {
                std::lock_guard<std::mutex> lock(m_capture_mutex);
                m_pending_captures.push_back(capture);
                while (m_pending_captures.size() > 12)
                    m_pending_captures.pop_front();
                return true;
            }
        }
        return false;
    }

    static bool trace_enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("WHITTYARCADE_TRACE");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    static bool is_display_psm(uint32 psm) {
        return psm == PSMCT32 || psm == PSMCT24 ||
               psm == PSMCT16 || psm == PSMCT16S;
    }

    static bool make_layer_frame(
            const CGSHandler::DISPLAY_INFO::LAYER& layer,
            CGSHandler::FRAME& frame) {
        if (!layer.enabled || !is_display_psm(layer.psm) ||
            layer.bufPtr % 8192 != 0 || layer.bufWidth == 0 ||
            layer.bufWidth % 64 != 0 || layer.width == 0 ||
            layer.height == 0)
            return false;
        frame = {};
        frame.nPtr = layer.bufPtr / 8192;
        frame.nWidth = layer.bufWidth / 64;
        frame.nPsm = layer.psm;
        return frame.GetWidth() != 0;
    }

    static bool get_layer_range(
            const CGSHandler::DISPLAY_INFO::LAYER& layer,
            uint32& begin, uint32& end) {
        CGSHandler::FRAME frame{};
        if (!make_layer_frame(layer, frame)) return false;
        const auto page_size = CGsPixelFormats::GetPsmPageSize(frame.nPsm);
        if (page_size.first == 0 || page_size.second == 0) return false;
        const uint64_t pages_wide =
            (frame.GetWidth() + page_size.first - 1) / page_size.first;
        const uint64_t pages_high =
            (static_cast<uint64_t>(layer.height) + page_size.second - 1) /
            page_size.second;
        const uint64_t size =
            pages_wide * pages_high * CGsPixelFormats::PAGESIZE;
        const uint64_t range_end =
            static_cast<uint64_t>(frame.GetBasePtr()) + size;
        if (size == 0 || size > CGSHandler::RAMSIZE ||
            range_end > CGSHandler::RAMSIZE)
            return false;
        begin = frame.GetBasePtr();
        end = static_cast<uint32>(range_end);
        return true;
    }

    static void blend_pixel(uint8_t* destination, uint8_t red,
                            uint8_t green, uint8_t blue,
                            uint8_t source_alpha, uint16_t factor) {
        factor = std::min<uint16_t>(factor, 255);
        if (factor == 255) {
            destination[0] = red;
            destination[1] = green;
            destination[2] = blue;
        } else if (factor != 0) {
            const uint16_t inverse = 255 - factor;
            destination[0] = static_cast<uint8_t>(
                (red * factor + destination[0] * inverse + 127) / 255);
            destination[1] = static_cast<uint8_t>(
                (green * factor + destination[1] * inverse + 127) / 255);
            destination[2] = static_cast<uint8_t>(
                (blue * factor + destination[2] * inverse + 127) / 255);
        }
        destination[3] = source_alpha;
    }

    template <typename PixelIndexor, bool Is16Bit>
    void composite_pixels(
            const CGSHandler::DISPLAY_INFO& display,
            const CGSHandler::DISPLAY_INFO::LAYER& layer,
            const CGSHandler::FRAME& frame, uint8_t* ram,
            std::vector<uint8_t>& rgba) {
        PixelIndexor pixels(ram, frame.GetBasePtr(), frame.nWidth);
        const unsigned width = std::min<unsigned>(
            {layer.width, frame.GetWidth(), display.width - layer.offsetX});
        const unsigned height = std::min<unsigned>(
            layer.height, display.height - layer.offsetY);
        const uint16_t constant_factor = std::min<unsigned>(
            static_cast<unsigned>(layer.constantAlpha) * 2, 255);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto pixel = pixels.GetPixel(x, y);
                uint8_t red = 0;
                uint8_t green = 0;
                uint8_t blue = 0;
                uint8_t alpha = 0xff;
                if constexpr (Is16Bit) {
                    red = static_cast<uint8_t>(
                        ((pixel & 0x001f) * 255 + 15) / 31);
                    green = static_cast<uint8_t>(
                        (((pixel >> 5) & 0x001f) * 255 + 15) / 31);
                    blue = static_cast<uint8_t>(
                        (((pixel >> 10) & 0x001f) * 255 + 15) / 31);
                    alpha = (pixel & 0x8000) ? 0xff : 0;
                } else {
                    red = static_cast<uint8_t>(pixel);
                    green = static_cast<uint8_t>(pixel >> 8);
                    blue = static_cast<uint8_t>(pixel >> 16);
                    alpha = static_cast<uint8_t>(pixel >> 24);
                }
                const uint16_t factor = layer.useConstantAlpha ?
                    constant_factor : alpha;
                uint8_t* destination = rgba.data() +
                    ((static_cast<std::size_t>(layer.offsetY + y) *
                      display.width + layer.offsetX + x) * 4);
                blend_pixel(destination, red, green, blue, alpha, factor);
            }
        }
    }

    bool composite_layer(
            const CGSHandler::DISPLAY_INFO& display,
            const CGSHandler::DISPLAY_INFO::LAYER& layer,
            uint8_t* ram, std::vector<uint8_t>& rgba) {
        if (layer.offsetX >= display.width ||
            layer.offsetY >= display.height)
            return false;
        CGSHandler::FRAME frame{};
        if (!make_layer_frame(layer, frame)) return false;
        switch (frame.nPsm) {
        case PSMCT32:
        case PSMCT24:
            composite_pixels<CGsPixelFormats::CPixelIndexorPSMCT32, false>(
                display, layer, frame, ram, rgba);
            return true;
        case PSMCT16:
            composite_pixels<CGsPixelFormats::CPixelIndexorPSMCT16, true>(
                display, layer, frame, ram, rgba);
            return true;
        case PSMCT16S:
            composite_pixels<CGsPixelFormats::CPixelIndexorPSMCT16S, true>(
                display, layer, frame, ram, rgba);
            return true;
        default:
            return false;
        }
    }

    bool compose_capture(const pending_capture& capture,
                         std::vector<uint8_t>& rgba,
                         int& width, int& height) {
        const auto& display = capture.display;
        if (display.width == 0 || display.height == 0 ||
            display.width > 2048 || display.height > 1024 ||
            display.width > static_cast<unsigned>(
                                std::numeric_limits<int>::max()) ||
            display.height > static_cast<unsigned>(
                                 std::numeric_limits<int>::max()))
            return false;

        rgba.assign(static_cast<std::size_t>(display.width) *
                        display.height * 4,
                    0);
        for (std::size_t pixel = 0; pixel < rgba.size(); pixel += 4)
            rgba[pixel + 3] = 0xff;

        unsigned enabled_layers = 0;
        for (const auto& layer : display.layers) {
            if (!layer.enabled) continue;
            ++enabled_layers;
            if (!composite_layer(display, layer, m_snapshot_ram.data(), rgba))
                return false;
        }
        if (enabled_layers == 0) return false;

        static const bool debug_world_probe = [] {
            const char* value = std::getenv("WHITTYARCADE_DEBUG_WORLD_PROBE");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        if (debug_world_probe) {
            CGSHandler::DISPLAY_INFO probe_display{};
            probe_display.width = 640;
            probe_display.height = 224;
            auto& probe_layer = probe_display.layers[0];
            probe_layer.enabled = true;
            probe_layer.width = 640;
            probe_layer.height = 224;
            probe_layer.bufPtr = 0x300000;
            probe_layer.bufWidth = 640;
            probe_layer.psm = PSMCT32;
            probe_layer.useConstantAlpha = true;
            probe_layer.constantAlpha = 0x80;
            std::vector<uint8_t> probe_rgba(640 * 224 * 4, 0);
            for (std::size_t pixel = 0; pixel < probe_rgba.size(); pixel += 4)
                probe_rgba[pixel + 3] = 0xff;
            if (composite_layer(probe_display, probe_layer,
                                m_snapshot_ram.data(), probe_rgba))
                dump_world_probe_frame(probe_rgba, 640, 224,
                                       capture.sequence);
        }

        width = static_cast<int>(display.width);
        height = static_cast<int>(display.height);
        if (capture.interlaced && capture.frame_mode) {
            dump_raw_field_pair(rgba, width, height, capture.sequence,
                                capture.field);
            if (m_have_trace_field) {
                if (capture.field == m_last_trace_field)
                    ++m_repeated_fields;
                else
                    ++m_alternating_fields;
            }
            m_last_trace_field = capture.field;
            m_have_trace_field = true;
            std::vector<uint8_t> progressive;
            if (!m_deinterlacer.process(
                    rgba.data(), width, height, capture.field,
                    progressive))
                return false;
            rgba = std::move(progressive);
            height *= 2;
        } else {
            m_deinterlacer.reset();
        }
        if (trace_enabled() && (++m_trace_captures % 60) == 1) {
            const auto& layer0 = display.layers[0];
            const auto& layer1 = display.layers[1];
            const auto fb1 = make_convertible<CGSHandler::DISPFB>(
                capture.dispfb[0]);
            const auto fb2 = make_convertible<CGSHandler::DISPFB>(
                capture.dispfb[1]);
            const auto disp1 = make_convertible<CGSHandler::DISPLAY>(
                capture.display_reg[0]);
            const auto disp2 = make_convertible<CGSHandler::DISPLAY>(
                capture.display_reg[1]);
            std::printf(
                "System 246 GS display=%ux%u capture=%dx%d layers=%u "
                "L0=%d:%ux%u+%u+%u@0x%x/bw%u/p%u/a%s%u "
                "L1=%d:%ux%u+%u+%u@0x%x/bw%u/p%u/a%s%u "
                "interlaced=%d ffmd=%d field=%d sequence=%llu\n",
                display.width, display.height, width, height,
                enabled_layers,
                layer0.enabled, layer0.width, layer0.height,
                layer0.offsetX, layer0.offsetY, layer0.bufPtr,
                layer0.bufWidth, layer0.psm,
                layer0.useConstantAlpha ? "c" : "s",
                layer0.constantAlpha,
                layer1.enabled, layer1.width, layer1.height,
                layer1.offsetX, layer1.offsetY, layer1.bufPtr,
                layer1.bufWidth, layer1.psm,
                layer1.useConstantAlpha ? "c" : "s",
                layer1.constantAlpha,
                capture.interlaced, capture.frame_mode, capture.field,
                static_cast<unsigned long long>(capture.sequence));
            std::printf(
                "System 246 PCRTC native=%ux%u pmode=%llx "
                "FB1=0x%x/bw%u/p%u+%u+%u "
                "D1=%u,%u mag=%u,%u size=%u,%u "
                "FB2=0x%x/bw%u/p%u+%u+%u "
                "D2=%u,%u mag=%u,%u size=%u,%u\n",
                capture.pcrtc_display.width,
                capture.pcrtc_display.height,
                static_cast<unsigned long long>(capture.pmode),
                fb1.GetBufPtr(), fb1.GetBufWidth(), fb1.nPSM,
                fb1.nX, fb1.nY,
                disp1.nX, disp1.nY, disp1.nMagX, disp1.nMagY,
                disp1.nW + 1, disp1.nH + 1,
                fb2.GetBufPtr(), fb2.GetBufWidth(), fb2.nPSM,
                fb2.nX, fb2.nY,
                disp2.nX, disp2.nY, disp2.nMagX, disp2.nMagY,
                disp2.nW + 1, disp2.nH + 1);
            std::printf(
                "System 246 GS field cadence alternating=%llu repeated=%llu\n",
                static_cast<unsigned long long>(m_alternating_fields),
                static_cast<unsigned long long>(m_repeated_fields));
            std::fflush(stdout);
        }
        return true;
    }

    uint64_t m_queued_sequence{};
    uint64_t m_trace_captures{};
    uint64_t m_alternating_fields{};
    uint64_t m_repeated_fields{};
    bool m_preclear_capture_queued{};
    bool m_have_trace_field{};
    bool m_last_trace_field{};
    std::mutex m_capture_mutex;
    std::deque<pending_capture> m_pending_captures;
    std::vector<uint8_t> m_snapshot_ram;
    system246_field_deinterlacer m_deinterlacer;
};

} // namespace

void system246_video_bridge::notify_frame() {
    m_produced_frames.fetch_add(1, std::memory_order_release);
    m_frame_ready.notify_one();
}

bool system246_video_bridge::wait_for_frame(
        std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_frame_mutex);
    const auto produced = [this] {
        return m_produced_frames.load(std::memory_order_acquire);
    };
    if (produced() == m_observed_produced_frames &&
        !m_frame_ready.wait_for(lock, timeout, [&] {
            return produced() != m_observed_produced_frames;
        }))
        return false;
    m_observed_produced_frames = produced();
    return true;
}

CGSHandler* system246_video_bridge::create_handler() {
    auto* handler = new system246_vulkan_handler();
    // Ridge Racer V clears colour and depth between its world and vehicle/HUD
    // phases. Preserve the completed world colour, but let the depth reset run
    // so later cars and overlays remain in front of it.
    handler->SetPostWorldClearSuppressionEnabled(true);
    if (const char* value = std::getenv("WHITTYARCADE_DISABLE_GS_DEPTH");
        value && *value && std::strcmp(value, "0") != 0)
        handler->SetDepthTestingEnabled(false);
    if (const char* value = std::getenv("WHITTYARCADE_DISABLE_GS_ALPHA_TEST");
        value && *value && std::strcmp(value, "0") != 0)
        handler->SetAlphaTestingEnabled(false);
    if (const char* value = std::getenv("WHITTYARCADE_DISABLE_GS_BLEND");
        value && *value && std::strcmp(value, "0") != 0)
        handler->SetAlphaBlendingEnabled(false);
    return handler;
}

bool system246_video_bridge::capture_latest(
        CGSHandler* handler, std::vector<uint8_t>& rgba,
        int& width, int& height) {
    if (!handler) return false;

    auto* vulkan = dynamic_cast<system246_vulkan_handler*>(handler);
    if (!vulkan) return false;

    uint64_t captured_sequence = 0;
    uint64_t elapsed = 0;
    if (!vulkan->copy_latest(rgba, width, height, captured_sequence,
                             m_consumed_sequence, elapsed))
        return false;
    m_last_readback_microseconds.store(elapsed, std::memory_order_release);
    uint64_t peak =
        m_peak_readback_microseconds.load(std::memory_order_relaxed);
    while (peak < elapsed &&
           !m_peak_readback_microseconds.compare_exchange_weak(
               peak, elapsed, std::memory_order_release,
               std::memory_order_relaxed)) {}

    if (captured_sequence > m_consumed_sequence + 1) {
        m_superseded_frames.fetch_add(
            captured_sequence - m_consumed_sequence - 1,
            std::memory_order_relaxed);
    }
    m_consumed_sequence = captured_sequence;
    m_captured_frames.fetch_add(1, std::memory_order_release);
    dump_debug_frame(rgba, width, height, captured_sequence);
    return true;
}

system246_video_metrics system246_video_bridge::metrics() const {
    return {
        m_produced_frames.load(std::memory_order_acquire),
        m_captured_frames.load(std::memory_order_acquire),
        m_superseded_frames.load(std::memory_order_acquire),
        m_last_readback_microseconds.load(std::memory_order_acquire),
        m_peak_readback_microseconds.load(std::memory_order_acquire),
    };
}
