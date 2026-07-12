/*
 * test_overlap_add.cpp — Lightweight, dependency-free unit tests for
 * DSP::OverlapAdd (see DSP/tests/test_biquad.cpp for the same style/rationale
 * applied to Biquad/Equalizer10Band).
 *
 * The central correctness check throughout is: block-by-block FFT
 * convolution (what OverlapAdd actually does) must equal straightforward
 * time-domain direct convolution (what a FIR filter is defined to do),
 * regardless of how the caller chops input into Process() calls.
 *
 * Build (standalone, no CMake needed):
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -I../../Equalizer/kissfft-131.2.0 \
 *       -c ../OverlapAdd.cpp -o OverlapAdd.o
 *   g++ -std=c++17 -O2 -I../../Equalizer/kissfft-131.2.0 \
 *       -c ../../Equalizer/kissfft-131.2.0/kiss_fft.c -o kiss_fft.o
 *   g++ -std=c++17 -O2 -I../../Equalizer/kissfft-131.2.0 \
 *       -c ../../Equalizer/kissfft-131.2.0/kiss_fftr.c -o kiss_fftr.o
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -c test_overlap_add.cpp -o test_overlap_add.o
 *   g++ OverlapAdd.o kiss_fft.o kiss_fftr.o test_overlap_add.o -o test_overlap_add
 *   ./test_overlap_add
 */
#include "../OverlapAdd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_currentTest = "";

