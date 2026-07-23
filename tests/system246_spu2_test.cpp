#include "Ps2Const.h"
#include "arcade_audio_output.h"
#include "iop/Iop_Dmac.h"
#include "iop/Iop_Intc.h"
#include "iop/Iop_SpuBase.h"
#include "iop/Iop_Spu2_Core.h"
#include "system246/play_audio.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint16_t spu_enabled = 0x8000;
constexpr std::size_t input_block_bytes = 0x400;
constexpr std::size_t input_block_frames = input_block_bytes / 4;

struct spu_fixture {
    std::vector<uint8> ram;
    Iop::CSpuSampleCache sample_cache;
    Iop::CSpuIrqWatcher irq_watcher;
    Iop::CSpuBase core;

    explicit spu_fixture(unsigned int core_index = 0)
        : ram(PS2::SPU_RAM_SIZE, 0),
          core(ram.data(), static_cast<uint32>(ram.size()), &sample_cache,
               &irq_watcher, core_index) {
        core.SetDestinationSamplingRate(44100);
        core.SetBaseSamplingRate(44100);
        core.SetReverbEnabled(false);
        core.SetMasterVolumeLeft(0x3FFF);
        core.SetMasterVolumeRight(0x3FFF);
    }
};

void put_s16(std::vector<uint8>& bytes, std::size_t sample_index,
             std::int16_t value) {
    std::memcpy(bytes.data() + (sample_index * sizeof(value)), &value,
                sizeof(value));
}

std::array<uint8, 32> make_looping_adpcm(uint8 packed_samples) {
    std::array<uint8, 32> data{};
    data[0] = 0x00;  // Predictor 0, shift 0.
    data[1] = 0x04;  // Loop start.
    std::fill(data.begin() + 2, data.begin() + 16, packed_samples);
    data[16] = 0x00;
    data[17] = 0x03; // Loop end and repeat.
    std::fill(data.begin() + 18, data.end(), packed_samples);
    return data;
}

void upload_voice_data(spu_fixture& fixture, uint32 address,
                       std::array<uint8, 32>& data) {
    fixture.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_VOICE);
    fixture.core.SetTransferAddress(address);
    fixture.core.SetControl(spu_enabled | Iop::CSpuBase::CONTROL_DMA_WRITE);
    assert(fixture.core.ReceiveDma(data.data(), 16, 2, 0) == 2);
    assert(fixture.core.GetTransferAddress() == address + data.size());
}

void start_voice_at_full_envelope(spu_fixture& fixture, uint32 address,
                                  unsigned int channel_index = 0) {
    auto& channel = fixture.core.GetChannel(channel_index);
    channel.volumeLeft <<= static_cast<uint16>(0x3FFF);
    channel.volumeRight <<= static_cast<uint16>(0x3FFF);
    channel.pitch = 0x1000;
    channel.address = address;
    channel.repeat = address;
    channel.repeatSet = true;
    channel.adsrLevel <<= static_cast<uint16>(0);
    channel.adsrRate <<= static_cast<uint16>(0);
    fixture.core.OnChannelPitchChanged(channel_index);
    fixture.core.SendKeyOn(1U << channel_index);

    // The first frame initializes and prefetches the ADPCM reader. Pinning the
    // envelope after that makes this a decoder/mixer test, not an ADSR test.
    std::array<int16, 2> warmup{};
    fixture.core.Render(warmup.data(), warmup.size());
    channel.status = Iop::CSpuBase::SUSTAIN;
    channel.adsrVolume = 0x7FFFFFFF;
}

void set_core_master(Iop::Spu2::CCore& registers, std::uint16_t level) {
    registers.WriteRegister(Iop::Spu2::CCore::P_MVOLL, level);
    registers.WriteRegister(Iop::Spu2::CCore::P_MVOLR, level);
}

