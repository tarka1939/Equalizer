/*
 * test_wasapi_backend.cpp — Unit tests for eq::WasapiBackend, the WASAPI
 * daemon backend stub (wasapi_backend.h).
 *
 * The real Windows daemon build only compiles this header on PLATFORM_WINDOWS
 * (see daemon/CMakeLists.txt), but as of this writing wasapi_backend.h is a
 * stub with no actual WASAPI/Win32 calls in it -- Open() just logs and
 * returns false, telling users to use the APO DLL instead (see the file's
 * header comment). That makes it safe to compile and exercise on any
 * platform, which is what this test does, guarded by defining BACKEND_WASAPI
 * for this target only (see CMakeLists.txt in this directory).
 *
 * IMPORTANT: once wasapi_backend.cpp grows a real implementation with actual
 * Win32/WASAPI calls, this test target will need to move behind
 * `if(PLATFORM_WINDOWS)` like the real eq-daemon target does -- it will no
 * longer build cross-platform. Until then, this is real, currently-exercised
 * coverage, not a placeholder.
 *
 * No external test framework (see DSP/tests/test_biquad.cpp for the same
 * hand-rolled pattern used throughout this project).
 */
#define BACKEND_WASAPI
#include "../wasapi_backend.h"

#include <cstdio>
#include <cstring>

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

void WasapiBackend_OpenReturnsFalse() {
    // Documents current (stub) behavior: Open() always fails until a real
    // WASAPI implementation lands. If this ever starts returning true
    // without a corresponding real implementation, something regressed.
    eq::EqState state;
    eq::WasapiBackend backend(&state);
    CHECK(backend.Open() == false);
}

void WasapiBackend_NameIdentifiesItselfAsStub() {
    eq::EqState state;
    eq::WasapiBackend backend(&state);
    const char* name = backend.Name();
    CHECK(name != nullptr);
    CHECK(std::strstr(name, "WASAPI") != nullptr);
    CHECK(std::strstr(name, "stub") != nullptr);
}

void WasapiBackend_CloseIsSafeWithoutOpen() {
    eq::EqState state;
    eq::WasapiBackend backend(&state);
    backend.Close();  // must not crash even though Open() was never called
    backend.Close();  // and must be safe to call more than once
    CHECK(true);       // reaching here means no crash
}

void WasapiBackend_DestructorCallsCloseSafely() {
    eq::EqState state;
    {
        eq::WasapiBackend backend(&state);
        (void)backend.Open();
    }  // destructor runs here; must not crash
    CHECK(true);
}

void CreateAudioBackend_FactoryReturnsWasapiBackend() {
    eq::EqState state;
    auto backend = eq::CreateAudioBackend(&state);
    CHECK(backend != nullptr);
    CHECK(std::strstr(backend->Name(), "WASAPI") != nullptr);
    CHECK(backend->Open() == false);
}

}  // namespace

int main() {
    RUN_TEST(WasapiBackend_OpenReturnsFalse);
    RUN_TEST(WasapiBackend_NameIdentifiesItselfAsStub);
    RUN_TEST(WasapiBackend_CloseIsSafeWithoutOpen);
    RUN_TEST(WasapiBackend_DestructorCallsCloseSafely);
    RUN_TEST(CreateAudioBackend_FactoryReturnsWasapiBackend);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
