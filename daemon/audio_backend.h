#pragma once
#include "eq_state.h"
#include <memory>

namespace eq {

/// Abstract interface for a platform audio backend.
///
/// Lifecycle:
///   1. Construct with a shared EqState*.
///   2. Call Open() once — sets up audio graph / hook.
///   3. The backend's RT callback reads EqState and applies DSP.
///   4. Call Close() before destruction.
class AudioBackend {
public:
    explicit AudioBackend(EqState* state) : m_state(state) {}
    virtual ~AudioBackend() = default;

    /// Open the audio backend and start processing.
    /// Returns true on success.
    virtual bool Open() = 0;

    /// Stop processing and release audio resources.
    virtual void Close() = 0;

    /// Human-readable name of the backend (e.g. "PipeWire", "WASAPI").
    virtual const char* Name() const = 0;

protected:
    EqState* m_state;  ///< Non-owning pointer — lifetime managed by main().
};

/// Factory function implemented in each platform-specific .cpp.
std::unique_ptr<AudioBackend> CreateAudioBackend(EqState* state);

} // namespace eq
