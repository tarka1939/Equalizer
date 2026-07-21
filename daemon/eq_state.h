#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace eq {

static constexpr int kBandCount = 10;
static constexpr float kDefaultCentreHz[kBandCount] = {
    31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f
};
static constexpr float kDefaultGainDb[kBandCount] = {
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f
};

/// Shared state between the IPC server (non-RT) and the audio processing
/// callback (RT thread).  Gain changes are published atomically so the RT
/// thread never blocks.
struct EqState {
    // Gains published atomically to the RT thread.
    // We use a simple double-buffer: the non-RT side writes to the pending
    // array and then flips pending_dirty; the RT thread reads it on the next
    // callback and applies via DSP::Equalizer10Band::SetBandsPeaking().
    std::array<float, kBandCount> pending_gains{};   // written by non-RT
    std::atomic<bool>             pending_dirty{false};
    std::atomic<float>            preamp_db{0.f};
    std::atomic<bool>             enabled{true};

    // For IPC state queries (readable from non-RT only).
    std::array<float, kBandCount> current_gains{};
    float                         sample_rate{48000.f};
    uint32_t                      channels{2};

    EqState() {
        pending_gains.fill(0.f);
        current_gains.fill(0.f);
    }

    /// Non-RT: publish new gains.  Called from IPC handler thread.
    void SetGains(const std::array<float, kBandCount>& g) {
        pending_gains = g;
        pending_dirty.store(true, std::memory_order_release);
    }

    /// RT: consume pending gains if dirty.  Returns true if gains were updated.
    bool ConsumePending(std::array<float, kBandCount>& out) {
        if (!pending_dirty.load(std::memory_order_acquire))
            return false;
        pending_dirty.store(false, std::memory_order_release);
        out = pending_gains;
        return true;
    }

    // ── FIR impulse response (room-correction / linear-phase filter) ──────
    // Published atomically to the RT thread the same way pending_gains /
    // current_gains are above. A fixed-capacity array, not a std::vector:
    // a resizable buffer written from a non-RT thread while the RT thread
    // concurrently reads it would risk a torn read racing a reallocation
    // (undefined behavior, not just "wrong values" the way a torn read of
    // a POD array is). kMaxFirTaps is a deliberate cap -- ipc_server.cpp's
    // set_fir handler rejects anything longer before it ever reaches here.
    static constexpr uint32_t kMaxFirTaps = 4096;

    std::array<float, kMaxFirTaps> pending_ir{};
    uint32_t                       pending_ir_length{0};   // 0 == "no FIR configured"
    std::atomic<bool>              pending_ir_dirty{false};

    // For IPC state queries (readable from non-RT only) -- same
    // non-synchronizing caveat as current_gains above: EqState itself does
    // not keep this in sync with pending_ir_length: ipc_server.cpp updates
    // it explicitly after a successful set_fir/clear_fir.
    uint32_t current_fir_length{0};

    /// Non-RT: publish a new FIR impulse response (length == 0 clears it).
    /// Truncates to kMaxFirTaps if length is larger -- callers are expected
    /// to validate and reject oversized requests themselves (see
    /// ipc_server.cpp's set_fir handler); this is a defense-in-depth
    /// backstop, not the primary validation path.
    void SetImpulseResponse(const float* taps, uint32_t length) {
        const uint32_t clamped = length > kMaxFirTaps ? kMaxFirTaps : length;
        if (clamped > 0 && taps != nullptr)
            std::copy(taps, taps + clamped, pending_ir.begin());
        pending_ir_length = clamped;
        pending_ir_dirty.store(true, std::memory_order_release);
    }

    /// Non-RT: explicitly clear FIR (equivalent to SetImpulseResponse(nullptr, 0)).
    void ClearImpulseResponse() {
        SetImpulseResponse(nullptr, 0);
    }

    /// RT: consume a pending impulse response if dirty. Returns true if
    /// something was published since the last consume; `outLength == 0`
    /// means "clear FIR" and the RT thread should deactivate it rather
    /// than reading `out`'s contents.
    bool ConsumePendingIr(std::array<float, kMaxFirTaps>& out, uint32_t& outLength) {
        if (!pending_ir_dirty.load(std::memory_order_acquire))
            return false;
        pending_ir_dirty.store(false, std::memory_order_release);
        out = pending_ir;
        outLength = pending_ir_length;
        return true;
    }
};

} // namespace eq
