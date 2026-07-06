#include "Equalizer.h"
#include "BandEqualizer.h"
#include "Diagnostics.h"
#include "../DSP/Equalizer10Band.h"
#include <initguid.h>
#include <algorithm>
#include <array>
#include <iostream>

// Define your own unique GUID for the APO
// You can generate one in Visual Studio: Tools → Create GUID
extern "C" const CLSID CLSID_Equalizer =
{ 0x12345678, 0x9abc, 0x4def, { 0x80, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };

Equalizer::Equalizer() {}

STDMETHODIMP Equalizer::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps)
{
    APO_REG_PROPERTIES* props = (APO_REG_PROPERTIES*)CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES));
    if (!props) return E_OUTOFMEMORY;

    props->clsid = CLSID_Equalizer;
    wcscpy_s(props->szFriendlyName, L"Equalizer");
    wcscpy_s(props->szCopyrightInfo, L"(c) Krzysztof Tarka");
    props->u32NumAPOInterfaces = 1;
    props->u32MaxInstances = 1;
    props->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObjectRT);
    props->u32MajorVersion = 1;
    props->u32MinorVersion = 0;
    props->u32MinInputConnections = 1;
    props->u32MaxInputConnections = 1;
    props->u32MinOutputConnections = 1;
    props->u32MaxOutputConnections = 1;
    props->Flags = APO_FLAG_NONE;

    *ppRegProps = props;
    return S_OK;
}

// Real-time processing function
void Equalizer::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    if (u32NumInputConnections == 0 || u32NumOutputConnections == 0)
        return;

    APO_CONNECTION_PROPERTY* inConn = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* outConn = ppOutputConnections[0];
    if (!inConn || !outConn)
        return;

    const UINT32 frameCount = inConn->u32ValidFrameCount;
    const float* in = reinterpret_cast<const float*>(inConn->pBuffer);
    float* out = reinterpret_cast<float*>(outConn->pBuffer);

    if (frameCount == 0)
    {
        outConn->u32ValidFrameCount = 0;
        return;
    }

    const UINT32 channels = (m_channels != 0) ? m_channels : 2;
    const UINT32 samples = frameCount * channels;

    // Basic gain (preamp). EQ chain (if prepared) is applied after.
    for (UINT32 i = 0; i < samples; ++i)
        out[i] = in[i] * m_gain;

    // Run through EQ chain (in-place).
    static DSP::Equalizer10Band s_eq;
    s_eq.Process(out, out, frameCount, channels);

    // Safety clamp to avoid runaway clipping for boosted EQ settings.
    for (UINT32 i = 0; i < samples; ++i)
    {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }

    outConn->u32ValidFrameCount = frameCount;
}

HRESULT Equalizer::IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
	IAudioMediaType** ppSupportedInputFormat)
{
	bool flag = pRequestedInputFormat != nullptr;
	if (!flag)
		return E_INVALIDARG;
	return S_OK;
}

HRESULT Equalizer::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
	IAudioMediaType** ppSupportedOutputFormat)
{
	bool flag = pRequestedOutputFormat != nullptr;
	if (!flag)
		return E_INVALIDARG;
	return S_OK;
}

HRESULT Equalizer::GetInputChannelCount(UINT32* pu32ChannelCount)
{
	if (pu32ChannelCount == nullptr)
		return E_POINTER;
	*pu32ChannelCount = 2; // Stereo
	return S_OK;
}

HRESULT Equalizer::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
	UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    Diagnostics::DebugLog(L"LockForProcess: entered");
    Diagnostics::AppendFileLine(L"C:\\driver\\eq_apo.txt", L"LockForProcess: entered");

    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1)
        return APOERR_NUM_CONNECTIONS_INVALID;

    if (!ppInputConnections || !ppOutputConnections || !ppInputConnections[0] || !ppOutputConnections[0])
        return E_INVALIDARG;

    IAudioMediaType* fmt = ppInputConnections[0]->pFormat;
    if (!fmt)
        return APOERR_INVALID_CONNECTION_FORMAT;

    WAVEFORMATEX const* wf = fmt->GetAudioFormat();
    if (!wf)
        return APOERR_INVALID_CONNECTION_FORMAT;

    m_channels = wf->nChannels;

    {
        wchar_t buf[256]{};
        swprintf_s(buf, L"LockForProcess: sr=%lu ch=%hu bits=%hu", wf->nSamplesPerSec, wf->nChannels, wf->wBitsPerSample);
        Diagnostics::DebugLog(buf);
        Diagnostics::AppendFileLine(L"C:\\driver\\eq_apo.txt", buf);
    }

    // Prepare and set up the default 10-band curve.
    static DSP::Equalizer10Band s_eq;
    s_eq.Prepare(static_cast<float>(wf->nSamplesPerSec), static_cast<uint32_t>(wf->nChannels));

    BandEqualizer bands;
    std::array<float, BandEqualizer::BandCount> centers{};
    std::array<float, BandEqualizer::BandCount> gains{};
    const auto& b = bands.GetBands();
    for (size_t i = 0; i < BandEqualizer::BandCount; ++i)
    {
        centers[i] = b[i].centerHz;
        gains[i] = b[i].gainDb;
    }

    s_eq.SetBandsPeaking(centers, gains, 1.0f);
    s_eq.Reset();

    return S_OK;
}

HRESULT Equalizer::UnlockForProcess()
{
	return S_OK;
}

