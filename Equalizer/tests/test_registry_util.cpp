/*
 * test_registry_util.cpp — Unit tests for RegistryUtil (Equalizer/RegistryUtil.h),
 * the registry helpers backing DllRegisterServer / DllUnregisterServer /
 * the Audio Engine APO catalog entry in ComExports.cpp.
 *
 * Every test here targets a scratch subtree under HKEY_CURRENT_USER
 * ("Software\EqualizerApoTests\..."), never the real HKEY_LOCAL_MACHINE
 * paths ComExports.cpp uses in production. That means no admin rights are
 * required and nothing outside our own scratch key is touched -- the
 * scratch tree is deleted at both the start and end of main() as cleanup.
 *
 * Windows-only (Win32 registry APIs) -- builds via
 * EqualizerRegistryUtilTests.vcxproj, not the cross-platform CMake build.
 * This project could not be compiled or run in the environment that wrote
 * these tests (no Windows SDK available there) -- build and run this in
 * Visual Studio to confirm.
 *
 * No external test framework (same hand-rolled pattern as the rest of the
 * project's tests).
 */
#include "../RegistryUtil.h"

#include <cstdio>
#include <cwchar>
#include <string>

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

constexpr wchar_t kScratchRoot[] = L"Software\\EqualizerApoTests\\RegistryUtilTests";
constexpr wchar_t kScratchCatalogPrefix[] = L"Software\\EqualizerApoTests\\ApoCatalog";

void CleanupScratch() {
    RegistryUtil::DeleteTree(HKEY_CURRENT_USER, kScratchRoot);
    RegistryUtil::DeleteTree(HKEY_CURRENT_USER, kScratchCatalogPrefix);
}

std::wstring ReadStringValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY hKey{};
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return L"<key-not-found>";

    wchar_t buf[512]{};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return L"<value-not-found>";
    return std::wstring(buf);
}

DWORD ReadDwordValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD notFoundSentinel) {
    HKEY hKey{};
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return notFoundSentinel;

    DWORD value = notFoundSentinel;
    DWORD cb = sizeof(value);
    DWORD type = 0;
    RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &cb);
    RegCloseKey(hKey);
    return value;
}

bool KeyExists(HKEY root, const wchar_t* subKey) {
    HKEY hKey{};
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    RegCloseKey(hKey);
    return true;
}

// ── GuidToString ─────────────────────────────────────────────────────────────

