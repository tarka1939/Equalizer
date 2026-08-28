#pragma once
/*
 * pipewire_backend.h  —  PipeWire audio backend for eq-daemon (Linux)
 *
 * Registers a "filter" node in the PipeWire graph. All audio routed through
 * the node is processed by the DSP::EqPipeline chain (FIR then IIR) and
 * forwarded to the default hardware sink.
 *
 * Threading (three threads, and the split matters):
 *
 *   RT thread      — OnProcess(). Does interleave / preamp / EqPipeline::Process
 *                    / clamp / deinterleave and nothing else. Never allocates,
 *                    never locks, never calls a DSP configuration method.
 *
 *   Control thread — ControlLoop(). Owns every non-RT DSP call:
 *                    EqPipeline::Prepare / SetBandsPeaking / SetImpulseResponse.
 *                    Wakes on EqState::WaitForUpdate(). This thread exists
 *                    because those calls run transcendental math and a full
 *                    FFT; OnProcess() used to make them inline, which meant a
 *                    heap allocation and an FFT inside the audio callback on
 *                    every set_fir command.
 *
 *   PipeWire main  — Open()/Close()/OnParamChanged(). OnParamChanged only
 *                    records the new format and hands the actual re-Prepare()
 *                    to the control thread, so reconfiguration always happens
 *                    on exactly one thread.
 *
 * Prepare() reallocates the buffers Process() reads, so it is the one
 * configuration call that cannot simply run concurrently with the RT thread.
 * BeginReconfigure()/EndReconfigure() fence it off; see the .cpp for the
 * handshake and why it is correct.
 *
 * !! UNVERIFIED AGAINST A REAL BUILD !!
 *
 * No machine that has touched this repo so far has had libpipewire-0.3 dev
 * headers, so this backend has never been compiled, let alone run. Reviewing
 * it against the PipeWire API turned up two things that mean it cannot have
 * been:
 *
 *   - Both event callbacks had the wrong signatures. pw_filter_events::process
 *     is `void (*)(void *data, struct spa_io_position *position)` and
 *     ::param_changed is `void (*)(void *data, void *port_data, uint32_t id,
 *     const struct spa_pod *param)`; they were declared with one and three
 *     parameters respectively. Assigning those into the events struct is a
 *     function-pointer type mismatch, i.e. a compile error.
 *
 *   - The RT callback mixed two different buffer APIs: it called
 *     pw_filter_get_dsp_buffer() (which returns the sample array for a
 *     MAP_BUFFERS DSP port) and cast the result to `struct pw_buffer *`, then
 *     dereferenced `->buffer->datas[0]`. That is the shape of the
 *     pw_filter_dequeue_buffer() API, not this one.
 *
 * Both are corrected here from the documented API, but corrected *by reading*,
 * not by building. Treat the PipeWire-facing parts of this file as unproven
 * until someone compiles and runs it on a Linux host with
 * `libpipewire-0.3-dev` installed. The threading structure above is
 * independent of that and stands on its own.
 */
#include "audio_backend.h"

#include <atomic>
#include <cstdint>
#include <thread>

// forward-declare PipeWire types so the header stays lean
struct pw_main_loop;
struct pw_filter;
struct pw_stream;
struct spa_io_position;

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

    // DSP state. Configured by the control thread, read by the RT thread.
    struct EqCore;
    EqCore* m_eq = nullptr;

    // ── Control thread ────────────────────────────────────────────────────
    std::thread       m_controlThread;
    std::atomic<bool> m_controlRunning{false};

    void ControlLoop();
    void StartControlThread();
    void StopControlThread();

    // ── RT <-> reconfiguration handshake ──────────────────────────────────
    // m_reconfigPending: a non-RT thread wants exclusive access to the DSP
    //                    objects (it is about to reallocate them).
    // m_rtInProcess:     the RT thread is inside the DSP section of OnProcess.
    // Both are seq_cst; see BeginReconfigure() for why that ordering is what
    // makes the handshake correct rather than merely likely to work.
    std::atomic<bool> m_reconfigPending{false};
    std::atomic<bool> m_rtInProcess{false};

    void BeginReconfigure();
    void EndReconfigure();

    // ── Pending format change, published by OnParamChanged ────────────────
    std::atomic<bool>     m_formatChanged{false};
    std::atomic<uint32_t> m_pendingRate{48000};

    // PipeWire event callbacks (static -> instance dispatch).
    // Signatures must match struct pw_filter_events exactly; see the warning
    // at the top of this header.
    static void OnProcess(void* userdata, struct spa_io_position* position);
    static void OnParamChanged(void* userdata, void* port_data, uint32_t id,
                               const struct spa_pod* param);
};

} // namespace eq
