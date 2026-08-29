#pragma once

#include <windows.h>

// File logging is OFF unless this DLL is built with
// EQUALIZER_ENABLE_FILE_LOG defined.
//
// Why it defaults off: this code runs inside audiodg.exe, the Windows audio
// engine's protected, restricted-token host process. Appending to a hardcoded
// path (it was C:\driver\eq_apo.txt) from there is inappropriate on three
// counts -- the path is not writable under that token so every call silently
// failed, the location is arbitrary and outside any per-user or per-app
// directory, and it did an open/seek/write/close on paths as hot as every COM
// activation and every LockForProcess. DebugLog() below is the right default:
// it goes to an attached debugger (or DebugView) and costs nothing otherwise.
#ifndef EQUALIZER_LOG_PATH
#  define EQUALIZER_LOG_PATH L"C:\\driver\\eq_apo.txt"
#endif

namespace Diagnostics
{
    // Default sink for AppendFileLine(). Only consulted when
    // EQUALIZER_ENABLE_FILE_LOG is defined at build time.
    inline constexpr const wchar_t* kDefaultLogPath = EQUALIZER_LOG_PATH;

    inline void DebugLog(const wchar_t* msg) noexcept
    {
        if (!msg)
            return;

        OutputDebugStringW(L"[EqualizerAPO] ");
        OutputDebugStringW(msg);
        OutputDebugStringW(L"\r\n");
    }

    inline void AppendFileLine([[maybe_unused]] const wchar_t* path,
                               [[maybe_unused]] const wchar_t* msg) noexcept
    {
#ifdef EQUALIZER_ENABLE_FILE_LOG
        if (!path || !msg)
            return;

        HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;

        // Move to end explicitly (some FS drivers ignore FILE_APPEND_DATA semantics).
        SetFilePointer(h, 0, nullptr, FILE_END);

        wchar_t line[512]{};
        wsprintfW(line, L"%lu %s\r\n", GetCurrentProcessId(), msg);

        DWORD bytes = 0;
        WriteFile(h, line, static_cast<DWORD>(lstrlenW(line) * sizeof(wchar_t)), &bytes, nullptr);
        CloseHandle(h);
#endif
    }
}
