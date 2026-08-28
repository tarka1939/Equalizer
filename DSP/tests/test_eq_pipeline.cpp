/*
 * test_eq_pipeline.cpp — Unit tests for DSP::EqPipeline, the combined
 * FIR (OverlapAdd) + IIR (Equalizer10Band) execution pipeline.
 *
 * Central correctness strategy throughout: EqPipeline must produce
 * bit-identical output to manually driving the same two sub-engines
 * (independently configured and Prepare()'d the same way) in the order
 * EqPipeline claims to use. If EqPipeline ever silently reordered stages,
 * skipped one it should have run, or ran one it shouldn't have, one of
 * these reference comparisons would catch it exactly (not "close enough").
 *
 * No external test framework (see DSP/tests/test_biquad.cpp and
 * DSP/tests/test_overlap_add.cpp for the same hand-rolled pattern).
 *
 * Build (standalone, no CMake needed):
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -I../../Equalizer/kissfft-131.2.0 \
 *       -c ../EqPipeline.cpp -o EqPipeline.o
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -I../../Equalizer/kissfft-131.2.0 \
 *       -c ../OverlapAdd.cpp -o OverlapAdd.o
 *   g++ -std=c++17 -O2 -I../../Equalizer/kissfft-131.2.0 \
 *       -c ../../Equalizer/kissfft-131.2.0/kiss_fft.c -o kiss_fft.o
 *   g++ -std=c++17 -O2 -I../../Equalizer/kissfft-131.2.0 \
 *       -c ../../Equalizer/kissfft-131.2.0/kiss_fftr.c -o kiss_fftr.o
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -c test_eq_pipeline.cpp -o test_eq_pipeline.o
 *   g++ EqPipeline.o OverlapAdd.o kiss_fft.o kiss_fftr.o test_eq_pipeline.o -o test_eq_pipeline
 *   ./test_eq_pipeline
 */
#include "../EqPipeline.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
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

// ── Helpers ───────────────────────────────────────────────────────────────

constexpr uint32_t kChannels = 2;
constexpr float kSampleRate = 48000.0f;
constexpr uint32_t kFirBlockSize = 64;
constexpr uint32_t kMaxImpulse = 32;

std::vector<float> GenerateTestSignal(uint32_t frames, uint32_t channels, uint32_t seed) {
    std::vector<float> v(frames * channels);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& s : v) s = dist(rng);
    return v;
}

std::array<float, DSP::Equalizer10Band::BandCount> DefaultCenters() {
    return { 31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f };
}

std::array<float, DSP::Equalizer10Band::BandCount> ZeroGains() {
    std::array<float, DSP::Equalizer10Band::BandCount> g{};
    g.fill(0.0f);
    return g;
}

std::array<float, DSP::Equalizer10Band::BandCount> SampleGains() {
    std::array<float, DSP::Equalizer10Band::BandCount> g{};
    g.fill(0.0f);
    g[2] = 6.0f;
    g[7] = -4.5f;
    return g;
}

std::vector<float> SampleTaps() {
    // An arbitrary, non-trivial (not identity, not symmetric) short FIR.
    return { 0.6f, 0.25f, -0.1f, 0.05f, -0.02f };
}

// ── Tests ─────────────────────────────────────────────────────────────────

void EqPipeline_DefaultsToPassthrough() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));

    CHECK(pipeline.IsFirActive() == false);
    CHECK(pipeline.IsIirActive() == false);
    CHECK(pipeline.GetLatencySamples() == 0u);

    auto input = GenerateTestSignal(200, kChannels, 1);
    std::vector<float> output(input.size(), -999.0f);
    pipeline.Process(input.data(), output.data(), 200, kChannels);

    for (size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == input[i]);
}

void EqPipeline_UnpreparedIsPassthrough() {
    // Never call Prepare() at all -- must match Equalizer10Band's own
    // "unprepared instance quietly passes through" contract (see
    // ARCHITECTURE.md section 7.1), not silently leave output untouched.
    DSP::EqPipeline pipeline;
    auto input = GenerateTestSignal(50, kChannels, 2);
    std::vector<float> output(input.size(), -999.0f);
    pipeline.Process(input.data(), output.data(), 50, kChannels);
    for (size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == input[i]);
}

void EqPipeline_AllZeroGainsLeavesIirInactive() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    pipeline.SetBandsPeaking(DefaultCenters(), ZeroGains(), 1.0f);
    CHECK(pipeline.IsIirActive() == false);
}

