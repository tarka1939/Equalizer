#pragma once

#include "Equalizer10Band.h"

namespace DSP
{
    inline void Equalizer10Band::Prepare(float sampleRate, uint32_t channels) noexcept
    {
        m_sampleRate = sampleRate > 0.0f ? sampleRate : 48000.0f;
        m_channels = channels;

        for (auto& b : m_bands)
            b.Prepare(m_sampleRate, m_channels);
    }

    inline void Equalizer10Band::Reset() noexcept
    {
        for (auto& b : m_bands)
            b.Reset();
    }

    inline void Equalizer10Band::SetBandsPeaking(const std::array<float, BandCount>& bandsCenterHz,
        const std::array<float, BandCount>& bandsGainDb,
        float Q) noexcept
    {
        const float safeQ = (Q > 0.001f) ? Q : 1.0f;

        for (size_t i = 0; i < BandCount; ++i)
            m_bands[i].SetPeaking(bandsCenterHz[i], safeQ, bandsGainDb[i]);
    }

    inline void Equalizer10Band::Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept
    {
        if (frames == 0 || channels == 0)
            return;

        if (m_channels == 0)
        {
            const uint32_t samples = frames * channels;
            if (output != input)
            {
                for (uint32_t i = 0; i < samples; ++i)
                    output[i] = input[i];
            }
            return;
        }

        m_bands[0].Process(input, output, frames, channels);
        for (size_t i = 1; i < BandCount; ++i)
            m_bands[i].Process(output, output, frames, channels);
    }
}
