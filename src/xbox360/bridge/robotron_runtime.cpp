#include "xbox360_runtime_api.h"
#include "robotron_init.h"

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/conversion.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

constexpr std::uint32_t kRobotronTitleId = 0x584107E0u;
constexpr std::size_t kAudioPacketFrames = 256;
constexpr std::size_t kAudioChannels = 2;
constexpr std::size_t kMaximumAudioPackets = 48;

bool pin_gpu_plugin(std::string& error) {
    // ReXGlue keeps GPU plugins in a function-local static registry. When the
    // runtime is hosted by another application, that registry's destructor
    // can otherwise become the last owner and unload plugin code while libc
    // still has process-exit callbacks to run from it. Keep one deliberately
    // process-lifetime reference, matching ReXGlue's own hard-exit launcher.
    static void* pinned_plugin = nullptr;
    static bool attempted = false;
    static std::string failure;
    if (!attempted) {
        attempted = true;
        std::error_code path_error;
        const auto executable =
            std::filesystem::read_symlink("/proc/self/exe", path_error);
        if (path_error) {
            failure = "Could not locate WhittyArcade's executable directory";
        } else {
            const auto plugin = executable.parent_path() /
                                "librexgpu-xenos.so";
            int flags = RTLD_NOW | RTLD_LOCAL;
#if defined(RTLD_NODELETE)
            flags |= RTLD_NODELETE;
#endif
            pinned_plugin = dlopen(plugin.c_str(), flags);
            if (!pinned_plugin) {
                const char* detail = dlerror();
                failure = "Could not pin ReXGlue's Xenos GPU plugin";
                if (detail) failure += ": " + std::string(detail);
            }
        }
    }
    if (pinned_plugin) return true;
    error = failure;
    return false;
}

void set_error(char* output, std::size_t capacity, const std::string& message) {
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, "%s", message.c_str());
}

struct input_mailbox {
    std::mutex mutex;
    whitty_xbox360_input state{sizeof(whitty_xbox360_input)};
};

class bridge_input_driver final : public rex::input::InputDriver {
public:
    explicit bridge_input_driver(input_mailbox* mailbox)
        : InputDriver(nullptr, 0), mailbox_(mailbox) {}

    X_STATUS Setup() override { return X_STATUS_SUCCESS; }

    X_RESULT GetCapabilities(std::uint32_t user_index, std::uint32_t,
                             rex::input::X_INPUT_CAPABILITIES* output) override {
        if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
        if (output) {
            std::memset(output, 0, sizeof(*output));
            output->type = rex::input::XINPUT_DEVTYPE_GAMEPAD;
            output->sub_type = 0x01;
            output->gamepad.buttons = 0xffff;
            output->gamepad.left_trigger = 0xff;
            output->gamepad.right_trigger = 0xff;
            output->gamepad.thumb_lx = static_cast<std::int16_t>(0x7fff);
            output->gamepad.thumb_ly = static_cast<std::int16_t>(0x7fff);
            output->gamepad.thumb_rx = static_cast<std::int16_t>(0x7fff);
            output->gamepad.thumb_ry = static_cast<std::int16_t>(0x7fff);
            output->vibration.left_motor_speed = 0xffff;
            output->vibration.right_motor_speed = 0xffff;
        }
        return X_ERROR_SUCCESS;
    }

    X_RESULT GetState(std::uint32_t user_index,
                      rex::input::X_INPUT_STATE* output) override {
        if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
        if (!output) return X_ERROR_SUCCESS;
        whitty_xbox360_input state{};
        {
            std::lock_guard<std::mutex> lock(mailbox_->mutex);
            state = mailbox_->state;
        }
        std::memset(output, 0, sizeof(*output));
        output->packet_number = state.packet_number;
        output->gamepad.buttons = state.buttons;
        output->gamepad.left_trigger = state.left_trigger;
        output->gamepad.right_trigger = state.right_trigger;
        output->gamepad.thumb_lx = state.left_x;
        output->gamepad.thumb_ly = state.left_y;
        output->gamepad.thumb_rx = state.right_x;
        output->gamepad.thumb_ry = state.right_y;
        return X_ERROR_SUCCESS;
    }

