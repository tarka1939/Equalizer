#pragma once
#include "kiss_fftr.h"  // if using real FFT
#include <audioenginebaseapo.h>
#include <audiopolicy.h>
#include <cmath>
#include <propkey.h>
#include <mmdeviceapi.h>
#include <wrl.h>

// COM CLSID for this APO (defined in `Equalizer.cpp`).
extern const CLSID CLSID_Equalizer;

using namespace Microsoft::WRL;

class Equalizer :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioProcessingObject, IAudioProcessingObjectRT, IAudioProcessingObjectConfiguration>
{
public:
    Equalizer();

    // IAudioProcessingObject
    STDMETHODIMP GetLatency(HNSTIME* pTime) override { *pTime = 0; return S_OK; }
    STDMETHODIMP Reset() override { return S_OK; }
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) override;
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override { return S_OK; }

    // IAudioProcessingObjectRT
    STDMETHOD_(void) APOProcess(
        UINT32 u32NumInputConnections,
        APO_CONNECTION_PROPERTY** ppInputConnections,
        UINT32 u32NumOutputConnections,
        APO_CONNECTION_PROPERTY** ppOutputConnections
    ) override;

    STDMETHOD_(UINT32) CalcInputFrames(UINT32 u32OutputFrameCount) override { return u32OutputFrameCount; }
    STDMETHOD_(UINT32) CalcOutputFrames(UINT32 u32InputFrameCount) override { return u32InputFrameCount; }

    HRESULT IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat) override;
    HRESULT IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat, IAudioMediaType** ppSupportedOutputFormat) override;
    HRESULT GetInputChannelCount(UINT32* pu32ChannelCount) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(
        UINT32 u32NumInputConnections,
        APO_CONNECTION_DESCRIPTOR** ppInputConnections,
        UINT32 u32NumOutputConnections,
        APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

private:
    float m_gain = 0.0f; // 80% volume
    UINT32 m_channels = 0;
};