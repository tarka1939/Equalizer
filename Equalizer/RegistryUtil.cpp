#include "RegistryUtil.h"
#include <strsafe.h>

namespace RegistryUtil
{
    HRESULT GuidToString(REFGUID guid, _Out_writes_(cch) wchar_t* buf, size_t cch) noexcept
    {
        if (!buf || cch == 0)
            return E_INVALIDARG;

        const int n = StringFromGUID2(guid, buf, static_cast<int>(cch));
        return (n > 0) ? S_OK : E_FAIL;
    }

    HRESULT SetStringValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* value) noexcept
    {
        HKEY hKey{};
        DWORD disp{};
        const LONG rc = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &hKey, &disp);
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);

        const wchar_t* v = value ? value : L"";
        const DWORD cb = static_cast<DWORD>((wcslen(v) + 1) * sizeof(wchar_t));
        const LONG rc2 = RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(v), cb);
        RegCloseKey(hKey);
        if (rc2 != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc2);

        return S_OK;
    }

    HRESULT SetDwordValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value) noexcept
    {
        HKEY hKey{};
        DWORD disp{};
        const LONG rc = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &hKey, &disp);
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);

        const LONG rc2 = RegSetValueExW(hKey, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(hKey);
        if (rc2 != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc2);

        return S_OK;
    }

    HRESULT DeleteTree(HKEY root, const wchar_t* subKey) noexcept
    {
        const LONG rc = RegDeleteTreeW(root, subKey);
        if (rc == ERROR_FILE_NOT_FOUND)
            return S_OK;
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);
        return S_OK;
    }

    HRESULT RegisterApoCatalogEntry(HKEY root, const wchar_t* catalogKeyPrefix, const wchar_t* clsidStr,
                                     const wchar_t* friendlyName, const wchar_t* copyrightInfo,
                                     DWORD majorVersion, DWORD minorVersion, DWORD flags) noexcept
    {
        if (!catalogKeyPrefix || !clsidStr)
            return E_INVALIDARG;

        wchar_t apoKey[512]{};
        HRESULT hr = StringCchPrintfW(apoKey, _countof(apoKey), L"%s\\%s", catalogKeyPrefix, clsidStr);
        if (FAILED(hr)) return hr;

        hr = SetStringValue(root, apoKey, L"FriendlyName", friendlyName ? friendlyName : L"Equalizer");
        if (FAILED(hr)) return hr;

        hr = SetStringValue(root, apoKey, L"Copyright", copyrightInfo ? copyrightInfo : L"");
        if (FAILED(hr)) return hr;

        hr = SetDwordValue(root, apoKey, L"MajorVersion", majorVersion);
        if (FAILED(hr)) return hr;

        hr = SetDwordValue(root, apoKey, L"MinorVersion", minorVersion);
        if (FAILED(hr)) return hr;

        // Optional but commonly present.
        hr = SetDwordValue(root, apoKey, L"Flags", flags);
        return hr;
    }

    HRESULT UnregisterApoCatalogEntry(HKEY root, const wchar_t* catalogKeyPrefix, const wchar_t* clsidStr) noexcept
    {
        if (!catalogKeyPrefix || !clsidStr)
            return E_INVALIDARG;

        wchar_t apoKey[512]{};
        HRESULT hr = StringCchPrintfW(apoKey, _countof(apoKey), L"%s\\%s", catalogKeyPrefix, clsidStr);
        if (FAILED(hr)) return hr;

        return DeleteTree(root, apoKey);
    }
}
