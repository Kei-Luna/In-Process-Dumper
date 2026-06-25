#include "dumper_internal.h"

#include <windows.h>

extern "C" __declspec(dllexport) DWORD WINAPI DumpCurrentProcess() {
    ipd::EnsureConsole();

    ipd::DumpContext context = ipd::CreateDumpContext();
    return ipd::RunDumpWorkflow(context, L"DumpCurrentProcess called.", false, false, false);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        OutputDebugStringW(L"[IPD] DLL_PROCESS_ATTACH\r\n");
        ipd::g_module = module;
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, ipd::DumpThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