void test_spu_dma_completion_is_deferred_past_the_chcr_store() {
    std::vector<uint8> ram(PS2::IOP_RAM_SIZE, 0);
    Iop::CIntc intc;
    intc.Reset();
    Iop::CDmac dmac(ram.data(), intc);

    unsigned int receive_calls = 0;
    dmac.SetReceiveFunction(
        Iop::CDmac::CHANNEL_SPU1,
        [&receive_calls](uint8*, uint32 block_size, uint32 block_amount,
                         uint32 direction) {
            assert(block_size == 64);
            assert(block_amount == 4);
            assert(direction ==
                   Iop::Dmac::CChannel::CHCR_DR_FROM);
            ++receive_calls;
            return block_amount;
        });

    constexpr uint32 source = 0x1000;
    constexpr uint32 transfer = 0x01000201;
    dmac.WriteRegister(
        Iop::CDmac::CH7_BASE + Iop::Dmac::CChannel::REG_MADR, source);
    dmac.WriteRegister(
        Iop::CDmac::CH7_BASE + Iop::Dmac::CChannel::REG_BCR, 0x00040010);
    dmac.WriteRegister(
        Iop::CDmac::CH7_BASE + Iop::Dmac::CChannel::REG_CHCR, transfer);

    // The payload is copied immediately, but completion cannot interrupt the
    // IOP from inside the MMIO write that started the transfer.
    assert(receive_calls == 1);
    assert(dmac.ReadRegister(
               Iop::CDmac::CH7_BASE + Iop::Dmac::CChannel::REG_MADR) ==
           source + 0x100);
    assert((dmac.ReadRegister(
                Iop::CDmac::CH7_BASE + Iop::Dmac::CChannel::REG_CHCR) &
            (1U << 24)) != 0);
    assert(intc.ReadRegister(Iop::CIntc::STATUS0) == 0);
    assert(intc.ReadRegister(Iop::CIntc::STATUS1) == 0);

    dmac.CountTicks(23);
    assert((dmac.ReadRegister(
                Iop::CDmac::CH7_BASE + Iop::Dmac::CChannel::REG_CHCR) &
            (1U << 24)) != 0);
    assert(intc.ReadRegister(Iop::CIntc::STATUS0) == 0);
    assert(intc.ReadRegister(Iop::CIntc::STATUS1) == 0);

    dmac.CountTicks(1);
    assert((dmac.ReadRegister(
                Iop::CDmac::CH7_BASE + Iop::Dmac::CChannel::REG_CHCR) &
            (1U << 24)) == 0);
    assert((intc.ReadRegister(Iop::CIntc::STATUS0) &
            (1U << Iop::CIntc::LINE_DMAC)) != 0);
    assert((intc.ReadRegister(Iop::CIntc::STATUS1) &
            (1U << (Iop::CIntc::LINE_DMA_SPU1 - 32))) != 0);
}

void test_voice_dma_decode_and_cache_invalidation() {
    constexpr uint32 address = 0x10000;
    spu_fixture fixture;

    auto positive_adpcm = make_looping_adpcm(0x11);
    upload_voice_data(fixture, address, positive_adpcm);
    start_voice_at_full_envelope(fixture, address);

    std::array<int16, 128> positive_output{};
    fixture.core.Render(positive_output.data(), positive_output.size());
    for (std::size_t frame = 0; frame < positive_output.size() / 2; ++frame) {
        assert(positive_output[frame * 2] > 0);
        assert(positive_output[frame * 2] == positive_output[frame * 2 + 1]);
    }

    // Replacing the same address must invalidate decoded ADPCM cache entries.
    auto negative_adpcm = make_looping_adpcm(0xFF);
    upload_voice_data(fixture, address, negative_adpcm);
    start_voice_at_full_envelope(fixture, address);

    std::array<int16, 128> negative_output{};
    fixture.core.Render(negative_output.data(), negative_output.size());
    for (std::size_t frame = 0; frame < negative_output.size() / 2; ++frame) {
        assert(negative_output[frame * 2] < 0);
        assert(negative_output[frame * 2] == negative_output[frame * 2 + 1]);
    }
}

