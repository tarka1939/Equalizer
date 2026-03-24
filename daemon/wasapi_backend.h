/*
 * wasapi_backend.h / wasapi_backend.cpp
 *
 * Windows WASAPI loopback + render backend (stub for future implementation).
 *
 * Strategy:
 *  - Open the default render endpoint in WASAPI shared mode.
 *  - Use WASAPI loopback capture to read what the system is playing.
 *  - Apply DSP, then re-render to a virtual audio device or use the
 *    APO pathway (Driver/ project).
 *
 * For now the Windows path still uses the APO DLL (Driver/Equalizer.dll).
 * This WASAPI daemon mode is stubbed for future use.
 */
#pragma once
#ifdef BACKEND_WASAPI
#include "audio_backend.h"

namespace eq {

class WasapiBackend final : public AudioBackend {
public:
    explicit WasapiBackend(EqState* state) : AudioBackend(state) {}
    ~WasapiBackend() override { Close(); }

    bool Open() override {
        // TODO: Implement WASAPI loopback capture + render pipeline.
        // For now, Windows users should use the APO DLL (Driver/ project).
        fprintf(stderr, "[WASAPI] Daemon backend not yet implemented. "
                        "Use the Driver/Equalizer.dll APO instead.\n");
        return false;
    }

    void Close() override {}
    const char* Name() const override { return "WASAPI (stub)"; }
};

inline std::unique_ptr<AudioBackend> CreateAudioBackend(EqState* state) {
    return std::make_unique<WasapiBackend>(state);
}

} // namespace eq
#endif // BACKEND_WASAPI
