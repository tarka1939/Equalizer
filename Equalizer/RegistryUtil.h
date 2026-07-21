#pragma once
/*
 * RegistryUtil.h — small, testable registry helpers used by ComExports.cpp
 * (DllRegisterServer / DllUnregisterServer / the Audio Engine APO catalog
 * entry).
 *
 * Pulled out of the anonymous namespace in ComExports.cpp so tests can
 * exercise them against a scratch key (e.g. HKEY_CURRENT_USER) instead of
 * mutating the real HKEY_LOCAL_MACHINE registration. See
 * Equalizer/tests/test_registry_util.cpp -- Windows-only, builds via
 * EqualizerApoTests.vcxproj, not the cross-platform CMake build (the
 * registry APIs used here don't exist off Windows).
 */
#include <windows.h>
#include <objbase.h>

namespace RegistryUtil
{
    // Formats `guid` as "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" into buf.
    HRESULT GuidToString(REFGUID guid, _Out_writes_(cch) wchar_t* buf, size_t cch) noexcept;

    // Creates subKey under root (if needed) and sets a REG_SZ value.
    // valueName == nullptr sets the key's default (unnamed) value.
    HRESULT SetStringValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* value) noexcept;

    // Creates subKey under root (if needed) and sets a REG_DWORD value.
    HRESULT SetDwordValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value) noexcept;

    // Recursively deletes subKey under root. Treats "not found" as success
    // (idempotent -- matches DllUnregisterServer's best-effort cleanup
    // semantics).
    HRESULT DeleteTree(HKEY root, const wchar_t* subKey) noexcept;

    // Registers a CLSID under the Windows Audio Engine's APO catalog, i.e.
    // "<catalogKeyPrefix>\<clsidStr>", rooted at `root` so tests can point
    // this at a scratch hive location instead of the real catalog path
    // (production passes root=HKEY_LOCAL_MACHINE, catalogKeyPrefix=
    // "SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\
    // AudioProcessingObjects").
    HRESULT RegisterApoCatalogEntry(HKEY root, const wchar_t* catalogKeyPrefix, const wchar_t* clsidStr,
                                     const wchar_t* friendlyName, const wchar_t* copyrightInfo,
                                     DWORD majorVersion, DWORD minorVersion, DWORD flags) noexcept;

    // Removes the catalog entry written by RegisterApoCatalogEntry.
    HRESULT UnregisterApoCatalogEntry(HKEY root, const wchar_t* catalogKeyPrefix, const wchar_t* clsidStr) noexcept;
}
