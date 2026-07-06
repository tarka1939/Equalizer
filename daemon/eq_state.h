#pragma once
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
};

} // namespace eq
