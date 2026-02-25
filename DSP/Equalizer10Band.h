#pragma once

#include "Biquad.h"
#include <array>
#include <cstdint>

namespace DSP
{
    class Equalizer10Band
    {
    public:
        static constexpr size_t BandCount = 10;

        Equalizer10Band() noexcept = default;

        void Prepare(float sampleRate, uint32_t channels) noexcept;
        void Reset() noexcept;

        // Non-RT: set coefficients for all bands.
        // bandsCenterHz: center frequency per band
        // bandsGainDb: gain per band
        // Q: shared Q for all bands
        void SetBandsPeaking(const std::array<float, BandCount>& bandsCenterHz,
            const std::array<float, BandCount>& bandsGainDb,
            float Q) noexcept;

        // RT-safe processing.
        void Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept;

    private:
        float m_sampleRate = 48000.0f;
        uint32_t m_channels = 0;
        std::array<Biquad, BandCount> m_bands;
    };
}

#include "Equalizer10Band.inline.h"
