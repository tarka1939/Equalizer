/*
 * pipewire_backend.cpp  —  PipeWire audio backend (Linux)
 *
 * Implements a PipeWire "filter" node that:
 *   1. Receives PCM audio from the graph (any application's output).
 *   2. Applies the FIR + 10-band biquad EQ chain (DSP::EqPipeline).
 *   3. Passes the processed audio back out to the hardware sink.
 *
 * See pipewire_backend.h for the three-thread split; the short version is
 * that OnProcess() does audio and nothing else, and ControlLoop() owns every
 * DSP call that allocates or runs an FFT.
 *
 * Build requirements:
 *   pkg-config --cflags --libs libpipewire-0.3
 */
#ifdef BACKEND_PIPEWIRE

#include "pipewire_backend.h"
#include "eq_state.h"
#include "EqPipeline.h"  // from DSP/

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace eq {

// FIR engine's internal analysis block size (see DSP::OverlapAdd::Prepare) --
// independent of PipeWire's own callback block size, which OverlapAdd absorbs
// regardless (DSP/OverlapAdd.h). maxImpulseLength is deliberately the *same*
// constant as eq::EqState::kMaxFirTaps (daemon/eq_state.h), not a separately
// chosen number that happens to match: that is the largest impulse response
// the IPC layer will ever hand this engine (see ipc_server.cpp's set_fir
// handler), so reusing the same symbol keeps the two in sync by construction
// rather than by two magic numbers staying accidentally equal.
static constexpr uint32_t kFirBlockSize = 256;
static constexpr uint32_t kFirMaxImpulseLength = EqState::kMaxFirTaps;

// This filter exposes exactly two mono DSP ports in each direction (FL/FR),
// so the interleaved buffer it builds is always stereo. Previously Prepare()
// was called with the channel count parsed out of the negotiated format while
// Process() was hardcoded to 2: whenever those disagreed,
// OverlapAdd::Process()'s `channels != m_channels` guard fired and the FIR
// stage silently did nothing at all. One constant now drives both, and
// OnParamChanged only takes the sample rate from the negotiated format.
static constexpr uint32_t kFilterChannels = 2;

// Upper bound on frames per callback that the interleave buffer is sized for.
// The buffer is allocated once (non-RT) at this size and the RT path clamps
// to it, replacing a fixed `static float interleaved[8192*2]` that was
// indexed with an unchecked frame count straight out of the buffer's chunk
// size -- i.e. an out-of-bounds write for any larger quantum.
static constexpr uint32_t kMaxFramesPerCallback = 16384;

// ── Internal DSP state ───────────────────────────────────────────────────────
struct PipeWireBackend::EqCore {
    DSP::EqPipeline eq;
    std::array<float, kBandCount> gains{};
    float sample_rate = 48000.f;
    uint32_t channels = kFilterChannels;

    // Interleave scratch, allocated once by the control thread.
    std::vector<float> interleaved;

    // Last impulse response the control thread installed. EqPipeline::Prepare()
    // resets FIR to inactive (OverlapAdd has to reallocate its FFT buffers, so
    // the installed taps cannot survive), and the IIR curve does survive -- so
    // after a format change only the FIR needs replaying, and this is where it
    // is kept to replay it from.
    std::vector<float> lastIr;
};

// ── Port descriptors ──────────────────────────────────────────────────────────
static const struct spa_audio_info_raw s_audioInfo = {
    .format   = SPA_AUDIO_FORMAT_F32P,   // planar float
    .rate     = 48000,
    .channels = kFilterChannels,
};

// ── Constructor / Destructor ──────────────────────────────────────────────────
PipeWireBackend::PipeWireBackend(EqState* state)
    : AudioBackend(state), m_eq(new EqCore{})
{}

PipeWireBackend::~PipeWireBackend() {
    // Close() only asks the main loop to quit; Open() is what tears the
    // PipeWire objects down, after pw_main_loop_run() has actually returned.
    // If Open() was never called (or failed before run()), clean up here.
    StopControlThread();
    if (m_filter) { pw_filter_destroy(m_filter); m_filter = nullptr; }
    if (m_loop)   { pw_main_loop_destroy(m_loop); m_loop = nullptr; }
    delete m_eq;
}

// ── Reconfiguration handshake ────────────────────────────────────────────────
//
// EqPipeline::Prepare() frees and reallocates every buffer OnProcess() reads.
// Running it concurrently with the RT callback is a use-after-free, so the RT
// thread has to be fenced off first -- without ever blocking it.
//
// Correctness (Dekker's algorithm): all four operations below are seq_cst, so
// they take a single total order. The RT thread stores m_rtInProcess=true and
// then loads m_reconfigPending; this thread stores m_reconfigPending=true and
// then loads m_rtInProcess. In any interleaving at least one of the two loads
// must observe the other's store -- both loads returning "clear" would require
// both stores to come after both loads in the total order, which is
// impossible. So either the RT thread sees the pending flag and bails out to
// passthrough for that block, or this thread sees rtInProcess and waits. Both
// happening at once is fine (the RT thread bails, then clears rtInProcess, and
// the wait below ends). Only the non-RT side ever spins.
void PipeWireBackend::BeginReconfigure() {
    m_reconfigPending.store(true);
    while (m_rtInProcess.load())
        std::this_thread::yield();
}

void PipeWireBackend::EndReconfigure() {
    m_reconfigPending.store(false);
}

// ── Control thread ───────────────────────────────────────────────────────────
void PipeWireBackend::StartControlThread() {
    if (m_controlRunning.exchange(true))
        return;
    m_controlThread = std::thread(&PipeWireBackend::ControlLoop, this);
}

void PipeWireBackend::StopControlThread() {
    m_controlRunning.store(false);
    // Wake it unconditionally: Close() may already have cleared the flag
    // without joining, in which case the thread is sitting in WaitForUpdate()
    // and would otherwise take up to its full timeout to notice.
    if (m_state) m_state->WakeWaiters();
    if (m_controlThread.joinable()) m_controlThread.join();
}

void PipeWireBackend::ControlLoop() {
    // Everything in this loop is non-RT: it may allocate, lock, and run FFTs.
    std::array<float, EqState::kMaxFirTaps> irBuf{};

    while (m_controlRunning.load()) {
        // 1. Format change (published by OnParamChanged). Handled here rather
        //    than in the callback itself so that all reconfiguration -- the
        //    one thing that must not race the RT thread -- happens on a
        //    single thread and behind a single handshake.
        if (m_formatChanged.exchange(false)) {
            const float sr = static_cast<float>(m_pendingRate.load());

            BeginReconfigure();
            const bool ok = m_eq->eq.Prepare(sr, kFilterChannels,
                                             kFirBlockSize, kFirMaxImpulseLength);
            if (ok) {
                m_eq->interleaved.assign(
                    static_cast<size_t>(kMaxFramesPerCallback) * kFilterChannels, 0.0f);

                // Prepare() preserves the IIR curve but drops the FIR (see
                // EqPipeline::Prepare's contract), so replay the taps. Done
                // inside the fence: doing it after EndReconfigure() would
                // leave a window where the RT thread runs IIR-only audio
                // despite a FIR being configured. SetImpulseResponse() does
                // not allocate (OverlapAdd::Prepare preallocates its padding
                // scratch), so this doesn't lengthen the fence meaningfully.
                if (!m_eq->lastIr.empty()) {
                    m_eq->eq.SetImpulseResponse(
                        m_eq->lastIr.data(), static_cast<uint32_t>(m_eq->lastIr.size()));
                }
            }
            EndReconfigure();

            if (!ok) {
                std::cerr << "[PipeWire] EqPipeline::Prepare failed for "
                          << sr << " Hz -- audio will pass through unfiltered\n";
            } else {
                m_eq->sample_rate = sr;
                m_eq->channels    = kFilterChannels;
                std::cout << "[PipeWire] Format applied: " << sr << " Hz, "
                          << kFilterChannels << "ch\n";
            }
        }

        // 2. Band gains. SetBandsPeaking() publishes through Biquad's atomic
        //    coefficient swap, so it is safe to run while OnProcess() is
        //    mid-block -- no handshake needed, unlike Prepare() above.
        std::array<float, kBandCount> newGains{};
        if (m_state->ConsumePending(newGains)) {
            std::array<float, kBandCount> centres{};
            for (int i = 0; i < kBandCount; ++i)
                centres[i] = kDefaultCentreHz[i];
            m_eq->eq.SetBandsPeaking(centres, newGains, 1.0f);
            m_eq->gains = newGains;
        }

        // 3. FIR impulse response. Same story: OverlapAdd publishes the new
        //    spectrum through its own atomic slot swap. This is the call that
        //    used to run inside the audio callback, FFT and all.
        uint32_t irLen = 0;
        if (m_state->ConsumePendingIr(irBuf, irLen)) {
            if (irLen > 0) {
                if (m_eq->eq.SetImpulseResponse(irBuf.data(), irLen)) {
                    m_eq->lastIr.assign(irBuf.begin(), irBuf.begin() + irLen);
                } else {
                    std::cerr << "[PipeWire] Rejected FIR impulse response ("
                              << irLen << " taps)\n";
                }
            } else {
                m_eq->eq.ClearImpulseResponse();
                m_eq->lastIr.clear();
            }
        }

        // Block until the IPC thread publishes something (or we're stopping).
        // The timeout is a backstop, not the normal path.
        m_state->WaitForUpdate(200);
    }
}

// ── Open ──────────────────────────────────────────────────────────────────────
bool PipeWireBackend::Open() {
    pw_init(nullptr, nullptr);

    m_loop = pw_main_loop_new(nullptr);
    if (!m_loop) {
        std::cerr << "[PipeWire] pw_main_loop_new failed\n";
        pw_deinit();
        return false;
    }

    // Filter events
    static const pw_filter_events filter_events = {
        .version    = PW_VERSION_FILTER_EVENTS,
        .process    = OnProcess,
        .param_changed = OnParamChanged,
    };

    m_filter = pw_filter_new_simple(
        pw_main_loop_get_loop(m_loop),
        "eq-daemon",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE,     "DSP",
            nullptr),
        &filter_events,
        this);

    if (!m_filter) {
        std::cerr << "[PipeWire] pw_filter_new_simple failed\n";
        pw_main_loop_destroy(m_loop);
        m_loop = nullptr;
        pw_deinit();
        return false;
    }

    // Add stereo ports
    auto make_port = [&](const char* name, uint32_t channel, bool is_output) -> void* {
        (void)channel;
        return pw_filter_add_port(
            m_filter,
            is_output ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            0, // port data size (no extra per-port data)
            pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_AUDIO_CHANNEL, name,
                nullptr),
            nullptr, 0);
    };

    m_in_L  = make_port("FL", 0, false);
    m_in_R  = make_port("FR", 1, false);
    m_out_L = make_port("FL", 0, true);
    m_out_R = make_port("FR", 1, true);

    if (!m_in_L || !m_in_R || !m_out_L || !m_out_R) {
        std::cerr << "[PipeWire] Failed to add audio ports\n";
        pw_filter_destroy(m_filter);
        m_filter = nullptr;
        pw_main_loop_destroy(m_loop);
        m_loop = nullptr;
        pw_deinit();
        return false;
    }

    // Prepare the DSP chain (default flat curve, FIR inactive) before any
    // callback can fire. This is on the caller's thread, pre-run(), so the
    // reconfiguration handshake isn't needed yet.
    if (!m_eq->eq.Prepare(48000.f, kFilterChannels, kFirBlockSize, kFirMaxImpulseLength)) {
        std::cerr << "[PipeWire] EqPipeline::Prepare failed\n";
        pw_filter_destroy(m_filter);
        m_filter = nullptr;
        pw_main_loop_destroy(m_loop);
        m_loop = nullptr;
        pw_deinit();
        return false;
    }
    m_eq->interleaved.assign(
        static_cast<size_t>(kMaxFramesPerCallback) * kFilterChannels, 0.0f);
    m_eq->gains.fill(0.f);
    m_eq->sample_rate = 48000.f;
    m_eq->channels    = kFilterChannels;

    StartControlThread();

    // Connect and run
    uint8_t          buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &s_audioInfo);

    if (pw_filter_connect(m_filter, PW_FILTER_FLAG_RT_PROCESS, params, 1) < 0) {
        std::cerr << "[PipeWire] pw_filter_connect failed\n";
        StopControlThread();
        pw_filter_destroy(m_filter);
        m_filter = nullptr;
        pw_main_loop_destroy(m_loop);
        m_loop = nullptr;
        pw_deinit();
        return false;
    }

    std::cout << "[PipeWire] Filter connected — running main loop\n";
    pw_main_loop_run(m_loop);  // blocks until Close() calls pw_main_loop_quit

    // Teardown happens *here*, on the thread that ran the loop, only after
    // run() has returned. Close() used to destroy the filter and loop
    // directly from the caller's thread while this call was still executing.
    StopControlThread();
    pw_filter_destroy(m_filter);
    m_filter = nullptr;
    pw_main_loop_destroy(m_loop);
    m_loop = nullptr;
    pw_deinit();
    return true;
}

