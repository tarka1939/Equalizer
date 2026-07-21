#pragma once

#include "Equalizer10Band.h"
#include "OverlapAdd.h"

#include <atomic>
#include <cstdint>

namespace DSP
{
    // Combines the FIR engine (OverlapAdd) and the IIR engine
    // (Equalizer10Band) into the single execution pipeline that actually
    // processes audio, deciding at runtime which stage(s) to run:
    //
    //   - Only IIR band gains configured (nonzero)  -> IIR only.
    //   - Only a FIR impulse response configured    -> FIR only.
    //   - Both configured                           -> FIR, then IIR.
    //   - Neither configured                        -> passthrough copy.
    //
    // "Configured" is tracked explicitly, not inferred from whether running
    // the stage would currently be a no-op:
    //   - IIR is "active" iff at least one of the 10 band gains passed to
    //     SetBandsPeaking() is nonzero. (A flat, all-zero-gain curve is
    //     mathematically a perfect passthrough already -- see Biquad's
    //     0 dB-is-exact-unity property in Biquad.h -- but this class still
    //     skips calling Equalizer10Band::Process() in that case, so a
    //     caller that only wants FIR correction doesn't pay for 10 biquad
    //     evaluations it didn't ask for.)
    //   - FIR is "active" iff SetImpulseResponse() has been called
    //     successfully and ClearImpulseResponse() hasn't been called since.
    //     OverlapAdd itself defaults to an identity impulse response after
    //     Prepare() (see OverlapAdd::Prepare), but this class does NOT
    //     treat that internal default as "active" -- FIR starts inactive
    //     and stays that way until a real impulse response is installed,
    //     so nothing pays OverlapAdd's block latency
    //     (GetLatencySamples() == blockSize - 1) unless FIR was actually
    //     requested.
    //
    // Order when both are active: FIR feeds IIR (SetImpulseResponse()'s
    // taps are applied first, then the 10-band peaking cascade), not the
    // other way around. This matches the conventional room-correction
    // signal chain -- a linear-phase or measured-impulse correction stage
    // upstream of a parametric tone-shaping stage -- and is a deliberate
    // choice, not an arbitrary one; see EqPipeline.cpp for the one-line
    // rationale kept next to Process().
    //
    // RT-safety: Prepare(), SetBandsPeaking(), SetImpulseResponse(), and
    // ClearImpulseResponse() are non-RT (they configure Biquad/OverlapAdd,
    // which themselves allocate/compute FFTs on their own non-RT paths).
    // Process() is RT-safe: no allocation, no locks -- it only loads two
    // atomic<bool> flags and forwards to the appropriate sub-engine(s),
    // each of which is independently RT-safe (see Biquad.h, OverlapAdd.h).
    class EqPipeline
    {
    public:
        EqPipeline() noexcept = default;

        // Non-RT. Prepares both sub-engines.
        //   sampleRate        - passed through to Equalizer10Band::Prepare().
        //   channels          - interleaved channel count, shared by both engines.
        //   blockSize         - OverlapAdd's internal analysis block size, in
        //                       frames (see OverlapAdd::Prepare). Does not
        //                       need to match the audio callback's own block
        //                       size -- OverlapAdd absorbs the difference.
        //   maxImpulseLength  - largest FIR length SetImpulseResponse() will
        //                       ever be given; fixes OverlapAdd's internal
        //                       FFT size for the lifetime of this instance.
        // FIR always resets to inactive after Prepare() -- OverlapAdd
        // itself always reinstalls an identity impulse response when
        // (re)Prepare()'d (see OverlapAdd::Prepare), so there is no
        // configured FIR left to keep active across this call. IIR's
        // active/inactive state, by contrast, is preserved across
        // Prepare() -- Equalizer10Band::Prepare() only resizes per-channel
        // filter history, it never touches configured coefficients (see
        // Biquad::Prepare()), so a previously-configured EQ curve survives
        // a later Prepare() call (e.g. a sample-rate change) exactly as
        // Equalizer10Band alone always has. On a freshly constructed
        // instance both are simply inactive because neither has been
        // configured at all yet.
        // Returns false if OverlapAdd::Prepare() rejects blockSize/
        // maxImpulseLength/channels (see OverlapAdd::Prepare's contract);
        // Equalizer10Band::Prepare() cannot fail.
        bool Prepare(float sampleRate, uint32_t channels, uint32_t blockSize, uint32_t maxImpulseLength) noexcept;

        // RT-safe. Clears buffered state in both sub-engines (see
        // Biquad::Reset()/OverlapAdd::Reset()) without changing which
        // stage(s) are active or their configured coefficients/taps.
        void Reset() noexcept;

        // Non-RT. Configures the IIR stage. IIR becomes active iff at least
        // one entry of bandsGainDb is nonzero.
        void SetBandsPeaking(const std::array<float, Equalizer10Band::BandCount>& bandsCenterHz,
            const std::array<float, Equalizer10Band::BandCount>& bandsGainDb,
            float Q) noexcept;

        // Non-RT. Installs a new FIR impulse response and marks FIR active.
        // See OverlapAdd::SetImpulseResponse() for the length contract.
        // Returns false (and leaves FIR's active/inactive state unchanged)
        // if OverlapAdd rejects the taps.
        bool SetImpulseResponse(const float* taps, uint32_t length) noexcept;

        // Non-RT. Marks FIR inactive -- Process() stops calling into
        // OverlapAdd until SetImpulseResponse() is called again. Resets
        // OverlapAdd's installed filter to identity so it stays internally
        // well-defined even though nothing will call it meanwhile.
        void ClearImpulseResponse() noexcept;

        // RT-safe. See class comment for which stage(s) run.
        void Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept;

        bool IsFirActive() const noexcept { return m_firActive.load(std::memory_order_acquire); }
        bool IsIirActive() const noexcept { return m_iirActive.load(std::memory_order_acquire); }

        // Algorithmic latency in samples: OverlapAdd's block latency while
        // FIR is active, 0 otherwise (Equalizer10Band introduces no
        // algorithmic latency of its own).
        uint32_t GetLatencySamples() const noexcept;

    private:
        Equalizer10Band m_iir;
        OverlapAdd m_fir;
        std::atomic<bool> m_firActive{false};
        std::atomic<bool> m_iirActive{false};
        uint32_t m_channels = 0;
    };
} // namespace DSP
