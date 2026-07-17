#include "../DSP/EqPipeline.h"
#include "../Equalizer/BandEqualizer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#pragma pack(push, 1)
struct RiffHeader
{
    char riff[4];
    uint32_t size;
    char wave[4];
};

struct ChunkHeader
{
    char id[4];
    uint32_t size;
};

struct FmtChunk
{
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

static bool ReadWavFloat32(const char* path, FmtChunk& fmtOut, std::vector<float>& samples)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;

    RiffHeader riff{};
    if (std::fread(&riff, 1, sizeof(riff), f) != sizeof(riff))
    {
        std::fclose(f);
        return false;
    }

    if (std::strncmp(riff.riff, "RIFF", 4) != 0 || std::strncmp(riff.wave, "WAVE", 4) != 0)
    {
        std::fclose(f);
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    uint32_t dataSize = 0;
    long dataPos = 0;

    while (true)
    {
        ChunkHeader ch{};
        const size_t n = std::fread(&ch, 1, sizeof(ch), f);
        if (n == 0)
            break;
        if (n != sizeof(ch))
        {
            std::fclose(f);
            return false;
        }

        // WAV chunks are word-aligned.
        const uint32_t chunkSize = ch.size;

        if (std::strncmp(ch.id, "fmt ", 4) == 0)
        {
            // Read only the common part, skip any extension.
            FmtChunk fmt{};
            if (chunkSize < sizeof(FmtChunk))
            {
                std::fclose(f);
                return false;
            }

            if (std::fread(&fmt, 1, sizeof(fmt), f) != sizeof(fmt))
            {
                std::fclose(f);
                return false;
            }

            // Skip remaining fmt bytes.
            const uint32_t remaining = chunkSize - static_cast<uint32_t>(sizeof(fmt));
            if (remaining > 0)
                std::fseek(f, remaining, SEEK_CUR);

            fmtOut = fmt;
            haveFmt = true;
        }
        else if (std::strncmp(ch.id, "data", 4) == 0)
        {
            dataPos = std::ftell(f);
            dataSize = chunkSize;
            std::fseek(f, chunkSize, SEEK_CUR);
            haveData = true;
        }
        else
        {
            std::fseek(f, chunkSize, SEEK_CUR);
        }

        // pad byte
        if (chunkSize & 1)
            std::fseek(f, 1, SEEK_CUR);

        if (haveFmt && haveData)
            break;
    }

    if (!haveFmt || !haveData)
    {
        std::fclose(f);
        return false;
    }

    // Validate format: IEEE float32
    if (fmtOut.audioFormat != 3 /* IEEE float */ || fmtOut.bitsPerSample != 32)
    {
        std::fclose(f);
        return false;
    }

    // Read samples
    if (dataPos < 0)
    {
        std::fclose(f);
        return false;
    }

    std::fseek(f, dataPos, SEEK_SET);
    const size_t sampleCount = dataSize / sizeof(float);
    samples.resize(sampleCount);
    if (dataSize > 0)
    {
        if (std::fread(samples.data(), 1, dataSize, f) != dataSize)
        {
            std::fclose(f);
            return false;
        }
    }

    std::fclose(f);
    return true;
}

static bool WriteWavFloat32(const char* path, const FmtChunk& fmt, const std::vector<float>& samples)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;

    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * sizeof(float));

    const uint32_t fmtChunkSize = sizeof(FmtChunk);
    const uint32_t riffSize =
        4 /* WAVE */ +
        8 + fmtChunkSize +
        8 + dataSize;

    RiffHeader riff{};
    std::memcpy(riff.riff, "RIFF", 4);
    riff.size = riffSize;
    std::memcpy(riff.wave, "WAVE", 4);

    ChunkHeader fmtHdr{};
    std::memcpy(fmtHdr.id, "fmt ", 4);
    fmtHdr.size = fmtChunkSize;

    ChunkHeader dataHdr{};
    std::memcpy(dataHdr.id, "data", 4);
    dataHdr.size = dataSize;

    if (std::fwrite(&riff, 1, sizeof(riff), f) != sizeof(riff)) { std::fclose(f); return false; }
    if (std::fwrite(&fmtHdr, 1, sizeof(fmtHdr), f) != sizeof(fmtHdr)) { std::fclose(f); return false; }
    if (std::fwrite(&fmt, 1, sizeof(fmt), f) != sizeof(fmt)) { std::fclose(f); return false; }
    if (std::fwrite(&dataHdr, 1, sizeof(dataHdr), f) != sizeof(dataHdr)) { std::fclose(f); return false; }

    if (!samples.empty())
    {
        if (std::fwrite(samples.data(), sizeof(float), samples.size(), f) != samples.size())
        {
            std::fclose(f);
            return false;
        }
    }

    std::fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
            "Usage: WavEqTest <in.wav float32> <out.wav> [gain=0.8] [eqStrength=1.0] [ir.wav]\n"
            "  ir.wav (optional): 32-bit float impulse response WAV -- installs it as the\n"
            "  FIR stage (channel 0 only, applied to every output channel). With eqStrength=0\n"
            "  and an ir.wav given, this exercises FIR-only mode; with ir.wav omitted, this\n"
            "  exercises the original IIR-only demo; with both, FIR-then-IIR cascades.\n");
        return 2;
    }

    const float gain = (argc >= 4) ? static_cast<float>(std::atof(argv[3])) : 0.8f;
    const float eqStrength = (argc >= 5) ? static_cast<float>(std::atof(argv[4])) : 1.0f;
    const char* irPath = (argc >= 6) ? argv[5] : nullptr;

    FmtChunk fmt{};
    std::vector<float> in;
    if (!ReadWavFloat32(argv[1], fmt, in))
    {
        std::fprintf(stderr, "Failed to read input WAV (must be 32-bit float PCM).\n");
        return 1;
    }

    const uint32_t channels = fmt.numChannels;
    const uint32_t frames = channels ? static_cast<uint32_t>(in.size() / channels) : 0;

    // Optional FIR impulse response, loaded as a plain float32 WAV. Only
    // channel 0 is used as the tap sequence -- OverlapAdd applies one FIR
    // uniformly to every output channel (see DSP/OverlapAdd.h), it does not
    // support a per-channel filter.
    std::vector<float> irTaps;
    if (irPath)
    {
        FmtChunk irFmt{};
        std::vector<float> irRaw;
        if (!ReadWavFloat32(irPath, irFmt, irRaw))
        {
            std::fprintf(stderr, "Failed to read impulse-response WAV (must be 32-bit float PCM).\n");
            return 1;
        }
        const uint32_t irChannels = irFmt.numChannels ? irFmt.numChannels : 1;
        const uint32_t irFrames = irChannels ? static_cast<uint32_t>(irRaw.size() / irChannels) : 0;
        irTaps.resize(irFrames);
        for (uint32_t i = 0; i < irFrames; ++i)
            irTaps[i] = irRaw[i * irChannels];
        std::fprintf(stderr, "Loaded impulse response: %u taps from %s\n", irFrames, irPath);
    }

    DSP::EqPipeline eq;
    constexpr uint32_t kFirBlockSize = 256;
    const uint32_t firMaxImpulse = irTaps.empty() ? 1u : static_cast<uint32_t>(irTaps.size());
    if (!eq.Prepare(static_cast<float>(fmt.sampleRate), channels, kFirBlockSize, firMaxImpulse))
    {
        std::fprintf(stderr, "Failed to prepare EqPipeline (channels=%u).\n", channels);
        return 1;
    }

    if (!irTaps.empty())
    {
        if (!eq.SetImpulseResponse(irTaps.data(), static_cast<uint32_t>(irTaps.size())))
            std::fprintf(stderr, "WARNING: failed to install impulse response (%zu taps).\n", irTaps.size());
    }

    // Use an exaggerated curve by default for audibility. eqStrength=0
    // makes every band gain exactly 0, which leaves EqPipeline's IIR stage
    // inactive (see EqPipeline::SetBandsPeaking) -- combined with an ir.wav,
    // that is how to exercise FIR-only mode from this tool.
    BandEqualizer bands;
    std::array<float, BandEqualizer::BandCount> centers{};
    std::array<float, BandEqualizer::BandCount> gains{};
    const auto& b = bands.GetBands();
    for (size_t i = 0; i < BandEqualizer::BandCount; ++i)
    {
        centers[i] = b[i].centerHz;
        gains[i] = b[i].gainDb * eqStrength;
    }

    eq.SetBandsPeaking(centers, gains, 0.7f);
    eq.Reset();

    std::fprintf(stderr, "Pipeline: FIR %s, IIR %s (latency %u samples)\n",
        eq.IsFirActive() ? "active" : "inactive",
        eq.IsIirActive() ? "active" : "inactive",
        eq.GetLatencySamples());

    std::vector<float> out(in.size());

    for (size_t i = 0; i < in.size(); ++i)
        out[i] = in[i] * gain;

    eq.Process(out.data(), out.data(), frames, channels);

    double inAbs = 0.0;
    double outAbs = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
    {
        inAbs += std::abs(in[i]);
        outAbs += std::abs(out[i]);
    }
    std::fprintf(stderr, "AbsSum in=%g out=%g (gain=%g eqStrength=%g)\n", inAbs, outAbs, gain, eqStrength);

    if (!WriteWavFloat32(argv[2], fmt, out))
    {
        std::fprintf(stderr, "Failed to write output WAV.\n");
        return 1;
    }

    return 0;
}