void PipeWireBackend::Close() {
    // Ask the loop to return; do NOT destroy anything here. pw_main_loop_quit
    // is asynchronous, and Open() is still inside pw_main_loop_run() on
    // another thread (main.cpp runs Open() on a dedicated thread and calls
    // Close() from main). Destroying the filter/loop from here raced that.
    m_controlRunning.store(false);
    if (m_state) m_state->WakeWaiters();
    if (m_loop) pw_main_loop_quit(m_loop);
}

// ── RT callback ───────────────────────────────────────────────────────────────
//
// Signature note: pw_filter_events::process is
//     void (*process)(void *data, struct spa_io_position *position)
// and the frame count for the cycle comes from position->clock.duration.
// This used to be declared as `void OnProcess(void*)` and assigned straight
// into the events struct, which is a function-pointer type mismatch -- see the
// warning at the top of pipewire_backend.h.
//
// Buffer access note: for a MAP_BUFFERS DSP port,
//     void *pw_filter_get_dsp_buffer(void *port_data, uint32_t n_samples)
// returns the sample array itself. The previous code cast that return value to
// `struct pw_buffer *` and dereferenced `->buffer->datas[0]`, mixing it with
// the pw_filter_dequeue_buffer() API, which returns a different type entirely.
void PipeWireBackend::OnProcess(void* userdata, struct spa_io_position* position) {
    auto* self = static_cast<PipeWireBackend*>(userdata);
    EqCore* core = self->m_eq;
    EqState* state = self->m_state;

    if (!position) return;

    // Clamp to the capacity the control thread allocated for the interleave
    // buffer. The old code took an unchecked frame count and indexed a fixed
    // `static float interleaved[8192*2]` with it.
    uint64_t requested = position->clock.duration;
    if (requested == 0) return;
    const uint32_t frames = static_cast<uint32_t>(
        std::min<uint64_t>(requested, kMaxFramesPerCallback));

    auto* inL  = static_cast<const float*>(pw_filter_get_dsp_buffer(self->m_in_L,  frames));
    auto* inR  = static_cast<const float*>(pw_filter_get_dsp_buffer(self->m_in_R,  frames));
    auto* outL = static_cast<float*>(pw_filter_get_dsp_buffer(self->m_out_L, frames));
    auto* outR = static_cast<float*>(pw_filter_get_dsp_buffer(self->m_out_R, frames));

    if (!outL || !outR) return;

    const size_t outBytes = static_cast<size_t>(frames) * sizeof(float);

    // An input port with nothing connected yields no buffer. Emit silence
    // rather than dereferencing null (the old code returned early and left the
    // output buffer holding whatever was in it).
    if (!inL || !inR) {
        std::memset(outL, 0, outBytes);
        std::memset(outR, 0, outBytes);
        return;
    }

    if (!state->enabled.load(std::memory_order_relaxed)) {
        // Bypass — copy through.
        std::memcpy(outL, inL, outBytes);
        std::memcpy(outR, inR, outBytes);
        return;
    }

    // Announce we're entering the DSP section, then check whether a
    // reconfiguration is in flight. See BeginReconfigure() for why this pair
    // of seq_cst operations is what makes the handshake sound.
    self->m_rtInProcess.store(true);
    if (self->m_reconfigPending.load()) {
        // EqPipeline's buffers are being reallocated right now. Pass audio
        // through untouched for this block rather than reading freed memory.
        self->m_rtInProcess.store(false);
        std::memcpy(outL, inL, outBytes);
        std::memcpy(outR, inR, outBytes);
        return;
    }

    // Interleave → preamp → process → clamp → deinterleave.
    // (EqPipeline/Equalizer10Band/OverlapAdd all expect interleaved float.)
    float* interleaved = core->interleaved.data();
    for (uint32_t i = 0; i < frames; ++i) {
        interleaved[i * kFilterChannels]     = inL[i];
        interleaved[i * kFilterChannels + 1] = inR[i];
    }

    const float preampDb = state->preamp_db.load(std::memory_order_relaxed);
    const float preamp = std::pow(10.f, preampDb / 20.f);
    if (std::isfinite(preamp) && preamp != 1.0f) {
        for (uint32_t i = 0; i < frames * kFilterChannels; ++i)
            interleaved[i] *= preamp;
    }

    core->eq.Process(interleaved, interleaved, frames, kFilterChannels);

    for (uint32_t i = 0; i < frames; ++i) {
        auto clamp = [](float v) -> float {
            // Also folds NaN to silence: every comparison against a NaN is
            // false, so the bare min/max form below would pass it straight
            // through to the sound card.
            if (!(v == v)) return 0.f;
            return v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
        };
        outL[i] = clamp(interleaved[i * kFilterChannels]);
        outR[i] = clamp(interleaved[i * kFilterChannels + 1]);
    }

    // Released only after the last read of `interleaved`: the deinterleave
    // loop above still touches the buffer the control thread may reallocate.
    self->m_rtInProcess.store(false);
}

