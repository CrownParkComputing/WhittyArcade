#include "play_core.h"

#include "play_audio.h"
#include "play_input.h"
#include "play_video.h"

#include "platform_paths.h"

#include "AppConfig.h"
#include "DiskUtils.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "Ps2Const.h"
#include "ee/PS2OS.h"
#include "ee/VuExecutor.h"
#include "iop/IopBios.h"
#include "iop/ioman/McDumpDevice.h"
#include "iop/namco_sys246/Iop_NamcoAcAtaAtapi.h"
#include "iop/namco_sys246/Iop_NamcoAcCdvd.h"
#include "iop/namco_sys246/Iop_NamcoAcLoCore.h"
#include "iop/namco_sys246/Iop_NamcoAcRam.h"
#include "iop/namco_sys246/Iop_NamcoPadMan.h"
#include "iop/namco_sys246/Iop_NamcoSys246.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

std::filesystem::path play_base_path() {
    std::filesystem::path root = whitty_platform::data_root();
    if (root.empty()) {
        std::error_code ec;
        root = std::filesystem::current_path(ec);
        if (ec) return "WhittyArcade/play";
    }
    const std::filesystem::path path = root / "WhittyArcade" / "play";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path;
}

void apply_rrv_patches(CPS2VM& vm) {
    static constexpr std::array<std::pair<uint32, uint32>, 3> patches = {{
        {0x00236D84, 0x00000000}, // Disable iLink initialization.
        {0x00229BD8, 0x00000000}, // Skip brake/gas calibration gate.
        {0x0026CE38, 0x00000000}, // Allow attract mode to start.
    }};
    for (const auto& patch : patches) {
        if (patch.first + sizeof(uint32) > PS2::EE_RAM_SIZE)
            throw std::runtime_error("RRV patch address is outside EE RAM.");
        std::memcpy(vm.m_ee->m_ram + patch.first, &patch.second,
                    sizeof(patch.second));
    }

    // pcsx2x6's NM00001 profile requires VU clamp mode 2 for both VUs.
    // Play's normal VU mode only clamps a few known-problem operands; RRV
    // needs every arithmetic operand and result clamped before its road and
    // car vertex packets are emitted.
    for (CVpu* vpu : {vm.m_ee->m_vpu0.get(), vm.m_ee->m_vpu1.get()}) {
        if (!vpu)
            throw std::runtime_error("RRV VU is unavailable.");
        auto* executor = dynamic_cast<CVuExecutor*>(
            vpu->GetContext().m_executor.get());
        if (!executor)
            throw std::runtime_error("RRV VU recompiler is unavailable.");
        executor->SetExtraClamping(true);
    }
}

} // namespace

// Play! deliberately leaves this application-specific method undefined.
fs::path CAppConfig::GetBasePath() const {
    return play_base_path();
}

struct system246_play_core::implementation {
    static constexpr std::uint32_t rrv_current_bgm_address = 0x003E5EB0;

    std::unique_ptr<CPS2VM> vm;
    CPS2VM::NewFrameEvent::Connection frame_connection;
    CPS2VM::NewFrameEvent::Connection bgm_connection;
    CPS2VM::NewFrameEvent::Connection stream_trace_connection;
    std::vector<uint8> dongle;
    std::string optical_path;
    std::shared_ptr<system246_input_channel> input;
    std::shared_ptr<system246_audio_control> audio;
    std::shared_ptr<system246_video_bridge> video;
    bool initialized{};
    bool is_paused{true};
    std::int32_t last_game_bgm{std::numeric_limits<std::int32_t>::min()};

    // Loads and starts the genuine RRV disc-driver chain from the dongle,
    // in dependency order (each module's imports must already resolve, via
    // HLE or an earlier real module in this same list, by the time its own
    // init code runs). Offsets are the verified dongle scan results in
    // docs/system246_256_board_plan.md. A failure anywhere here is logged
    // and non-fatal: boot continues with CAcCdvd's existing SIF-level stub,
    // which stays registered throughout as a fallback.
    int32 load_and_start_dongle_module(
            CIopBios& iop_bios, std::size_t offset, const char* module_label) {
        if (offset >= dongle.size()) {
            std::fprintf(stderr,
                         "System 246 real driver chain: dongle too small for "
                         "%s at offset 0x%zX (dongle size=%zu)\n",
                         module_label, offset, dongle.size());
            return -1;
        }
        int32 module_id = iop_bios.LoadModuleFromHost(dongle.data() + offset);
        if (module_id < 0) {
            std::fprintf(stderr,
                         "System 246 real driver chain: failed to load %s "
                         "from dongle offset 0x%zX\n", module_label, offset);
            return -1;
        }
        iop_bios.StartModule(
            CIopBios::MODULESTARTREQUEST_SOURCE::HOST, module_id,
            module_label, "", 0);
        std::printf(
            "System 246 real driver chain: queued %s (moduleId=%d) from "
            "dongle offset 0x%zX\n", module_label, module_id, offset);
        std::fflush(stdout);
        return module_id;
    }

