#include "dumper_internal.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ipd {
namespace {

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct NtObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct NtIoStatusBlock {
    union {
        LONG Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

using CreateFileWFn = HANDLE(WINAPI*)(
    LPCWSTR,
    DWORD,
    DWORD,
    LPSECURITY_ATTRIBUTES,
    DWORD,
    DWORD,
    HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(
    LPCSTR,
    DWORD,
    DWORD,
    LPSECURITY_ATTRIBUTES,
    DWORD,
    DWORD,
    HANDLE);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using NtCreateFileFn = LONG(WINAPI*)(
    PHANDLE,
    ACCESS_MASK,
    NtObjectAttributes*,
    NtIoStatusBlock*,
    PLARGE_INTEGER,
    ULONG,
    ULONG,
    ULONG,
    ULONG,
    PVOID,
    ULONG);
using NtOpenFileFn = LONG(WINAPI*)(PHANDLE, ACCESS_MASK, NtObjectAttributes*, NtIoStatusBlock*, ULONG, ULONG);
using NtReadFileFn = LONG(WINAPI*)(
    HANDLE,
    HANDLE,
    PVOID,
    PVOID,
    NtIoStatusBlock*,
    PVOID,
    ULONG,
    PLARGE_INTEGER,
    PULONG);

CreateFileWFn g_originalCreateFileW = nullptr;
CreateFileAFn g_originalCreateFileA = nullptr;
ReadFileFn g_originalReadFile = nullptr;
NtCreateFileFn g_originalNtCreateFile = nullptr;
NtOpenFileFn g_originalNtOpenFile = nullptr;
NtReadFileFn g_originalNtReadFile = nullptr;
NtCreateFileFn g_trampolineNtCreateFile = nullptr;
NtOpenFileFn g_trampolineNtOpenFile = nullptr;
NtReadFileFn g_trampolineNtReadFile = nullptr;
std::once_flag g_installOnce;
std::mutex g_watchMutex;
std::vector<HANDLE> g_metadataHandles;
std::wstring g_logPath;

struct WatchedBuffer {
    const BYTE* base = nullptr;
    DWORD size = 0;
};

std::vector<WatchedBuffer> g_watchedBuffers;

DWORD ReadLe32Local(const BYTE* data) {
    return static_cast<DWORD>(data[0]) |
           (static_cast<DWORD>(data[1]) << 8) |
           (static_cast<DWORD>(data[2]) << 16) |
           (static_cast<DWORD>(data[3]) << 24);
}

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::wstring ToLowerWide(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return value;
}

bool IsMetadataPath(LPCWSTR path) {
    if (path == nullptr) {
        return false;
    }

    std::wstring lower = ToLowerWide(path);
    return lower.find(L"global-metadata.dat") != std::wstring::npos;
}

bool IsMetadataPath(LPCSTR path) {
    if (path == nullptr) {
        return false;
    }

    std::string lower = ToLowerAscii(path);
    return lower.find("global-metadata.dat") != std::string::npos;
}

bool IsMetadataPath(NtObjectAttributes* objectAttributes) {
    if (objectAttributes == nullptr ||
        objectAttributes->ObjectName == nullptr ||
        objectAttributes->ObjectName->Buffer == nullptr ||
        objectAttributes->ObjectName->Length == 0) {
        return false;
    }

    std::wstring path(
        objectAttributes->ObjectName->Buffer,
        objectAttributes->ObjectName->Length / sizeof(wchar_t));
    return ToLowerWide(path).find(L"global-metadata.dat") != std::wstring::npos;
}

void AddMetadataHandle(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_watchMutex);
    if (std::find(g_metadataHandles.begin(), g_metadataHandles.end(), handle) == g_metadataHandles.end()) {
        g_metadataHandles.push_back(handle);
        Log(g_logPath, L"Unity metadata file handle watched. count=" + std::to_wstring(g_metadataHandles.size()));
    }
}

bool IsMetadataHandle(HANDLE handle) {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    return std::find(g_metadataHandles.begin(), g_metadataHandles.end(), handle) != g_metadataHandles.end();
}

void AddWatchedBuffer(const void* base, DWORD size) {
    if (base == nullptr || size < 0x100) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_watchMutex);
    const BYTE* bytes = static_cast<const BYTE*>(base);
    for (const auto& watched : g_watchedBuffers) {
        if (watched.base == bytes && watched.size == size) {
            return;
        }
    }

    g_watchedBuffers.push_back({bytes, size});
    Log(
        g_logPath,
        L"Unity metadata read buffer watched. base=" + FormatHexPtr(bytes) +
            L" size=" + std::to_wstring(size) +
            L" count=" + std::to_wstring(g_watchedBuffers.size()));
}

HANDLE WINAPI HookedCreateFileW(
    LPCWSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES securityAttributes,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileW(
        fileName,
        desiredAccess,
        shareMode,
        securityAttributes,
        creationDisposition,
        flagsAndAttributes,
        templateFile);
    if (IsMetadataPath(fileName)) {
        AddMetadataHandle(handle);
    }
    return handle;
}

HANDLE WINAPI HookedCreateFileA(
    LPCSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES securityAttributes,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileA(
        fileName,
        desiredAccess,
        shareMode,
        securityAttributes,
        creationDisposition,
        flagsAndAttributes,
        templateFile);
    if (IsMetadataPath(fileName)) {
        AddMetadataHandle(handle);
    }
    return handle;
}

BOOL WINAPI HookedReadFile(HANDLE file, LPVOID buffer, DWORD bytesToRead, LPDWORD bytesRead, LPOVERLAPPED overlapped) {
    BOOL ok = g_originalReadFile(file, buffer, bytesToRead, bytesRead, overlapped);
    DWORD read = bytesRead != nullptr ? *bytesRead : bytesToRead;
    if (ok && IsMetadataHandle(file)) {
        AddWatchedBuffer(buffer, read);
    }
    return ok;
}

LONG WINAPI HookedNtCreateFile(
    PHANDLE fileHandle,
    ACCESS_MASK desiredAccess,
    NtObjectAttributes* objectAttributes,
    NtIoStatusBlock* ioStatusBlock,
    PLARGE_INTEGER allocationSize,
    ULONG fileAttributes,
    ULONG shareAccess,
    ULONG createDisposition,
    ULONG createOptions,
    PVOID eaBuffer,
    ULONG eaLength) {
    NtCreateFileFn original = g_trampolineNtCreateFile != nullptr ? g_trampolineNtCreateFile : g_originalNtCreateFile;
    LONG status = original(
        fileHandle,
        desiredAccess,
        objectAttributes,
        ioStatusBlock,
        allocationSize,
        fileAttributes,
        shareAccess,
        createDisposition,
        createOptions,
        eaBuffer,
        eaLength);
    if (status >= 0 && fileHandle != nullptr && IsMetadataPath(objectAttributes)) {
        AddMetadataHandle(*fileHandle);
    }
    return status;
}

LONG WINAPI HookedNtOpenFile(
    PHANDLE fileHandle,
    ACCESS_MASK desiredAccess,
    NtObjectAttributes* objectAttributes,
    NtIoStatusBlock* ioStatusBlock,
    ULONG shareAccess,
    ULONG openOptions) {
    NtOpenFileFn original = g_trampolineNtOpenFile != nullptr ? g_trampolineNtOpenFile : g_originalNtOpenFile;
    LONG status = original(fileHandle, desiredAccess, objectAttributes, ioStatusBlock, shareAccess, openOptions);
    if (status >= 0 && fileHandle != nullptr && IsMetadataPath(objectAttributes)) {
        AddMetadataHandle(*fileHandle);
    }
    return status;
}

LONG WINAPI HookedNtReadFile(
    HANDLE fileHandle,
    HANDLE event,
    PVOID apcRoutine,
    PVOID apcContext,
    NtIoStatusBlock* ioStatusBlock,
    PVOID buffer,
    ULONG length,
    PLARGE_INTEGER byteOffset,
    PULONG key) {
    NtReadFileFn original = g_trampolineNtReadFile != nullptr ? g_trampolineNtReadFile : g_originalNtReadFile;
    LONG status = original(
        fileHandle,
        event,
        apcRoutine,
        apcContext,
        ioStatusBlock,
        buffer,
        length,
        byteOffset,
        key);
    if (status >= 0 && IsMetadataHandle(fileHandle)) {
        ULONG read = length;
        if (ioStatusBlock != nullptr && ioStatusBlock->Information != 0 && ioStatusBlock->Information <= length) {
            read = static_cast<ULONG>(ioStatusBlock->Information);
        }
        AddWatchedBuffer(buffer, read);
    }
    return status;
}

bool InstallAbsoluteJump(void* target, void* replacement, void** trampolineOut) {
#if defined(_M_X64)
    constexpr SIZE_T kPatchSize = 16;
    auto* targetBytes = static_cast<BYTE*>(target);
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, kPatchSize + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        return false;
    }

    std::memcpy(trampoline, targetBytes, kPatchSize);
    trampoline[kPatchSize + 0] = 0x48;
    trampoline[kPatchSize + 1] = 0xB8;
    *reinterpret_cast<void**>(trampoline + kPatchSize + 2) = targetBytes + kPatchSize;
    trampoline[kPatchSize + 10] = 0xFF;
    trampoline[kPatchSize + 11] = 0xE0;

    DWORD oldProtect = 0;
    if (!VirtualProtect(targetBytes, kPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    targetBytes[0] = 0x48;
    targetBytes[1] = 0xB8;
    *reinterpret_cast<void**>(targetBytes + 2) = replacement;
    targetBytes[10] = 0xFF;
    targetBytes[11] = 0xE0;
    for (SIZE_T i = 12; i < kPatchSize; ++i) {
        targetBytes[i] = 0x90;
    }

    FlushInstructionCache(GetCurrentProcess(), targetBytes, kPatchSize);
    DWORD ignored = 0;
    VirtualProtect(targetBytes, kPatchSize, oldProtect, &ignored);
    *trampolineOut = trampoline;
    return true;
#else
    (void)target;
    (void)replacement;
    (void)trampolineOut;
    return false;
#endif
}

DWORD InstallInlineNtHooks() {
    DWORD installed = 0;
    if (g_originalNtCreateFile != nullptr && g_trampolineNtCreateFile == nullptr) {
        void* trampoline = nullptr;
        if (InstallAbsoluteJump(reinterpret_cast<void*>(g_originalNtCreateFile), reinterpret_cast<void*>(HookedNtCreateFile), &trampoline)) {
            g_trampolineNtCreateFile = reinterpret_cast<NtCreateFileFn>(trampoline);
            ++installed;
        }
    }
    if (g_originalNtOpenFile != nullptr && g_trampolineNtOpenFile == nullptr) {
        void* trampoline = nullptr;
        if (InstallAbsoluteJump(reinterpret_cast<void*>(g_originalNtOpenFile), reinterpret_cast<void*>(HookedNtOpenFile), &trampoline)) {
            g_trampolineNtOpenFile = reinterpret_cast<NtOpenFileFn>(trampoline);
            ++installed;
        }
    }
    if (g_originalNtReadFile != nullptr && g_trampolineNtReadFile == nullptr) {
        void* trampoline = nullptr;
        if (InstallAbsoluteJump(reinterpret_cast<void*>(g_originalNtReadFile), reinterpret_cast<void*>(HookedNtReadFile), &trampoline)) {
            g_trampolineNtReadFile = reinterpret_cast<NtReadFileFn>(trampoline);
            ++installed;
        }
    }

    return installed;
}

bool PatchIatEntry(HMODULE module, const char* functionName, void* replacement) {
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        return false;
    }

    bool patched = false;
    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
        if (imports->OriginalFirstThunk == 0) {
            thunk = iat;
        }

        for (; thunk->u1.AddressOfData != 0; ++thunk, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) {
                continue;
            }

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), functionName) != 0) {
                continue;
            }

            DWORD oldProtect = 0;
            if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &oldProtect)) {
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                DWORD ignored = 0;
                VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), oldProtect, &ignored);
                patched = true;
            }
        }
    }

    return patched;
}

