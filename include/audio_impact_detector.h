// Audio transient detector for cabinet impact feedback.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// Detects sudden broadband energy rises while adapting to steady engines,
// music and crowd noise. Input is normalized floating-point stereo or signed
// 16-bit PCM; the returned 0..1 value is an impact strength, not loudness.
class audio_impact_detector {
public:
    void reset() { *this = {}; }

    float consume_float(const float* samples, std::size_t frames,
                        int channels) {
        return consume(samples, frames, channels, 1.0f);
    }

    float consume_int16(const short* samples, std::size_t sample_count,
                        int channels) {
        const std::size_t frames = channels > 0 ?
            sample_count / static_cast<std::size_t>(channels) : 0;
        return consume(samples, frames, channels, 1.0f / 32768.0f);
    }

private:
    template <typename Sample>
    float consume(const Sample* samples, std::size_t frames, int channels,
                  float scale) {
        if (!samples || frames == 0 || channels <= 0) return 0.0f;
        float strongest = 0.0f;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            float mono = 0.0f;
            for (int channel = 0; channel < channels; ++channel)
                mono += static_cast<float>(samples[frame * channels + channel]) *
                        scale;
            mono /= static_cast<float>(channels);
            const float delta = mono - m_previous;
            m_previous = mono;
            m_sum_square += mono * mono;
            m_sum_delta_square += delta * delta;
            m_peak = std::max(m_peak, std::fabs(mono));
            if (++m_block_frames == block_size)
                strongest = std::max(strongest, finish_block());
        }
        return strongest;
    }

    float finish_block() {
        const float rms = std::sqrt(m_sum_square / block_size);
        const float flux = std::sqrt(m_sum_delta_square / block_size);
        const float crest = m_peak / (rms + 0.001f);
        m_sum_square = 0.0f;
        m_sum_delta_square = 0.0f;
        m_peak = 0.0f;
        m_block_frames = 0;

        if (m_warmup_blocks < 8) {
            const float blend = m_warmup_blocks == 0 ? 1.0f : 0.25f;
            m_baseline_rms += blend * (rms - m_baseline_rms);
            m_baseline_flux += blend * (flux - m_baseline_flux);
            ++m_warmup_blocks;
            return 0.0f;
        }
        if (m_cooldown_blocks > 0) --m_cooldown_blocks;

        const float rise = rms / (m_baseline_rms + 0.015f);
        const float flux_rise = flux / (m_baseline_flux + 0.010f);
        const bool broadband_impact = rms > 0.035f &&
            rise > 1.50f && flux_rise > 1.30f &&
            (crest > 1.75f || flux > 0.065f);
        // A wall strike or scrape is often laid over an already-loud engine.
        // Its total level barely rises, but its rapidly changing texture does.
        const bool texture_impact = rms > 0.04f && rise > 0.90f &&
            flux > 0.075f && flux_rise > 2.20f;
        // Several early arcade death sounds are sustained tonal cues rather
        // than noisy explosions. A sufficiently dramatic energy step catches
        // those once without making steady music or engines vibrate.
        const bool dramatic_tonal_cue = rms > 0.08f && rise > 2.40f;
        const bool impact = m_cooldown_blocks == 0 &&
            (broadband_impact || texture_impact || dramatic_tonal_cue);

        // Do not let one crash instantly become the new normal. Normal
        // programme audio follows in roughly half a second; detected impacts
        // are admitted much more slowly.
        const float admitted_rms = impact ?
            std::min(rms, m_baseline_rms * 1.25f + 0.005f) : rms;
        const float admitted_flux = impact ?
            std::min(flux, m_baseline_flux * 1.25f + 0.005f) : flux;
        const float blend = impact ? 0.004f : 0.012f;
        m_baseline_rms += blend * (admitted_rms - m_baseline_rms);
        m_baseline_flux += blend * (admitted_flux - m_baseline_flux);

        if (!impact) return 0.0f;
        m_cooldown_blocks = 14; // about 75 ms at 48 kHz
        return std::clamp(0.18f + (rise - 1.5f) * 0.28f +
                          (flux_rise - 1.3f) * 0.16f + rms * 0.55f,
                          0.20f, 1.0f);
    }

    static constexpr std::size_t block_size = 256;
    float m_previous{};
    float m_sum_square{};
    float m_sum_delta_square{};
    float m_peak{};
    float m_baseline_rms{};
    float m_baseline_flux{};
    std::size_t m_block_frames{};
    unsigned m_warmup_blocks{};
    unsigned m_cooldown_blocks{};
};
