/*
 * test_biquad.cpp — Lightweight, dependency-free unit tests for DSP::Biquad
 * and DSP::Equalizer10Band.
 *
 * No external test framework is used (matching the project's existing
 * "no external dependency" style, e.g. daemon/ipc_server.cpp's hand-rolled
 * JSON parsing). Each test function returns true/false; failures print a
 * message. main() aggregates results and returns a nonzero exit code if
 * anything failed, so this can be wired into CI / CMake as a normal test
 * executable.
 *
 * Build (standalone, no CMake needed):
 *   g++ -std=c++17 -Wall -Wextra -O2 -I../.. -I.. -c ../Biquad.cpp -o Biquad.o
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -c test_biquad.cpp -o test_biquad.o
 *   g++ Biquad.o test_biquad.o -o test_biquad && ./test_biquad
 */
#include "../Biquad.h"
#include "../Equalizer10Band.h"

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

// ── Helpers ───────────────────────────────────────────────────────────────────

constexpr float kPi = 3.14159265358979323846f;

std::vector<float> GenerateSine(float freqHz, float sampleRate, uint32_t frames,
                                 float amplitude = 0.5f) {
    std::vector<float> out(frames);
    for (uint32_t i = 0; i < frames; ++i)
        out[i] = amplitude * std::sin(2.0f * kPi * freqHz * static_cast<float>(i) / sampleRate);
    return out;
}

// RMS over the tail of a buffer (skips the first `skip` samples to let an
// IIR filter's transient settle before measuring steady-state amplitude).
float TailRms(const std::vector<float>& buf, uint32_t skip) {
    if (skip >= buf.size()) return 0.0f;
    double sumSq = 0.0;
    uint32_t n = 0;
    for (uint32_t i = skip; i < buf.size(); ++i, ++n)
        sumSq += static_cast<double>(buf[i]) * buf[i];
    return n ? static_cast<float>(std::sqrt(sumSq / n)) : 0.0f;
}

float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

// ── Biquad tests ──────────────────────────────────────────────────────────────

void Biquad_DefaultIsUnityPassthrough() {
    DSP::Biquad bq;
    bq.Prepare(48000.0f, 1);

    std::vector<float> in  = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, 0.0f, -1.0f, 1.0f };
    std::vector<float> out(in.size());
    bq.Process(in.data(), out.data(), static_cast<uint32_t>(in.size()), 1);

    for (size_t i = 0; i < in.size(); ++i)
        CHECK_NEAR(out[i], in[i], 1e-6f);
}

void Biquad_ZeroDbPeakingIsPassthrough() {
    DSP::Biquad bq;
    bq.Prepare(48000.0f, 1);
    bq.SetPeaking(1000.0f, 1.0f, 0.0f);  // 0 dB gain: numerator == denominator

    auto sine = GenerateSine(1000.0f, 48000.0f, 2000);
    std::vector<float> out(sine.size());
    bq.Process(sine.data(), out.data(), static_cast<uint32_t>(sine.size()), 1);

    // A 0 dB peaking biquad is the identity transfer function (b_i == a_i for
    // all i once normalized), so output should track input essentially exactly.
    for (size_t i = 100; i < sine.size(); ++i)
        CHECK_NEAR(out[i], sine[i], 1e-4f);
}

void Biquad_PositiveGainBoostsCenterFrequency() {
    DSP::Biquad bq;
    const float sr = 48000.0f;
    const float freq = 1000.0f;
    const float gainDb = 6.0f;
    bq.Prepare(sr, 1);
    bq.SetPeaking(freq, 1.0f, gainDb);

    auto sine = GenerateSine(freq, sr, 4000, 0.3f);
    std::vector<float> out(sine.size());
    bq.Process(sine.data(), out.data(), static_cast<uint32_t>(sine.size()), 1);

    float inRms  = TailRms(sine, 500);
    float outRms = TailRms(out, 500);
    float ratio  = outRms / inRms;
    float expected = DbToLinear(gainDb);  // ~1.995x for +6 dB

    CHECK_NEAR(ratio, expected, 0.1f);
}

