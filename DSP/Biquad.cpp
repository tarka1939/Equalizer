#include "Biquad.h"
#include <cmath>
#include <cstring> // for memset

using namespace DSP;

Biquad::Biquad() noexcept
    : m_sampleRate(48000.0f)
    , m_channels(0)
    , m_activeIndex(0)
{
    // default to unity coefficients
    Coeffs unity = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    m_coeffs[0] = unity;
    m_coeffs[1] = unity;
}

void Biquad::Prepare(float sampleRate, uint32_t channels) noexcept
{
    m_sampleRate = sampleRate > 0.0f ? sampleRate : 48000.0f;
    m_channels = channels;
    m_states.resize(m_channels);
    for (auto &s : m_states)
    {
        s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
    }
}

void Biquad::SetCoefficients(const Coeffs& c) noexcept
{
    // write to the inactive slot then switch atomically
    uint32_t inactive = (m_activeIndex.load(std::memory_order_relaxed) ^ 1u);
    m_coeffs[inactive] = c;
    // publish new index
    m_activeIndex.store(inactive, std::memory_order_release);
}

void Biquad::SetPeaking(float centerHz, float Q, float gainDb) noexcept
{
    // Use RBJ cookbook peaking EQ formula
    const float sr = m_sampleRate > 0.0f ? m_sampleRate : 48000.0f;
    const float omega = 2.0f * static_cast<float>(M_PI) * centerHz / sr;
    const float sinw = std::sin(omega);
    const float cosw = std::cos(omega);
    const float A = std::pow(10.0f, gainDb / 40.0f); // sqrt of power
    const float alpha = sinw / (2.0f * Q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cosw;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cosw;
    float a2 = 1.0f - alpha / A;

    // normalize (make a0 = 1)
    Coeffs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;

    SetCoefficients(c);
}

void Biquad::Reset() noexcept
{
    for (auto &s : m_states)
    {
        s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
    }
}

void Biquad::Process(const float* input, float* output, uint32_t frames, uint32_t channels) noexcept
{
    if (frames == 0 || channels == 0 || m_channels == 0)
        return;

    // read active coeffs once (atomic load)
    const uint32_t idx = m_activeIndex.load(std::memory_order_acquire);
    const Coeffs coeff = m_coeffs[idx];

    // Assume interleaved input: frame*channels + ch
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            const uint32_t stateIndex = (ch < m_channels) ? ch : (ch % m_channels);
            ChannelState &st = m_states[stateIndex];

            const uint32_t pos = frame * channels + ch;
            const float x = input[pos];

            // Direct Form I
            const float y = coeff.b0 * x
                + coeff.b1 * st.x1
                + coeff.b2 * st.x2
                - coeff.a1 * st.y1
                - coeff.a2 * st.y2;

            // shift states
            st.x2 = st.x1;
            st.x1 = x;
            st.y2 = st.y1;
            st.y1 = y;

            output[pos] = y;
        }
    }
}