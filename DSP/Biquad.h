#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace DSP
{
    // Simple, RT-safe biquad (peaking) implementation.
    // Public API:
    //  - Prepare() and SetPeaking()/SetCoefficients() run in non-RT context.
    //  - Process() is RT-safe: no allocations, no locks, only atomic switch of coefficients.
    class Biquad
    {
    public:
        struct Coeffs
        {
            float b0;
            float b1;
            float b2;
            float a1; // a0 assumed = 1.0f
            float a2;
        };
		float M_PI = 3.14159265358979323846f;
        Biquad() noexcept;
        ~Biquad() = default;

        // Prepare internal state for the given sample rate and number of channels.
        // Non-RT.
        void Prepare(float sampleRate, uint32_t channels) noexcept;

        // Compute peaking EQ coefficients (non-RT) and install them.
        // centerHz: center frequency in Hz
        // Q: quality factor
        // gainDb: gain in dB
        void SetPeaking(float centerHz, float Q, float gainDb) noexcept;

        // Install precomputed coefficients (non-RT). Atomic swap is used to publish to RT.
        void SetCoefficients(const Coeffs& c) noexcept;

        // Reset internal filter states (RT-safe).
        void Reset() noexcept;

        // Process an interleaved float buffer in-place or source->dest.
        // input and output may be the same pointer for in-place processing.
        // frames: number of audio frames
        // channels: number of channels (must match Prepare())
        // RT path: must be fast and allocation-free.
        void Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept;

    private:
        struct ChannelState
        {
            float x1;
            float x2;
            float y1;
            float y2;
        };

        float m_sampleRate;
        uint32_t m_channels;

        // Double-buffered coefficients. Active index switched atomically.
        Coeffs m_coeffs[2];
        std::atomic<uint32_t> m_activeIndex;

        // Per-channel states (size = channels). Preallocated in Prepare().
        std::vector<ChannelState> m_states;
    };
} // namespace DSP