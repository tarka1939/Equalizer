#pragma once
/*
 * ApoDsp.h — platform-agnostic per-block DSP for the Windows APO.
 *
 * Pulled out of Equalizer::APOProcess (Equalizer.cpp) so the gain/EQ/clamp
 * math can be unit tested without the Windows APO_CONNECTION_PROPERTY /
 * COM plumbing. This header has no Windows dependencies, so it also builds
 * and runs under the cross-platform CMake build (see Equalizer/tests/).
 */
#include "../DSP/Equalizer10Band.h"
#include <cstdint>

namespace ApoDsp
{
    // Runs one processing block:
    //   1. out[i] = in[i] * gain            (preamp)
    //   2. eq.Process(out, out, frameCount, channels)   (in-place 10-band EQ)
    //   3. clamp out to [-1.0, 1.0]          (safety limiter for boosted EQ)
    //
    // `in` and `out` may point at the same buffer (in-place). Both must be
    // able to hold frameCount * channels floats. If frameCount == 0, `out`
    // is left untouched and 0 is returned.
    //
    // RT-safe: no allocation, no locking, no blocking calls.
    uint32_t ProcessBlock(const float* in, float* out, uint32_t frameCount,
                           uint32_t channels, float gain, DSP::Equalizer10Band& eq) noexcept;
}