void Biquad_NegativeGainCutsCenterFrequency() {
    DSP::Biquad bq;
    const float sr = 48000.0f;
    const float freq = 1000.0f;
    const float gainDb = -6.0f;
    bq.Prepare(sr, 1);
    bq.SetPeaking(freq, 1.0f, gainDb);

    auto sine = GenerateSine(freq, sr, 4000, 0.3f);
    std::vector<float> out(sine.size());
    bq.Process(sine.data(), out.data(), static_cast<uint32_t>(sine.size()), 1);

    float inRms  = TailRms(sine, 500);
    float outRms = TailRms(out, 500);
    float ratio  = outRms / inRms;
    float expected = DbToLinear(gainDb);  // ~0.501x for -6 dB

    CHECK_NEAR(ratio, expected, 0.05f);
}

void Biquad_MultiChannelIndependence() {
    DSP::Biquad bq;
    const float sr = 48000.0f;
    bq.Prepare(sr, 2);
    // Boost 1kHz; channel 0 carries 1kHz (boosted), channel 1 carries 200Hz (not boosted).
    bq.SetPeaking(1000.0f, 4.0f, 12.0f);

    const uint32_t frames = 4000;
    auto ch0 = GenerateSine(1000.0f, sr, frames, 0.2f);
    auto ch1 = GenerateSine(200.0f,  sr, frames, 0.2f);

    std::vector<float> interleaved(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        interleaved[i * 2]     = ch0[i];
        interleaved[i * 2 + 1] = ch1[i];
    }

    std::vector<float> out(interleaved.size());
    bq.Process(interleaved.data(), out.data(), frames, 2);

    std::vector<float> outCh0(frames), outCh1(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        outCh0[i] = out[i * 2];
        outCh1[i] = out[i * 2 + 1];
    }

    float ratio0 = TailRms(outCh0, 500) / TailRms(ch0, 500);
    float ratio1 = TailRms(outCh1, 500) / TailRms(ch1, 500);

    // Channel 0 (at the boosted frequency) should gain ~+12dB; channel 1
    // (far from the boosted band) should be close to unity. Channels must
    // not bleed into each other's filter state.
    CHECK_NEAR(ratio0, DbToLinear(12.0f), 0.15f);
    CHECK_NEAR(ratio1, 1.0f, 0.1f);
}

void Biquad_ResetClearsInternalState() {
    DSP::Biquad bq;
    bq.Prepare(48000.0f, 1);
    bq.SetPeaking(1000.0f, 0.5f, 12.0f);  // low-Q, high-gain: pronounced ringing

    auto sine = GenerateSine(1000.0f, 48000.0f, 500, 0.8f);
    std::vector<float> out1(sine.size());
    bq.Process(sine.data(), out1.data(), static_cast<uint32_t>(sine.size()), 1);
    CHECK(TailRms(out1, 0) > 0.0f);  // sanity: filter did something

    bq.Reset();

    std::vector<float> silence(200, 0.0f);
    std::vector<float> out2(silence.size());
    bq.Process(silence.data(), out2.data(), static_cast<uint32_t>(silence.size()), 1);

    // After Reset(), feeding silence must produce silence -- no leftover
    // ringing from the previous buffer's filter state (x1/x2/y1/y2).
    for (float v : out2)
        CHECK(v == 0.0f);
}

// ── Equalizer10Band tests ─────────────────────────────────────────────────────

