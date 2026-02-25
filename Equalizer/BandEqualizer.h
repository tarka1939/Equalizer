#pragma once

#include <array>
#include <cstddef>

class BandEqualizer
{
public:
    static constexpr size_t BandCount = 10;

    struct Band
    {
        float centerHz;
        float gainDb;
    };

    BandEqualizer() noexcept;

    const std::array<Band, BandCount>& GetBands() const noexcept { return m_bands; }

    void SetBandGain(size_t bandIndex, float gainDb) noexcept;
    float GetBandGain(size_t bandIndex) const noexcept;

private:
    std::array<Band, BandCount> m_bands;
};