DWORD PatchLoadedModules() {
    std::wstring processName = ToLowerWide(GetProcessName());
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD patchedModules = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            HMODULE module = entry.hModule;
            if (module == g_module) {
                continue;
            }

            std::wstring moduleName = ToLowerWide(entry.szModule);
            bool targetModule =
                moduleName == processName ||
                moduleName == L"unityplayer.dll" ||
                moduleName == L"gameassembly.dll" ||
                moduleName == L"libil2cpp.dll";
            if (!targetModule) {
                continue;
            }

            bool patched = false;
            patched |= PatchIatEntry(module, "CreateFileW", reinterpret_cast<void*>(HookedCreateFileW));
            patched |= PatchIatEntry(module, "CreateFileA", reinterpret_cast<void*>(HookedCreateFileA));
            patched |= PatchIatEntry(module, "ReadFile", reinterpret_cast<void*>(HookedReadFile));
            patched |= PatchIatEntry(module, "NtCreateFile", reinterpret_cast<void*>(HookedNtCreateFile));
            patched |= PatchIatEntry(module, "NtOpenFile", reinterpret_cast<void*>(HookedNtOpenFile));
            patched |= PatchIatEntry(module, "NtReadFile", reinterpret_cast<void*>(HookedNtReadFile));
            if (patched) {
                ++patchedModules;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return patchedModules;
}

bool TryGetWatchedUnityMetadataSize(const BYTE* candidate, DWORD maxSize, DWORD* metadataSize, DWORD* version, bool* repairMagic) {
    constexpr DWORD kUnityMetadataMagic = 0xFAB11BAF;
    constexpr DWORD kMaxMetadataSize = 512 * 1024 * 1024;
    if (maxSize < 0x100) {
        return false;
    }

    DWORD magic = ReadLe32Local(candidate);
    if (magic != kUnityMetadataMagic && magic != 0) {
        return false;
    }

    DWORD metadataVersion = ReadLe32Local(candidate + 4);
    if (metadataVersion < 16 || metadataVersion > 31) {
        return false;
    }

    DWORD firstOffset = ReadLe32Local(candidate + 8);
    if (firstOffset < 0x20 || firstOffset > maxSize || firstOffset > 4096 || (firstOffset % 4) != 0) {
        return false;
    }

    DWORD pairBytes = firstOffset - 8;
    if ((pairBytes % 8) != 0 || pairBytes < 8 * 4) {
        return false;
    }

    DWORD pairCount = pairBytes / 8;
    DWORD maxEnd = firstOffset;
    DWORD nonEmptyPairs = 0;
    for (DWORD i = 0; i < pairCount; ++i) {
        const BYTE* pair = candidate + 8 + (i * 8);
        DWORD offset = ReadLe32Local(pair);
        DWORD size = ReadLe32Local(pair + 4);
        if (size == 0) {
            continue;
        }
        if (offset < firstOffset || offset > kMaxMetadataSize || size > kMaxMetadataSize - offset) {
            return false;
        }

        ++nonEmptyPairs;
        DWORD end = offset + size;
        if (end > maxEnd) {
            maxEnd = end;
        }
    }

    if (nonEmptyPairs < 4 || maxEnd < 0x100 || maxEnd > maxSize) {
        return false;
    }

    *metadataSize = maxEnd;
    *version = metadataVersion;
    *repairMagic = magic == 0;
    return true;
}

}  // namespace

