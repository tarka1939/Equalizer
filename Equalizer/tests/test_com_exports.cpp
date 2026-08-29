/*
 * test_com_exports.cpp — Unit tests for the exported COM entry points in
 * ComExports.cpp (DllGetClassObject, DllCanUnloadNow, EqualizerClassFactory)
 * plus a direct test of Equalizer::APOProcess using the real
 * APO_CONNECTION_PROPERTY layout, verified against Microsoft's
 * documentation:
 * https://learn.microsoft.com/windows/win32/api/audioapotypes/ns-audioapotypes-apo_connection_property
 *
 *   typedef struct APO_CONNECTION_PROPERTY {
 *     UINT_PTR         pBuffer;
 *     UINT32           u32ValidFrameCount;
 *     APO_BUFFER_FLAGS u32BufferFlags;
 *     UINT32           u32Signature;
 *   } APO_CONNECTION_PROPERTY;
 *
 * Windows-only (COM, audioenginebaseapo.h) -- builds via
 * EqualizerComExportsTests.vcxproj, not the cross-platform CMake build.
 * An earlier version of this comment said the project could not be compiled
 * or run here; that was wrong, and it is why several real breakages sat
 * unnoticed. It builds and runs with MSVC (see CLAUDE.md for activating the
 * toolchain).
 *
 * Deliberately NOT covered here: DllRegisterServer / DllUnregisterServer.
 * Both write to the real HKEY_LOCAL_MACHINE registration paths and require
 * admin rights -- see LOCAL_TEST_GUIDE.md for the existing manual
 * install/verify/uninstall procedure. The registry logic those two
 * functions call into (RegistryUtil.cpp) IS unit tested, but against a
 * HKEY_CURRENT_USER scratch key -- see test_registry_util.cpp.
 *
 * No external test framework (same hand-rolled pattern as the rest of the
 * project's tests).
 */
#include "../Equalizer.h"

#include <cmath>
#include <cstdio>
#include <vector>

// Exported from ComExports.cpp; CLSID_Equalizer is defined in Equalizer.cpp.
extern "C" const CLSID CLSID_Equalizer;
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv);
STDAPI DllCanUnloadNow(void);

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_currentTest = "";

void Check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s: %s (%s:%d)\n", g_currentTest, expr, file, line);
    }
}

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)
#define RUN_TEST(fn) do { g_currentTest = #fn; fn(); } while (0)

// Some unrelated CLSID -- only needs to not equal CLSID_Equalizer.
const CLSID kUnknownClsid = { 0x11111111, 0x2222, 0x3333, { 0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44 } };

