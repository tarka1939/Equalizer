/*
 * test_eq_state.cpp — Unit tests for eq::EqState, the lock-free handoff
 * structure shared between the IPC (non-RT) thread and the audio (RT)
 * callback.
 *
 * No external test framework (see DSP/tests/test_biquad.cpp for the same
 * hand-rolled pattern used throughout this project).
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra -O2 -I.. -c test_eq_state.cpp -o test_eq_state.o
 *   g++ test_eq_state.o -o test_eq_state && ./test_eq_state
 */
#include "../eq_state.h"

#include <cstdio>

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
#define RUN_TEST(fn) do { g_currentTest = #fn; fn(); } while (0)

void EqState_DefaultsAreSane() {
    eq::EqState state;
    for (int i = 0; i < eq::kBandCount; ++i) {
        CHECK(state.pending_gains[i] == 0.0f);
        CHECK(state.current_gains[i] == 0.0f);
    }
    CHECK(state.preamp_db.load() == 0.0f);
    CHECK(state.enabled.load() == true);
    CHECK(state.pending_dirty.load() == false);
    CHECK(state.sample_rate == 48000.0f);
    CHECK(state.channels == 2u);
}

void EqState_SetGainsMarksDirtyAndStoresPending() {
    eq::EqState state;
    std::array<float, eq::kBandCount> gains{};
    gains[3] = 4.5f;
    gains[9] = -2.0f;

    state.SetGains(gains);

    CHECK(state.pending_dirty.load() == true);
    for (int i = 0; i < eq::kBandCount; ++i)
        CHECK(state.pending_gains[i] == gains[i]);

    // SetGains must NOT touch current_gains -- that's the IPC layer's job
    // (ipc_server.cpp assigns current_gains separately after a successful
    // set_bands command). This is a real seam in the design: the two
    // arrays are not automatically kept in sync by EqState itself.
    for (int i = 0; i < eq::kBandCount; ++i)
        CHECK(state.current_gains[i] == 0.0f);
}

void EqState_ConsumePendingReturnsTrueOnceThenFalse() {
    eq::EqState state;
    std::array<float, eq::kBandCount> gains{};
    gains[0] = 1.0f;
    state.SetGains(gains);

    std::array<float, eq::kBandCount> out{};
    bool first = state.ConsumePending(out);
    CHECK(first == true);
    CHECK(out[0] == 1.0f);
    CHECK(state.pending_dirty.load() == false);

    // Second call with nothing new published should report "no update".
    std::array<float, eq::kBandCount> out2{};
    out2.fill(-99.0f);
    bool second = state.ConsumePending(out2);
    CHECK(second == false);
    // `out2` must be left untouched when there's nothing to consume.
    for (int i = 0; i < eq::kBandCount; ++i)
        CHECK(out2[i] == -99.0f);
}

void EqState_SecondSetGainsOverwritesFirstBeforeConsume() {
    eq::EqState state;
    std::array<float, eq::kBandCount> gainsA{};
    gainsA.fill(2.0f);
    std::array<float, eq::kBandCount> gainsB{};
    gainsB.fill(7.0f);

    state.SetGains(gainsA);
    state.SetGains(gainsB);  // overwrites before anyone consumed gainsA

    std::array<float, eq::kBandCount> out{};
    bool consumed = state.ConsumePending(out);
    CHECK(consumed == true);
    for (int i = 0; i < eq::kBandCount; ++i)
        CHECK(out[i] == 7.0f);  // only the latest write survives
}

}  // namespace

int main() {
    RUN_TEST(EqState_DefaultsAreSane);
    RUN_TEST(EqState_SetGainsMarksDirtyAndStoresPending);
    RUN_TEST(EqState_ConsumePendingReturnsTrueOnceThenFalse);
    RUN_TEST(EqState_SecondSetGainsOverwritesFirstBeforeConsume);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