    X_RESULT SetState(std::uint32_t user_index,
                      rex::input::X_INPUT_VIBRATION*) override {
        return user_index == 0 ? X_ERROR_SUCCESS :
                                 X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT GetKeystroke(std::uint32_t user_index, std::uint32_t,
                          rex::input::X_INPUT_KEYSTROKE*) override {
        return user_index == 0 ? X_ERROR_EMPTY :
                                 X_ERROR_DEVICE_NOT_CONNECTED;
    }

private:
    input_mailbox* mailbox_{};
};

struct audio_packet {
    std::array<std::int16_t, kAudioPacketFrames * kAudioChannels> samples{};
    std::size_t offset{};
    rex::thread::Semaphore* completion{};
};

class audio_mailbox {
public:
    void push(audio_packet packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (packets_.size() >= kMaximumAudioPackets) {
            release(packets_.front());
            packets_.pop_front();
        }
        packets_.push_back(std::move(packet));
    }

    std::size_t read(std::int16_t* output, std::size_t frame_capacity) {
        if (!output || frame_capacity == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t frames_written = 0;
        while (frames_written < frame_capacity && !packets_.empty()) {
            audio_packet& packet = packets_.front();
            const std::size_t available = kAudioPacketFrames - packet.offset;
            const std::size_t count =
                std::min(available, frame_capacity - frames_written);
            std::memcpy(output + frames_written * kAudioChannels,
                        packet.samples.data() + packet.offset * kAudioChannels,
                        count * kAudioChannels * sizeof(std::int16_t));
            packet.offset += count;
            frames_written += count;
            if (packet.offset == kAudioPacketFrames) {
                release(packet);
                packets_.pop_front();
            }
        }
        return frames_written;
    }

    void discard(rex::thread::Semaphore* completion) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = packets_.begin(); it != packets_.end();) {
            if (it->completion == completion) {
                it = packets_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        packets_.clear();
    }

private:
    static void release(audio_packet& packet) {
        if (packet.completion) packet.completion->Release(1, nullptr);
        packet.completion = nullptr;
    }

    std::mutex mutex_;
    std::deque<audio_packet> packets_;
};

class bridge_audio_driver final : public rex::audio::AudioDriver {
public:
    bridge_audio_driver(rex::memory::Memory* memory,
                        rex::thread::Semaphore* completion,
                        audio_mailbox* mailbox)
        : AudioDriver(memory), completion_(completion), mailbox_(mailbox) {}

    ~bridge_audio_driver() override {
        if (mailbox_) mailbox_->discard(completion_);
    }

    void SubmitFrame(std::uint32_t samples_ptr) override {
        const float* input = memory_->TranslateVirtual<float*>(samples_ptr);
        if (!input) {
            completion_->Release(1, nullptr);
            return;
        }
        std::array<float, kAudioPacketFrames * kAudioChannels> stereo{};
        rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
            stereo.data(), input, kAudioPacketFrames);
        audio_packet packet;
        packet.completion = completion_;
        for (std::size_t index = 0; index < stereo.size(); ++index) {
            const float sample = std::isfinite(stereo[index]) ? stereo[index] : 0.0f;
            packet.samples[index] = static_cast<std::int16_t>(std::lround(
                std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
        }
        mailbox_->push(std::move(packet));
    }

private:
    rex::thread::Semaphore* completion_{};
    audio_mailbox* mailbox_{};
};

class bridge_audio_system final : public rex::audio::AudioSystem {
public:
    bridge_audio_system(rex::runtime::FunctionDispatcher* dispatcher,
                        audio_mailbox* mailbox)
        : AudioSystem(dispatcher), mailbox_(mailbox) {}

protected:
    X_STATUS CreateDriver(std::size_t, rex::thread::Semaphore* completion,
                          rex::audio::AudioDriver** output) override {
        if (!output) return X_STATUS_INVALID_PARAMETER;
        *output = new bridge_audio_driver(memory_, completion, mailbox_);
        return X_STATUS_SUCCESS;
    }