void test_running_engine_voice_observes_ring_refill() {
    constexpr uint32 address = 0x18000;
    spu_fixture fixture;
    auto first_lap = make_looping_adpcm(0x11);
    upload_voice_data(fixture, address, first_lap);
    start_voice_at_full_envelope(fixture, address);

    std::array<int16, 128> before_refill{};
    fixture.core.Render(before_refill.data(), before_refill.size());
    assert(std::all_of(
        before_refill.begin(), before_refill.end(),
        [](int16 sample) { return sample > 0; }));

    // RRV keeps its long-running race voices keyed on and refills their SPU
    // ring underneath the decoder. Replacing the ring must invalidate decoded
    // blocks without requiring another key-on.
    auto next_lap = make_looping_adpcm(0xFF);
    upload_voice_data(fixture, address, next_lap);
    std::array<int16, 512> after_refill{};
    fixture.core.Render(after_refill.data(), after_refill.size());
    assert(std::all_of(
        after_refill.end() - 128, after_refill.end(),
        [](int16 sample) { return sample < 0; }));
}

void test_voice_dma_wrap_and_stop() {
    spu_fixture fixture;
    std::array<uint8, 32> source{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<uint8>(index + 1);
    }

    fixture.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_VOICE);
    fixture.core.SetTransferAddress(PS2::SPU_RAM_SIZE - 16);
    fixture.core.SetControl(spu_enabled | Iop::CSpuBase::CONTROL_DMA_WRITE);
    assert(fixture.core.ReceiveDma(source.data(), 16, 2, 0) == 2);
    assert(std::equal(source.begin(), source.begin() + 16,
                      fixture.ram.end() - 16));
    assert(std::equal(source.begin() + 16, source.end(), fixture.ram.begin()));
    assert(fixture.core.GetTransferAddress() == 16);

    fixture.core.SetTransferAddress(0x2000);
    fixture.core.SetControl(spu_enabled | Iop::CSpuBase::CONTROL_DMA_STOP);
    const auto before = fixture.ram[0x2000];
    assert(fixture.core.ReceiveDma(source.data(), 16, 1, 0) == 0);
    assert(fixture.ram[0x2000] == before);
    assert(fixture.core.GetTransferAddress() == 0x2000);
}

void test_system246_voice_dma_is_not_deferred() {
    constexpr uint32 block_size = 16;
    constexpr uint32 block_count = 0x180;
    std::vector<uint8> source(block_size * block_count, 0x5A);

    // Preserve Play!'s conservative transfer pacing for normal PS2 titles.
    spu_fixture standard;
    standard.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_VOICE);
    standard.core.SetTransferAddress(0x20000);
    standard.core.SetControl(
        spu_enabled | Iop::CSpuBase::CONTROL_DMA_WRITE);
    assert(standard.core.ReceiveDma(
               source.data(), block_size, block_count, 0) == 0x100);

    // System 246 hardware finishes a plain SPU DMA in one operation. Deferring
    // the tail makes the game's streaming driver start voices on partial data.
    spu_fixture system246;
    system246.core.SetVoiceDmaWriteThrottlingEnabled(false);
    system246.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_VOICE);
    system246.core.SetTransferAddress(0x20000);
    system246.core.SetControl(
        spu_enabled | Iop::CSpuBase::CONTROL_DMA_WRITE);
    assert(system246.core.ReceiveDma(
               source.data(), block_size, block_count, 0) == block_count);
    assert(system246.core.GetTransferAddress() ==
           0x20000 + (block_size * block_count));
}

