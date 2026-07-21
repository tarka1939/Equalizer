#include "Equalizer.h"
#include "RegistryUtil.h"

#include <windows.h>
#include <combaseapi.h>
#include <objbase.h>
#include <strsafe.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace
{
    // Real APO catalog location under HKLM. Kept as a named constant (rather
    // than inlined into RegisterAsAudioProcessingObject below) so tests can
    // see, at a glance, that production always targets this path while
    // RegistryUtil's functions themselves take the root/prefix as arguments
    // and are exercised against a scratch key in
    // Equalizer/tests/test_registry_util.cpp.
    constexpr wchar_t kApoCatalogKeyPrefix[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioEngine\\AudioProcessingObjects";

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

    HRESULT RegisterAsAudioProcessingObject(const wchar_t* clsidStr) noexcept
    {
        // Use our existing registration properties to fill friendly name/version where possible.
        APO_REG_PROPERTIES* props = nullptr;
        HRESULT hr;
        {
            Equalizer tmp;
            hr = tmp.GetRegistrationProperties(&props);
            if (FAILED(hr)) return hr;
        }

        hr = RegistryUtil::RegisterApoCatalogEntry(HKEY_LOCAL_MACHINE, kApoCatalogKeyPrefix, clsidStr,
            props ? props->szFriendlyName : L"Equalizer",
            props ? props->szCopyrightInfo : L"",
            props ? props->u32MajorVersion : 1,
            props ? props->u32MinorVersion : 0,
            props ? static_cast<DWORD>(props->Flags) : 0);
        CoTaskMemFree(props);
        return hr;
    }

    HRESULT UnregisterAsAudioProcessingObject(const wchar_t* clsidStr) noexcept
    {
        return RegistryUtil::UnregisterApoCatalogEntry(HKEY_LOCAL_MACHINE, kApoCatalogKeyPrefix, clsidStr);
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
    HRESULT hr = RegistryUtil::GuidToString(CLSID_Equalizer, clsidStr, _countof(clsidStr));
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

    hr = RegistryUtil::SetStringValue(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"Equalizer");
    if (FAILED(hr)) return hr;

    hr = RegistryUtil::SetStringValue(HKEY_LOCAL_MACHINE, inprocKey, nullptr, modulePath);
    if (FAILED(hr)) return hr;

    hr = RegistryUtil::SetStringValue(HKEY_LOCAL_MACHINE, inprocKey, L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;

    // Register under Audio Engine APO catalog.
    hr = RegisterAsAudioProcessingObject(clsidStr);
    if (FAILED(hr)) return hr;

    return S_OK;
}

STDAPI DllUnregisterServer(void)
{
    wchar_t clsidStr[64]{};
    HRESULT hr = RegistryUtil::GuidToString(CLSID_Equalizer, clsidStr, _countof(clsidStr));
    if (FAILED(hr)) return hr;

    // Best-effort: remove APO catalog entry first.
    (void)UnregisterAsAudioProcessingObject(clsidStr);

    wchar_t clsidKey[256]{};
    hr = StringCchPrintfW(clsidKey, _countof(clsidKey), L"Software\\Classes\\CLSID\\%s", clsidStr);
    if (FAILED(hr)) return hr;

    return RegistryUtil::DeleteTree(HKEY_LOCAL_MACHINE, clsidKey);
}