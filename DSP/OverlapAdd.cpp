#include "OverlapAdd.h"

#include "kiss_fftr.h"

#include <algorithm>

namespace DSP
{
    // ── Normalization derivation ─────────────────────────────────────────
    // Empirically (see kissfft normalization check performed against these
    // vendored sources): kiss_fftri(kiss_fftr(x)) == fftSize * x, i.e. a
    // forward+inverse round trip is unnormalized and scales by exactly N.
    //
    // Let X = kiss_fftr(block) and H = kiss_fftr(zero-padded taps), both
    // "raw" (unnormalized) DFTs. The true, correctly-scaled linear
    // convolution output is y = IDFT_normalized(X * H). Since
    // kiss_fftri(Z) == N * IDFT_normalized(Z) for any spectrum Z, we get
    // y = kiss_fftri(X * H) / N.
    //
    // If we instead pre-scale the filter spectrum once, at
    // SetImpulseResponse() time (non-RT), as H' = H / N, then:
    //   kiss_fftri(X * H') == N * IDFT_normalized(X * H / N)
    //                      == IDFT_normalized(X * H) == y.
    // So multiplying by the pre-scaled H' and calling kiss_fftri() directly
    // yields the correctly-scaled result with zero extra arithmetic in the
    // RT path. This is why SetImpulseResponse() divides by m_fftSize below
    // and Process()/ProcessOneBlock() never do.

    OverlapAdd::OverlapAdd() noexcept = default;

    OverlapAdd::~OverlapAdd()
    {
        FreeFft();
    }

    void OverlapAdd::FreeFft() noexcept
    {
        if (m_fwdRt) { kiss_fftr_free(m_fwdRt); m_fwdRt = nullptr; }
        if (m_fwdControl) { kiss_fftr_free(m_fwdControl); m_fwdControl = nullptr; }
        if (m_inv) { kiss_fftr_free(m_inv); m_inv = nullptr; }
    }

    bool OverlapAdd::Prepare(uint32_t blockSize, uint32_t maxImpulseLength, uint32_t channels) noexcept
    {
        // Complex must be layout-compatible with kiss_fft_cpx so the
        // reinterpret_cast calls in ProcessOneBlock()/SetImpulseResponse()
        // are well-founded, not just assumed. This holds for the default
        // (non-SIMD, non-fixed-point) KissFFT build this project uses, where
        // kiss_fft_scalar == float; if that build configuration ever
        // changes (USE_SIMD / FIXED_POINT), this assert will catch it.
        static_assert(sizeof(Complex) == sizeof(kiss_fft_cpx), "Complex/kiss_fft_cpx size mismatch");
        static_assert(alignof(Complex) <= alignof(kiss_fft_cpx), "Complex/kiss_fft_cpx alignment mismatch");

        if (blockSize == 0 || maxImpulseLength == 0 || channels == 0)
            return false;

        FreeFft();

        m_blockSize = blockSize;
        m_maxImpulseLength = maxImpulseLength;
        m_channels = channels;

        const uint32_t minFftSize = blockSize + maxImpulseLength - 1;
        m_fftSize = static_cast<uint32_t>(kiss_fftr_next_fast_size_real(static_cast<int>(minFftSize)));
        m_numBins = m_fftSize / 2 + 1;
        m_ringCapacity = kRingBlocks * m_blockSize;

        m_fwdRt = kiss_fftr_alloc(static_cast<int>(m_fftSize), 0, nullptr, nullptr);
        m_fwdControl = kiss_fftr_alloc(static_cast<int>(m_fftSize), 0, nullptr, nullptr);
        m_inv = kiss_fftr_alloc(static_cast<int>(m_fftSize), 1, nullptr, nullptr);
        if (!m_fwdRt || !m_fwdControl || !m_inv)
        {
            FreeFft();
            return false;
        }

        for (auto& slot : m_filterSpectrum)
            slot.assign(m_numBins, Complex{0.0f, 0.0f});
        m_activeFilter.store(0, std::memory_order_relaxed);

        const uint32_t tailLen = m_fftSize - m_blockSize;
        m_channelStates.clear();
        m_channelStates.resize(m_channels);
        for (auto& cs : m_channelStates)
        {
            cs.blockBuf.assign(m_fftSize, 0.0f);
            cs.spectrum.assign(m_numBins, Complex{0.0f, 0.0f});
            cs.product.assign(m_numBins, Complex{0.0f, 0.0f});
            cs.invTimeBuf.assign(m_fftSize, 0.0f);
            cs.tailAcc.assign(tailLen, 0.0f);
            cs.outputRing.assign(m_ringCapacity, 0.0f);
        }

        m_stageFill = 0;
        m_ringRead = 0;
        m_ringWrite = 0;
        m_ringCount = 0;

        return SetIdentity();
    }

    bool OverlapAdd::SetImpulseResponse(const float* taps, uint32_t length) noexcept
    {
        if (!m_fwdControl || taps == nullptr || length == 0 || length > m_maxImpulseLength)
            return false;

        // Zero-pad to fftSize (non-RT scratch; this function is documented
        // as non-RT precisely because of allocations like this one).
        std::vector<float> padded(m_fftSize, 0.0f);
        std::copy(taps, taps + length, padded.begin());

        const uint32_t inactive = 1u - m_activeFilter.load(std::memory_order_relaxed);
        std::vector<Complex>& dst = m_filterSpectrum[inactive];

        kiss_fftr(m_fwdControl, padded.data(), reinterpret_cast<kiss_fft_cpx*>(dst.data()));

        const float scale = 1.0f / static_cast<float>(m_fftSize);
        for (auto& c : dst)
        {
            c.r *= scale;
            c.i *= scale;
        }

        m_activeFilter.store(inactive, std::memory_order_release);
        return true;
    }