    void DestroyDriver(rex::audio::AudioDriver* driver) override {
        delete driver;
    }

private:
    audio_mailbox* mailbox_{};
};

struct owned_achievement {
    std::uint32_t id{};
    std::uint32_t gamerscore{};
    bool unlocked{};
    std::uint64_t unlocked_at{};
    std::string label;
    std::string description;
};

void copy_achievement(const owned_achievement& source,
                      whitty_xbox360_achievement* destination) {
    destination->id = source.id;
    destination->gamerscore = source.gamerscore;
    destination->unlocked = source.unlocked ? 1 : 0;
    destination->unlocked_at_filetime = source.unlocked_at;
    destination->label = source.label.c_str();
    destination->description = source.description.c_str();
}

} // namespace

struct whitty_xbox360_runtime {
    input_mailbox input;
    audio_mailbox audio;
    std::unique_ptr<rex::Runtime> runtime;
    rex::system::object_ref<rex::system::XThread> main_thread;
    rex::system::AchievementListenerHandle achievement_listener{};
    std::mutex achievement_mutex;
    std::vector<owned_achievement> achievement_snapshot;
    std::deque<owned_achievement> achievement_events;
    rex::ui::RawImage captured_frame;
    std::uint64_t frame_sequence{};
    std::atomic<bool> paused{};

    bool initialize(const whitty_xbox360_config& config, std::string& error) {
        namespace fs = std::filesystem;
        const fs::path game_root(config.game_root);
        const fs::path user_root(config.user_root);
        const fs::path cache_root(config.cache_root);
        if (!fs::is_regular_file(game_root / "default.xex")) {
            error = "Robotron default.xex is missing from " + game_root.string();
            return false;
        }
        std::error_code directory_error;
        fs::create_directories(user_root, directory_error);
        directory_error.clear();
        fs::create_directories(cache_root, directory_error);

        const bool discover_functions =
            std::getenv("WHITTY_XBOX360_DISCOVER_FUNCTIONS") != nullptr;
        if (discover_functions) {
            rex::LogConfig logging;
            logging.default_level = spdlog::level::err;
            logging.log_to_console = true;
            logging.flush_level = spdlog::level::err;
            rex::InitLogging(logging);
            rex::cvar::SetFlagByName("unregistered_function_nonfatal", "true");
        } else {
            rex::InitLogging(nullptr, spdlog::level::off);
        }
        if (!pin_gpu_plugin(error)) return false;
        auto graphics = rex::system::LoadGpuPlugin("xenos", "vulkan");
        if (!graphics) {
            error = "ReXGlue's Xenos Vulkan plugin could not be loaded";
            return false;
        }
        const X_STATUS presentation_status = graphics->SetupPresentation(nullptr);
        if (XFAILED(presentation_status)) {
            error = "ReXGlue could not create its offscreen Vulkan presenter";
            return false;
        }

        runtime = std::make_unique<rex::Runtime>(
            game_root, user_root, fs::path{}, cache_root, fs::path{});
        rex::RuntimeConfig runtime_config;
        runtime_config.graphics = std::move(graphics);
        runtime_config.input_factory = [this](bool) {
            auto system = std::make_unique<rex::input::InputSystem>(nullptr);
            system->AddDriver(std::make_unique<bridge_input_driver>(&input));
            return system;
        };
        runtime_config.audio_factory = [this](rex::runtime::FunctionDispatcher* dispatcher) {
            return std::make_unique<bridge_audio_system>(dispatcher, &audio);
        };
        runtime_config.kernel_init = rex::kernel::InitializeKernel;

        const X_STATUS setup_status =
            runtime->Setup(PPCImageConfig, std::move(runtime_config));
        if (XFAILED(setup_status)) {
            error = "ReXGlue runtime setup failed";
            runtime.reset();
            return false;
        }
        if (PPCImageConfig.register_modules)
            PPCImageConfig.register_modules(runtime->kernel_state());

        const X_STATUS load_status = runtime->LoadXexImage("game:\\default.xex");
        if (XFAILED(load_status) ||
            runtime->kernel_state()->title_id() != kRobotronTitleId) {
            error = "ReXGlue could not load the Robotron 584107E0 XEX";
            runtime->Shutdown();
            runtime.reset();
            return false;
        }

        auto& achievements = runtime->kernel_state()->achievements();
        achievement_listener = achievements.RegisterUnlockCallback(
            [this](const rex::system::AchievementEvent& event) {
                owned_achievement value;
                value.id = event.achievement.id;
                value.gamerscore = event.achievement.gamerscore;
                value.unlocked = true;
                value.unlocked_at = event.unlocked_at_filetime;
                value.label = event.achievement.label;
                value.description = event.achievement.description;
                std::lock_guard<std::mutex> lock(achievement_mutex);
                achievement_events.push_back(std::move(value));
            });

        if (auto* graphics_system = runtime->graphics_system()) {
            graphics_system->InitializeShaderStorage(
                cache_root, kRobotronTitleId, true);
        }
        main_thread = runtime->PrepareModuleLaunch();
        if (!main_thread) {
            error = "ReXGlue could not prepare Robotron's main thread";
            shutdown();
            return false;
        }
        if (XFAILED(main_thread->Resume())) {
            error = "ReXGlue could not start Robotron's main thread";
            shutdown();
            return false;
        }
        return true;
    }

