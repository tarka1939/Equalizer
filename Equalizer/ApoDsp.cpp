#include "ApoDsp.h"

namespace ApoDsp
{
    uint32_t ProcessBlock(const float* in, float* out, uint32_t frameCount,
                           uint32_t channels, float gain, DSP::Equalizer10Band& eq) noexcept
    {
        if (frameCount == 0)
            return 0;

        const uint32_t samples = frameCount * channels;

        // Basic gain (preamp).
        for (uint32_t i = 0; i < samples; ++i)
            out[i] = in[i] * gain;

        // 10-band EQ chain (in-place).
        eq.Process(out, out, frameCount, channels);

        // Safety clamp to avoid runaway clipping for boosted EQ settings.
        for (uint32_t i = 0; i < samples; ++i)
        {
            if (out[i] > 1.0f) out[i] = 1.0f;
            else if (out[i] < -1.0f) out[i] = -1.0f;
        }

        return frameCount;
    }
}