    bool OverlapAdd::SetIdentity() noexcept
    {
        const float one = 1.0f;
        return SetImpulseResponse(&one, 1);
    }

    void OverlapAdd::Reset() noexcept
    {
        m_stageFill = 0;
        m_ringRead = 0;
        m_ringWrite = 0;
        m_ringCount = 0;
        for (auto& cs : m_channelStates)
        {
            std::fill(cs.blockBuf.begin(), cs.blockBuf.end(), 0.0f);
            std::fill(cs.invTimeBuf.begin(), cs.invTimeBuf.end(), 0.0f);
            std::fill(cs.tailAcc.begin(), cs.tailAcc.end(), 0.0f);
            std::fill(cs.outputRing.begin(), cs.outputRing.end(), 0.0f);
        }
    }

    void OverlapAdd::ProcessOneBlock(ChannelState& cs, uint32_t ringWritePos) noexcept
    {
        // Positions [0, blockSize) were already written in-place by
        // Process(); zero-pad the rest before transforming.
        std::fill(cs.blockBuf.begin() + m_blockSize, cs.blockBuf.end(), 0.0f);

        kiss_fftr(m_fwdRt, cs.blockBuf.data(), reinterpret_cast<kiss_fft_cpx*>(cs.spectrum.data()));

        const uint32_t active = m_activeFilter.load(std::memory_order_acquire);
        const Complex* h = m_filterSpectrum[active].data();
        const Complex* x = cs.spectrum.data();
        Complex* y = cs.product.data();
        for (uint32_t k = 0; k < m_numBins; ++k)
        {
            const float xr = x[k].r, xi = x[k].i;
            const float hr = h[k].r, hi = h[k].i;
            y[k].r = xr * hr - xi * hi;
            y[k].i = xr * hi + xi * hr;
        }

        kiss_fftri(m_inv, reinterpret_cast<const kiss_fft_cpx*>(cs.product.data()), cs.invTimeBuf.data());

        const uint32_t tailLen = m_fftSize - m_blockSize;

        // Combine this block's result with the tail carried from the
        // previous block; emit exactly blockSize output samples.
        for (uint32_t i = 0; i < m_blockSize; ++i)
        {
            const float carried = (i < tailLen) ? cs.tailAcc[i] : 0.0f;
            cs.outputRing[(ringWritePos + i) % m_ringCapacity] = cs.invTimeBuf[i] + carried;
        }

        // Compute the new tail. Safe in-place: writing tailAcc[j] only ever
        // reads tailAcc[blockSize + j], and blockSize + j > j for every j
        // (blockSize >= 1), so no index is read after an earlier iteration
        // of this same loop has already overwritten it.
        for (uint32_t j = 0; j < tailLen; ++j)
        {
            const float carried = (m_blockSize + j < tailLen) ? cs.tailAcc[m_blockSize + j] : 0.0f;
            cs.tailAcc[j] = cs.invTimeBuf[m_blockSize + j] + carried;
        }
    }

    void OverlapAdd::Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept
    {
        if (channels != m_channels || m_channelStates.empty())
            return;

        // Ring occupancy bound (informal proof, not just hope): production
        // adds exactly blockSize samples once every blockSize iterations;
        // consumption removes at most 1 sample per iteration and only when
        // ringCount > 0. Tracing the steady state shows ringCount oscillates
        // in [0, blockSize] and is immediately decremented within the same
        // iteration that a block completes -- it never sustains a value
        // above blockSize - 1 across iteration boundaries. kRingBlocks == 4
        // leaves a 4x margin over that bound.
        for (uint32_t i = 0; i < frames; ++i)
        {
            for (uint32_t ch = 0; ch < channels; ++ch)
                m_channelStates[ch].blockBuf[m_stageFill] = input[i * channels + ch];
            ++m_stageFill;

            if (m_stageFill == m_blockSize)
            {
                for (uint32_t ch = 0; ch < channels; ++ch)
                    ProcessOneBlock(m_channelStates[ch], m_ringWrite);
                m_ringWrite = (m_ringWrite + m_blockSize) % m_ringCapacity;
                m_ringCount += m_blockSize;
                m_stageFill = 0;
            }

            if (m_ringCount > 0)
            {
                for (uint32_t ch = 0; ch < channels; ++ch)
                    output[i * channels + ch] = m_channelStates[ch].outputRing[m_ringRead];
                m_ringRead = (m_ringRead + 1) % m_ringCapacity;
                --m_ringCount;
            }
            else
            {
                // No convolved data available yet (still filling the very
                // first block) -- emit silence. This is the source of
                // GetLatencySamples(): exactly blockSize - 1 leading zeros.
                for (uint32_t ch = 0; ch < channels; ++ch)
                    output[i * channels + ch] = 0.0f;
            }
        }
    }

    uint32_t OverlapAdd::GetLatencySamples() const noexcept
    {
        return m_blockSize > 0 ? m_blockSize - 1 : 0;
    }
} // namespace DSP