    void load_real_driver_chain(CIopBios& iop_bios) {
        constexpr std::size_t acacd_offset = 0x26A030;
        constexpr std::size_t accdfs_offset = 0x26CCB0;
        constexpr std::size_t accdc_offset = 0x26F300;
        constexpr std::size_t accde_offset = 0x274180;
        try {
            load_and_start_dongle_module(iop_bios, acacd_offset, "acacd");
            load_and_start_dongle_module(iop_bios, accdfs_offset, "accdfs");
            load_and_start_dongle_module(iop_bios, accdc_offset, "accdc");
            load_and_start_dongle_module(iop_bios, accde_offset, "accde");
        } catch (const std::exception& exception) {
            std::fprintf(stderr,
                         "System 246 real driver chain load failed: %s\n",
                         exception.what());
        } catch (...) {
            std::fprintf(stderr,
                         "System 246 real driver chain load failed with an "
                         "unknown error\n");
        }
    }

    void prepare_environment(CPS2VM& machine) {
        // System 246's SPU DMA completes in one operation. Play!'s normal PS2
        // throttling can expose a partially filled streaming buffer to RRV.
        machine.m_iop->m_spuCore0.SetVoiceDmaWriteThrottlingEnabled(false);
        machine.m_iop->m_spuCore1.SetVoiceDmaWriteThrottlingEnabled(false);

        // Validate the generated ISO before handing it to Play!'s CDVD layer.
        auto media = DiskUtils::CreateOpticalMediaFromPath(optical_path);
        if (!media)
            throw std::runtime_error("Play! rejected the RRV optical image.");
        media.reset();

        CAppConfig::GetInstance().SetPreferencePath(
            PREF_PS2_CDROM0_PATH, optical_path);
        machine.CDROM0_SyncPath();
        if (!machine.m_cdrom0)
            throw std::runtime_error("Play! could not mount the RRV ISO.");

        auto* iop_bios = dynamic_cast<CIopBios*>(
            machine.m_iop->m_bios.get());
        if (!iop_bios)
            throw std::runtime_error("Play! IOP BIOS HLE is unavailable.");

        auto dongle_device = std::make_shared<Iop::Ioman::CMcDumpDevice>(
            dongle);
        iop_bios->GetIoman()->RegisterDevice("mc0", dongle_device);
        iop_bios->GetIoman()->RegisterDevice("ac0", dongle_device);
        iop_bios->SetDefaultImageVersion(2050);

        auto ac_ram = std::make_shared<Iop::Namco::CAcRam>(
            *iop_bios, machine.m_iop->m_ram);
        iop_bios->RegisterModule(ac_ram);
        iop_bios->RegisterHleModuleReplacement(
            "Arcade_Ext._Memory", ac_ram);

        auto ac_cdvd = std::make_shared<Iop::Namco::CAcCdvd>(
            *iop_bios->GetSifman(), *iop_bios->GetCdvdman(),
            machine.m_iop->m_ram, *ac_ram);
        ac_cdvd->SetOpticalMedia(machine.m_cdrom0.get());
        ac_cdvd->SetSectorReadObserver(
            [audio_control = audio](uint32 first_sector,
                                    uint32 sector_count) {
                if (audio_control)
                    audio_control->observe_disc_read(
                        first_sector, sector_count);
            });
        iop_bios->RegisterModule(ac_cdvd);
        iop_bios->RegisterHleModuleReplacement(
            "ATA/ATAPI_driver", ac_cdvd);
        iop_bios->RegisterHleModuleReplacement(
            "CD/DVD_Compatible", ac_cdvd);

        auto ac_lo_core = std::make_shared<Iop::Namco::CAcLoCore>();
        iop_bios->RegisterModule(ac_lo_core);

        auto ac_ata_atapi = std::make_shared<Iop::Namco::CAcAtaAtapi>(
            *iop_bios, machine.m_iop->m_ram);
        ac_ata_atapi->SetOpticalMedia(machine.m_cdrom0.get());
        iop_bios->RegisterModule(ac_ata_atapi);

        if (!std::getenv("WHITTYARCADE_DISABLE_REAL_DRIVER_CHAIN"))
            load_real_driver_chain(*iop_bios);

        auto pad_man = std::make_shared<Iop::Namco::CPadMan>();
        iop_bios->RegisterHleModuleReplacement("rom0:PADMAN", pad_man);
        iop_bios->RegisterHleModuleReplacement("rom0:SIO2MAN", pad_man);

        auto system = std::make_shared<Iop::Namco::CSys246>(
            *iop_bios->GetSifman(), *iop_bios->GetSifcmd(), *ac_ram,
            "rrvac");
        system->SetJvsMode(Iop::Namco::CSys246::JVS_MODE::DRIVE);
        // RRV's FCA-1 wiring differs from Play!'s generic JVS layout.  Keep
        // the logical cabinet controls in the same bit positions used by the
        // game's dedicated shared-memory input frame.
        system->SetButton(6, PS2::CControllerInfo::SELECT);   // service credit
        system->SetButton(7, PS2::CControllerInfo::START);
        system->SetButton(14, PS2::CControllerInfo::L1);      // shift down
        system->SetButton(15, PS2::CControllerInfo::R1);      // shift up
        system->SetButton(0, PS2::CControllerInfo::SQUARE);   // view change
        system->SetButton(1, PS2::CControllerInfo::CROSS);    // enter alt
        iop_bios->RegisterModule(system);
        iop_bios->RegisterHleModuleReplacement("rom0:DAEMON", system);
        if (!machine.m_pad)
            throw std::runtime_error("Play! pad handler is unavailable.");
        machine.m_pad->InsertListener(system.get());
    }