void DllGetClassObject_UnknownClsidFails() {
    IClassFactory* factory = nullptr;
    HRESULT hr = DllGetClassObject(kUnknownClsid, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    CHECK(hr == CLASS_E_CLASSNOTAVAILABLE);
    CHECK(factory == nullptr);
}

void DllGetClassObject_KnownClsidSucceeds() {
    IClassFactory* factory = nullptr;
    HRESULT hr = DllGetClassObject(CLSID_Equalizer, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    CHECK(SUCCEEDED(hr));
    CHECK(factory != nullptr);
    if (factory) factory->Release();
}

void ClassFactory_CreateInstance_RejectsAggregation() {
    IClassFactory* factory = nullptr;
    HRESULT hr = DllGetClassObject(CLSID_Equalizer, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    CHECK(SUCCEEDED(hr) && factory != nullptr);
    if (!factory) return;

    // EqualizerClassFactory::CreateInstance only null-checks pUnkOuter before
    // rejecting aggregation -- it's never dereferenced, so a non-null
    // sentinel value is safe here without a real IUnknown behind it.
    IUnknown* fakeOuter = reinterpret_cast<IUnknown*>(1);
    IUnknown* instance = nullptr;
    HRESULT hr2 = factory->CreateInstance(fakeOuter, __uuidof(IUnknown), reinterpret_cast<void**>(&instance));
    CHECK(hr2 == CLASS_E_NOAGGREGATION);
    CHECK(instance == nullptr);
    factory->Release();
}

void ClassFactory_CreateInstance_ProducesAudioProcessingObjectRT() {
    IClassFactory* factory = nullptr;
    HRESULT hr = DllGetClassObject(CLSID_Equalizer, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    CHECK(SUCCEEDED(hr) && factory != nullptr);
    if (!factory) return;

    IAudioProcessingObjectRT* rt = nullptr;
    HRESULT hr2 = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&rt));
    CHECK(SUCCEEDED(hr2));
    CHECK(rt != nullptr);
    if (rt) rt->Release();
    factory->Release();
}

void DllCanUnloadNow_TracksLocksAndLiveObjects() {
    // DllCanUnloadNow() answers "are there outstanding locks *or* live
    // objects", not just locks. It used to consult only the explicit
    // LockServer() counter, so it reported "safe to unload" while the audio
    // engine still held live Equalizer instances -- unloading under them is a
    // crash in audiodg. It now also consults Module<InProc>::GetObjectCount(),
    // which tracks the WRL RuntimeClass instances (both Equalizer and the
    // class factory itself are RuntimeClass<ClassicCom>).
    //
    // This test previously asserted S_OK while still holding a factory
    // reference, which is exactly the unsafe assumption the change removed.
    // Verified against the real object count: it goes 0 -> 1 on
    // DllGetClassObject, 1 -> 2 on CreateInstance, and back down to 0 on the
    // matching Releases -- so the counter is balanced, not leaked.
    CHECK(DllCanUnloadNow() == S_OK);  // nothing outstanding yet

    IClassFactory* factory = nullptr;
    HRESULT hr = DllGetClassObject(CLSID_Equalizer, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    CHECK(SUCCEEDED(hr) && factory != nullptr);
    if (!factory) return;

    // The factory is itself a live RuntimeClass the caller holds a reference
    // to, so the DLL is not unloadable while it is outstanding.
    CHECK(DllCanUnloadNow() == S_FALSE);

    factory->LockServer(TRUE);
    CHECK(DllCanUnloadNow() == S_FALSE);

    factory->LockServer(FALSE);
    CHECK(DllCanUnloadNow() == S_FALSE);  // lock released, but factory still alive

    factory->Release();
    CHECK(DllCanUnloadNow() == S_OK);     // now genuinely unloadable
}

void ApoProcess_WithoutLockForProcessLeavesOutputUntouched() {
    // Builds real APO_CONNECTION_PROPERTY structures and calls APOProcess()
    // directly, bypassing LockForProcess.
    //
    // This test used to assert that APOProcess still wrote a full block (of
    // silence, back when m_gain defaulted to 0.0f). APOProcess now refuses to
    // touch the buffers at all unless LockForProcess has run and accepted the
    // format -- it reports 0 valid frames and returns. That guard is the
    // point: APOProcess reinterpret_casts pBuffer to float*, and without a
    // validated format there is nothing establishing that the buffer really
    // is float32. Writing into it on that assumption is what the guard
    // prevents, so the correct expectation here is "output untouched", not
    // "output zeroed".
    //
    // The locked path -- where the curve actually gets applied -- is covered
    // by ApoProcess_AppliesTheCurveConfiguredByLockForProcess below.
    Equalizer eqObj;

    const UINT32 frames = 4;
    const UINT32 channels = 2;
    std::vector<float> inBuf(static_cast<size_t>(frames) * channels, 0.5f);
    std::vector<float> outBuf(static_cast<size_t>(frames) * channels, -9.0f);  // sentinel

    APO_CONNECTION_PROPERTY inConn{};
    inConn.pBuffer = reinterpret_cast<UINT_PTR>(inBuf.data());
    inConn.u32ValidFrameCount = frames;
    inConn.u32BufferFlags = BUFFER_VALID;

    APO_CONNECTION_PROPERTY outConn{};
    outConn.pBuffer = reinterpret_cast<UINT_PTR>(outBuf.data());
    outConn.u32ValidFrameCount = 0;
    outConn.u32BufferFlags = BUFFER_VALID;

    APO_CONNECTION_PROPERTY* inPtr = &inConn;
    APO_CONNECTION_PROPERTY* outPtr = &outConn;

    eqObj.APOProcess(1, &inPtr, 1, &outPtr);

    CHECK(outConn.u32ValidFrameCount == 0);   // reported no frames produced
    for (float v : outBuf)
        CHECK(v == -9.0f);                    // sentinel intact: never written
}

// Minimal IAudioMediaType so LockForProcess() can be driven from a test.
// Only GetAudioFormat() is ever called on this path; the rest satisfy the
// vtable. Stack-allocated in the test, so the ref count is deliberately inert.
class FakeMediaType final : public IAudioMediaType {
public:
    explicit FakeMediaType(WORD channels, DWORD sampleRate) {
        m_fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        m_fmt.nChannels       = channels;
        m_fmt.nSamplesPerSec  = sampleRate;
        m_fmt.wBitsPerSample  = 32;
        m_fmt.nBlockAlign     = static_cast<WORD>(channels * 4);
        m_fmt.nAvgBytesPerSec = sampleRate * m_fmt.nBlockAlign;
        m_fmt.cbSize          = 0;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IAudioMediaType))) {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL* pfCompressed) override {
        if (pfCompressed) *pfCompressed = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsEqual(IAudioMediaType*, DWORD* pdwFlags) override {
        if (pdwFlags) *pdwFlags = 0;
        return S_OK;
    }
    const WAVEFORMATEX* STDMETHODCALLTYPE GetAudioFormat() override { return &m_fmt; }
    HRESULT STDMETHODCALLTYPE GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT*) override {
        return E_NOTIMPL;
    }

private:
    WAVEFORMATEX m_fmt{};
};

void ApoProcess_AppliesTheCurveConfiguredByLockForProcess() {
    // The regression guard for ARCHITECTURE.md section 7.1 and 7.6.
    //
    // LockForProcess() and APOProcess() used to declare their own
    // function-local `static DSP::Equalizer10Band s_eq` -- two separate
    // objects sharing a name. The one configured with the band curve was
    // never the one that processed audio, and the processing one was never
    // Prepare()'d, so it degraded to a passthrough copy. Separately, m_gain
    // defaulted to 0.0f, which zeroed every sample. Together those meant the
    // shipped APO could not audibly apply its curve at all.
    //
    // This test drives the real path -- LockForProcess() with a real float32
    // format, then APOProcess() -- and asserts the default curve is actually
    // audible. BandEqualizer's default is a "smiley": +3 dB at 62 Hz, 0 dB at
    // 1 kHz. So a 62 Hz tone must come out louder than it went in, while a
    // 1 kHz tone must come out at roughly unity. Under the old code the 62 Hz
    // case produced silence (m_gain == 0.0f), and with gain alone fixed it
    // would produce unity -- so this distinguishes the curve actually being
    // applied from both prior states.
    const DWORD  sr       = 48000;
    const WORD   channels = 1;
    const UINT32 frames   = 4096;

    FakeMediaType mediaType(channels, sr);

    APO_CONNECTION_DESCRIPTOR inDesc{};
    inDesc.Type             = APO_CONNECTION_BUFFER_TYPE_ALLOCATED;
    inDesc.u32MaxFrameCount = frames;
    inDesc.pFormat          = &mediaType;

    APO_CONNECTION_DESCRIPTOR outDesc = inDesc;

    APO_CONNECTION_DESCRIPTOR* inDescPtr  = &inDesc;
    APO_CONNECTION_DESCRIPTOR* outDescPtr = &outDesc;

    auto peakOf = [&](double toneHz) -> float {
        Equalizer eqObj;
        HRESULT hr = eqObj.LockForProcess(1, &inDescPtr, 1, &outDescPtr);
        CHECK(SUCCEEDED(hr));
        if (FAILED(hr)) return 0.0f;

        std::vector<float> in(frames), out(frames, 0.0f);
        for (UINT32 i = 0; i < frames; ++i)
            in[i] = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * toneHz * i / sr));

        APO_CONNECTION_PROPERTY inConn{};
        inConn.pBuffer            = reinterpret_cast<UINT_PTR>(in.data());
        inConn.u32ValidFrameCount = frames;
        inConn.u32BufferFlags     = BUFFER_VALID;

        APO_CONNECTION_PROPERTY outConn{};
        outConn.pBuffer            = reinterpret_cast<UINT_PTR>(out.data());
        outConn.u32ValidFrameCount = 0;
        outConn.u32BufferFlags     = BUFFER_VALID;

        APO_CONNECTION_PROPERTY* inPtr  = &inConn;
        APO_CONNECTION_PROPERTY* outPtr = &outConn;
        eqObj.APOProcess(1, &inPtr, 1, &outPtr);
        CHECK(outConn.u32ValidFrameCount == frames);

        // Skip the filter's settling transient before measuring.
        float peak = 0.0f;
        for (UINT32 i = frames / 2; i < frames; ++i)
            peak = (std::fabs(out[i]) > peak) ? std::fabs(out[i]) : peak;
        return peak;
    };

    const float bassPeak = peakOf(62.0);
    const float midPeak  = peakOf(1000.0);

    // Not silence -- this alone fails against the old m_gain == 0.0f default.
    CHECK(bassPeak > 0.01f);
    CHECK(midPeak  > 0.01f);

    // 1 kHz sits on a 0 dB band, so it passes through at roughly unity.
    CHECK(midPeak > 0.20f && midPeak < 0.30f);

    // 62 Hz sits on a +3 dB band (~1.41x). Passthrough would leave it at
    // 0.25, so requiring a clear boost is what pins the curve being applied.
    CHECK(bassPeak > 0.30f);
}