std::array<float, DSP::Equalizer10Band::BandCount> StandardCentres() {
    return { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
}

void Equalizer10Band_UnpreparedActsAsPassthrough() {
    // Regression test for the exact failure mode found in Equalizer.cpp's
    // APOProcess(): a default-constructed, never-Prepare()'d Equalizer10Band
    // has m_channels == 0, and Process() must degrade to a straight copy
    // rather than silently doing nothing or crashing.
    DSP::Equalizer10Band eq;  // never Prepare()'d
    std::vector<float> in  = { 0.1f, 0.2f, -0.3f, 0.4f, -0.5f };
    std::vector<float> out(in.size(), 0.0f);

    eq.Process(in.data(), out.data(), static_cast<uint32_t>(in.size()), 1);

    for (size_t i = 0; i < in.size(); ++i)
        CHECK(out[i] == in[i]);
}

void Equalizer10Band_AllZeroGainsIsPassthrough() {
    DSP::Equalizer10Band eq;
    eq.Prepare(48000.0f, 1);
    auto centres = StandardCentres();
    std::array<float, DSP::Equalizer10Band::BandCount> gains{};  // all 0 dB
    eq.SetBandsPeaking(centres, gains, 1.0f);
    eq.Reset();

    auto sine = GenerateSine(1000.0f, 48000.0f, 2000, 0.4f);
    std::vector<float> out(sine.size());
    eq.Process(sine.data(), out.data(), static_cast<uint32_t>(sine.size()), 1);

    for (size_t i = 200; i < sine.size(); ++i)
        CHECK_NEAR(out[i], sine[i], 1e-3f);
}

void Equalizer10Band_BoostAtBandCenterIncreasesAmplitude() {
    DSP::Equalizer10Band eq;
    const float sr = 48000.0f;
    eq.Prepare(sr, 1);
    auto centres = StandardCentres();
    std::array<float, DSP::Equalizer10Band::BandCount> gains{};
    gains[5] = 6.0f;  // band 5 == 1000 Hz
    eq.SetBandsPeaking(centres, gains, 1.0f);
    eq.Reset();

    auto sine = GenerateSine(1000.0f, sr, 4000, 0.3f);
    std::vector<float> out(sine.size());
    eq.Process(sine.data(), out.data(), static_cast<uint32_t>(sine.size()), 1);

    float ratio = TailRms(out, 500) / TailRms(sine, 500);
    CHECK_NEAR(ratio, DbToLinear(6.0f), 0.15f);
}

void Equalizer10Band_InPlaceMatchesOutOfPlace() {
    DSP::Equalizer10Band eqA, eqB;
    eqA.Prepare(48000.0f, 1);
    eqB.Prepare(48000.0f, 1);
    auto centres = StandardCentres();
    std::array<float, DSP::Equalizer10Band::BandCount> gains{};
    gains[2] = 5.0f;
    gains[7] = -4.0f;
    eqA.SetBandsPeaking(centres, gains, 1.0f);
    eqB.SetBandsPeaking(centres, gains, 1.0f);
    eqA.Reset();
    eqB.Reset();

    auto sineA = GenerateSine(700.0f, 48000.0f, 1000, 0.5f);
    auto sineB = sineA;  // copy for in-place processing

    std::vector<float> outOfPlace(sineA.size());
    eqA.Process(sineA.data(), outOfPlace.data(), static_cast<uint32_t>(sineA.size()), 1);

    eqB.Process(sineB.data(), sineB.data(), static_cast<uint32_t>(sineB.size()), 1);  // in-place

    for (size_t i = 0; i < sineA.size(); ++i)
        CHECK_NEAR(outOfPlace[i], sineB[i], 1e-6f);
}

}  // namespace

int main() {
    RUN_TEST(Biquad_DefaultIsUnityPassthrough);
    RUN_TEST(Biquad_ZeroDbPeakingIsPassthrough);
    RUN_TEST(Biquad_PositiveGainBoostsCenterFrequency);
    RUN_TEST(Biquad_NegativeGainCutsCenterFrequency);
    RUN_TEST(Biquad_MultiChannelIndependence);
    RUN_TEST(Biquad_ResetClearsInternalState);

    RUN_TEST(Equalizer10Band_UnpreparedActsAsPassthrough);
    RUN_TEST(Equalizer10Band_AllZeroGainsIsPassthrough);
    RUN_TEST(Equalizer10Band_BoostAtBandCenterIncreasesAmplitude);
    RUN_TEST(Equalizer10Band_InPlaceMatchesOutOfPlace);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