void InstallUnityMetadataFileWatch(const std::wstring& logPath) {
    std::call_once(g_installOnce, [&]() {
        g_logPath = logPath;
        g_originalCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW"));
        g_originalCreateFileA = reinterpret_cast<CreateFileAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileA"));
        g_originalReadFile = reinterpret_cast<ReadFileFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadFile"));
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        g_originalNtCreateFile = reinterpret_cast<NtCreateFileFn>(GetProcAddress(ntdll, "NtCreateFile"));
        g_originalNtOpenFile = reinterpret_cast<NtOpenFileFn>(GetProcAddress(ntdll, "NtOpenFile"));
        g_originalNtReadFile = reinterpret_cast<NtReadFileFn>(GetProcAddress(ntdll, "NtReadFile"));
        if (g_originalCreateFileW == nullptr ||
            g_originalCreateFileA == nullptr ||
            g_originalReadFile == nullptr ||
            g_originalNtCreateFile == nullptr ||
            g_originalNtOpenFile == nullptr ||
            g_originalNtReadFile == nullptr) {
            Log(logPath, L"Unity metadata file watch install failed: missing kernel32/ntdll exports.");
            return;
        }

        DWORD patchedModules = PatchLoadedModules();
        DWORD inlineHooks = ShouldInlineWatchUnityMetadataFile() ? InstallInlineNtHooks() : 0;
        Log(
            logPath,
            L"Unity metadata file watch installed. patched_modules=" +
                std::to_wstring(patchedModules) +
                L" inline_nt_hooks=" + std::to_wstring(inlineHooks));
    });
}