void ApoProcess_ZeroInputConnectionsIsNoOp() {
    Equalizer eqObj;

    APO_CONNECTION_PROPERTY outConn{};
    std::vector<float> outBuf(4, -9.0f);
    outConn.pBuffer = reinterpret_cast<UINT_PTR>(outBuf.data());
    outConn.u32ValidFrameCount = 123;  // sentinel, must be untouched
    APO_CONNECTION_PROPERTY* outPtr = &outConn;

    eqObj.APOProcess(0, nullptr, 1, &outPtr);

    CHECK(outConn.u32ValidFrameCount == 123);  // untouched
    for (float v : outBuf)
        CHECK(v == -9.0f);  // untouched
}

void ApoProcess_ZeroFrameCountSetsOutputFrameCountToZero() {
    Equalizer eqObj;

    std::vector<float> inBuf(2, 0.5f);
    std::vector<float> outBuf(2, -9.0f);

    APO_CONNECTION_PROPERTY inConn{};
    inConn.pBuffer = reinterpret_cast<UINT_PTR>(inBuf.data());
    inConn.u32ValidFrameCount = 0;
    inConn.u32BufferFlags = BUFFER_VALID;

    APO_CONNECTION_PROPERTY outConn{};
    outConn.pBuffer = reinterpret_cast<UINT_PTR>(outBuf.data());
    outConn.u32ValidFrameCount = 55;  // sentinel
    outConn.u32BufferFlags = BUFFER_VALID;

    APO_CONNECTION_PROPERTY* inPtr = &inConn;
    APO_CONNECTION_PROPERTY* outPtr = &outConn;

    eqObj.APOProcess(1, &inPtr, 1, &outPtr);

    CHECK(outConn.u32ValidFrameCount == 0);
    for (float v : outBuf)
        CHECK(v == -9.0f);  // untouched -- zero frames means no writes
}

}  // namespace

int main() {
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CHECK(SUCCEEDED(hrInit));

    RUN_TEST(DllGetClassObject_UnknownClsidFails);
    RUN_TEST(DllGetClassObject_KnownClsidSucceeds);
    RUN_TEST(ClassFactory_CreateInstance_RejectsAggregation);
    RUN_TEST(ClassFactory_CreateInstance_ProducesAudioProcessingObjectRT);
    RUN_TEST(DllCanUnloadNow_TracksLocksAndLiveObjects);
    RUN_TEST(ApoProcess_WithoutLockForProcessLeavesOutputUntouched);
    RUN_TEST(ApoProcess_AppliesTheCurveConfiguredByLockForProcess);
    RUN_TEST(ApoProcess_ZeroInputConnectionsIsNoOp);
    RUN_TEST(ApoProcess_ZeroFrameCountSetsOutputFrameCountToZero);

    if (SUCCEEDED(hrInit))
        CoUninitialize();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
