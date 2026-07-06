#pragma once

#include <windows.h>

namespace Diagnostics
{
    inline void DebugLog(const wchar_t* msg) noexcept
    {
        if (!msg)
            return;

        OutputDebugStringW(L"[EqualizerAPO] ");
        OutputDebugStringW(msg);
        OutputDebugStringW(L"\r\n");
    }

    inline void AppendFileLine(const wchar_t* path, const wchar_t* msg) noexcept
    {
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
    }
}