void RefreshUnityMetadataFileWatch(const std::wstring& logPath) {
    g_logPath = logPath;
    if (g_originalCreateFileW == nullptr || g_originalCreateFileA == nullptr || g_originalReadFile == nullptr) {
        return;
    }

    DWORD patchedModules = PatchLoadedModules();
    if (patchedModules != 0) {
        Log(
            logPath,
            L"Unity metadata file watch refreshed. patched_modules=" + std::to_wstring(patchedModules));
    }
}

DWORD DumpWatchedUnityMetadataBuffers(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead) {
    std::vector<WatchedBuffer> buffers;
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        buffers = g_watchedBuffers;
    }

    if (buffers.empty()) {
        return 0;
    }

    std::wstring outputDir = BuildSidecarDirectory(dumpPath, L".unity_metadata");
    CreateDirectoryW(outputDir.c_str(), nullptr);

    DWORD dumped = 0;
    for (const auto& buffer : buffers) {
        DWORD metadataSize = 0;
        DWORD version = 0;
        bool repairMagic = false;
        if (!TryGetWatchedUnityMetadataSize(buffer.base, buffer.size, &metadataSize, &version, &repairMagic)) {
            std::array<BYTE, 32> head{};
            SIZE_T bytesRead = 0;
            if (ReadMemoryBlock(buffer.base, head.data(), static_cast<DWORD>(head.size()), aggressiveRead, &bytesRead) &&
                bytesRead >= 16) {
                Log(
                    logPath,
                    L"unity_metadata_watch rejected base=" + FormatHexPtr(buffer.base) +
                        L" max_size=" + std::to_wstring(buffer.size) +
                        L" dword0=" + FormatHex(ReadLe32Local(head.data())) +
                        L" dword1=" + FormatHex(ReadLe32Local(head.data() + 4)) +
                        L" dword2=" + FormatHex(ReadLe32Local(head.data() + 8)) +
                        L" dword3=" + FormatHex(ReadLe32Local(head.data() + 12)));
            } else {
                Log(
                    logPath,
                    L"unity_metadata_watch rejected unreadable base=" + FormatHexPtr(buffer.base) +
                        L" max_size=" + std::to_wstring(buffer.size));
            }
            continue;
        }

        std::wstring outputPath = BuildUniqueFilePath(outputDir, L"global-metadata_watch.dat");
        ReconstructionStats stats{};
        DWORD error = ERROR_SUCCESS;
        HANDLE file = CreateFileW(
            outputPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            Log(logPath, L"unity_metadata_watch failed_write path=" + outputPath + L" error=" + std::to_wstring(GetLastError()));
            continue;
        }

        bool ok = WriteImageMemoryAt(file, 0, buffer.base, metadataSize, aggressiveRead, &stats, &error);
        if (ok && repairMagic) {
            constexpr DWORD kUnityMetadataMagic = 0xFAB11BAF;
            DWORD written = 0;
            LARGE_INTEGER zero{};
            SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
            ok = WriteFile(file, &kUnityMetadataMagic, sizeof(kUnityMetadataMagic), &written, nullptr) == TRUE &&
                 written == sizeof(kUnityMetadataMagic);
            if (!ok) {
                error = GetLastError();
            }
        }
        CloseHandle(file);
        if (ok) {
            ++dumped;
            Log(
                logPath,
                L"unity_metadata_watch path=" + outputPath +
                    L" base=" + FormatHexPtr(buffer.base) +
                    L" size=" + std::to_wstring(metadataSize) +
                    L" version=" + std::to_wstring(version) +
                    L" repaired_magic=" + std::to_wstring(repairMagic ? 1 : 0));
        } else {
            Log(logPath, L"unity_metadata_watch failed_write path=" + outputPath + L" error=" + std::to_wstring(error));
        }
    }

    return dumped;
}

}  // namespace ipd