void test_voice_current_address_tracks_consumed_block() {
    constexpr uint32 address = 0x30000;
    spu_fixture fixture;
    std::array<uint8, 64> source{};
    for (std::size_t block = 0; block < source.size() / 16; ++block) {
        source[(block * 16)] = 0x00;
        source[(block * 16) + 1] = 0x00;
        std::fill(source.begin() + (block * 16) + 2,
                  source.begin() + ((block + 1) * 16), 0x11);
    }

    fixture.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_VOICE);
    fixture.core.SetTransferAddress(address);
    fixture.core.SetControl(
        spu_enabled | Iop::CSpuBase::CONTROL_DMA_WRITE);
    assert(fixture.core.ReceiveDma(source.data(), 16, 4, 0) == 4);

    auto& channel = fixture.core.GetChannel(0);
    channel.pitch = 0x1000;
    channel.address = address;
    channel.repeat = address;
    fixture.core.OnChannelPitchChanged(0);
    fixture.core.SendKeyOn(1);
    std::array<int16, 2> first_frame{};
    fixture.core.Render(first_frame.data(), first_frame.size());

    // NAX is a position in the block being consumed. Decoder prefetch must
    // not expose the address beyond two buffered 16-byte ADPCM blocks.
    assert(channel.current >= address);
    assert(channel.current < address + 16);

    std::array<int16, 56> following_frames{};
    fixture.core.Render(following_frames.data(), following_frames.size());
    assert(channel.current >= address + 16);
    assert(channel.current < address + 32);
}

void test_voice_irq_follows_consumption_not_prefetch() {
    constexpr uint32 address = 0x38000;
    spu_fixture fixture;
    std::array<uint8, 64> source{};
    for (std::size_t block = 0; block < source.size() / 16; ++block) {
        source[(block * 16)] = 0x00;
        source[(block * 16) + 1] = 0x00;
        std::fill(source.begin() + (block * 16) + 2,
                  source.begin() + ((block + 1) * 16), 0x11);
    }

    fixture.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_VOICE);
    fixture.core.SetTransferAddress(address);
    fixture.core.SetControl(
        spu_enabled | Iop::CSpuBase::CONTROL_DMA_WRITE);
    assert(fixture.core.ReceiveDma(source.data(), 16, 4, 0) == 4);

    auto& channel = fixture.core.GetChannel(0);
    channel.pitch = 0x1000;
    channel.address = address;
    channel.repeat = address;
    fixture.core.OnChannelPitchChanged(0);
    fixture.core.SetIrqAddress(address + 16);
    fixture.core.SetControl(spu_enabled | Iop::CSpuBase::CONTROL_IRQ);
    fixture.core.SendKeyOn(1);

    std::array<int16, 2> first_frame{};
    fixture.core.Render(first_frame.data(), first_frame.size());
    assert(!fixture.core.GetIrqPending());

    std::array<int16, 60> following_frames{};
    fixture.core.Render(following_frames.data(), following_frames.size());
    assert(fixture.core.GetIrqPending());

    // Reading IRQINFO acknowledges the event. Remaining within the same
    // 16-byte ADPCM block must not immediately raise it again; RRV otherwise
    // enters a transfer-finished callback storm and never refills its stream.
    fixture.core.ClearIrqPending();
    std::array<int16, 8> same_block_frames{};
    fixture.core.Render(same_block_frames.data(), same_block_frames.size());
    assert(!fixture.core.GetIrqPending());
}

void test_spdif_bypass_consumes_all_interleaved_frames() {
    spu_fixture fixture(0);
    fixture.core.SetInputBypass(true);
    fixture.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_BLOCK_CORE0IN);

    std::vector<uint8> block(input_block_bytes, 0);
    for (std::size_t frame = 0; frame < input_block_frames; ++frame) {
        put_s16(block, frame * 2, static_cast<std::int16_t>(frame + 1));
        put_s16(block, frame * 2 + 1,
                static_cast<std::int16_t>(-static_cast<int>(frame + 1)));
    }
    assert(fixture.core.ReceiveDma(block.data(), 16, 64, 0) == 64);

    std::array<int16, input_block_frames * 2> output{};
    fixture.core.Render(output.data(), output.size());
    for (std::size_t frame = 0; frame < input_block_frames; ++frame) {
        assert(output[frame * 2] == static_cast<int16>(frame + 1));
        assert(output[frame * 2 + 1] ==
               static_cast<int16>(-static_cast<int>(frame + 1)));
    }
}

