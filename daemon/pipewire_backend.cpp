/*
 * pipewire_backend.cpp  —  PipeWire audio backend (Linux)
 *
 * Implements a PipeWire "filter" node that:
 *   1. Receives PCM audio from the graph (any application's output).
 *   2. Applies the 10-band biquad EQ.
 *   3. Passes the processed audio back out to the hardware sink.
 *
 * Build requirements:
 *   pkg-config --cflags --libs libpipewire-0.3
 */
#ifdef BACKEND_PIPEWIRE

#include "pipewire_backend.h"
#include "eq_state.h"
#include "Equalizer10Band.h"  // from DSP/

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace eq {

// ── Internal RT state ─────────────────────────────────────────────────────────
struct PipeWireBackend::EqCore {
    DSP::Equalizer10Band eq;
    std::array<float, kBandCount> gains{};
    float sample_rate = 48000.f;
    uint32_t channels = 2;
};

// ── Port descriptors ──────────────────────────────────────────────────────────
static const struct spa_audio_info_raw s_audioInfo = {
    .format   = SPA_AUDIO_FORMAT_F32P,   // planar float
    .rate     = 48000,
    .channels = 2,
};

// ── Constructor / Destructor ──────────────────────────────────────────────────
PipeWireBackend::PipeWireBackend(EqState* state)
    : AudioBackend(state), m_eq(new EqCore{})
{}

PipeWireBackend::~PipeWireBackend() {
    Close();
    delete m_eq;
}

// ── Open ──────────────────────────────────────────────────────────────────────
bool PipeWireBackend::Open() {
    pw_init(nullptr, nullptr);

    m_loop = pw_main_loop_new(nullptr);
    if (!m_loop) {
        std::cerr << "[PipeWire] pw_main_loop_new failed\n";
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
        return false;
    }

    // Add stereo ports
    auto make_port = [&](const char* name, uint32_t channel, bool is_output) -> void* {
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
        return false;
    }

    // Prepare the EQ core (default flat curve)
    m_eq->eq.Prepare(48000.f, 2);
    m_eq->gains.fill(0.f);
    m_eq->sample_rate = 48000.f;
    m_eq->channels    = 2;

    // Connect and run
    uint8_t          buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &s_audioInfo);

    if (pw_filter_connect(m_filter, PW_FILTER_FLAG_RT_PROCESS, params, 1) < 0) {
        std::cerr << "[PipeWire] pw_filter_connect failed\n";
        return false;
    }

    std::cout << "[PipeWire] Filter connected — running main loop\n";
    pw_main_loop_run(m_loop);  // blocks until Close() calls pw_main_loop_quit
    return true;
}

void PipeWireBackend::Close() {
    if (m_loop)   pw_main_loop_quit(m_loop);
    if (m_filter) { pw_filter_destroy(m_filter); m_filter = nullptr; }
    if (m_loop)   { pw_main_loop_destroy(m_loop); m_loop = nullptr; }
    pw_deinit();
}

// ── RT callback ───────────────────────────────────────────────────────────────
void PipeWireBackend::OnProcess(void* userdata) {
    auto* self = static_cast<PipeWireBackend*>(userdata);
    EqCore* core = self->m_eq;
    EqState* state = self->m_state;

    // Check for updated gains from non-RT thread (atomic, no alloc).
    std::array<float, kBandCount> newGains{};
    if (state->ConsumePending(newGains)) {
        static const float centres[kBandCount] = {
            31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
        core->eq.SetBandsPeaking(
            std::array<float, kBandCount>{centres[0],centres[1],centres[2],centres[3],
                                          centres[4],centres[5],centres[6],centres[7],
                                          centres[8],centres[9]},
            newGains, 1.0f);
        core->gains = newGains;
    }

    // Get buffers from PipeWire ports
    auto* pw_buf_in_L  = static_cast<pw_buffer*>(pw_filter_get_dsp_buffer(self->m_in_L,  0));
    auto* pw_buf_in_R  = static_cast<pw_buffer*>(pw_filter_get_dsp_buffer(self->m_in_R,  0));
    auto* pw_buf_out_L = static_cast<pw_buffer*>(pw_filter_get_dsp_buffer(self->m_out_L, 0));
    auto* pw_buf_out_R = static_cast<pw_buffer*>(pw_filter_get_dsp_buffer(self->m_out_R, 0));

    if (!pw_buf_in_L || !pw_buf_in_R || !pw_buf_out_L || !pw_buf_out_R) return;

    const uint32_t n_samples = pw_buf_in_L->buffer->datas[0].chunk->size / sizeof(float);
    const float* inL  = static_cast<const float*>(pw_buf_in_L->buffer->datas[0].data);
    const float* inR  = static_cast<const float*>(pw_buf_in_R->buffer->datas[0].data);
    float*       outL = static_cast<float*>(pw_buf_out_L->buffer->datas[0].data);
    float*       outR = static_cast<float*>(pw_buf_out_R->buffer->datas[0].data);

    if (!state->enabled.load(std::memory_order_relaxed)) {
        // Bypass — copy through.
        std::memcpy(outL, inL, n_samples * sizeof(float));
        std::memcpy(outR, inR, n_samples * sizeof(float));
        return;
    }

    // Interleave → process → deinterleave
    // (Equalizer10Band expects interleaved float)
    static float interleaved[8192 * 2];  // enough for 8192 frames stereo
    const uint32_t frames = n_samples;
    for (uint32_t i = 0; i < frames; ++i) {
        interleaved[i * 2]     = inL[i];
        interleaved[i * 2 + 1] = inR[i];
    }

    float preamp = std::pow(10.f, state->preamp_db.load(std::memory_order_relaxed) / 20.f);
    for (uint32_t i = 0; i < frames * 2; ++i)
        interleaved[i] *= preamp;

    core->eq.Process(interleaved, interleaved, frames, 2);

    // Clamp and deinterleave
    for (uint32_t i = 0; i < frames; ++i) {
        auto clamp = [](float v) -> float { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
        outL[i] = clamp(interleaved[i * 2]);
        outR[i] = clamp(interleaved[i * 2 + 1]);
    }

    pw_buf_out_L->buffer->datas[0].chunk->size = n_samples * sizeof(float);
    pw_buf_out_R->buffer->datas[0].chunk->size = n_samples * sizeof(float);
}

void PipeWireBackend::OnParamChanged(void* userdata, uint32_t /*id*/,
                                     const struct spa_pod* param) {
    if (!param) return;
    auto* self = static_cast<PipeWireBackend*>(userdata);

    struct spa_audio_info info{};
    if (spa_format_audio_raw_parse(param, &info.info.raw) != 0) return;

    const float sr       = static_cast<float>(info.info.raw.rate);
    const uint32_t ch    = info.info.raw.channels;
    self->m_eq->eq.Prepare(sr, ch);
    self->m_eq->sample_rate = sr;
    self->m_eq->channels    = ch;
    self->m_state->sample_rate = sr;
    self->m_state->channels    = ch;
    std::cout << "[PipeWire] Format: " << sr << " Hz, " << ch << "ch\n";
}

// ── Factory ───────────────────────────────────────────────────────────────────
std::unique_ptr<AudioBackend> CreateAudioBackend(EqState* state) {
    return std::make_unique<PipeWireBackend>(state);
}

} // namespace eq

#endif // BACKEND_PIPEWIRE