void EqPipeline_NonZeroGainActivatesIir() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    pipeline.SetBandsPeaking(DefaultCenters(), SampleGains(), 1.0f);
    CHECK(pipeline.IsIirActive() == true);
    CHECK(pipeline.IsFirActive() == false);  // untouched by SetBandsPeaking
}

void EqPipeline_IirOnlyMatchesStandaloneEqualizer10Band() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    pipeline.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);

    DSP::Equalizer10Band reference;
    reference.Prepare(kSampleRate, kChannels);
    reference.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);

    auto input = GenerateTestSignal(500, kChannels, 3);
    std::vector<float> actual(input.size());
    std::vector<float> expected(input.size());

    pipeline.Process(input.data(), actual.data(), 500, kChannels);
    reference.Process(input.data(), expected.data(), 500, kChannels);

    for (size_t i = 0; i < input.size(); ++i)
        CHECK(actual[i] == expected[i]);
}

void EqPipeline_FirOnlyMatchesStandaloneOverlapAdd() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    auto taps = SampleTaps();
    CHECK(pipeline.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));
    CHECK(pipeline.IsFirActive() == true);
    CHECK(pipeline.IsIirActive() == false);  // untouched by SetImpulseResponse
    CHECK(pipeline.GetLatencySamples() == kFirBlockSize - 1);

    DSP::OverlapAdd reference;
    CHECK(reference.Prepare(kFirBlockSize, kMaxImpulse, kChannels));
    CHECK(reference.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));

    auto input = GenerateTestSignal(500, kChannels, 4);
    std::vector<float> actual(input.size());
    std::vector<float> expected(input.size());

    pipeline.Process(input.data(), actual.data(), 500, kChannels);
    reference.Process(input.data(), expected.data(), 500, kChannels);

    for (size_t i = 0; i < input.size(); ++i)
        CHECK(actual[i] == expected[i]);
}

void EqPipeline_BothActiveRunsFirThenIir() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    auto taps = SampleTaps();
    CHECK(pipeline.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));
    pipeline.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);
    CHECK(pipeline.IsFirActive() == true);
    CHECK(pipeline.IsIirActive() == true);

    // Reference: independently-configured FIR feeding an independently-
    // configured IIR, in that order -- exactly what the class comment in
    // EqPipeline.h claims. If the implementation ever ran IIR first, or
    // ran them on separate copies of the input instead of cascading, this
    // comparison would diverge.
    DSP::OverlapAdd referenceFir;
    CHECK(referenceFir.Prepare(kFirBlockSize, kMaxImpulse, kChannels));
    CHECK(referenceFir.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));

    DSP::Equalizer10Band referenceIir;
    referenceIir.Prepare(kSampleRate, kChannels);
    referenceIir.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);

    auto input = GenerateTestSignal(500, kChannels, 5);
    std::vector<float> actual(input.size());
    std::vector<float> expected(input.size());

    pipeline.Process(input.data(), actual.data(), 500, kChannels);

    referenceFir.Process(input.data(), expected.data(), 500, kChannels);
    referenceIir.Process(expected.data(), expected.data(), 500, kChannels);

    for (size_t i = 0; i < input.size(); ++i)
        CHECK(actual[i] == expected[i]);

    // Sanity: cascading through both stages must differ from running IIR
    // alone on the same input (otherwise this test could pass vacuously,
    // e.g. if FIR were silently skipped).
    std::vector<float> iirOnly(input.size());
    DSP::Equalizer10Band iirOnlyRef;
    iirOnlyRef.Prepare(kSampleRate, kChannels);
    iirOnlyRef.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);
    iirOnlyRef.Process(input.data(), iirOnly.data(), 500, kChannels);

    bool anyDifferent = false;
    for (size_t i = 0; i < input.size(); ++i) {
        if (std::fabs(actual[i] - iirOnly[i]) > 1e-9f) { anyDifferent = true; break; }
    }
    CHECK(anyDifferent);
}

void EqPipeline_ClearImpulseResponseFallsBackToIirOnly() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    auto taps = SampleTaps();
    CHECK(pipeline.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));
    pipeline.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);
    CHECK(pipeline.IsFirActive() == true);

    pipeline.ClearImpulseResponse();
    CHECK(pipeline.IsFirActive() == false);
    CHECK(pipeline.IsIirActive() == true);  // untouched by ClearImpulseResponse
    CHECK(pipeline.GetLatencySamples() == 0u);

    DSP::Equalizer10Band reference;
    reference.Prepare(kSampleRate, kChannels);
    reference.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);

    auto input = GenerateTestSignal(300, kChannels, 6);
    std::vector<float> actual(input.size());
    std::vector<float> expected(input.size());

    pipeline.Process(input.data(), actual.data(), 300, kChannels);
    reference.Process(input.data(), expected.data(), 300, kChannels);

    for (size_t i = 0; i < input.size(); ++i)
        CHECK(actual[i] == expected[i]);
}