void test_planar_input_consumes_all_frames() {
    spu_fixture fixture(0);
    fixture.core.SetInputBypass(false);
    fixture.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_BLOCK_CORE0IN);

    std::vector<uint8> block(input_block_bytes, 0);
    for (std::size_t frame = 0; frame < input_block_frames; ++frame) {
        put_s16(block, frame, static_cast<std::int16_t>(1000 + frame));
        put_s16(block, input_block_frames + frame,
                static_cast<std::int16_t>(-1000 - static_cast<int>(frame)));
    }
    assert(fixture.core.ReceiveDma(block.data(), 32, 32, 0) == 32);

    std::array<int16, input_block_frames * 2> output{};
    fixture.core.Render(output.data(), output.size());
    for (std::size_t frame = 0; frame < input_block_frames; ++frame) {
        assert(output[frame * 2] == static_cast<int16>(1000 + frame));
        assert(output[frame * 2 + 1] ==
               static_cast<int16>(-1000 - static_cast<int>(frame)));
    }
}

void test_voice_dry_routing_and_mmix_gates() {
    constexpr uint32 address = 0x42000;
    spu_fixture fixture(1);
    auto voice = make_looping_adpcm(0x11);
    upload_voice_data(fixture, address, voice);
    start_voice_at_full_envelope(fixture, address);

    fixture.core.SetChannelDryLeftLo(0x0001);
    fixture.core.SetChannelDryLeftHi(0x0000);
    fixture.core.SetChannelDryRightLo(0x0000);
    fixture.core.SetChannelDryRightHi(0x0000);
    fixture.core.SetMixControl(Iop::CSpuBase::MIX_DRY_SOUND_LEFT |
                               Iop::CSpuBase::MIX_DRY_SOUND_RIGHT);
    std::array<int16, 32> left_only{};
    fixture.core.Render(left_only.data(), left_only.size());
    for (std::size_t frame = 0; frame < left_only.size() / 2; ++frame) {
        assert(left_only[frame * 2] > 0);
        assert(left_only[frame * 2 + 1] == 0);
    }

    fixture.core.SetChannelDryLeftLo(0x0000);
    fixture.core.SetChannelDryRightLo(0x0001);
    std::array<int16, 32> right_only{};
    fixture.core.Render(right_only.data(), right_only.size());
    for (std::size_t frame = 0; frame < right_only.size() / 2; ++frame) {
        assert(right_only[frame * 2] == 0);
        assert(right_only[frame * 2 + 1] > 0);
    }

    fixture.core.SetChannelDryLeftLo(0x0001);
    fixture.core.SetMixControl(0);
    std::array<int16, 32> muted{};
    fixture.core.Render(muted.data(), muted.size());
    assert(std::all_of(muted.begin(), muted.end(),
                       [](int16 sample) { return sample == 0; }));
}

void test_spu2_routing_registers_reach_the_mixer() {
    spu_fixture fixture(1);
    Iop::Spu2::CCore registers(1, fixture.core);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXL_HI, 0x0001);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXL_LO, 0x0000);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXR_HI, 0x0002);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXR_LO, 0x0000);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXEL_HI, 0x0004);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXEL_LO, 0x0000);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXER_HI, 0x0008);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXER_LO, 0x0000);
    registers.WriteRegister(Iop::Spu2::CCore::P_MMIX, 0x0C00);
    assert(fixture.core.GetChannelDryLeft().f == 0x000001);
    assert(fixture.core.GetChannelDryRight().f == 0x000002);
    assert(fixture.core.GetChannelWetLeft().f == 0x000004);
    assert(fixture.core.GetChannelReverb().f == 0x000008);
    assert(fixture.core.GetMixControl() == 0x0C00);
}

