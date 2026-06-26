#pragma once

#include <windows.h>

#include <string>

namespace ipd {

extern HMODULE g_module;

struct ReconstructionStats {
    ULONGLONG zeroBytes = 0;
    ULONGLONG unreadableBytes = 0;
    ULONGLONG readFailureBytes = 0;
    ULONGLONG protectRecoveredBytes = 0;
};

struct DumpContext {
    std::wstring dumpPath;
    std::wstring logPath;
    std::wstring exePath;
    DWORD flags = 0;
    bool aggressiveRead = false;
};

void EnsureConsole();

std::wstring FormatWin32Error(DWORD error);
std::wstring GetProcessName();
std::wstring SanitizeFileName(std::wstring name);
std::wstring FormatHex(DWORD value);
std::wstring FormatHexPtr(const void* value);
std::wstring GetFileSizeString(const std::wstring& path);
std::wstring StripFileExtension(std::wstring path);
std::wstring ReplaceFileExtension(const std::wstring& path, const wchar_t* extension);
std::wstring AddSuffixBeforeExtension(const std::wstring& fileName, const std::wstring& suffix);
std::wstring AddDumpSuffixBeforeExtension(const std::wstring& fileName);
std::wstring BuildSidecarDirectory(const std::wstring& dumpPath, const wchar_t* suffix);
std::wstring BuildUniqueFilePath(const std::wstring& directory, const std::wstring& fileName);

DWORD AlignUp(DWORD value, DWORD alignment);
DWORD ParseDumpFlags();
DWORD ParseDumpDelaySeconds();
DWORD ParseUnityMetadataScanSeconds();
bool IsReadableMemory(const MEMORY_BASIC_INFORMATION& mbi);
bool ReadMemoryBlock(const BYTE* source, void* buffer, DWORD size, bool aggressiveRead, SIZE_T* bytesRead);

bool ShouldUnload();
bool ShouldWriteReconstructedExe();
bool ShouldAggressivelyReadMemory();
bool ShouldDumpExecutableRegions();
bool ShouldDumpModules();
bool ShouldDumpUnityMetadata();
bool ShouldWatchUnityMetadataFile();
bool ShouldInlineWatchUnityMetadataFile();

std::wstring BuildDumpPath();
std::wstring BuildLogPath(const std::wstring& dumpPath);
std::wstring BuildReconstructedExePath(const std::wstring& dumpPath);

void Log(const std::wstring& logPath, const std::wstring& message);

bool WriteReconstructedImage(
    const BYTE* base,
    const std::wstring& imagePath,
    const std::wstring& logPath,
    const std::wstring& imageName,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error);

bool WriteReconstructedExe(
    const std::wstring& exePath,
    const std::wstring& logPath,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error);
bool WriteImageMemoryAt(
    HANDLE file,
    DWORD fileOffset,
    const BYTE* source,
    DWORD size,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error);

void DumpExecutableRegions(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead);
void DumpLoadedModules(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead);
DWORD DumpUnityMetadata(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead);
void InstallUnityMetadataFileWatch(const std::wstring& logPath);
void RefreshUnityMetadataFileWatch(const std::wstring& logPath);
DWORD DumpWatchedUnityMetadataBuffers(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead);

DumpContext CreateDumpContext();
DWORD RunDumpWorkflow(
    const DumpContext& context,
    const wchar_t* startMessage,
    bool includeProcessId,
    bool includeDumpFlags,
    bool logStatusPath);
DWORD WINAPI DumpThread(void*);

}  // namespace ipd
