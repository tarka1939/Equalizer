#include "Equalizer.h"

#include <windows.h>
#include <combaseapi.h>
#include <objbase.h>
#include <strsafe.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace
{
    long g_moduleRefCount = 0;

    HMODULE GetThisModule() noexcept
    {
        HMODULE hm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetThisModule),
            &hm);
        return hm;
    }

    HRESULT GetModulePath(_Out_writes_(cch) wchar_t* path, size_t cch) noexcept
    {
        if (!path || cch == 0)
            return E_INVALIDARG;

        const HMODULE hm = GetThisModule();
        if (!hm)
            return E_FAIL;

        const DWORD n = GetModuleFileNameW(hm, path, static_cast<DWORD>(cch));
        if (n == 0 || n >= cch)
            return HRESULT_FROM_WIN32(GetLastError());

        return S_OK;
    }

    HRESULT GuidToString(REFGUID guid, _Out_writes_(cch) wchar_t* buf, size_t cch) noexcept
    {
        if (!buf || cch == 0)
            return E_INVALIDARG;

        const int n = StringFromGUID2(guid, buf, static_cast<int>(cch));
        return (n > 0) ? S_OK : E_FAIL;
    }

    HRESULT SetRegStringValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* value) noexcept
    {
        HKEY hKey{};
        DWORD disp{};
        const LONG rc = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &hKey, &disp);
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);

        const wchar_t* v = value ? value : L"";;
        const DWORD cb = static_cast<DWORD>((wcslen(v) + 1) * sizeof(wchar_t));
        const LONG rc2 = RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(v), cb);
        RegCloseKey(hKey);
        if (rc2 != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc2);

        return S_OK;
    }

    HRESULT DeleteRegTree(HKEY root, const wchar_t* subKey) noexcept
    {
        const LONG rc = RegDeleteTreeW(root, subKey);
        if (rc == ERROR_FILE_NOT_FOUND)
            return S_OK;
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);
        return S_OK;
    }

    HRESULT SetRegDwordValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value) noexcept
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

    HRESULT RegisterAsAudioProcessingObject(const wchar_t* clsidStr) noexcept
    {
        // Minimal Win10/11 registration so the Audio Engine recognizes the CLSID as an APO.
        wchar_t apoKey[512]{};
        HRESULT hr = StringCchPrintfW(apoKey, _countof(apoKey),
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioEngine\\AudioProcessingObjects\\%s",
            clsidStr);
        if (FAILED(hr)) return hr;

        // Use our existing registration properties to fill friendly name/version where possible.
        APO_REG_PROPERTIES* props = nullptr;
        {
            Equalizer tmp;
            hr = tmp.GetRegistrationProperties(&props);
            if (FAILED(hr)) return hr;
        }

        hr = SetRegStringValue(HKEY_LOCAL_MACHINE, apoKey, L"FriendlyName", props ? props->szFriendlyName : L"Equalizer");
        if (FAILED(hr)) { CoTaskMemFree(props); return hr; }

        hr = SetRegStringValue(HKEY_LOCAL_MACHINE, apoKey, L"Copyright", props ? props->szCopyrightInfo : L"");
        if (FAILED(hr)) { CoTaskMemFree(props); return hr; }

        hr = SetRegDwordValue(HKEY_LOCAL_MACHINE, apoKey, L"MajorVersion", props ? props->u32MajorVersion : 1);
        if (FAILED(hr)) { CoTaskMemFree(props); return hr; }

        hr = SetRegDwordValue(HKEY_LOCAL_MACHINE, apoKey, L"MinorVersion", props ? props->u32MinorVersion : 0);
        if (FAILED(hr)) { CoTaskMemFree(props); return hr; }

        // Optional but commonly present.
        hr = SetRegDwordValue(HKEY_LOCAL_MACHINE, apoKey, L"Flags", props ? static_cast<DWORD>(props->Flags) : 0);
        CoTaskMemFree(props);
        return hr;
    }

    HRESULT UnregisterAsAudioProcessingObject(const wchar_t* clsidStr) noexcept
    {
        wchar_t apoKey[512]{};
        HRESULT hr = StringCchPrintfW(apoKey, _countof(apoKey),
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioEngine\\AudioProcessingObjects\\%s",
            clsidStr);
        if (FAILED(hr)) return hr;

        return DeleteRegTree(HKEY_LOCAL_MACHINE, apoKey);
    }

    class EqualizerClassFactory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory>
    {
    public:
        STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override
        {
            if (!ppvObject)
                return E_POINTER;
            *ppvObject = nullptr;

            if (pUnkOuter)
                return CLASS_E_NOAGGREGATION;

            auto instance = Make<Equalizer>();
            if (!instance)
                return E_OUTOFMEMORY;

            return instance.CopyTo(riid, ppvObject);
        }

        STDMETHODIMP LockServer(BOOL fLock) override
        {
            if (fLock)
                InterlockedIncrement(&g_moduleRefCount);
            else
                InterlockedDecrement(&g_moduleRefCount);
            return S_OK;
        }
    };
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    if (rclsid != CLSID_Equalizer)
        return CLASS_E_CLASSNOTAVAILABLE;

    // Trace COM activation attempts.
    {
        wchar_t buf[128]{};
        StringCchPrintfW(buf, _countof(buf), L"DllGetClassObject: requested");
        OutputDebugStringW(L"[EqualizerAPO] ");
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\r\n");

        // Avoid including Diagnostics.h here; keep it self-contained.
        HANDLE h = CreateFileW(L"C:\\driver\\eq_apo.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            SetFilePointer(h, 0, nullptr, FILE_END);
            wchar_t line[256]{};
            wsprintfW(line, L"%lu %s\r\n", GetCurrentProcessId(), buf);
            DWORD bytes = 0;
            WriteFile(h, line, static_cast<DWORD>(lstrlenW(line) * sizeof(wchar_t)), &bytes, nullptr);
            CloseHandle(h);
        }
    }

    auto factory = Make<EqualizerClassFactory>();
    if (!factory)
        return E_OUTOFMEMORY;

    return factory.CopyTo(riid, ppv);
}