void test_high_engine_voice_uses_upper_register_bank() {
    constexpr uint32 address = 0x46000;
    constexpr unsigned int voice_index = 21;
    constexpr std::uint16_t upper_voice_bit =
        1U << (voice_index - 16);
    spu_fixture fixture(1);
    Iop::Spu2::CCore registers(1, fixture.core);
    auto voice = make_looping_adpcm(0x11);
    upload_voice_data(fixture, address, voice);

    auto& channel = fixture.core.GetChannel(voice_index);
    channel.volumeLeft <<= static_cast<uint16>(0x3FFF);
    channel.volumeRight <<= static_cast<uint16>(0x3FFF);
    channel.pitch = 0x1000;
    channel.address = address;
    channel.repeat = address;
    channel.repeatSet = true;
    fixture.core.OnChannelPitchChanged(voice_index);

    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXL_HI, 0);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXR_HI, 0);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXL_LO,
                            upper_voice_bit);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXR_LO,
                            upper_voice_bit);
    registers.WriteRegister(
        Iop::Spu2::CCore::P_MMIX,
        Iop::CSpuBase::MIX_DRY_SOUND_LEFT |
            Iop::CSpuBase::MIX_DRY_SOUND_RIGHT);
    set_core_master(registers, 0x3FFF);
    registers.WriteRegister(Iop::Spu2::CCore::A_KON_LO,
                            upper_voice_bit);

    std::array<int16, 2> warmup{};
    fixture.core.Render(warmup.data(), warmup.size());
    channel.status = Iop::CSpuBase::SUSTAIN;
    channel.adsrVolume = 0x7FFFFFFF;

    std::array<int16, 128> output{};
    fixture.core.Render(output.data(), output.size());
    for (std::size_t frame = 0; frame < output.size() / 2; ++frame) {
        assert(output[frame * 2] > 0);
        assert(output[frame * 2] == output[frame * 2 + 1]);
    }
}

void test_high_engine_noise_voice_does_not_require_sample_ram() {
    constexpr unsigned int voice_index = 22;
    constexpr std::uint16_t upper_voice_bit =
        1U << (voice_index - 16);
    spu_fixture fixture(1);
    Iop::Spu2::CCore registers(1, fixture.core);

    auto& channel = fixture.core.GetChannel(voice_index);
    channel.volumeLeft <<= static_cast<uint16>(0x3FFF);
    channel.volumeRight <<= static_cast<uint16>(0x3FFF);
    channel.pitch = 0x1000;
    channel.address = 0x52000;
    channel.repeat = channel.address;
    channel.repeatSet = true;
    fixture.core.OnChannelPitchChanged(voice_index);

    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXL_HI, 0);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXR_HI, 0);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXL_LO,
                            upper_voice_bit);
    registers.WriteRegister(Iop::Spu2::CCore::S_VMIXR_LO,
                            upper_voice_bit);
    registers.WriteRegister(Iop::Spu2::CCore::S_NON_HI, 0);
    registers.WriteRegister(Iop::Spu2::CCore::S_NON_LO,
                            upper_voice_bit);
    registers.WriteRegister(
        Iop::Spu2::CCore::P_MMIX,
        Iop::CSpuBase::MIX_DRY_SOUND_LEFT |
            Iop::CSpuBase::MIX_DRY_SOUND_RIGHT);
    set_core_master(registers, 0x3FFF);
    registers.WriteRegister(
        Iop::Spu2::CCore::CORE_ATTR,
        spu_enabled | (0x3FU << 8));
    registers.WriteRegister(Iop::Spu2::CCore::A_KON_LO,
                            upper_voice_bit);

    std::array<int16, 2> warmup{};
    fixture.core.Render(warmup.data(), warmup.size());
    channel.status = Iop::CSpuBase::SUSTAIN;
    channel.adsrVolume = 0x7FFFFFFF;

    std::array<int16, 256> output{};
    fixture.core.Render(output.data(), output.size());
    assert(std::any_of(output.begin(), output.end(),
                       [](int16 sample) { return sample != 0; }));
    for (std::size_t frame = 0; frame < output.size() / 2; ++frame) {
        assert(output[frame * 2] == output[frame * 2 + 1]);
    }
}