    void stop_noexcept() noexcept {
        if (!vm) return;
        try {
            if (initialized) vm->Pause();
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "System 246 pause warning: %s\n",
                         exception.what());
        } catch (...) {
            std::fprintf(stderr, "System 246 pause warning: unknown error\n");
        }
        is_paused = true;
        vm->BeforeExecutableReloaded = {};
        vm->AfterExecutableReloaded = {};
        bgm_connection.reset();
        stream_trace_connection.reset();
        frame_connection.reset();
        // DestroyImpl owns the strict GS -> pad -> sound teardown order and
        // joins Play!'s VM thread before the CPS2VM object is released.
        vm->Destroy();
        vm.reset();
        initialized = false;
        input.reset();
        audio.reset();
        video.reset();
    }
};

system246_play_core::system246_play_core()
    : m_impl(std::make_unique<implementation>()) {}

system246_play_core::~system246_play_core() { shutdown(); }

bool system246_play_core::initialize(
        const system246_rom_load_result& roms,
        const std::string& optical_path,
        std::shared_ptr<system246_input_channel> input,
        std::shared_ptr<system246_audio_control> audio,
        std::shared_ptr<system246_video_bridge> video,
        std::string& error) {
    shutdown();
    error.clear();
    if (!roms || optical_path.empty() || !input || !audio || !video) {
        error = "System 246 core received incomplete startup data.";
        return false;
    }

    try {
        m_impl->dongle.assign(roms.dongle.begin(), roms.dongle.end());
        m_impl->optical_path = optical_path;
        m_impl->input = std::move(input);
        m_impl->audio = std::move(audio);
        m_impl->video = std::move(video);
        if (!m_impl->audio->configure_music(optical_path, error))
            throw std::runtime_error(
                "RRV disc music could not be initialized: " + error);
        m_impl->vm = std::make_unique<CPS2VM>();
        m_impl->vm->Initialize();
        m_impl->initialized = true;

        m_impl->vm->CreateSoundHandler([audio_control = m_impl->audio]() {
            return make_system246_sound_handler(audio_control);
        });
        m_impl->vm->CreatePadHandler([input_channel = m_impl->input]() {
            return make_system246_pad_handler(input_channel);
        });
        m_impl->vm->CreateGSHandler(
            &system246_video_bridge::create_handler);
        // A VM vblank can precede completion of the Vulkan GS flip.  Driving
        // readback from that event paired new display registers with an older
        // framebuffer and showed up as rapid alternating/flickering output.
        // Only advertise a frame once the GS queue has completed its flip.
        m_impl->frame_connection = m_impl->vm->GetGSHandler()->OnFlipComplete.Connect(
            [video_bridge = m_impl->video]() {
                video_bridge->notify_frame();
            });
        implementation* state = m_impl.get();
        m_impl->bgm_connection = m_impl->vm->OnNewFrame.Connect([state]() {
            if (!state->vm || !state->vm->m_ee ||
                !state->vm->m_ee->m_ram || !state->audio)
                return;
            std::int32_t game_bgm = -1;
            std::memcpy(
                &game_bgm,
                state->vm->m_ee->m_ram +
                    implementation::rrv_current_bgm_address,
                sizeof(game_bgm));
            if (game_bgm == state->last_game_bgm) return;
            state->last_game_bgm = game_bgm;
            state->audio->observe_game_bgm(game_bgm);
            std::printf("RRV game BGM command: %d\n", game_bgm);
            std::fflush(stdout);
        });

        if (const char* value = std::getenv("WHITTYARCADE_TRACE_SPUSIF");
            value && *value && std::strcmp(value, "0") != 0) {
            m_impl->stream_trace_connection = m_impl->vm->OnNewFrame.Connect(
                [state]() {
                    if (!state->vm || !state->vm->m_ee ||
                        !state->vm->m_ee->m_ram)
                        return;
                    static std::uint32_t tick = 0;
                    if ((++tick % 60) != 0) return;
                    const uint8* ram = state->vm->m_ee->m_ram;
                    // RRV's two active disc-stream structs (0x50 bytes each);
                    // the game's chunk-read pump is expected to compare a
                    // running counter at +0x48 against a threshold at +0x4A
                    // to decide when to advance +0x30 (start sector) and
                    // issue the next 0x0A ACCDVD read.
                    for (uint32 base : {0x01EB6E00u, 0x01EB6E50u}) {
                        std::uint32_t start_sector = 0;
                        std::uint32_t sector_count = 0;
                        std::uint32_t marker = 0;
                        std::uint16_t counter = 0;
                        std::uint16_t threshold = 0;
                        std::memcpy(&marker, ram + base + 0x2C, 4);
                        std::memcpy(&start_sector, ram + base + 0x30, 4);
                        std::memcpy(&sector_count, ram + base + 0x34, 4);
                        std::memcpy(&counter, ram + base + 0x48, 2);
                        std::memcpy(&threshold, ram + base + 0x4A, 2);
                        std::printf(
                            "System 246 RRV stream struct @%08x active=%u "
                            "active2=%u marker=%08x start=%08x count=%08x "
                            "counter=%u threshold=%u flag27=%u flag4d=%u\n",
                            base, ram[base + 0x00], ram[base + 0x28], marker,
                            start_sector, sector_count, counter, threshold,
                            ram[base + 0x27], ram[base + 0x4D]);
                    }
                    std::fflush(stdout);
                });
        }

        // A 20-block SPU packet is roughly 20 ms at 44.1 kHz, cutting the
        // upstream default's audio latency without starving OpenAL.
        CAppConfig::GetInstance().SetPreferenceInteger(
            PREF_AUDIO_SPUBLOCKCOUNT, 20);
        m_impl->vm->ReloadSpuBlockCount();
        CAppConfig::GetInstance().SetPreferenceBoolean(
            PREF_PS2_LIMIT_FRAMERATE, true);
        m_impl->vm->ReloadFrameRateLimit();

        m_impl->vm->Pause();
        m_impl->vm->Reset(PS2::EE_EXT_RAM_SIZE, PS2::IOP_EXT_RAM_SIZE);
        m_impl->prepare_environment(*m_impl->vm);
        m_impl->vm->m_ee->m_os->BootFromVirtualPath(
            "ac0:START", {"DANGLE"});
        apply_rrv_patches(*m_impl->vm);

        m_impl->vm->BeforeExecutableReloaded =
            [state](CPS2VM* machine) {
                state->last_game_bgm =
                    std::numeric_limits<std::int32_t>::min();
                state->prepare_environment(*machine);
            };
        m_impl->vm->AfterExecutableReloaded =
            [state](CPS2VM* machine) {
                apply_rrv_patches(*machine);
                state->last_game_bgm =
                    std::numeric_limits<std::int32_t>::min();
            };

        m_impl->vm->Resume();
        m_impl->is_paused = false;
        return true;
    } catch (const std::exception& exception) {
        error = std::string("System 246 core startup failed: ") +
                exception.what();
    } catch (...) {
        error = "System 246 core startup failed with an unknown error.";
    }
    m_impl->stop_noexcept();
    return false;
}

void system246_play_core::shutdown() {
    if (m_impl) m_impl->stop_noexcept();
}

void system246_play_core::set_paused(bool paused) {
    if (!m_impl || !m_impl->vm || !m_impl->initialized ||
        paused == m_impl->is_paused)
        return;
    if (paused)
        m_impl->vm->Pause();
    else
        m_impl->vm->Resume();
    m_impl->is_paused = paused;
}

bool system246_play_core::paused() const {
    return !m_impl || m_impl->is_paused;
}

CGSHandler* system246_play_core::gs_handler() const {
    return m_impl && m_impl->vm ? m_impl->vm->GetGSHandler() : nullptr;
}
