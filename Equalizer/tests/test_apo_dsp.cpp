/*
 * test_apo_dsp.cpp — Lightweight, dependency-free unit tests for
 * ApoDsp::ProcessBlock, the per-block gain/EQ/clamp pipeline pulled out of
 * Equalizer::APOProcess (Equalizer.cpp) so it can be tested without the
 * Windows COM/APO_CONNECTION_PROPERTY plumbing.
 *
 * Same no-external-framework style as DSP/tests/test_biquad.cpp. Builds and
 * runs cross-platform (this file and ApoDsp.{h,cpp} have zero Windows
 * dependencies) -- unlike the rest of Equalizer/, which only builds on
 * Windows via Equalizer.vcxproj.
 */
#include "../ApoDsp.h"
#include "../../DSP/Equalizer10Band.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int   g_checks = 0;
int   g_failures = 0;
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

// ── Tests ────────────────────────────────────────────────────────────────────

void ProcessBlock_ZeroFramesReturnsZeroAndTouchesNothing() {
    DSP::Equalizer10Band eq;
    std::vector<float> in = { 1.0f, 1.0f };
    std::vector<float> out = { -5.0f, -5.0f };  // sentinel values

    uint32_t written = ApoDsp::ProcessBlock(in.data(), out.data(), 0, 2, 1.0f, eq);

    CHECK(written == 0);
    CHECK(out[0] == -5.0f);
    CHECK(out[1] == -5.0f);
}

void ProcessBlock_UnityGainUnpreparedEqIsPassthrough() {
    // An unprepared Equalizer10Band degrades to a straight copy (see
    // DSP/tests/test_biquad.cpp: Equalizer10Band_UnpreparedActsAsPassthrough).
    // With gain == 1.0 and values inside [-1, 1], ProcessBlock should be a
    // pure passthrough.
    DSP::Equalizer10Band eq;  // never Prepare()'d
    std::vector<float> in  = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, 0.0f };
    std::vector<float> out(in.size());

    uint32_t written = ApoDsp::ProcessBlock(in.data(), out.data(),
        static_cast<uint32_t>(in.size()), 1, 1.0f, eq);

    CHECK(written == static_cast<uint32_t>(in.size()));
    for (size_t i = 0; i < in.size(); ++i)
        CHECK_NEAR(out[i], in[i], 1e-6f);
}

void ProcessBlock_GainIsAppliedBeforeClamping() {
    DSP::Equalizer10Band eq;  // unprepared -> passthrough EQ stage
    std::vector<float> in  = { 0.5f, -0.5f };
    std::vector<float> out(in.size());

    uint32_t written = ApoDsp::ProcessBlock(in.data(), out.data(),
        static_cast<uint32_t>(in.size()), 1, 0.5f, eq);

    CHECK(written == 2);
    CHECK_NEAR(out[0], 0.25f, 1e-6f);
    CHECK_NEAR(out[1], -0.25f, 1e-6f);
}

void ProcessBlock_ZeroGainProducesSilence() {
    // Documents the effect of Equalizer::m_gain's current default (0.0f,
    // despite the "// 80% volume" comment next to its declaration in
    // Equalizer.h) -- with gain == 0, every sample collapses to 0 regardless
    // of input or EQ settings. This looks like a real bug in the production
    // default; flagged here rather than silently changed, since only
    // extraction for testability was in scope for this pass.
    DSP::Equalizer10Band eq;
    std::vector<float> in = { 0.9f, -0.9f, 0.5f };
    std::vector<float> out(in.size());

    ApoDsp::ProcessBlock(in.data(), out.data(), static_cast<uint32_t>(in.size()), 1, 0.0f, eq);

    for (float v : out)
        CHECK(v == 0.0f);
}

void ProcessBlock_ClampsPositiveOverrange() {
    DSP::Equalizer10Band eq;  // unprepared -> passthrough EQ stage
    std::vector<float> in  = { 0.9f };
    std::vector<float> out(in.size());

    // gain 2.0 * 0.9 = 1.8, must clamp to 1.0
    ApoDsp::ProcessBlock(in.data(), out.data(), 1, 1, 2.0f, eq);

    CHECK_NEAR(out[0], 1.0f, 1e-6f);
}

void ProcessBlock_ClampsNegativeOverrange() {
    DSP::Equalizer10Band eq;
    std::vector<float> in  = { -0.9f };
    std::vector<float> out(in.size());

    ApoDsp::ProcessBlock(in.data(), out.data(), 1, 1, 2.0f, eq);

    CHECK_NEAR(out[0], -1.0f, 1e-6f);
}

void ProcessBlock_InPlaceMatchesOutOfPlace() {
    DSP::Equalizer10Band eqA, eqB;
    auto centres = std::array<float, DSP::Equalizer10Band::BandCount>{31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    std::array<float, DSP::Equalizer10Band::BandCount> gains{};
    gains[4] = 6.0f;
    eqA.Prepare(48000.0f, 1);
    eqB.Prepare(48000.0f, 1);
    eqA.SetBandsPeaking(centres, gains, 1.0f);
    eqB.SetBandsPeaking(centres, gains, 1.0f);
    eqA.Reset();
    eqB.Reset();

    std::vector<float> in;
    for (int i = 0; i < 500; ++i)
        in.push_back(0.3f * std::sin(2.0f * 3.14159265f * 500.0f * static_cast<float>(i) / 48000.0f));

    std::vector<float> outOfPlace(in.size());
    ApoDsp::ProcessBlock(in.data(), outOfPlace.data(), static_cast<uint32_t>(in.size()), 1, 0.8f, eqA);

    std::vector<float> inPlace = in;  // separate copy, processed in-place
    ApoDsp::ProcessBlock(inPlace.data(), inPlace.data(), static_cast<uint32_t>(inPlace.size()), 1, 0.8f, eqB);

    for (size_t i = 0; i < in.size(); ++i)
        CHECK_NEAR(outOfPlace[i], inPlace[i], 1e-6f);
}

void ProcessBlock_MultiChannelInterleavedFrameCount() {
    DSP::Equalizer10Band eq;  // unprepared -> passthrough
    const uint32_t frames = 3;
    const uint32_t channels = 2;
    std::vector<float> in = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };  // 3 frames * 2ch
    std::vector<float> out(in.size());

    uint32_t written = ApoDsp::ProcessBlock(in.data(), out.data(), frames, channels, 1.0f, eq);

    CHECK(written == frames);  // return value is frames, not samples
    for (size_t i = 0; i < in.size(); ++i)
        CHECK_NEAR(out[i], in[i], 1e-6f);
}

}  // namespace

int main() {
    RUN_TEST(ProcessBlock_ZeroFramesReturnsZeroAndTouchesNothing);
    RUN_TEST(ProcessBlock_UnityGainUnpreparedEqIsPassthrough);
    RUN_TEST(ProcessBlock_GainIsAppliedBeforeClamping);
    RUN_TEST(ProcessBlock_ZeroGainProducesSilence);
    RUN_TEST(ProcessBlock_ClampsPositiveOverrange);
    RUN_TEST(ProcessBlock_ClampsNegativeOverrange);
    RUN_TEST(ProcessBlock_InPlaceMatchesOutOfPlace);
    RUN_TEST(ProcessBlock_MultiChannelInterleavedFrameCount);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
