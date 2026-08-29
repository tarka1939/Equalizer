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

        Biquad() noexcept;
        ~Biquad() = default;

        // Prepare internal state for the given sample rate and number of channels.
        // Non-RT.
        void Prepare(float sampleRate, uint32_t channels) noexcept;

        // Compute peaking EQ coefficients (non-RT) and install them.
        // centerHz: center frequency in Hz  (clamped to [1 Hz, 0.99*Nyquist])
        // Q:        quality factor          (clamped to >= 0.001)
        // gainDb:   gain in dB
        // A non-finite argument is rejected outright: the currently installed
        // coefficients stay in place rather than being replaced with NaN.
        void SetPeaking(float centerHz, float Q, float gainDb) noexcept;

        // Install precomputed coefficients (non-RT). Atomic swap is used to publish to RT.
        // Coefficient sets containing a NaN/Inf are ignored -- a non-finite
        // coefficient would otherwise land in the filter history and make
        // every subsequent output NaN forever, which no later valid curve
        // can undo.
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
        //
        // Scope of the guarantee, stated precisely: a single SetCoefficients()
        // call can never tear a coefficient set Process() is reading, because
        // it writes the slot Process() is *not* pointed at. What it does NOT
        // cover is two SetCoefficients() calls landing while one Process()
        // call is in flight -- the second reuses the slot the first vacated,
        // which is the slot Process() may still be reading. Process() copies
        // the active Coeffs into a local once at entry, so the exposure is a
        // few instructions rather than the whole call, and the control side is
        // rate-limited in practice (the GUI throttles at 120 ms; the IPC
        // server is command-driven). Closing it completely needs an
        // RCU/hazard-pointer scheme; deliberately not done here.
        Coeffs m_coeffs[2];
        std::atomic<uint32_t> m_activeIndex;

        // Per-channel states (size = channels). Preallocated in Prepare().
        std::vector<ChannelState> m_states;
    };
} // namespace DSP
