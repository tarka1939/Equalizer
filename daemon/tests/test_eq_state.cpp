/*
 * test_eq_state.cpp — Unit tests for eq::EqState, the handoff structure
 * shared between the IPC thread and the audio backend's control thread.
 *
 * Note the threading model changed: EqState used to be described as a
 * lock-free RT<->non-RT structure, but the RT thread no longer consumes from
 * it at all (it only reads the `enabled`/`preamp_db` atomics). Gains and
 * impulse responses are consumed by a non-RT control thread, because applying
 * them runs transcendental math and a full FFT. Both ends being non-RT is why
 * the pending arrays can now be mutex-guarded -- which they need to be: the
 * previous "double buffer" was a single buffer, and a writer could overwrite
 * it mid-copy. The ConsumePending*() semantics these tests pin are unchanged.
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

// ── FIR impulse response state ──────────────────────────────────────────

void EqState_FirDefaultsAreSane() {
    eq::EqState state;
    CHECK(state.pending_ir_length == 0u);
    CHECK(state.pending_ir_dirty.load() == false);
    CHECK(state.current_fir_length == 0u);
    for (uint32_t i = 0; i < eq::EqState::kMaxFirTaps; ++i)
        CHECK(state.pending_ir[i] == 0.0f);
}

void EqState_SetImpulseResponseMarksDirtyAndStoresPending() {
    eq::EqState state;
    std::array<float, 4> taps{ 0.5f, 0.25f, -0.1f, 0.05f };

    state.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size()));

    CHECK(state.pending_ir_dirty.load() == true);
    CHECK(state.pending_ir_length == 4u);
    for (size_t i = 0; i < taps.size(); ++i)
        CHECK(state.pending_ir[i] == taps[i]);

    // SetImpulseResponse must NOT touch current_fir_length -- same seam
    // as SetGains()/current_gains (the IPC layer updates it separately).
    CHECK(state.current_fir_length == 0u);
}

void EqState_ConsumePendingIrReturnsTrueOnceThenFalse() {
    eq::EqState state;
    std::array<float, 2> taps{ 0.9f, -0.3f };
    state.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size()));

    std::array<float, eq::EqState::kMaxFirTaps> out{};
    uint32_t outLength = 0;
    bool first = state.ConsumePendingIr(out, outLength);
    CHECK(first == true);
    CHECK(outLength == 2u);
    CHECK(out[0] == 0.9f);
    CHECK(out[1] == -0.3f);
    CHECK(state.pending_ir_dirty.load() == false);

    std::array<float, eq::EqState::kMaxFirTaps> out2{};
    out2.fill(-99.0f);
    uint32_t outLength2 = 12345;
    bool second = state.ConsumePendingIr(out2, outLength2);
    CHECK(second == false);
    // Nothing new published -- out/outLength must be left untouched.
    CHECK(outLength2 == 12345u);
    CHECK(out2[0] == -99.0f);
}

void EqState_ClearImpulseResponsePublishesZeroLength() {
    eq::EqState state;
    std::array<float, 3> taps{ 1.0f, 2.0f, 3.0f };
    state.SetImpulseResponse(taps.data(), static_cast<uint32_t>(taps.size()));

    std::array<float, eq::EqState::kMaxFirTaps> drained{};
    uint32_t drainedLength = 0;
    CHECK(state.ConsumePendingIr(drained, drainedLength) == true);
    CHECK(drainedLength == 3u);

    state.ClearImpulseResponse();
    CHECK(state.pending_ir_dirty.load() == true);
    CHECK(state.pending_ir_length == 0u);

    std::array<float, eq::EqState::kMaxFirTaps> out{};
    uint32_t outLength = 999;
    CHECK(state.ConsumePendingIr(out, outLength) == true);
    CHECK(outLength == 0u);  // 0 == "clear FIR", per ConsumePendingIr's contract
}

void EqState_SetImpulseResponseTruncatesOversizedInput() {
    eq::EqState state;
    std::vector<float> tooLong(eq::EqState::kMaxFirTaps + 10, 0.42f);

    state.SetImpulseResponse(tooLong.data(), static_cast<uint32_t>(tooLong.size()));

    // Defense-in-depth clamp (see eq_state.h's comment) -- the IPC layer
    // is expected to reject this before it gets here, but EqState itself
    // must not overrun pending_ir's fixed capacity either way.
    CHECK(state.pending_ir_length == eq::EqState::kMaxFirTaps);
}

}  // namespace

int main() {
    RUN_TEST(EqState_DefaultsAreSane);
    RUN_TEST(EqState_SetGainsMarksDirtyAndStoresPending);
    RUN_TEST(EqState_ConsumePendingReturnsTrueOnceThenFalse);
    RUN_TEST(EqState_SecondSetGainsOverwritesFirstBeforeConsume);
    RUN_TEST(EqState_FirDefaultsAreSane);
    RUN_TEST(EqState_SetImpulseResponseMarksDirtyAndStoresPending);
    RUN_TEST(EqState_ConsumePendingIrReturnsTrueOnceThenFalse);
    RUN_TEST(EqState_ClearImpulseResponsePublishesZeroLength);
    RUN_TEST(EqState_SetImpulseResponseTruncatesOversizedInput);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