    void shutdown() {
        if (!runtime) return;
        if (achievement_listener && runtime->kernel_state()) {
            runtime->kernel_state()->achievements().UnregisterCallback(
                achievement_listener);
            achievement_listener = 0;
        }
        if (runtime->kernel_state()) {
            runtime->kernel_state()->TerminateTitle();
            // TerminateTitle gives guest threads a short cooperative window,
            // then deliberately leaves any stragglers for the standalone
            // launcher's hard-exit path.  An embedded host must keep the guest
            // memory alive long enough for Robotron's final wait callbacks to
            // unwind before Runtime::Shutdown releases it.
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        main_thread.reset();
        audio.clear();
        runtime->Shutdown();
        runtime.reset();
        rex::FlushLogging();
    }

    owned_achievement make_achievement(
            const rex::system::AchievementInfo& info) const {
        owned_achievement value;
        value.id = info.id;
        value.gamerscore = info.gamerscore;
        value.label = info.label;
        value.description = info.description;
        if (runtime && runtime->kernel_state()) {
            value.unlocked = runtime->kernel_state()->IsAchievementUnlocked(info.id);
            value.unlocked_at =
                runtime->kernel_state()->GetAchievementUnlockTime(info.id);
        }
        return value;
    }
};

namespace {

whitty_xbox360_runtime* create_runtime(const whitty_xbox360_config* config,
                                       char* error,
                                       std::size_t error_capacity) {
    if (!config || config->struct_size < sizeof(*config) ||
        !config->game_root || !config->user_root || !config->cache_root) {
        set_error(error, error_capacity, "Invalid Robotron runtime configuration");
        return nullptr;
    }
    try {
        auto instance = std::make_unique<whitty_xbox360_runtime>();
        std::string detail;
        if (!instance->initialize(*config, detail)) {
            set_error(error, error_capacity, detail);
            return nullptr;
        }
        return instance.release();
    } catch (const std::exception& exception) {
        set_error(error, error_capacity, exception.what());
    } catch (...) {
        set_error(error, error_capacity, "Unknown Robotron runtime failure");
    }
    return nullptr;
}

void destroy_runtime(whitty_xbox360_runtime* runtime) {
    if (!runtime) return;
    runtime->shutdown();
    delete runtime;
}

void set_input(whitty_xbox360_runtime* runtime,
               const whitty_xbox360_input* input) {
    if (!runtime || !input || input->struct_size < sizeof(*input)) return;
    std::lock_guard<std::mutex> lock(runtime->input.mutex);
    runtime->input.state = *input;
}

int capture_frame(whitty_xbox360_runtime* runtime,
                  whitty_xbox360_frame* output) {
    if (!runtime || !runtime->runtime || !output ||
        output->struct_size < sizeof(*output)) return 0;
    auto* graphics = runtime->runtime->graphics_system();
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    if (!presenter || !presenter->CaptureGuestOutput(runtime->captured_frame) ||
        runtime->captured_frame.data.empty()) return 0;
    ++runtime->frame_sequence;
    output->rgba = runtime->captured_frame.data.data();
    output->width = runtime->captured_frame.width;
    output->height = runtime->captured_frame.height;
    output->stride = static_cast<std::uint32_t>(runtime->captured_frame.stride);
    output->sequence = runtime->frame_sequence;
    return 1;
}

std::size_t read_audio(whitty_xbox360_runtime* runtime,
                       std::int16_t* output, std::size_t frame_capacity) {
    return runtime ? runtime->audio.read(output, frame_capacity) : 0;
}

void set_paused(whitty_xbox360_runtime* runtime, int paused) {
    if (!runtime || !runtime->main_thread ||
        !runtime->main_thread->is_running()) return;
    const bool requested = paused != 0;
    if (runtime->paused.exchange(requested) == requested) return;
    if (requested) {
        runtime->main_thread->Suspend();
    } else {
        runtime->main_thread->Resume();
    }
}

int is_running(whitty_xbox360_runtime* runtime) {
    return runtime && runtime->main_thread &&
           runtime->main_thread->is_running() ? 1 : 0;
}

std::size_t achievement_count(whitty_xbox360_runtime* runtime) {
    return runtime && runtime->runtime && runtime->runtime->kernel_state() ?
        runtime->runtime->kernel_state()->loaded_achievements().size() : 0;
}

int achievement_at(whitty_xbox360_runtime* runtime, std::size_t index,
                   whitty_xbox360_achievement* output) {
    if (!runtime || !runtime->runtime || !runtime->runtime->kernel_state() ||
        !output || output->struct_size < sizeof(*output)) return 0;
    const auto achievements =
        runtime->runtime->kernel_state()->loaded_achievements();
    if (index >= achievements.size()) return 0;
    std::lock_guard<std::mutex> lock(runtime->achievement_mutex);
    runtime->achievement_snapshot.clear();
    runtime->achievement_snapshot.push_back(
        runtime->make_achievement(achievements[index]));
    copy_achievement(runtime->achievement_snapshot.front(), output);
    return 1;
}

int take_unlocked_achievement(whitty_xbox360_runtime* runtime,
                              whitty_xbox360_achievement* output) {
    if (!runtime || !output || output->struct_size < sizeof(*output)) return 0;
    std::lock_guard<std::mutex> lock(runtime->achievement_mutex);
    if (runtime->achievement_events.empty()) return 0;
    runtime->achievement_snapshot.clear();
    runtime->achievement_snapshot.push_back(
        std::move(runtime->achievement_events.front()));
    runtime->achievement_events.pop_front();
    copy_achievement(runtime->achievement_snapshot.front(), output);
    return 1;
}

const whitty_xbox360_api kApi{
    sizeof(whitty_xbox360_api),
    WHITTY_XBOX360_RUNTIME_ABI,
    create_runtime,
    destroy_runtime,
    set_input,
    capture_frame,
    read_audio,
    set_paused,
    is_running,
    achievement_count,
    achievement_at,
    take_unlocked_achievement,
};

} // namespace

extern "C" WHITTY_XBOX360_EXPORT const whitty_xbox360_api*
whitty_xbox360_get_api(std::uint32_t requested_abi) {
    return requested_abi == WHITTY_XBOX360_RUNTIME_ABI ? &kApi : nullptr;
}

// Robotron may initialize its network layer, but the arcade build never lets
// an Xbox networking request reach a host socket. Local profile and achievement
// services remain available through ReXGlue's XAM implementation.
#define WHITTY_OFFLINE_SUCCESS(name) \
    extern "C" REX_FUNC(name) { ctx.r3.u64 = 0; }
#define WHITTY_OFFLINE_FAILURE(name) \
    extern "C" REX_FUNC(name) { ctx.r3.u64 = UINT32_MAX; }

WHITTY_OFFLINE_SUCCESS(__imp__NetDll_XNetStartup)
WHITTY_OFFLINE_SUCCESS(__imp__NetDll_XNetCleanup)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_XNetXnAddrToInAddr)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_XNetInAddrToXnAddr)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_XNetUnregisterInAddr)
WHITTY_OFFLINE_SUCCESS(__imp__NetDll_XNetGetConnectStatus)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_XNetQosListen)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_XNetQosLookup)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_XNetQosRelease)
WHITTY_OFFLINE_SUCCESS(__imp__NetDll_XNetGetTitleXnAddr)
WHITTY_OFFLINE_SUCCESS(__imp__NetDll_XNetGetEthernetLinkStatus)
WHITTY_OFFLINE_SUCCESS(__imp__NetDll_WSAStartup)
WHITTY_OFFLINE_SUCCESS(__imp__NetDll_WSACleanup)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_socket)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_closesocket)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_ioctlsocket)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_setsockopt)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_bind)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_recv)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_recvfrom)
WHITTY_OFFLINE_FAILURE(__imp__NetDll_sendto)

extern "C" REX_FUNC(__imp__NetDll_WSAGetLastError) {
    ctx.r3.u64 = 10050; // WSAENETDOWN
}
