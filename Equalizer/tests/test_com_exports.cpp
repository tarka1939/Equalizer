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
 * This project could not be compiled or run in the environment that wrote
 * these tests (no Windows SDK available there) -- build and run this in
 * Visual Studio to confirm.
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

void DllCanUnloadNow_TracksLockServerRefCount() {
    IClassFactory* factory = nullptr;
    HRESULT hr = DllGetClassObject(CLSID_Equalizer, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    CHECK(SUCCEEDED(hr) && factory != nullptr);
    if (!factory) return;

    CHECK(DllCanUnloadNow() == S_OK);  // nothing locked yet (assuming no other test left a lock)

    factory->LockServer(TRUE);
    CHECK(DllCanUnloadNow() == S_FALSE);

    factory->LockServer(FALSE);
    CHECK(DllCanUnloadNow() == S_OK);

    factory->Release();
}

void ApoProcess_AppliesGainThenClamps() {
    // Builds real APO_CONNECTION_PROPERTY structures and calls APOProcess()
    // directly, bypassing LockForProcess -- m_channels defaults to 0, which
    // APOProcess resolves to 2 (see Equalizer.cpp).
    //
    // NOTE: this exercises the *current* production default of m_gain ==
    // 0.0f (see Equalizer.h's `float m_gain = 0.0f; // 80% volume`), which
    // zeroes all output regardless of input. That looks like an unintended
    // bug -- documented here via a passing test (so it's a real regression
    // guard against further silent breakage), not silently fixed, since
    // only test extraction was in scope for this pass. If m_gain's default
    // is corrected to 0.8f, this assertion will need updating to
    // CHECK_NEAR(v, 0.5f * 0.8f, ...) instead of CHECK(v == 0.0f).
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

    CHECK(outConn.u32ValidFrameCount == frames);
    for (float v : outBuf)
        CHECK(v == 0.0f);  // current (likely unintended) default: silence
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
    RUN_TEST(DllCanUnloadNow_TracksLockServerRefCount);
    RUN_TEST(ApoProcess_AppliesGainThenClamps);
    RUN_TEST(ApoProcess_ZeroInputConnectionsIsNoOp);
    RUN_TEST(ApoProcess_ZeroFrameCountSetsOutputFrameCountToZero);

    if (SUCCEEDED(hrInit))
        CoUninitialize();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
