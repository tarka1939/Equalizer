#include <windows.h>
#include "Diagnostics.h"

BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Diagnostics::DebugLog(L"DllMain: DLL_PROCESS_ATTACH");
        break;
    case DLL_PROCESS_DETACH:
        Diagnostics::DebugLog(L"DllMain: DLL_PROCESS_DETACH");
        break;
    default:
        break;
    }
    return TRUE;
}