void EqPipeline_ResetPreservesActiveFlagsAndConfiguration() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    auto taps = SampleTaps();
    CHECK(pipeline.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));
    pipeline.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);

    pipeline.Reset();

    CHECK(pipeline.IsFirActive() == true);
    CHECK(pipeline.IsIirActive() == true);
}

void EqPipeline_SetImpulseResponseRejectsOversizedTaps() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));
    std::vector<float> tooLong(kMaxImpulse + 1, 0.1f);
    CHECK(pipeline.SetImpulseResponse(tooLong.data(), static_cast<uint32_t>(tooLong.size())) == false);
    CHECK(pipeline.IsFirActive() == false);
}

void EqPipeline_RePrepareResetsFirButPreservesIir() {
    // Regression test for a real behavioral difference between the two
    // sub-engines: OverlapAdd::Prepare() always reinstalls an identity
    // filter (so FIR necessarily goes inactive on re-Prepare), but
    // Equalizer10Band::Prepare() (-> Biquad::Prepare()) only resizes
    // per-channel filter history and never touches configured
    // coefficients, so a configured IIR curve must survive a re-Prepare()
    // (e.g. pipewire_backend.cpp's OnParamChanged() re-Prepares on every
    // sample-rate/channel change without re-sending set_bands). If this
    // ever regresses, a live EQ curve would silently drop to passthrough
    // on the next format change in production.
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(kSampleRate, kChannels, kFirBlockSize, kMaxImpulse));

    auto taps = SampleTaps();
    CHECK(pipeline.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));
    pipeline.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);
    CHECK(pipeline.IsFirActive() == true);
    CHECK(pipeline.IsIirActive() == true);

    // Simulate a format change (e.g. sample rate switch): re-Prepare with
    // a different sample rate, same channel count.
    CHECK(pipeline.Prepare(44100.0f, kChannels, kFirBlockSize, kMaxImpulse));

    CHECK(pipeline.IsFirActive() == false);   // FIR: necessarily reset
    CHECK(pipeline.IsIirActive() == true);    // IIR: must survive

    // And IIR must still actually apply the previously-configured
    // coefficients, not just report itself active -- compare against a
    // reference engine driven through the exact same sequence (Prepare at
    // the original rate, SetBandsPeaking, then re-Prepare at the new rate
    // *without* re-calling SetBandsPeaking). This reference is NOT
    // "SetBandsPeaking recomputed for 44100 Hz": Biquad's coefficients are
    // computed once, at SetPeaking() time, from whatever sample rate was
    // active *then* (see DSP/Biquad.cpp's omega = 2*pi*centerHz/sr), and
    // Prepare() never recomputes them. So the correct expectation after a
    // sample-rate-only re-Prepare is "still filtering with the *old*
    // rate's coefficients", exactly matching pre-existing
    // Equalizer10Band behavior (and pipewire_backend.cpp's
    // OnParamChanged(), which never re-sends set_bands on a format
    // change) -- not "coefficients magically re-tuned to the new rate".
    DSP::Equalizer10Band reference;
    reference.Prepare(kSampleRate, kChannels);
    reference.SetBandsPeaking(DefaultCenters(), SampleGains(), 0.9f);
    reference.Prepare(44100.0f, kChannels);

    auto input = GenerateTestSignal(200, kChannels, 7);
    std::vector<float> actual(input.size());
    std::vector<float> expected(input.size());
    pipeline.Process(input.data(), actual.data(), 200, kChannels);
    reference.Process(input.data(), expected.data(), 200, kChannels);
    for (size_t i = 0; i < input.size(); ++i)
        CHECK(actual[i] == expected[i]);
}

