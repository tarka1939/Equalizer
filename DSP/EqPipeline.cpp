#include "EqPipeline.h"

#include <algorithm>

namespace DSP
{
    bool EqPipeline::Prepare(float sampleRate, uint32_t channels, uint32_t blockSize, uint32_t maxImpulseLength) noexcept
    {
        if (channels == 0)
            return false;

        m_iir.Prepare(sampleRate, channels);

        if (!m_fir.Prepare(blockSize, maxImpulseLength, channels))
        {
            m_channels = 0;
            return false;
        }

        m_channels = channels;

        // FIR always resets to inactive here: OverlapAdd::Prepare() always
        // reinstalls an identity impulse response internally as part of
        // (re)allocating its FFT buffers for the new blockSize/fftSize (see
        // OverlapAdd::Prepare's contract) -- there is no way to Prepare()
        // it without losing whatever FIR taps were configured before, so
        // this flag must follow suit or it would claim FIR is still doing
        // real filtering when it's actually just identity again.
        m_firActive.store(false, std::memory_order_release);

        // IIR's active/inactive state is deliberately left untouched here.
        // Unlike OverlapAdd, Equalizer10Band::Prepare() (-> Biquad::Prepare())
        // only resizes/clears per-channel filter *history*
        // (Biquad::m_states) -- it never touches the configured
        // coefficients (Biquad::m_coeffs), which live in a separate
        // double-buffer written only by SetPeaking()/SetCoefficients(). So
        // a previously-configured EQ curve survives a Prepare() call (e.g.
        // pipewire_backend.cpp's OnParamChanged() re-Prepares on every
        // sample-rate/channel-count change) exactly as it already did
        // before this class existed; resetting m_iirActive here would be a
        // regression, silently dropping a live curve back to passthrough
        // on every format change. m_iirActive's only false-by-default state
        // is its constructor initializer, before SetBandsPeaking() has ever
        // been called.
        return true;
    }

    void EqPipeline::Reset() noexcept
    {
        m_iir.Reset();
        m_fir.Reset();
    }

    void EqPipeline::SetBandsPeaking(const std::array<float, Equalizer10Band::BandCount>& bandsCenterHz,
        const std::array<float, Equalizer10Band::BandCount>& bandsGainDb,
        float Q) noexcept
    {
        m_iir.SetBandsPeaking(bandsCenterHz, bandsGainDb, Q);

        bool anyNonZero = false;
        for (float g : bandsGainDb)
        {
            if (g != 0.0f)
            {
                anyNonZero = true;
                break;
            }
        }
        m_iirActive.store(anyNonZero, std::memory_order_release);
    }

    bool EqPipeline::SetImpulseResponse(const float* taps, uint32_t length) noexcept
    {
        if (!m_fir.SetImpulseResponse(taps, length))
            return false;

        m_firActive.store(true, std::memory_order_release);
        return true;
    }

    void EqPipeline::ClearImpulseResponse() noexcept
    {
        m_fir.SetIdentity();
        m_firActive.store(false, std::memory_order_release);
    }

    void EqPipeline::Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept
    {
        if (frames == 0 || channels == 0 || !input || !output)
            return;

        if (m_channels != 0 && channels != m_channels)
        {
            // Channel count doesn't match what Prepare() was given. Previously
            // this fell straight through to the stage dispatch below, where
            // OverlapAdd::Process()'s own `channels != m_channels` guard made
            // it return without writing anything -- leaving `output`
            // untouched, i.e. holding whatever stale or uninitialised data the
            // caller's buffer had. That is the one degenerate case in this
            // class that did not degrade to passthrough like all the others.
            // Make it consistent: pass through, so a mismatched call produces
            // unfiltered audio rather than garbage.
            if (output != input)
            {
                const uint32_t samples = frames * channels;
                for (uint32_t i = 0; i < samples; ++i)
                    output[i] = input[i];
            }
            return;
        }

        if (m_channels == 0)
        {
            // Never Prepare()'d -- same passthrough contract as
            // Equalizer10Band when unprepared (see Equalizer10Band::Process
            // and ARCHITECTURE.md section 7.1 for why this specific
            // "unprepared instance quietly passes through" behavior matters
            // elsewhere in this codebase). Deliberately preserved here
            // rather than introducing a different unprepared-state behavior.
            if (output != input)
            {
                const uint32_t samples = frames * channels;
                for (uint32_t i = 0; i < samples; ++i)
                    output[i] = input[i];
            }
            return;
        }

        const bool fir = m_firActive.load(std::memory_order_acquire);
        const bool iir = m_iirActive.load(std::memory_order_acquire);

        if (fir && iir)
        {
            // FIR feeds IIR: a measured/linear-phase correction stage
            // upstream of parametric tone shaping, matching how a
            // room-correction FIR (from CurveGen's impulse-response
            // measurements) is conventionally placed ahead of a "to taste"
            // graphic EQ rather than after it -- correcting the room first,
            // then shaping the corrected result, rather than shaping first
            // and letting the room correction re-color that shaping.
            m_fir.Process(input, output, frames, channels);
            m_iir.Process(output, output, frames, channels);
        }
        else if (fir)
        {
            m_fir.Process(input, output, frames, channels);
        }
        else if (iir)
        {
            m_iir.Process(input, output, frames, channels);
        }
        else if (output != input)
        {
            const uint32_t samples = frames * channels;
            for (uint32_t i = 0; i < samples; ++i)
                output[i] = input[i];
        }
    }

    uint32_t EqPipeline::GetLatencySamples() const noexcept
    {
        return m_firActive.load(std::memory_order_acquire) ? m_fir.GetLatencySamples() : 0;
    }
} // namespace DSP
