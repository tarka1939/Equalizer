#pragma once
/*
 * pipewire_backend.h  —  PipeWire audio backend for eq-daemon (Linux)
 *
 * Registers a "sink" filter node in the PipeWire graph.
 * All audio that is routed through the node is processed by the
 * DSP::Equalizer10Band chain and forwarded (passthrough) to the
 * default hardware sink.
 */
#include "audio_backend.h"

// forward-declare PipeWire types so the header stays lean
struct pw_main_loop;
struct pw_filter;
struct pw_stream;

namespace eq {

class PipeWireBackend final : public AudioBackend {
public:
    explicit PipeWireBackend(EqState* state);
    ~PipeWireBackend() override;

    bool       Open()  override;
    void       Close() override;
    const char* Name() const override { return "PipeWire"; }

private:
    // PipeWire opaque handles
    pw_main_loop* m_loop   = nullptr;
    pw_filter*    m_filter = nullptr;

    // Audio ports (stereo in + out)
    void* m_in_L  = nullptr;
    void* m_in_R  = nullptr;
    void* m_out_L = nullptr;
    void* m_out_R = nullptr;

    // RT-side EQ (only touched from the RT callback)
    struct EqCore;
    EqCore* m_eq = nullptr;

    // PipeWire event callbacks (static -> instance dispatch)
    static void OnProcess(void* userdata);
    static void OnParamChanged(void* userdata, uint32_t id,
                               const struct spa_pod* param);
};

} // namespace eq
