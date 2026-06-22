#include "dumper_internal.h"

#include <dbghelp.h>

#include <cwchar>
#include <string>

namespace ipd {
std::wstring BuildStatusPath(const std::wstring& dumpPath) {
    return ReplaceFileExtension(dumpPath, L".status.txt");
}

void WriteStatusFile(
    const std::wstring& dumpPath,
    DWORD dumpError,
    const std::wstring& exePath,
    DWORD exeError) {
    std::wstring statusPath = BuildStatusPath(dumpPath);
    HANDLE file = CreateFileW(
        statusPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t message[512]{};
    swprintf_s(
        message,
        L"dump_path=%s\r\nprocess_id=%lu\r\ndump_error=%lu\r\nexe_path=%s\r\nexe_error=%lu\r\n",
        dumpPath.c_str(),
        GetCurrentProcessId(),
        dumpError,
        exePath.c_str(),
        exeError);

    int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        message,
        static_cast<int>(wcslen(message)),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required > 0) {
        std::string utf8(static_cast<size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(wcslen(message)), utf8.data(), required, nullptr, nullptr);

        DWORD bytes = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &bytes, nullptr);
    }

    CloseHandle(file);
}

bool WriteDump(const std::wstring& dumpPath, DWORD flags, DWORD* error) {
    HANDLE file = CreateFileW(
        dumpPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION* exceptionInfo = nullptr;
    BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        static_cast<MINIDUMP_TYPE>(flags),
        exceptionInfo,
        nullptr,
        nullptr);

    *error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return ok == TRUE;
}

DumpContext CreateDumpContext() {
    DumpContext context{};
    context.dumpPath = BuildDumpPath();
    context.logPath = BuildLogPath(context.dumpPath);
    context.exePath = BuildReconstructedExePath(context.dumpPath);
    context.flags = ParseDumpFlags();
    context.aggressiveRead = ShouldAggressivelyReadMemory();
    return context;
}

void LogDumpContext(const DumpContext& context, bool includeProcessId, bool includeDumpFlags) {
    if (includeProcessId) {
        Log(context.logPath, L"process_id=" + std::to_wstring(GetCurrentProcessId()));
    }

    Log(context.logPath, L"dump_path=" + context.dumpPath);
    Log(context.logPath, L"exe_path=" + context.exePath);
    Log(context.logPath, L"log_path=" + context.logPath);
    if (includeDumpFlags) {
        Log(context.logPath, L"dump_flags=" + FormatHex(context.flags));
    }
    Log(context.logPath, L"aggressive_read=" + std::to_wstring(context.aggressiveRead ? 1 : 0));
}

DWORD WriteConfiguredDump(const DumpContext& context) {
    DWORD error = ERROR_SUCCESS;
    Log(context.logPath, L"Writing dump...");
    if (WriteDump(context.dumpPath, context.flags, &error)) {
        Log(context.logPath, L"Dump completed successfully.");
    } else {
        Log(
            context.logPath,
            L"Dump failed. error=" + std::to_wstring(error) + L" " + FormatWin32Error(error));
    }

    return error;
}

DWORD WriteConfiguredExe(const DumpContext& context) {
    DWORD exeError = ERROR_SUCCESS;
    ReconstructionStats exeStats{};

    if (ShouldWriteReconstructedExe()) {
        Log(context.logPath, L"Writing reconstructed EXE...");
        if (WriteReconstructedExe(context.exePath, context.logPath, context.aggressiveRead, &exeStats, &exeError)) {
            Log(context.logPath, L"Reconstructed EXE completed successfully. size=" + GetFileSizeString(context.exePath));
            Log(
                context.logPath,
                L"Reconstructed EXE zero_bytes=" + std::to_wstring(exeStats.zeroBytes) +
                    L" unreadable_bytes=" + std::to_wstring(exeStats.unreadableBytes) +
                    L" read_failure_bytes=" + std::to_wstring(exeStats.readFailureBytes) +
                    L" protect_recovered_bytes=" + std::to_wstring(exeStats.protectRecoveredBytes));
        } else {
            Log(
                context.logPath,
                L"Reconstructed EXE failed. error=" + std::to_wstring(exeError) + L" " + FormatWin32Error(exeError));
        }
    } else {
        exeError = ERROR_CANCELLED;
        Log(context.logPath, L"Skipping reconstructed EXE because IPD_WRITE_EXE is disabled.");
    }

    return exeError;
}

DWORD RunDumpWorkflow(
    const DumpContext& context,
    const wchar_t* startMessage,
    bool includeProcessId,
    bool includeDumpFlags,
    bool logStatusPath) {
    Log(context.logPath, startMessage);
    LogDumpContext(context, includeProcessId, includeDumpFlags);

    DWORD delaySeconds = ParseDumpDelaySeconds();
    if (delaySeconds > 0) {
        Log(context.logPath, L"Delaying dump start by " + std::to_wstring(delaySeconds) + L" second(s).");
        Sleep(delaySeconds * 1000);
    }

    DWORD dumpError = WriteConfiguredDump(context);
    DWORD exeError = WriteConfiguredExe(context);

    if (ShouldDumpExecutableRegions()) {
        DumpExecutableRegions(context.dumpPath, context.logPath, context.aggressiveRead);
    }

    if (ShouldDumpModules()) {
        DumpLoadedModules(context.dumpPath, context.logPath, context.aggressiveRead);
    }

    WriteStatusFile(context.dumpPath, dumpError, context.exePath, exeError);
    if (logStatusPath) {
        Log(context.logPath, L"status_path=" + BuildStatusPath(context.dumpPath));
    }

    return dumpError;
}

DWORD WINAPI DumpThread(void*) {
    EnsureConsole();

    DumpContext context = CreateDumpContext();
    DWORD error = RunDumpWorkflow(context, L"DLL loaded into process.", true, true, true);
    if (ShouldUnload() && g_module != nullptr) {
        Log(context.logPath, L"Unloading DLL.");
        FreeLibraryAndExitThread(g_module, error);
    }

    Log(context.logPath, L"Leaving DLL loaded because IPD_UNLOAD is disabled.");
    return error;
}

}  // namespace ipd

