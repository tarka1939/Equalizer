#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace eq {

static constexpr int kBandCount = 10;
static constexpr float kDefaultCentreHz[kBandCount] = {
    31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f
};
static constexpr float kDefaultGainDb[kBandCount] = {
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f
};

/// Shared state between the IPC server and the audio backend.
///
/// Threading model (changed -- read this before adding a caller):
///
///   - `enabled` and `preamp_db` are plain atomics read directly by the RT
///     audio callback every block. Those are the ONLY two members the RT
///     thread touches.
///
///   - Everything else (`pending_gains`, `pending_ir`, the `current_*`
///     mirrors) is handed from the IPC thread to a non-RT *control* thread
///     owned by the audio backend (see daemon/pipewire_backend.cpp). The
///     control thread is what calls DSP::EqPipeline::SetBandsPeaking() /
///     SetImpulseResponse(), both of which run transcendental math and a
///     full FFT and are documented non-RT. The RT callback used to call
///     ConsumePending()/ConsumePendingIr() and then those setters itself,
///     which meant a heap allocation and an FFT inside the audio callback on
///     every set_fir; that is what this split exists to prevent.
///
///     Because both ends of that handoff are now non-RT threads, it is
///     guarded by an ordinary mutex rather than the previous
///     dirty-flag-plus-unsynchronised-array scheme. That earlier scheme was
///     described as a "double buffer" but was a single buffer: the writer
///     could overwrite `pending_gains`/`pending_ir` while the reader was
///     mid-copy, which is a data race (undefined behaviour), not merely a
///     stale read. `ConsumePending*()`'s once-then-false semantics are
///     unchanged.
///
///   - `NotifyUpdate()` / `WaitForUpdate()` let the control thread block
///     until there is something to do instead of polling. Purely a wake-up
///     channel; the data handoff is still the pending_*/dirty pair, so
///     ConsumePending*() work identically whether or not anyone is waiting.
struct EqState {
    // Gains handed to the control thread.
    std::array<float, kBandCount> pending_gains{};   // guarded by `mutex`
    std::atomic<bool>             pending_dirty{false};
    std::atomic<float>            preamp_db{0.f};
    std::atomic<bool>             enabled{true};

    // For IPC state queries. current_gains is IPC-thread-only; the format
    // fields are written by the audio backend's format-change callback and
    // read by get_state on the IPC thread, so they are atomic.
    std::array<float, kBandCount> current_gains{};
    std::atomic<float>            sample_rate{48000.f};
    std::atomic<uint32_t>         channels{2};

    EqState() {
        pending_gains.fill(0.f);
        current_gains.fill(0.f);
    }

    /// Publish new gains. Called from the IPC handler thread.
    void SetGains(const std::array<float, kBandCount>& g) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_gains = g;
        }
        pending_dirty.store(true, std::memory_order_release);
        NotifyUpdate();
    }

    /// Consume pending gains if dirty. Returns true if gains were updated.
    /// Called from the backend's control thread (non-RT).
    bool ConsumePending(std::array<float, kBandCount>& out) {
        if (!pending_dirty.load(std::memory_order_acquire))
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Clear the flag while holding the lock, so a SetGains() that
            // lands between the copy and the clear is not swallowed: it
            // blocks on the mutex, then sets the flag again afterwards.
            pending_dirty.store(false, std::memory_order_release);
            out = pending_gains;
        }
        return true;
    }

    // ── FIR impulse response (room-correction / linear-phase filter) ──────
    // Handed to the control thread the same way pending_gains is above. A
    // fixed-capacity array rather than a std::vector so the buffer's address
    // and size never change once EqState exists. kMaxFirTaps is a deliberate
    // cap -- ipc_server.cpp's set_fir handler rejects anything longer before
    // it ever reaches here.
    static constexpr uint32_t kMaxFirTaps = 4096;

    std::array<float, kMaxFirTaps> pending_ir{};                // guarded by `mutex`
    uint32_t                       pending_ir_length{0};        // guarded by `mutex`
    std::atomic<bool>              pending_ir_dirty{false};

    // For IPC state queries (IPC thread only) -- EqState does not keep this
    // in sync with pending_ir_length; ipc_server.cpp updates it explicitly
    // after a successful set_fir/clear_fir.
    uint32_t current_fir_length{0};

    /// Publish a new FIR impulse response (length == 0 clears it).
    /// Truncates to kMaxFirTaps if length is larger -- callers are expected
    /// to validate and reject oversized requests themselves (see
    /// ipc_server.cpp's set_fir handler); this is a defense-in-depth
    /// backstop, not the primary validation path.
    void SetImpulseResponse(const float* taps, uint32_t length) {
        const uint32_t clamped = length > kMaxFirTaps ? kMaxFirTaps : length;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (clamped > 0 && taps != nullptr)
                std::copy(taps, taps + clamped, pending_ir.begin());
            pending_ir_length = clamped;
        }
        pending_ir_dirty.store(true, std::memory_order_release);
        NotifyUpdate();
    }

    /// Explicitly clear FIR (equivalent to SetImpulseResponse(nullptr, 0)).
    void ClearImpulseResponse() {
        SetImpulseResponse(nullptr, 0);
    }

    /// Consume a pending impulse response if dirty. Returns true if
    /// something was published since the last consume; `outLength == 0`
    /// means "clear FIR" and the caller should deactivate it rather than
    /// reading `out`'s contents. Called from the control thread (non-RT).
    bool ConsumePendingIr(std::array<float, kMaxFirTaps>& out, uint32_t& outLength) {
        if (!pending_ir_dirty.load(std::memory_order_acquire))
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_ir_dirty.store(false, std::memory_order_release);
            out = pending_ir;
            outLength = pending_ir_length;
        }
        return true;
    }

    // ── Control-thread wake-up channel ────────────────────────────────────

    /// Bump the update sequence and wake anyone in WaitForUpdate(). Called
    /// automatically by SetGains()/SetImpulseResponse().
    void NotifyUpdate() {
        {
            std::lock_guard<std::mutex> lock(notify_mutex);
            ++notify_seq;
        }
        notify_cv.notify_all();
    }

    /// Block until NotifyUpdate() is called or `timeoutMs` elapses. Returns
    /// immediately if an update arrived since the caller's last wait. Never
    /// call this from the RT thread.
    void WaitForUpdate(int timeoutMs) {
        std::unique_lock<std::mutex> lock(notify_mutex);
        const uint64_t seen = last_seen_seq;
        notify_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [&] { return notify_seq != seen; });
        last_seen_seq = notify_seq;
    }

    /// Wake any waiter unconditionally (used on shutdown so the control
    /// thread's wait returns promptly instead of running out its timeout).
    void WakeWaiters() { NotifyUpdate(); }

private:
    std::mutex              mutex;         // guards pending_gains / pending_ir(_length)
    std::mutex              notify_mutex;  // guards notify_seq / last_seen_seq
    std::condition_variable notify_cv;
    uint64_t                notify_seq{0};
    uint64_t                last_seen_seq{0};
};

} // namespace eq
