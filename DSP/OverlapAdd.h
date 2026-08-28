#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Opaque forward declaration of KissFFT's real-FFT config handle. The full
// KissFFT C API (kiss_fft.h / kiss_fftr.h) is only included by OverlapAdd.cpp,
// so consumers of this header don't inherit KissFFT's C-linkage macros or its
// build-time scalar-type configuration (USE_SIMD / FIXED_POINT).
extern "C" {
    struct kiss_fftr_state;
    typedef struct kiss_fftr_state* kiss_fftr_cfg;
}

namespace DSP
{
    // Block-based FFT convolution engine (classic Overlap-Add / "OLA").
    //
    // Status: wired into the live signal path via DSP::EqPipeline, which the
    // PipeWire daemon backend drives (daemon/pipewire_backend.cpp). The IIR
    // peaking cascade (Equalizer10Band) still handles the 10-band EQ; this
    // engine handles measured/linear-phase FIR correction upstream of it.
    // The Windows APO (Equalizer/) does not use it.
    //
    // Algorithm:
    //   - Input arrives in a continuous stream; internally it is grouped
    //     into fixed analysis blocks of `blockSize` (B) frames.
    //   - Each block is zero-padded to `fftSize` (N), a 2/3/5-smooth size
    //     with N >= B + M - 1 (M = the largest impulse-response length this
    //     instance will ever be given), and transformed with a real FFT.
    //   - The transformed block is multiplied bin-by-bin against the
    //     filter's precomputed spectrum, then inverse-transformed. Because
    //     N >= B + M - 1, this circular convolution equals the true linear
    //     convolution of the block against the filter -- no wraparound.
    //   - Consecutive blocks' "tails" (the N-B samples of each inverse-FFT
    //     result that extend past the current block) are carried forward
    //     and added into the next block's output -- the "Add" in
    //     Overlap-Add -- which is what reassembles a continuous linear
    //     convolution out of independent per-block circular convolutions.
    //
    // Normalization: KissFFT's kiss_fftr()/kiss_fftri() pair is *not*
    // individually normalized -- a forward+inverse round trip scales every
    // sample by exactly N (confirmed empirically against the vendored
    // kissfft-131.2.0 sources, not assumed). Rather than pay a per-sample
    // division in the RT path, this class pre-scales the filter's spectrum
    // by 1/N once, in SetImpulseResponse() (non-RT): if X = kiss_fftr(block)
    // and H = kiss_fftr(zero-padded taps), then using H_scaled = H/N gives
    //     kiss_fftri(X * H_scaled) == true linear-convolution output,
    // with no further scaling needed in Process(). See OverlapAdd.cpp for
    // the derivation in comment form.
    //
    // RT-safety:
    //   - Prepare() is non-RT: it allocates every buffer the other methods
    //     use, including SetImpulseResponse()'s zero-padding scratch.
    //   - SetImpulseResponse() is non-RT: it does not allocate (the scratch
    //     is preallocated), but it runs a full forward FFT of fftSize, which
    //     is far too much work for an audio callback. Call it from a control
    //     thread; it is designed to run concurrently with Process().
    //   - Process() is RT-safe: no allocation, no locks. The active filter
    //     spectrum is published via the same double-buffer + atomic-index
    //     pattern Biquad uses (see Biquad.h): a single concurrent
    //     SetImpulseResponse() writes the slot Process() is not pointed at,
    //     so it cannot tear that read. As with Biquad, two SetImpulseResponse()
    //     calls landing inside one ProcessOneBlock() can reuse the slot the
    //     RT thread holds -- see the note on m_filterSpectrum below for the
    //     exact scope of the guarantee.
    //   - kiss_fftr()/kiss_fftri() themselves are allocation-free for every
    //     fftSize this class will realistically produce. fftSize is always
    //     picked via kiss_fftr_next_fast_size_real(), which guarantees
    //     fftSize/2 has only {2,3,5} as prime factors; tracing kf_factor()
    //     in kiss_fft.c shows that for any such value >= 2, every stage
    //     kf_work() dispatches on is exactly radix 2, 3, 4, or 5 -- never
    //     the generic/fallback radix, which is KissFFT's one internal path
    //     that can call alloca()/malloc() per transform (kf_bfly_generic()).
    //     The sole exception is the fully degenerate fftSize/2 == 1 case
    //     (i.e. blockSize == maxImpulseLength == 1), where kf_factor()'s
    //     boundary handling emits a spurious radix-1 stage that does hit
    //     kf_bfly_generic(); this only matters for an unrealistic
    //     one-sample block/filter and is irrelevant for any real audio
    //     block size. Verified by reading kiss_fft.c / kiss_fftr.c line by
    //     line, not assumed -- see DSP/tests/test_overlap_add.cpp for the
    //     fftSize-smoothness check this claim is tied to.
    //
    // Latency: GetLatencySamples() reports the number of leading output
    // samples that are silence before the first true convolution result
    // is available (see .cpp for why this is exactly blockSize - 1).
    class OverlapAdd
    {
    public:
        OverlapAdd() noexcept;
        ~OverlapAdd();

        OverlapAdd(const OverlapAdd&) = delete;
        OverlapAdd& operator=(const OverlapAdd&) = delete;