std::array<int16, 128> render_voice_with_core_master(
        std::uint16_t master_level) {
    constexpr uint32 address = 0x4A000;
    spu_fixture fixture(1);
    Iop::Spu2::CCore registers(1, fixture.core);
    auto voice = make_looping_adpcm(0x11);
    upload_voice_data(fixture, address, voice);
    start_voice_at_full_envelope(fixture, address);
    fixture.core.SetChannelDryLeftLo(1);
    fixture.core.SetChannelDryRightLo(1);
    fixture.core.SetMixControl(Iop::CSpuBase::MIX_DRY_SOUND_LEFT |
                               Iop::CSpuBase::MIX_DRY_SOUND_RIGHT);
    set_core_master(registers, master_level);
    std::array<int16, 128> output{};
    fixture.core.Render(output.data(), output.size());
    return output;
}

void test_spu2_core_master_volume_controls_effects() {
    const auto muted = render_voice_with_core_master(0);
    const auto half = render_voice_with_core_master(0x1FFF);
    const auto full = render_voice_with_core_master(0x3FFF);
    assert(std::all_of(muted.begin(), muted.end(),
                       [](int16 sample) { return sample == 0; }));
    for (std::size_t index = 0; index < full.size(); ++index) {
        assert(full[index] != 0);
        assert(std::abs(static_cast<int>(half[index]) * 2 -
                        static_cast<int>(full[index])) <= 2);
    }
}

void test_block_input_respects_mmix_side_gates() {
    spu_fixture fixture(0);
    fixture.core.SetInputBypass(false);
    fixture.core.SetTransferMode(Iop::CSpuBase::TRANSFER_MODE_BLOCK_CORE0IN);
    fixture.core.SetMixControl(Iop::CSpuBase::MIX_DRY_INPUT_LEFT);

    std::vector<uint8> block(input_block_bytes, 0);
    for (std::size_t frame = 0; frame < input_block_frames; ++frame) {
        put_s16(block, frame, static_cast<std::int16_t>(1000 + frame));
        put_s16(block, input_block_frames + frame,
                static_cast<std::int16_t>(-1000 - static_cast<int>(frame)));
    }
    assert(fixture.core.ReceiveDma(block.data(), 32, 32, 0) == 32);
    std::array<int16, input_block_frames * 2> output{};
    fixture.core.Render(output.data(), output.size());
    for (std::size_t frame = 0; frame < input_block_frames; ++frame) {
        assert(output[frame * 2] == static_cast<int16>(1000 + frame));
        assert(output[frame * 2 + 1] == 0);
    }
}

void test_mixer_saturates_without_wrapping() {
    int16 positive = 30000;
    Iop::CSpuBase::MixSamples(10000, 0x7FFF, &positive);
    assert(positive == SHRT_MAX);

    int16 negative = -30000;
    Iop::CSpuBase::MixSamples(-10000, 0x7FFF, &negative);
    assert(negative == SHRT_MIN);

    int16 muted = 1234;
    Iop::CSpuBase::MixSamples(20000, 0, &muted);
    assert(muted == 1234);
}

void test_system246_wide_mix_uses_common_output_normalizer() {
    assert(system246_effect_bus_sample(100, 100) == 1600);
    assert(system246_effect_bus_sample(-100, 50) == -800);
    assert(system246_effect_bus_sample(1000, 100) == 16000);
    assert(system246_effect_bus_sample(2000, 100) == 32000);
    assert(system246_mix_reference_percent(100, 100) == 100);
    assert(system246_mix_reference_percent(50, 50) == 50);

    std::array<std::int32_t, 960> quiet_effects{};
    std::array<std::int32_t, 960> loud_mix{};
    for (std::size_t index = 0; index < quiet_effects.size(); ++index) {
        quiet_effects[index] = system246_effect_bus_sample(
            (index & 1) ? 100 : -100, 100);
        // A wide effects + disc sum deliberately exceeds int16 range.
        loud_mix[index] = (index & 1) ? 40000 : -40000;
    }
    std::array<std::int16_t, 960> quiet_output{};
    std::array<std::int16_t, 960> loud_output{};
    arcade_audio_output::loudness_normalizer quiet_normalizer;
    arcade_audio_output::loudness_normalizer loud_normalizer;
    const auto quiet_metrics = quiet_normalizer.process_from_int32(
        quiet_effects.data(), quiet_output.data(), quiet_effects.size(),
        2, 48000, 100, 100);
    const auto loud_metrics = loud_normalizer.process_from_int32(
        loud_mix.data(), loud_output.data(), loud_mix.size(),
        2, 48000, 100, 100);
    assert(std::abs(quiet_metrics.peak - 6000) <= 1);
    assert(std::abs(loud_metrics.peak - 10000) <= 1);
    assert(quiet_metrics.limited_samples == 0);
    assert(loud_metrics.limited_samples == 0);
}

