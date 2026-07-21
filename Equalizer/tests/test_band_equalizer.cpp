/*
 * test_band_equalizer.cpp — Lightweight, dependency-free unit tests for
 * BandEqualizer (Equalizer/BandEqualizer.{h,cpp}), the plain-data 10-band
 * default-curve holder used by both the Windows APO (Equalizer.cpp) and
 * Tools/WavEqTest.cpp. Has zero Windows dependencies, so it builds and runs
 * cross-platform -- unlike the rest of Equalizer/.
 */
#include "../BandEqualizer.h"

#include <cstdio>

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
#define RUN_TEST(fn) do { g_currentTest = #fn; fn(); } while (0)

// ── Tests ────────────────────────────────────────────────────────────────────

void DefaultConstruction_HasTenBands() {
    BandEqualizer eq;
    CHECK(eq.GetBands().size() == BandEqualizer::BandCount);
    CHECK(BandEqualizer::BandCount == 10);
}

void DefaultConstruction_CentersAreStandardIsoBands() {
    BandEqualizer eq;
    const auto& bands = eq.GetBands();
    const float expectedCenters[10] = {
        31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
    };
    for (size_t i = 0; i < BandEqualizer::BandCount; ++i)
        CHECK(bands[i].centerHz == expectedCenters[i]);
}

void DefaultConstruction_CentersAreMonotonicallyIncreasing() {
    // Regression guard: whatever the exact center frequencies are, they must
    // stay in ascending order, or every caller that assumes "index == band
    // position on the curve" (LockForProcess, the GUI's 10 sliders) breaks.
    BandEqualizer eq;
    const auto& bands = eq.GetBands();
    for (size_t i = 1; i < BandEqualizer::BandCount; ++i)
        CHECK(bands[i].centerHz > bands[i - 1].centerHz);
}

void GetSetBandGain_RoundTrips() {
    BandEqualizer eq;
    eq.SetBandGain(3, -6.5f);
    CHECK(eq.GetBandGain(3) == -6.5f);
    // Untouched bands keep their default.
    CHECK(eq.GetBandGain(0) == 5.0f);
}

void SetBandGain_OutOfRangeIndexIsNoOp() {
    BandEqualizer eq;
    const float before = eq.GetBandGain(9);
    eq.SetBandGain(BandEqualizer::BandCount, 99.0f);       // == size(), one past the end
    eq.SetBandGain(BandEqualizer::BandCount + 100, 99.0f); // wildly out of range
    CHECK(eq.GetBandGain(9) == before);  // no adjacent band was corrupted
}

void GetBandGain_OutOfRangeIndexReturnsZero() {
    BandEqualizer eq;
    CHECK(eq.GetBandGain(BandEqualizer::BandCount) == 0.0f);
    CHECK(eq.GetBandGain(BandEqualizer::BandCount + 100) == 0.0f);
}

void SetBandGain_DoesNotAffectOtherBandsCenterFrequency() {
    BandEqualizer eq;
    const float centerBefore = eq.GetBands()[5].centerHz;
    eq.SetBandGain(5, 10.0f);
    CHECK(eq.GetBands()[5].centerHz == centerBefore);
}

}  // namespace

int main() {
    RUN_TEST(DefaultConstruction_HasTenBands);
    RUN_TEST(DefaultConstruction_CentersAreStandardIsoBands);
    RUN_TEST(DefaultConstruction_CentersAreMonotonicallyIncreasing);
    RUN_TEST(GetSetBandGain_RoundTrips);
    RUN_TEST(SetBandGain_OutOfRangeIndexIsNoOp);
    RUN_TEST(GetBandGain_OutOfRangeIndexReturnsZero);
    RUN_TEST(SetBandGain_DoesNotAffectOtherBandsCenterFrequency);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