STDAPI DllCanUnloadNow(void)
{
    return (g_moduleRefCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer(void)
{
    wchar_t clsidStr[64]{};
    HRESULT hr = GuidToString(CLSID_Equalizer, clsidStr, _countof(clsidStr));
    if (FAILED(hr)) return hr;

    wchar_t modulePath[MAX_PATH]{};
    hr = GetModulePath(modulePath, _countof(modulePath));
    if (FAILED(hr)) return hr;

    wchar_t clsidKey[256]{};
    hr = StringCchPrintfW(clsidKey, _countof(clsidKey), L"Software\\Classes\\CLSID\\%s", clsidStr);
    if (FAILED(hr)) return hr;

    wchar_t inprocKey[300]{};
    hr = StringCchPrintfW(inprocKey, _countof(inprocKey), L"%s\\InprocServer32", clsidKey);
    if (FAILED(hr)) return hr;

    hr = SetRegStringValue(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"Equalizer");
    if (FAILED(hr)) return hr;

    hr = SetRegStringValue(HKEY_LOCAL_MACHINE, inprocKey, nullptr, modulePath);
    if (FAILED(hr)) return hr;

    hr = SetRegStringValue(HKEY_LOCAL_MACHINE, inprocKey, L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;

    // Register under Audio Engine APO catalog.
    hr = RegisterAsAudioProcessingObject(clsidStr);
    if (FAILED(hr)) return hr;

    return S_OK;
}

STDAPI DllUnregisterServer(void)
{
    wchar_t clsidStr[64]{};
    HRESULT hr = GuidToString(CLSID_Equalizer, clsidStr, _countof(clsidStr));
    if (FAILED(hr)) return hr;

    // Best-effort: remove APO catalog entry first.
    (void)UnregisterAsAudioProcessingObject(clsidStr);

    wchar_t clsidKey[256]{};
    hr = StringCchPrintfW(clsidKey, _countof(clsidKey), L"Software\\Classes\\CLSID\\%s", clsidStr);
    if (FAILED(hr)) return hr;

    return DeleteRegTree(HKEY_LOCAL_MACHINE, clsidKey);
}