// A Process() call whose `channels` disagrees with what Prepare() was given
// must pass audio through, not leave the output buffer alone. Previously the
// call fell through to the stage dispatch, where OverlapAdd::Process()'s own
// channel guard returned without writing anything -- so with FIR active the
// caller got back whatever uninitialised or stale data its buffer held, while
// every other degenerate case in this class degrades to passthrough.
void EqPipeline_ChannelMismatchIsPassthroughNotUntouched() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(48000.0f, 2, 64, 16));

    const std::vector<float> taps{ 0.5f, 0.25f, 0.125f };
    CHECK(pipeline.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size())));
    CHECK(pipeline.IsFirActive());

    std::vector<float> input(120);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i) * 0.01f;

    // Poison the output so "left untouched" is distinguishable from "copied".
    std::vector<float> output(input.size(), -12345.0f);

    // Prepared for 2 channels, called with 3.
    pipeline.Process(input.data(), output.data(), 40, 3);

    for (size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == input[i]);
}

// Same guard, in place (input == output): must be a no-op, never a partial
// write of half-filtered data.
void EqPipeline_ChannelMismatchInPlaceLeavesDataIntact() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(48000.0f, 2, 64, 16));

    std::array<float, DSP::Equalizer10Band::BandCount> centres{
        31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f };
    std::array<float, DSP::Equalizer10Band::BandCount> gains{};
    gains[4] = 6.0f;
    pipeline.SetBandsPeaking(centres, gains, 1.0f);
    CHECK(pipeline.IsIirActive());

    std::vector<float> buffer(90);
    for (size_t i = 0; i < buffer.size(); ++i)
        buffer[i] = static_cast<float>(i) * 0.5f;
    const std::vector<float> original = buffer;

    pipeline.Process(buffer.data(), buffer.data(), 30, 3);  // prepared for 2

    for (size_t i = 0; i < buffer.size(); ++i)
        CHECK(buffer[i] == original[i]);
}

// Null buffers must be rejected rather than dereferenced.
void EqPipeline_NullBuffersAreIgnored() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(48000.0f, 2, 64, 16));

    std::vector<float> buffer(64, 1.0f);
    pipeline.Process(nullptr, buffer.data(), 32, 2);
    pipeline.Process(buffer.data(), nullptr, 32, 2);

    for (float v : buffer)
        CHECK(v == 1.0f);
}

// Non-finite taps must be refused, leaving the previously installed filter
// in place -- a NaN tap smears across every FFT bin and from there into every
// output sample, permanently.
void EqPipeline_NonFiniteTapsAreRejected() {
    DSP::EqPipeline pipeline;
    CHECK(pipeline.Prepare(48000.0f, 2, 64, 16));

    const std::vector<float> good{ 1.0f, 0.5f };
    CHECK(pipeline.SetImpulseResponse(good.data(), 2));
    CHECK(pipeline.IsFirActive());

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> bad{ 1.0f, nan, 0.25f };
    CHECK(pipeline.SetImpulseResponse(bad.data(), 3) == false);
    CHECK(pipeline.IsFirActive());  // still the good filter, still active

    const float inf = std::numeric_limits<float>::infinity();
    const std::vector<float> bad2{ inf, 0.1f };
    CHECK(pipeline.SetImpulseResponse(bad2.data(), 2) == false);

    // Output must remain finite.
    std::vector<float> input(128, 0.25f);
    std::vector<float> output(128, 0.0f);
    pipeline.Process(input.data(), output.data(), 64, 2);
    for (float v : output)
        CHECK(std::isfinite(v));
}

}  // namespace

int main() {
    RUN_TEST(EqPipeline_DefaultsToPassthrough);
    RUN_TEST(EqPipeline_UnpreparedIsPassthrough);
    RUN_TEST(EqPipeline_AllZeroGainsLeavesIirInactive);
    RUN_TEST(EqPipeline_NonZeroGainActivatesIir);
    RUN_TEST(EqPipeline_IirOnlyMatchesStandaloneEqualizer10Band);
    RUN_TEST(EqPipeline_FirOnlyMatchesStandaloneOverlapAdd);
    RUN_TEST(EqPipeline_BothActiveRunsFirThenIir);
    RUN_TEST(EqPipeline_ClearImpulseResponseFallsBackToIirOnly);
    RUN_TEST(EqPipeline_ResetPreservesActiveFlagsAndConfiguration);
    RUN_TEST(EqPipeline_SetImpulseResponseRejectsOversizedTaps);
    RUN_TEST(EqPipeline_RePrepareResetsFirButPreservesIir);
    RUN_TEST(EqPipeline_ChannelMismatchIsPassthroughNotUntouched);
    RUN_TEST(EqPipeline_ChannelMismatchInPlaceLeavesDataIntact);
    RUN_TEST(EqPipeline_NullBuffersAreIgnored);
    RUN_TEST(EqPipeline_NonFiniteTapsAreRejected);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