        // Non-RT. Allocates every buffer Process() will ever touch, so
        // Process() itself never allocates.
        //   blockSize        - analysis block length B, in frames (e.g. 128/256/512).
        //   maxImpulseLength - largest FIR length M that SetImpulseResponse()
        //                      will ever be given; fixes fftSize for the
        //                      lifetime of this instance.
        //   channels         - channel count (interleaved, matches Process()).
        // Returns false on invalid parameters (any argument is 0).
        // Installs an identity (passthrough) impulse response on success.
        bool Prepare(uint32_t blockSize, uint32_t maxImpulseLength, uint32_t channels) noexcept;

        // Non-RT (allocation-free, but runs a full fftSize forward FFT --
        // call it from a control thread, never from an audio callback).
        // Installs a new impulse response (1 <= length <= maxImpulseLength,
        // as fixed by Prepare()). The same FIR is applied to every channel.
        // Computes the filter's frequency-domain representation and publishes
        // it atomically for Process() to pick up. Returns false if length is
        // 0, exceeds maxImpulseLength, any tap is non-finite, or Prepare()
        // hasn't been called successfully; in every failure case the
        // previously installed filter is left untouched.
        bool SetImpulseResponse(const float* taps, uint32_t length) noexcept;

        // Non-RT. Installs an identity impulse ([1, 0, 0, ...]) -- pure
        // passthrough (still subject to the engine's inherent block latency).
        bool SetIdentity() noexcept;

        // RT-safe. Clears all buffered state (partial input block, carried
        // tails, pending output) without changing the installed impulse
        // response. Introduces a discontinuity, same as Biquad::Reset().
        void Reset() noexcept;

        // RT-safe. Processes `frames` interleaved samples. `frames` need not
        // be a multiple of blockSize and may vary call to call; internal
        // staging absorbs the difference. `input`/`output` may alias
        // (in-place), matching Biquad::Process()'s contract. `channels` must
        // equal the value given to Prepare(); mismatches are a no-op.
        void Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept;

        // Constant algorithmic latency introduced by this engine, in
        // samples, once Prepare() has succeeded.
        uint32_t GetLatencySamples() const noexcept;

        uint32_t BlockSize() const noexcept { return m_blockSize; }
        uint32_t FftSize() const noexcept { return m_fftSize; }
        uint32_t MaxImpulseLength() const noexcept { return m_maxImpulseLength; }

    private:
        // Binary-layout equivalent of KissFFT's kiss_fft_cpx (a struct of two
        // consecutive scalars, {r, i}) for the default (non-SIMD, non-fixed
        // point) build this project uses -- i.e. kiss_fft_scalar == float.
        // Kept as our own POD type so this header never has to include
        // kiss_fft.h. OverlapAdd.cpp static_asserts size/alignment
        // compatibility before ever reinterpret_cast'ing between the two.
        struct Complex
        {
            float r;
            float i;
        };

        struct ChannelState
        {
            std::vector<float> blockBuf;     // size fftSize; time-domain, zero-padded per block
            std::vector<Complex> spectrum;   // size numBins; this block's forward FFT
            std::vector<Complex> product;    // size numBins; spectrum * filter spectrum
            std::vector<float> invTimeBuf;   // size fftSize; inverse FFT of product
            std::vector<float> tailAcc;      // size fftSize - blockSize; carried overlap
            std::vector<float> outputRing;   // size kRingBlocks * blockSize
        };

        static constexpr uint32_t kRingBlocks = 4;

        uint32_t m_blockSize = 0;
        uint32_t m_fftSize = 0;
        uint32_t m_maxImpulseLength = 0;
        uint32_t m_channels = 0;
        uint32_t m_numBins = 0;       // fftSize/2 + 1
        uint32_t m_ringCapacity = 0;  // kRingBlocks * blockSize

        // Shared cursors: valid because every channel is fed and drained in
        // lockstep (same frame count, same blockSize), so per-channel
        // copies of these would always be numerically identical.
        uint32_t m_stageFill = 0;
        uint32_t m_ringRead = 0;
        uint32_t m_ringWrite = 0;
        uint32_t m_ringCount = 0;

        // KissFFT configs. Separate forward configs for the RT input path
        // and the non-RT filter-design path so SetImpulseResponse() (called
        // from a control thread) can never race Process() (RT thread) over
        // scratch state inside a single kiss_fftr_cfg.
        kiss_fftr_cfg m_fwdRt = nullptr;
        kiss_fftr_cfg m_fwdControl = nullptr;
        kiss_fftr_cfg m_inv = nullptr;

        // Double-buffered filter spectrum (pre-scaled by 1/fftSize -- see
        // class comment). Same shape/size for every channel (mono FIR
        // applied uniformly), so only one copy is kept, not one per channel.
        //
        // Scope of the tear-freedom guarantee (same as Biquad::m_coeffs):
        // one concurrent SetImpulseResponse() is safe because it writes the
        // slot ProcessOneBlock() is not reading. Two of them inside a single
        // ProcessOneBlock() call would reuse the slot the RT thread holds.
        // Impulse responses change on explicit user action (an IPC set_fir),
        // not continuously, so that is not a rate the control side reaches in
        // practice. Closing it fully needs an RCU/hazard-pointer scheme;
        // deliberately not done here.
        std::vector<Complex> m_filterSpectrum[2];
        std::atomic<uint32_t> m_activeFilter{0};

        // Zero-padding scratch for SetImpulseResponse(), sized fftSize and
        // allocated in Prepare() so that call path never allocates.
        std::vector<float> m_padScratch;

        std::vector<ChannelState> m_channelStates;

        void ProcessOneBlock(ChannelState& cs, uint32_t ringWritePos) noexcept;
        void FreeFft() noexcept;
    };
} // namespace DSP
