#pragma once
#include "../DSP/Equalizer10Band.h"
#include <audioenginebaseapo.h>
#include <audiopolicy.h>
#include <cmath>
#include <propkey.h>
#include <mmdeviceapi.h>
#include <wrl.h>

// COM CLSID for this APO (defined in `Equalizer.cpp`).
//
// extern "C", matching the definition in Equalizer.cpp and the declaration in
// tests/test_com_exports.cpp. This used to be declared with C++ linkage here
// while being defined with C linkage there, so a translation unit that only
// saw the header (ComExports.cpp) referenced the C++-mangled symbol while
// Equalizer.cpp emitted the C one.
extern "C" const CLSID CLSID_Equalizer;

using namespace Microsoft::WRL;

class Equalizer :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioProcessingObject, IAudioProcessingObjectRT, IAudioProcessingObjectConfiguration>
{
public:
    Equalizer();

    // IAudioProcessingObject
    STDMETHODIMP GetLatency(HNSTIME* pTime) override
    {
        if (!pTime) return E_POINTER;
        *pTime = 0;
        return S_OK;
    }
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

    // STDMETHODIMP, not bare HRESULT: the interface declares these
    // STDMETHODCALLTYPE (__stdcall), and a bare HRESULT is __cdecl on x86.
    // x64 has only one calling convention so the mismatch was invisible
    // there, but every 32-bit build failed with "error C2695: overriding
    // virtual function differs ... only by calling convention" -- and, as a
    // knock-on, left Equalizer abstract so Make<Equalizer>() in
    // ComExports.cpp failed too. The out-of-class definitions in
    // Equalizer.cpp inherit the convention from these declarations, so they
    // need no change (same as LockForProcess/UnlockForProcess already do).
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat, IAudioMediaType** ppSupportedOutputFormat) override;
    STDMETHODIMP GetInputChannelCount(UINT32* pu32ChannelCount) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(
        UINT32 u32NumInputConnections,
        APO_CONNECTION_DESCRIPTOR** ppInputConnections,
        UINT32 u32NumOutputConnections,
        APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

private:
    // Unity. This was 0.0f behind a "// 80% volume" comment, so the shipped
    // APO multiplied every sample by zero and output silence -- there is no
    // code path in this repo that ever assigned it. Unity is the right
    // default for an equalizer: any level change should come from the band
    // curve or an explicit preamp, not from a hardcoded scalar the user
    // cannot see. (0.8f would match the old comment but would mean the APO
    // silently attenuates by ~1.9 dB.)
    float m_gain = 1.0f;
    UINT32 m_channels = 0;

    // The 10-band EQ this APO instance runs audio through.
    //
    // This used to be two *separate* function-local `static
    // DSP::Equalizer10Band s_eq` objects -- one in LockForProcess(), one in
    // APOProcess() -- that merely shared a name. `static` inside a function
    // scopes lifetime, not identity across functions, so the instance
    // configured with the real band curve was never the one that processed
    // audio; the processing one was never Prepare()'d and so degraded to a
    // passthrough copy (silence only once m_gain's 0.0f default was applied
    // on top). That is why the APO never audibly applied its curve
    // (ARCHITECTURE.md section 7.1).
    //
    // A function-local static is also process-wide shared state across every
    // Equalizer instance, which is fragile on its own. That is latent rather
    // than active today -- GetRegistrationProperties() reports
    // u32MaxInstances = 1 -- but a member is the right shape regardless.
    //
    // THREADING: LockForProcess() calls m_eq.Prepare(), which resizes each
    // Biquad's per-channel state vector (Biquad::Prepare -> m_states.resize),
    // i.e. it reallocates buffers that APOProcess() reads. Those two must
    // never overlap. They don't, because the APO contract only permits
    // APOProcess() between LockForProcess() and UnlockForProcess() -- the
    // same contract that already lets m_channels/m_maxFrameCount/
    // m_formatLocked be written and read across those calls without
    // synchronisation. Note this constraint did not exist while the two
    // statics were separate objects, so don't add an on-the-fly
    // reconfiguration path here without fencing the RT callback off first;
    // see daemon/pipewire_backend.cpp's Begin/EndReconfigure for how the
    // daemon side does it.
    DSP::Equalizer10Band m_eq;

    // Frame capacity of the locked connections (the smaller of the input and
    // output descriptors' u32MaxFrameCount). APOProcess() clamps the caller's
    // u32ValidFrameCount to this before writing.
    UINT32 m_maxFrameCount = 0;

    // Set by LockForProcess() only once the negotiated format has been
    // verified as 32-bit float. APOProcess() reinterpret_casts the connection
    // buffers as float*, so it must not run at all when this is false.
    bool m_formatLocked = false;
};