void test_system246_master_volume_reaches_zero_and_half_level() {
    assert(system246_effective_bus_percent(0, 100) == 0);
    assert(system246_effective_bus_percent(60, 100) == 60);
    assert(system246_effective_bus_percent(60, 50) == 30);
    assert(system246_effective_bus_percent(150, 100) == 100);
    assert(system246_output_master_percent(0) == 100);
    assert(system246_output_master_percent(60) == 100);
    assert(system246_output_master_percent(150) == 150);
    assert(system246_effect_bus_sample(100, 60, 100) == 960);

    std::array<std::int32_t, 960> muted_input{};
    std::array<std::int32_t, 960> half_input{};
    std::array<std::int32_t, 960> full_input{};
    for (std::size_t index = 0; index < full_input.size(); ++index) {
        const std::int16_t sample = (index & 1) ? 100 : -100;
        muted_input[index] =
            system246_effect_bus_sample(sample, 0, 100);
        half_input[index] =
            system246_effect_bus_sample(sample, 50, 100);
        full_input[index] =
            system246_effect_bus_sample(sample, 100, 100);
    }
    std::array<std::int16_t, 960> muted{};
    std::array<std::int16_t, 960> half{};
    std::array<std::int16_t, 960> full{};
    arcade_audio_output::loudness_normalizer muted_normalizer;
    arcade_audio_output::loudness_normalizer half_normalizer;
    arcade_audio_output::loudness_normalizer full_normalizer;
    muted_normalizer.process_from_int32(
        muted_input.data(), muted.data(), muted_input.size(), 2, 48000,
        system246_output_master_percent(0),
        system246_effective_bus_percent(0, 100));
    half_normalizer.process_from_int32(
        half_input.data(), half.data(), half_input.size(), 2, 48000,
        system246_output_master_percent(50),
        system246_effective_bus_percent(50, 100));
    full_normalizer.process_from_int32(
        full_input.data(), full.data(), full_input.size(), 2, 48000,
        system246_output_master_percent(100),
        system246_effective_bus_percent(100, 100));
    assert(std::all_of(muted.begin(), muted.end(),
                       [](std::int16_t sample) { return sample == 0; }));
    assert(std::abs(static_cast<int>(half.front()) * 2 -
                    static_cast<int>(full.front())) <= 1);

}

} // namespace

int main() {
    test_spu_dma_completion_is_deferred_past_the_chcr_store();
    test_voice_dma_decode_and_cache_invalidation();
    test_running_engine_voice_observes_ring_refill();
    test_voice_dma_wrap_and_stop();
    test_system246_voice_dma_is_not_deferred();
    test_voice_current_address_tracks_consumed_block();
    test_voice_irq_follows_consumption_not_prefetch();
    test_spdif_bypass_consumes_all_interleaved_frames();
    test_planar_input_consumes_all_frames();
    test_voice_dry_routing_and_mmix_gates();
    test_spu2_routing_registers_reach_the_mixer();
    test_high_engine_voice_uses_upper_register_bank();
    test_high_engine_noise_voice_does_not_require_sample_ram();
    test_spu2_core_master_volume_controls_effects();
    test_block_input_respects_mmix_side_gates();
    test_mixer_saturates_without_wrapping();
    test_system246_wide_mix_uses_common_output_normalizer();
    test_system246_master_volume_reaches_zero_and_half_level();
    return 0;
}