// Signature note: pw_filter_events::param_changed is
//     void (*param_changed)(void *data, void *port_data, uint32_t id,
//                           const struct spa_pod *param)
// -- four parameters. This was declared with three.
void PipeWireBackend::OnParamChanged(void* userdata, void* /*port_data*/, uint32_t id,
                                     const struct spa_pod* param) {
    if (!param || id != SPA_PARAM_Format) return;
    auto* self = static_cast<PipeWireBackend*>(userdata);

    struct spa_audio_info info{};
    if (spa_format_audio_raw_parse(param, &info.info.raw) != 0) return;

    const uint32_t rate = info.info.raw.rate;
    if (rate == 0) return;

    // Record the change and let the control thread do the actual re-Prepare().
    // Doing it inline here reallocated OverlapAdd's buffers from the PipeWire
    // main-loop thread with nothing stopping the RT callback from being inside
    // Process() at the same time.
    self->m_pendingRate.store(rate);
    self->m_formatChanged.store(true);

    // The channel count is deliberately not taken from the negotiated format:
    // this filter always exposes exactly two mono DSP ports (see
    // kFilterChannels), so 2 is the only value the RT path can ever produce.
    self->m_state->sample_rate.store(static_cast<float>(rate));
    self->m_state->channels.store(kFilterChannels);
    self->m_state->NotifyUpdate();

    std::cout << "[PipeWire] Format negotiated: " << rate << " Hz, "
              << kFilterChannels << "ch (applying on control thread)\n";
}

// ── Factory ───────────────────────────────────────────────────────────────────
std::unique_ptr<AudioBackend> CreateAudioBackend(EqState* state) {
    return std::make_unique<PipeWireBackend>(state);
}

} // namespace eq

#endif // BACKEND_PIPEWIRE
