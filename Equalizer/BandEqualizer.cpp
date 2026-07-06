#include "BandEqualizer.h"

BandEqualizer::BandEqualizer() noexcept
    : m_bands{ {
        {31.25f, 5.0f},
        {62.5f, 3.0f},
        {125.0f, 2.0f},
        {250.0f, 0.0f},
        {500.0f, 0.0f},
        {1000.0f, 0.0f},
        {2000.0f, 0.0f},
        {4000.0f, 2.0f},
        {8000.0f, 3.0f},
        {16000.0f, 5.0f}
    } }
{
}

void BandEqualizer::SetBandGain(size_t bandIndex, float gainDb) noexcept
{
    if (bandIndex >= BandCount)
        return;

    m_bands[bandIndex].gainDb = gainDb;
}

float BandEqualizer::GetBandGain(size_t bandIndex) const noexcept
{
    if (bandIndex >= BandCount)
        return 0.0f;

    return m_bands[bandIndex].gainDb;
}