void GuidToString_ProducesBracedUppercaseFormat() {
    // {12345678-9ABC-4DEF-8011-223344556677} -- same shape as CLSID_Equalizer.
    GUID guid{ 0x12345678, 0x9abc, 0x4def, { 0x80, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };
    wchar_t buf[64]{};
    HRESULT hr = RegistryUtil::GuidToString(guid, buf, 64);

    CHECK(SUCCEEDED(hr));
    CHECK(std::wcslen(buf) == 38);  // "{" + 36 GUID chars + "}"
    CHECK(buf[0] == L'{');
    CHECK(buf[37] == L'}');
    CHECK(std::wcsstr(buf, L"12345678-9ABC-4DEF-8011-223344556677") != nullptr);
}

void GuidToString_RejectsNullOrZeroLengthBuffer() {
    GUID guid{};
    wchar_t buf[64]{};
    CHECK(RegistryUtil::GuidToString(guid, nullptr, 64) == E_INVALIDARG);
    CHECK(RegistryUtil::GuidToString(guid, buf, 0) == E_INVALIDARG);
}

// ── SetStringValue / SetDwordValue / DeleteTree ─────────────────────────────

void SetStringValue_WritesReadableValue() {
    CleanupScratch();
    HRESULT hr = RegistryUtil::SetStringValue(HKEY_CURRENT_USER, kScratchRoot, L"Name", L"Equalizer");
    CHECK(SUCCEEDED(hr));
    CHECK(ReadStringValue(HKEY_CURRENT_USER, kScratchRoot, L"Name") == L"Equalizer");
    CleanupScratch();
}

void SetStringValue_NullValueWritesEmptyString() {
    CleanupScratch();
    HRESULT hr = RegistryUtil::SetStringValue(HKEY_CURRENT_USER, kScratchRoot, L"Name", nullptr);
    CHECK(SUCCEEDED(hr));
    CHECK(ReadStringValue(HKEY_CURRENT_USER, kScratchRoot, L"Name") == L"");
    CleanupScratch();
}

void SetDwordValue_WritesReadableValue() {
    CleanupScratch();
    HRESULT hr = RegistryUtil::SetDwordValue(HKEY_CURRENT_USER, kScratchRoot, L"MajorVersion", 42u);
    CHECK(SUCCEEDED(hr));
    CHECK(ReadDwordValue(HKEY_CURRENT_USER, kScratchRoot, L"MajorVersion", 0xFFFFFFFFu) == 42u);
    CleanupScratch();
}

void DeleteTree_RemovesKeyAndAllValues() {
    CleanupScratch();
    RegistryUtil::SetStringValue(HKEY_CURRENT_USER, kScratchRoot, L"Name", L"x");
    CHECK(KeyExists(HKEY_CURRENT_USER, kScratchRoot));

    HRESULT hr = RegistryUtil::DeleteTree(HKEY_CURRENT_USER, kScratchRoot);
    CHECK(SUCCEEDED(hr));
    CHECK(!KeyExists(HKEY_CURRENT_USER, kScratchRoot));
}

void DeleteTree_OnMissingKeyIsSuccessNotError() {
    CleanupScratch();  // ensure it doesn't exist
    HRESULT hr = RegistryUtil::DeleteTree(HKEY_CURRENT_USER, kScratchRoot);
    CHECK(hr == S_OK);  // idempotent, matches DllUnregisterServer's best-effort semantics
}

// ── RegisterApoCatalogEntry / UnregisterApoCatalogEntry ─────────────────────

void RegisterApoCatalogEntry_WritesAllExpectedValues() {
    CleanupScratch();
    const wchar_t* clsidStr = L"{TEST-CLSID}";

    HRESULT hr = RegistryUtil::RegisterApoCatalogEntry(
        HKEY_CURRENT_USER, kScratchCatalogPrefix, clsidStr,
        L"My Friendly Name", L"(c) Test", /*major*/ 3u, /*minor*/ 7u, /*flags*/ 0xABu);
    CHECK(SUCCEEDED(hr));

    wchar_t fullKey[512]{};
    wsprintfW(fullKey, L"%s\\%s", kScratchCatalogPrefix, clsidStr);

    CHECK(ReadStringValue(HKEY_CURRENT_USER, fullKey, L"FriendlyName") == L"My Friendly Name");
    CHECK(ReadStringValue(HKEY_CURRENT_USER, fullKey, L"Copyright") == L"(c) Test");
    CHECK(ReadDwordValue(HKEY_CURRENT_USER, fullKey, L"MajorVersion", 0) == 3u);
    CHECK(ReadDwordValue(HKEY_CURRENT_USER, fullKey, L"MinorVersion", 0) == 7u);
    CHECK(ReadDwordValue(HKEY_CURRENT_USER, fullKey, L"Flags", 0) == 0xABu);

    CleanupScratch();
}

void UnregisterApoCatalogEntry_RemovesWhatRegisterWrote() {
    CleanupScratch();
    const wchar_t* clsidStr = L"{TEST-CLSID-2}";
    RegistryUtil::RegisterApoCatalogEntry(HKEY_CURRENT_USER, kScratchCatalogPrefix, clsidStr,
        L"Name", L"Copy", 1u, 0u, 0u);

    wchar_t fullKey[512]{};
    wsprintfW(fullKey, L"%s\\%s", kScratchCatalogPrefix, clsidStr);
    CHECK(KeyExists(HKEY_CURRENT_USER, fullKey));

    HRESULT hr = RegistryUtil::UnregisterApoCatalogEntry(HKEY_CURRENT_USER, kScratchCatalogPrefix, clsidStr);
    CHECK(SUCCEEDED(hr));
    CHECK(!KeyExists(HKEY_CURRENT_USER, fullKey));
}

void RegisterApoCatalogEntry_MissingFriendlyNameFallsBackToDefault() {
    CleanupScratch();
    const wchar_t* clsidStr = L"{TEST-CLSID-3}";
    RegistryUtil::RegisterApoCatalogEntry(HKEY_CURRENT_USER, kScratchCatalogPrefix, clsidStr,
        /*friendlyName*/ nullptr, /*copyrightInfo*/ nullptr, 1u, 0u, 0u);

    wchar_t fullKey[512]{};
    wsprintfW(fullKey, L"%s\\%s", kScratchCatalogPrefix, clsidStr);
    CHECK(ReadStringValue(HKEY_CURRENT_USER, fullKey, L"FriendlyName") == L"Equalizer");
    CHECK(ReadStringValue(HKEY_CURRENT_USER, fullKey, L"Copyright") == L"");

    CleanupScratch();
}

}  // namespace

int main() {
    CleanupScratch();

    RUN_TEST(GuidToString_ProducesBracedUppercaseFormat);
    RUN_TEST(GuidToString_RejectsNullOrZeroLengthBuffer);
    RUN_TEST(SetStringValue_WritesReadableValue);
    RUN_TEST(SetStringValue_NullValueWritesEmptyString);
    RUN_TEST(SetDwordValue_WritesReadableValue);
    RUN_TEST(DeleteTree_RemovesKeyAndAllValues);
    RUN_TEST(DeleteTree_OnMissingKeyIsSuccessNotError);
    RUN_TEST(RegisterApoCatalogEntry_WritesAllExpectedValues);
    RUN_TEST(UnregisterApoCatalogEntry_RemovesWhatRegisterWrote);
    RUN_TEST(RegisterApoCatalogEntry_MissingFriendlyNameFallsBackToDefault);

    CleanupScratch();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
