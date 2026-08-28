#include "Equalizer.h"
#include "ApoDsp.h"
#include "BandEqualizer.h"
#include "Diagnostics.h"
#include "../DSP/Equalizer10Band.h"
#include <initguid.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <algorithm>
#include <array>
#include <iostream>

// COM CLSID for this APO: {8E259F55-B32B-4FB8-8995-5965798B2C08}
//
// This replaces a hand-typed placeholder ({12345678-9ABC-4DEF-8011-...})
// that was shipped as-is next to a "generate your own" comment. A CLSID is
// a machine-global registry key; a made-up, obviously-patterned value risks
// colliding with anyone else who typed the same digits.
//
// MIGRATION: any machine where the old CLSID was already registered still
// has those keys. Unregister with the OLD value before installing this
// build, or the stale HKLM\Software\Classes\CLSID and APO-catalog entries
// are orphaned. See LOCAL_TEST_GUIDE.md.
//
// Kept in sync with installer/EqualizerTest.inf and LOCAL_TEST_GUIDE.md.
extern "C" const CLSID CLSID_Equalizer =
{ 0x8e259f55, 0xb32b, 0x4fb8, { 0x89, 0x95, 0x59, 0x65, 0x79, 0x8b, 0x2c, 0x08 } };

namespace
{
    // The processing path in APOProcess() reinterpret_casts the connection
    // buffers as float*. Nothing used to enforce that: IsInputFormatSupported
    // and IsOutputFormatSupported only null-checked their argument and
    // returned S_OK, so a 16-bit endpoint would be read as float -- garbage
    // audio and a 2x buffer over-read. Check the format for real.
    bool IsFloat32Format(const WAVEFORMATEX* wf) noexcept
    {
        if (!wf)
            return false;

        if (wf->wBitsPerSample != 32)
            return false;

        if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            return true;

        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
            return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        }

        return false;
    }

    HRESULT CheckFormatSupported(IAudioMediaType* requested, IAudioMediaType** ppSupported) noexcept
    {
        // Per the IAudioProcessingObject contract this out-parameter must be
        // set (to null) even on the failure paths; it was never touched at
        // all before, leaving the caller's pointer uninitialised.
        if (ppSupported)
            *ppSupported = nullptr;

        if (!requested)
            return E_INVALIDARG;

        const WAVEFORMATEX* wf = requested->GetAudioFormat();
        if (!wf)
            return APOERR_INVALID_CONNECTION_FORMAT;

        if (!IsFloat32Format(wf))
            return APOERR_FORMAT_NOT_SUPPORTED;

        if (ppSupported)
        {
            *ppSupported = requested;
            requested->AddRef();
        }
        return S_OK;
    }
}

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

    // Never process more frames than LockForProcess() said the connections
    // can hold. u32ValidFrameCount is supplied by the caller each block and
    // was previously used unchecked to drive writes into outConn->pBuffer.
    UINT32 frameCount = inConn->u32ValidFrameCount;
    if (m_maxFrameCount != 0 && frameCount > m_maxFrameCount)
        frameCount = m_maxFrameCount;

    const float* in = reinterpret_cast<const float*>(inConn->pBuffer);
    float* out = reinterpret_cast<float*>(outConn->pBuffer);

    if (frameCount == 0 || !in || !out)
    {
        outConn->u32ValidFrameCount = 0;
        return;
    }

    // LockForProcess() rejects any format we can't safely reinterpret as
    // float32; if it never ran (or rejected), don't touch the buffers.
    if (!m_formatLocked)
    {
        outConn->u32ValidFrameCount = 0;
        return;
    }

    const UINT32 channels = (m_channels != 0) ? m_channels : 2;

    // NOTE: this s_eq is a distinct static local from the one in
    // LockForProcess() below -- they do NOT share storage. The band gains
    // configured in LockForProcess are never applied here; this instance is
    // always in its default (unprepared/flat) state. That predates this
    // refactor -- see ApoDsp::ProcessBlock's unit tests (Equalizer/tests/)
    // for the isolated, correctly-wired behavior, and the project's tests
    // README for details. Not fixed here since only extraction for
    // testability was in scope, not behavior changes.
    static DSP::Equalizer10Band s_eq;
    const UINT32 written = ApoDsp::ProcessBlock(in, out, frameCount, channels, m_gain, s_eq);

    outConn->u32ValidFrameCount = written;
}

HRESULT Equalizer::IsInputFormatSupported(IAudioMediaType* /*pOppositeFormat*/, IAudioMediaType* pRequestedInputFormat,
	IAudioMediaType** ppSupportedInputFormat)
{
	return CheckFormatSupported(pRequestedInputFormat, ppSupportedInputFormat);
}

HRESULT Equalizer::IsOutputFormatSupported(IAudioMediaType* /*pOppositeFormat*/, IAudioMediaType* pRequestedOutputFormat,
	IAudioMediaType** ppSupportedOutputFormat)
{
	return CheckFormatSupported(pRequestedOutputFormat, ppSupportedOutputFormat);
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

    // APOProcess() reinterprets the connection buffers as float*, so refuse
    // to lock onto anything else rather than producing noise from a 16-bit
    // stream (and over-reading its buffer by 2x while doing so).
    if (!IsFloat32Format(wf))
    {
        Diagnostics::DebugLog(L"LockForProcess: rejected non-float32 format");
        return APOERR_FORMAT_NOT_SUPPORTED;
    }

    m_channels = wf->nChannels;
    if (m_channels == 0)
        return APOERR_INVALID_CONNECTION_FORMAT;

    // Remember the frame capacity so APOProcess() can clamp to it.
    m_maxFrameCount = ppInputConnections[0]->u32MaxFrameCount;
    const UINT32 outMaxFrames = ppOutputConnections[0]->u32MaxFrameCount;
    if (outMaxFrames < m_maxFrameCount)
        m_maxFrameCount = outMaxFrames;

    m_formatLocked = true;

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
	m_formatLocked = false;
	m_maxFrameCount = 0;
	m_channels = 0;
	return S_OK;
}