void Check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s: %s (%s:%d)\n", g_currentTest, expr, file, line);
    }
}

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) Check(std::fabs((a) - (b)) <= (tol), \
    #a " ~= " #b, __FILE__, __LINE__)

#define RUN_TEST(fn) do { g_currentTest = #fn; fn(); } while (0)

// ── Helpers ───────────────────────────────────────────────────────────────────

// Deterministic (fixed-seed) pseudo-random test signal. Reproducibility only
// needs to hold within a single test run (OLA output vs. the direct-
// convolution reference computed from the very same buffer), so mt19937's
// cross-implementation portability doesn't matter here.
std::vector<float> GenerateTestSignal(uint32_t n, uint32_t seed) {
    std::vector<float> v(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& s : v) s = dist(rng);
    return v;
}

// Textbook O(n*m) direct convolution -- the ground truth OverlapAdd's FFT
// path must match. Output length is x.size() + h.size() - 1.
std::vector<float> DirectConvolve(const std::vector<float>& x, const std::vector<float>& h) {
    std::vector<float> y(x.size() + h.size() - 1, 0.0f);
    for (size_t n = 0; n < x.size(); ++n)
        for (size_t k = 0; k < h.size(); ++k)
            y[n + k] += x[n] * h[k];
    return y;
}

// Runs `x` through `ola` split into (possibly irregular) chunks given by
// `chunkSizes`, concatenating the outputs. If chunkSizes is empty, feeds the
// whole buffer in a single Process() call. Returns exactly x.size() samples
// (OverlapAdd::Process()'s contract: one output frame per input frame).
std::vector<float> RunChunked(DSP::OverlapAdd& ola, const std::vector<float>& x,
                               const std::vector<uint32_t>& chunkSizes) {
    std::vector<float> out(x.size());
    size_t pos = 0;
    size_t chunkIdx = 0;
    while (pos < x.size()) {
        uint32_t n = chunkSizes.empty()
            ? static_cast<uint32_t>(x.size())
            : chunkSizes[chunkIdx++ % chunkSizes.size()];
        n = static_cast<uint32_t>(std::min<size_t>(n, x.size() - pos));
        if (n == 0) n = 1;  // guard against a 0-sized entry in chunkSizes
        ola.Process(x.data() + pos, out.data() + pos, n, 1);
        pos += n;
    }
    return out;
}

// Checks that `n` has only factors of 2, 3, and 5 -- the property
// kiss_fftr_next_fast_size_real() guarantees for fftSize/2. See
// OverlapAdd.h's class comment for exactly what this does (and does not)
// prove about KissFFT's allocation behaviour.
bool IsTwoThreeFiveSmooth(uint32_t n) {
    while (n % 2 == 0) n /= 2;
    while (n % 3 == 0) n /= 3;
    while (n % 5 == 0) n /= 5;
    return n == 1;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

void OverlapAdd_PrepareRejectsInvalidParams() {
    DSP::OverlapAdd ola;
    CHECK(ola.Prepare(0, 8, 1) == false);
    CHECK(ola.Prepare(64, 0, 1) == false);
    CHECK(ola.Prepare(64, 8, 0) == false);
    CHECK(ola.Prepare(64, 8, 2) == true);  // valid, sanity check
}

void OverlapAdd_FftSizeIsAlwaysTwoThreeFiveSmooth() {
    const uint32_t combos[][2] = { {32, 1}, {64, 16}, {100, 7}, {1, 1}, {256, 257}, {17, 33} };
    for (auto& combo : combos) {
        DSP::OverlapAdd ola;
        CHECK(ola.Prepare(combo[0], combo[1], 1));
        CHECK(ola.FftSize() >= ola.BlockSize() + ola.MaxImpulseLength() - 1);
        CHECK(IsTwoThreeFiveSmooth(ola.FftSize()));
    }
}

void OverlapAdd_SetImpulseResponseRejectsBadArgs() {
    DSP::OverlapAdd ola;
    CHECK(ola.Prepare(64, 8, 1));

    float taps[16] = {};
    CHECK(ola.SetImpulseResponse(taps, 0) == false);       // zero length
    CHECK(ola.SetImpulseResponse(taps, 9) == false);       // exceeds maxImpulseLength (8)
    CHECK(ola.SetImpulseResponse(nullptr, 4) == false);    // null taps
    CHECK(ola.SetImpulseResponse(taps, 8) == true);        // exactly at the limit
}

void OverlapAdd_LatencyIsBlockSizeMinusOne() {
    DSP::OverlapAdd ola;
    CHECK(ola.Prepare(128, 32, 1));
    CHECK(ola.GetLatencySamples() == 127);
}

void OverlapAdd_IdentityIsDelayedPassthrough() {
    DSP::OverlapAdd ola;
    const uint32_t blockSize = 64;
    CHECK(ola.Prepare(blockSize, 8, 1));  // Prepare() installs identity by default
    const uint32_t latency = ola.GetLatencySamples();
    CHECK(latency == blockSize - 1);

    auto x = GenerateTestSignal(500, /*seed=*/1);
    std::vector<float> flushed = x;
    flushed.resize(x.size() + latency + blockSize, 0.0f);  // let the last block fully drain

    std::vector<float> out(flushed.size());
    ola.Process(flushed.data(), out.data(), static_cast<uint32_t>(flushed.size()), 1);

    for (uint32_t i = 0; i < latency; ++i)
        CHECK(out[i] == 0.0f);  // exact: nothing but zero-fill has reached the output yet

    for (size_t n = 0; n < x.size(); ++n)
        CHECK_NEAR(out[n + latency], x[n], 1e-4f);
}

void OverlapAdd_MatchesDirectConvolutionSingleCall() {
    const uint32_t blockSize = 64;
    const std::vector<float> h = { 0.2f, 0.2f, 0.2f, 0.2f, 0.2f };  // 5-tap moving average

    DSP::OverlapAdd ola;
    CHECK(ola.Prepare(blockSize, static_cast<uint32_t>(h.size()), 1));
    CHECK(ola.SetImpulseResponse(h.data(), static_cast<uint32_t>(h.size())));

    auto x = GenerateTestSignal(1000, /*seed=*/2);
    const uint32_t latency = ola.GetLatencySamples();

    std::vector<float> flushed = x;
    flushed.resize(x.size() + latency + h.size() + blockSize, 0.0f);

    std::vector<float> out(flushed.size());
    ola.Process(flushed.data(), out.data(), static_cast<uint32_t>(flushed.size()), 1);

    auto reference = DirectConvolve(x, h);
    for (size_t n = 0; n < reference.size(); ++n)
        CHECK_NEAR(out[n + latency], reference[n], 1e-3f);
}

void OverlapAdd_ArbitraryCallSizesMatchDirectConvolution() {
    // Same filter/signal as above, but fed through Process() in irregular,
    // deliberately non-block-aligned chunks -- this is the streaming
    // contract Process() actually promises (frames need not be a multiple
    // of blockSize, and may vary call to call).
    const uint32_t blockSize = 96;
    const std::vector<float> h = { 1.0f, -0.5f, 0.25f, -0.125f, 0.0625f, 0.1f, -0.2f };

    DSP::OverlapAdd ola;
    CHECK(ola.Prepare(blockSize, static_cast<uint32_t>(h.size()), 1));
    CHECK(ola.SetImpulseResponse(h.data(), static_cast<uint32_t>(h.size())));

    auto x = GenerateTestSignal(2000, /*seed=*/3);
    const uint32_t latency = ola.GetLatencySamples();

    std::vector<float> flushed = x;
    flushed.resize(x.size() + latency + h.size() + blockSize, 0.0f);

    const std::vector<uint32_t> chunkSizes = { 1, 37, 41, 100, 3, 251, 17 };
    auto out = RunChunked(ola, flushed, chunkSizes);

    auto reference = DirectConvolve(x, h);
    for (size_t n = 0; n < reference.size(); ++n)
        CHECK_NEAR(out[n + latency], reference[n], 1e-3f);
}

void OverlapAdd_MultiChannelIndependence() {
    const uint32_t blockSize = 64;
    const std::vector<float> h = { 0.5f, 0.3f, 0.2f };

    DSP::OverlapAdd olaMulti;
    CHECK(olaMulti.Prepare(blockSize, static_cast<uint32_t>(h.size()), 2));
    CHECK(olaMulti.SetImpulseResponse(h.data(), static_cast<uint32_t>(h.size())));

    DSP::OverlapAdd olaCh0Alone, olaCh1Alone;
    CHECK(olaCh0Alone.Prepare(blockSize, static_cast<uint32_t>(h.size()), 1));
    CHECK(olaCh0Alone.SetImpulseResponse(h.data(), static_cast<uint32_t>(h.size())));
    CHECK(olaCh1Alone.Prepare(blockSize, static_cast<uint32_t>(h.size()), 1));
    CHECK(olaCh1Alone.SetImpulseResponse(h.data(), static_cast<uint32_t>(h.size())));

    auto ch0 = GenerateTestSignal(600, /*seed=*/4);
    auto ch1 = GenerateTestSignal(600, /*seed=*/5);
    const uint32_t pad = (blockSize - 1) + static_cast<uint32_t>(h.size()) + blockSize;
    ch0.resize(ch0.size() + pad, 0.0f);
    ch1.resize(ch1.size() + pad, 0.0f);

    std::vector<float> interleaved(ch0.size() * 2);
    for (size_t i = 0; i < ch0.size(); ++i) {
        interleaved[i * 2]     = ch0[i];
        interleaved[i * 2 + 1] = ch1[i];
    }

    std::vector<float> outMulti(interleaved.size());
    olaMulti.Process(interleaved.data(), outMulti.data(), static_cast<uint32_t>(ch0.size()), 2);

    std::vector<float> outCh0Alone(ch0.size()), outCh1Alone(ch1.size());
    olaCh0Alone.Process(ch0.data(), outCh0Alone.data(), static_cast<uint32_t>(ch0.size()), 1);
    olaCh1Alone.Process(ch1.data(), outCh1Alone.data(), static_cast<uint32_t>(ch1.size()), 1);

    for (size_t i = 0; i < ch0.size(); ++i) {
        CHECK_NEAR(outMulti[i * 2],     outCh0Alone[i], 1e-5f);
        CHECK_NEAR(outMulti[i * 2 + 1], outCh1Alone[i], 1e-5f);
    }
}

void OverlapAdd_ResetClearsCarriedTail() {
    const uint32_t blockSize = 32;
    // A long-ish filter so its tail clearly extends past one block, making
    // leftover-tail bleed obvious if Reset() failed to clear it.
    std::vector<float> h(20, 0.1f);

    DSP::OverlapAdd ola;
    CHECK(ola.Prepare(blockSize, static_cast<uint32_t>(h.size()), 1));
    CHECK(ola.SetImpulseResponse(h.data(), static_cast<uint32_t>(h.size())));

    auto x = GenerateTestSignal(300, /*seed=*/6);
    std::vector<float> out1(x.size());
    ola.Process(x.data(), out1.data(), static_cast<uint32_t>(x.size()), 1);
    CHECK(out1.back() != 0.0f || out1.front() != 0.0f);  // sanity: it did something

    ola.Reset();

    std::vector<float> silence(200, 0.0f);
    std::vector<float> out2(silence.size());
    ola.Process(silence.data(), out2.data(), static_cast<uint32_t>(silence.size()), 1);

    for (float v : out2)
        CHECK(v == 0.0f);  // no leftover tail/ring/stage content after Reset()
}

void OverlapAdd_ImpulseResponseHotSwapAffectsOnlyFutureBlocks() {
    // Documents (rather than merely asserts) the atomic double-buffer
    // publish semantics: SetImpulseResponse() takes effect the next time
    // ProcessOneBlock() actually runs the FFT (i.e. the next block that
    // *completes after* the swap), not retroactively against a block whose
    // convolution already ran and is merely waiting, already-computed, in
    // the output ring. This matches Biquad's SetPeaking()/SetCoefficients()
    // contract (see Biquad.h): the RT thread always sees either the fully
    // old or fully new filter, never a torn mix, and the exact block
    // boundary where the switch takes effect is the one in progress when
    // the swap is published.
    const uint32_t blockSize = 64;
    DSP::OverlapAdd ola;
    CHECK(ola.Prepare(blockSize, 1, 1));  // identity filter installed by Prepare()

    // Call 1: exactly one block's worth of input. This fully computes
    // block 1 (under the still-active identity filter) and queues its
    // blockSize output samples into the ring; the same call already drains
    // one of them (the ring becomes non-empty and is drained within the
    // same iteration it's filled), leaving blockSize - 1 queued.
    std::vector<float> ones(blockSize, 1.0f);
    std::vector<float> out1(blockSize);
    ola.Process(ones.data(), out1.data(), blockSize, 1);

    // Swap the filter *after* block 1's convolution has already run.
    const float half = 0.5f;
    CHECK(ola.SetImpulseResponse(&half, 1));

    // Call 2: another full block. The first blockSize - 1 samples drained
    // here are the leftover queue from block 1 -- already computed under
    // the OLD (identity) filter -- so they must still read 1.0 despite the
    // swap having already happened. Only the new sample produced once
    // block 2 completes (the very last sample of this call, since block 2
    // finishes exactly when this call's input is exhausted) should reflect
    // the NEW (0.5x) filter.
    std::vector<float> out2(blockSize);
    ola.Process(ones.data(), out2.data(), blockSize, 1);

    for (uint32_t i = 0; i < blockSize - 1; ++i)
        CHECK_NEAR(out2[i], 1.0f, 1e-4f);
    CHECK_NEAR(out2[blockSize - 1], 0.5f, 1e-4f);
}

}  // namespace

int main() {
    RUN_TEST(OverlapAdd_PrepareRejectsInvalidParams);
    RUN_TEST(OverlapAdd_FftSizeIsAlwaysTwoThreeFiveSmooth);
    RUN_TEST(OverlapAdd_SetImpulseResponseRejectsBadArgs);
    RUN_TEST(OverlapAdd_LatencyIsBlockSizeMinusOne);
    RUN_TEST(OverlapAdd_IdentityIsDelayedPassthrough);
    RUN_TEST(OverlapAdd_MatchesDirectConvolutionSingleCall);
    RUN_TEST(OverlapAdd_ArbitraryCallSizesMatchDirectConvolution);
    RUN_TEST(OverlapAdd_MultiChannelIndependence);
    RUN_TEST(OverlapAdd_ResetClearsCarriedTail);
    RUN_TEST(OverlapAdd_ImpulseResponseHotSwapAffectsOnlyFutureBlocks);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
