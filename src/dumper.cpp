#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <array>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr DWORD kDefaultDumpFlags =
    MiniDumpWithFullMemory |
    MiniDumpWithHandleData |
    MiniDumpWithThreadInfo |
    MiniDumpWithUnloadedModules;

HMODULE g_module = nullptr;

struct Config {
    bool loaded = false;
    std::wstring text;
};

Config g_config;

void TrimTrailingSlashes(std::wstring& path);

void EnsureConsole() {
    if (GetConsoleWindow() != nullptr) {
        return;
    }

    if (!AllocConsole()) {
        return;
    }

    SetConsoleTitleW(L"In-Process-Dumper");
    FILE* ignored = nullptr;
    freopen_s(&ignored, "CONOUT$", "w", stdout);
    freopen_s(&ignored, "CONOUT$", "w", stderr);
}

std::wstring FormatWin32Error(DWORD error) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr) {
        return L"";
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);

    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }

    return message;
}

std::wstring Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t buffer[64]{};
    swprintf_s(
        buffer,
        L"%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);

    return buffer;
}

std::wstring GetEnvVar(const wchar_t* name) {
    DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }

    std::wstring value(required, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return {};
    }

    value.resize(written);
    return value;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
    return wide;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string utf8(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), required, nullptr, nullptr);
    return utf8;
}

std::wstring GetBaseName(std::wstring path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.erase(0, slash + 1);
    }

    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        path.resize(dot);
    }

    return path.empty() ? L"process" : path;
}

std::wstring GetProcessName() {
    std::array<wchar_t, MAX_PATH> buffer{};
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return L"process";
    }

    return GetBaseName(std::wstring(buffer.data(), length));
}

std::wstring GetDirectoryName(std::wstring path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }

    if (slash == 0) {
        return path.substr(0, 1);
    }

    return path.substr(0, slash);
}

std::wstring GetDllDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    DWORD length = GetModuleFileNameW(g_module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return L".";
    }

    return GetDirectoryName(std::wstring(buffer.data(), length));
}

std::wstring GetConfigPath() {
    std::wstring directory = GetDllDirectory();
    TrimTrailingSlashes(directory);
    return directory + L"\\config.json";
}

std::wstring ReadTextFileUtf8(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return {};
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) {
        return {};
    }

    bytes.resize(read);
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf) {
        bytes.erase(0, 3);
    }

    return Utf8ToWide(bytes);
}

void LoadConfig() {
    if (g_config.loaded) {
        return;
    }

    g_config.loaded = true;
    g_config.text = ReadTextFileUtf8(GetConfigPath());
}

void SkipJsonSpace(const std::wstring& text, size_t* index) {
    while (*index < text.size() &&
           (text[*index] == L' ' || text[*index] == L'\t' || text[*index] == L'\r' || text[*index] == L'\n')) {
        ++(*index);
    }
}

bool ParseJsonString(const std::wstring& text, size_t* index, std::wstring* value) {
    if (*index >= text.size() || text[*index] != L'"') {
        return false;
    }

    ++(*index);
    std::wstring result;
    while (*index < text.size()) {
        wchar_t ch = text[(*index)++];
        if (ch == L'"') {
            *value = result;
            return true;
        }
        if (ch == L'\\' && *index < text.size()) {
            wchar_t escaped = text[(*index)++];
            switch (escaped) {
                case L'"':
                case L'\\':
                case L'/':
                    result.push_back(escaped);
                    break;
                case L'b':
                    result.push_back(L'\b');
                    break;
                case L'f':
                    result.push_back(L'\f');
                    break;
                case L'n':
                    result.push_back(L'\n');
                    break;
                case L'r':
                    result.push_back(L'\r');
                    break;
                case L't':
                    result.push_back(L'\t');
                    break;
                default:
                    result.push_back(escaped);
                    break;
            }
        } else {
            result.push_back(ch);
        }
    }

    return false;
}

bool GetConfigRawValue(const wchar_t* key, std::wstring* value) {
    LoadConfig();
    if (g_config.text.empty()) {
        return false;
    }

    std::wstring quotedKey = L"\"" + std::wstring(key) + L"\"";
    size_t pos = g_config.text.find(quotedKey);
    if (pos == std::wstring::npos) {
        return false;
    }

    pos += quotedKey.size();
    SkipJsonSpace(g_config.text, &pos);
    if (pos >= g_config.text.size() || g_config.text[pos] != L':') {
        return false;
    }

    ++pos;
    SkipJsonSpace(g_config.text, &pos);
    if (pos >= g_config.text.size()) {
        return false;
    }

    if (g_config.text[pos] == L'"') {
        return ParseJsonString(g_config.text, &pos, value);
    }

    size_t start = pos;
    while (pos < g_config.text.size() && g_config.text[pos] != L',' && g_config.text[pos] != L'}') {
        ++pos;
    }

    size_t end = pos;
    while (end > start &&
           (g_config.text[end - 1] == L' ' || g_config.text[end - 1] == L'\t' ||
            g_config.text[end - 1] == L'\r' || g_config.text[end - 1] == L'\n')) {
        --end;
    }

    *value = g_config.text.substr(start, end - start);
    return !value->empty();
}

std::wstring GetSettingString(const wchar_t* envName, const wchar_t* configName) {
    std::wstring env = GetEnvVar(envName);
    if (!env.empty()) {
        return env;
    }

    std::wstring value;
    if (GetConfigRawValue(configName, &value)) {
        return value;
    }

    return {};
}

bool ParseBoolSetting(const std::wstring& value, bool defaultValue) {
    if (value.empty()) {
        return defaultValue;
    }

    return value != L"0" &&
           value != L"false" &&
           value != L"FALSE" &&
           value != L"False" &&
           value != L"no" &&
           value != L"NO" &&
           value != L"null";
}

std::wstring GetDefaultDumpDirectory() {
    std::wstring configured = GetSettingString(L"IPD_DUMP_DIR", L"dump_dir");
    if (!configured.empty()) {
        return configured;
    }

    return GetDllDirectory();
}

void TrimTrailingSlashes(std::wstring& path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
}

std::wstring BuildDumpPath() {
    std::wstring configuredName = GetSettingString(L"IPD_DUMP_NAME", L"dump_name");
    if (!configuredName.empty()) {
        return configuredName;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t fileName[256]{};
    swprintf_s(
        fileName,
        L"%s_%lu_%04hu%02hu%02hu_%02hu%02hu%02hu.dmp",
        GetProcessName().c_str(),
        GetCurrentProcessId(),
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond);

    std::wstring directory = GetDefaultDumpDirectory();
    TrimTrailingSlashes(directory);
    return directory + L"\\" + fileName;
}

std::wstring BuildLogPath(const std::wstring& dumpPath) {
    (void)dumpPath;

    std::wstring configured = GetSettingString(L"IPD_LOG_NAME", L"log_name");
    if (!configured.empty()) {
        return configured;
    }

    wchar_t fileName[64]{};
    swprintf_s(fileName, L"InProcessDumper_%lu.log.txt", GetCurrentProcessId());

    std::wstring directory = GetDefaultDumpDirectory();
    TrimTrailingSlashes(directory);
    return directory + L"\\" + fileName;
}

std::wstring BuildReconstructedExePath(const std::wstring& dumpPath) {
    std::wstring configured = GetSettingString(L"IPD_EXE_NAME", L"exe_name");
    if (!configured.empty()) {
        return configured;
    }

    std::wstring path = dumpPath;
    constexpr wchar_t kDumpExtension[] = L".dmp";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, kDumpExtension) == 0) {
        path.resize(path.size() - 4);
    }

    return path + L".reconstructed.exe";
}

std::wstring SanitizeFileName(std::wstring name) {
    for (wchar_t& ch : name) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' ||
            ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }

    return name.empty() ? L"module" : name;
}

std::wstring FormatHex(DWORD value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08lX", value);
    return buffer;
}

std::wstring FormatHexPtr(const void* value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%p", value);
    return buffer;
}

DWORD AlignUp(DWORD value, DWORD alignment) {
    if (alignment == 0) {
        return value;
    }

    DWORD remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }

    return value + alignment - remainder;
}

void AppendLogFile(const std::wstring& logPath, const std::wstring& line) {
    if (logPath.empty()) {
        return;
    }

    HANDLE file = CreateFileW(
        logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    std::wstring output = line + L"\r\n";
    int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        output.c_str(),
        static_cast<int>(output.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required > 0) {
        std::string utf8(static_cast<size_t>(required), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            output.c_str(),
            static_cast<int>(output.size()),
            utf8.data(),
            required,
            nullptr,
            nullptr);

        DWORD bytes = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &bytes, nullptr);
    }

    CloseHandle(file);
}

void Log(const std::wstring& logPath, const std::wstring& message) {
    std::wstring line = L"[IPD] " + Timestamp() + L" " + message;

    OutputDebugStringW((line + L"\r\n").c_str());

    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (console != nullptr && console != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        std::wstring consoleLine = line + L"\r\n";
        WriteConsoleW(console, consoleLine.c_str(), static_cast<DWORD>(consoleLine.size()), &written, nullptr);
    }

    AppendLogFile(logPath, line);
}

std::wstring GetFileSizeString(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return L"unknown";
    }

    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return std::to_wstring(size.QuadPart);
}

DWORD ParseDumpFlags() {
    std::wstring value = GetSettingString(L"IPD_DUMP_FLAGS", L"dump_flags");
    if (value.empty()) {
        return kDefaultDumpFlags;
    }

    wchar_t* end = nullptr;
    unsigned long flags = wcstoul(value.c_str(), &end, 0);
    if (end == value.c_str()) {
        return kDefaultDumpFlags;
    }

    return static_cast<DWORD>(flags);
}

bool ShouldUnload() {
    return ParseBoolSetting(GetSettingString(L"IPD_UNLOAD", L"unload"), true);
}

bool ShouldWriteReconstructedExe() {
    return ParseBoolSetting(GetSettingString(L"IPD_WRITE_EXE", L"write_exe"), true);
}

bool ShouldAggressivelyReadMemory() {
    return ParseBoolSetting(GetSettingString(L"IPD_AGGRESSIVE_READ", L"aggressive_read"), false);
}

bool ShouldDumpExecutableRegions() {
    return ParseBoolSetting(GetSettingString(L"IPD_DUMP_EXEC_REGIONS", L"dump_exec_regions"), false);
}

bool ShouldDumpModules() {
    return ParseBoolSetting(GetSettingString(L"IPD_DUMP_MODULES", L"dump_modules"), true);
}

struct ReconstructionStats {
    ULONGLONG zeroBytes = 0;
    ULONGLONG unreadableBytes = 0;
    ULONGLONG readFailureBytes = 0;
    ULONGLONG protectRecoveredBytes = 0;
};

struct MemoryStats {
    ULONGLONG zeroBytes = 0;
    ULONGLONG ccBytes = 0;
    ULONGLONG nonZeroBytes = 0;
    ULONGLONG unreadableBytes = 0;
};

bool WriteReconstructedImage(
    const BYTE* base,
    const std::wstring& imagePath,
    const std::wstring& logPath,
    const std::wstring& imageName,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error);

bool WriteBufferAt(HANDLE file, DWORD fileOffset, const void* buffer, DWORD size, DWORD* error) {
    LARGE_INTEGER offset{};
    offset.QuadPart = fileOffset;
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        *error = GetLastError();
        return false;
    }

    const auto* bytes = static_cast<const BYTE*>(buffer);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(file, bytes, remaining, &written, nullptr) || written == 0) {
            *error = GetLastError();
            return false;
        }

        bytes += written;
        remaining -= written;
    }

    return true;
}

bool WriteZerosAt(HANDLE file, DWORD fileOffset, DWORD size, DWORD* error) {
    std::array<BYTE, 4096> zeros{};
    DWORD remaining = size;
    DWORD offset = fileOffset;

    while (remaining > 0) {
        DWORD chunk = remaining > zeros.size() ? static_cast<DWORD>(zeros.size()) : remaining;
        if (!WriteBufferAt(file, offset, zeros.data(), chunk, error)) {
            return false;
        }

        offset += chunk;
        remaining -= chunk;
    }

    return true;
}

bool IsReadableMemory(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    DWORD protect = mbi.Protect & 0xff;
    return protect == PAGE_READONLY ||
           protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool IsExecutableMemory(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    DWORD protect = mbi.Protect & 0xff;
    return protect == PAGE_EXECUTE ||
           protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool TryReadWithTemporaryProtection(const BYTE* source, void* buffer, DWORD size, SIZE_T* bytesRead) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(const_cast<BYTE*>(source), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    BOOL ok = ReadProcessMemory(GetCurrentProcess(), source, buffer, size, bytesRead);

    DWORD ignored = 0;
    VirtualProtect(const_cast<BYTE*>(source), size, oldProtect, &ignored);
    return ok == TRUE && *bytesRead != 0;
}

MemoryStats CountMemoryBytes(const BYTE* source, DWORD size, bool aggressiveRead) {
    constexpr SIZE_T kChunkSize = 64 * 1024;
    std::vector<BYTE> buffer(kChunkSize);
    MemoryStats stats{};

    DWORD total = 0;
    while (total < size) {
        const BYTE* current = source + total;
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T querySize = VirtualQuery(current, &mbi, sizeof(mbi));
        SIZE_T regionRemaining = size - total;
        bool readable = false;

        if (querySize != 0) {
            auto regionEnd = reinterpret_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd > current) {
                regionRemaining = regionEnd - current;
                if (regionRemaining > size - total) {
                    regionRemaining = size - total;
                }
                readable = IsReadableMemory(mbi);
            }
        }

        if (regionRemaining == 0) {
            regionRemaining = size - total;
        }

        while (regionRemaining > 0) {
            DWORD chunk = regionRemaining > kChunkSize ? static_cast<DWORD>(kChunkSize) : static_cast<DWORD>(regionRemaining);
            SIZE_T bytesRead = 0;

            if (readable) {
                ReadProcessMemory(GetCurrentProcess(), current, buffer.data(), chunk, &bytesRead);
            } else if (aggressiveRead && mbi.State == MEM_COMMIT) {
                TryReadWithTemporaryProtection(current, buffer.data(), chunk, &bytesRead);
            }

            if (bytesRead == 0) {
                stats.unreadableBytes += chunk;
            } else {
                for (SIZE_T i = 0; i < bytesRead; ++i) {
                    if (buffer[i] == 0) {
                        ++stats.zeroBytes;
                    } else if (buffer[i] == 0xcc) {
                        ++stats.ccBytes;
                    } else {
                        ++stats.nonZeroBytes;
                    }
                }
                if (bytesRead < chunk) {
                    stats.unreadableBytes += chunk - bytesRead;
                }
            }

            current += chunk;
            total += chunk;
            regionRemaining -= chunk;
        }
    }

    return stats;
}

bool WriteImageMemoryAt(
    HANDLE file,
    DWORD fileOffset,
    const BYTE* source,
    DWORD size,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error) {
    constexpr SIZE_T kChunkSize = 64 * 1024;
    std::vector<BYTE> buffer(kChunkSize);

    DWORD writtenTotal = 0;
    while (writtenTotal < size) {
        const BYTE* current = source + writtenTotal;
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T querySize = VirtualQuery(current, &mbi, sizeof(mbi));

        SIZE_T regionRemaining = size - writtenTotal;
        bool readable = false;
        if (querySize != 0) {
            auto regionEnd = reinterpret_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd > current) {
                regionRemaining = regionEnd - current;
                if (regionRemaining > size - writtenTotal) {
                    regionRemaining = size - writtenTotal;
                }
                readable = IsReadableMemory(mbi);
            }
        }

        if (regionRemaining == 0) {
            regionRemaining = size - writtenTotal;
        }

        while (regionRemaining > 0) {
            DWORD chunk = regionRemaining > kChunkSize ? static_cast<DWORD>(kChunkSize) : static_cast<DWORD>(regionRemaining);
            if (readable) {
                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), current, buffer.data(), chunk, &bytesRead) || bytesRead == 0) {
                    bool recovered = aggressiveRead && TryReadWithTemporaryProtection(current, buffer.data(), chunk, &bytesRead);
                    if (recovered) {
                        stats->protectRecoveredBytes += bytesRead;
                    }
                }

                if (bytesRead == 0) {
                    stats->readFailureBytes += chunk;
                    stats->zeroBytes += chunk;
                    if (!WriteZerosAt(file, fileOffset + writtenTotal, chunk, error)) {
                        return false;
                    }
                } else {
                    if (!WriteBufferAt(file, fileOffset + writtenTotal, buffer.data(), static_cast<DWORD>(bytesRead), error)) {
                        return false;
                    }
                    if (bytesRead < chunk &&
                        !WriteZerosAt(file, fileOffset + writtenTotal + static_cast<DWORD>(bytesRead), chunk - static_cast<DWORD>(bytesRead), error)) {
                        return false;
                    }
                    if (bytesRead < chunk) {
                        stats->readFailureBytes += chunk - bytesRead;
                        stats->zeroBytes += chunk - bytesRead;
                    }
                }
            } else {
                SIZE_T bytesRead = 0;
                bool recovered = aggressiveRead &&
                    mbi.State == MEM_COMMIT &&
                    TryReadWithTemporaryProtection(current, buffer.data(), chunk, &bytesRead);

                if (recovered) {
                    stats->protectRecoveredBytes += bytesRead;
                    if (!WriteBufferAt(file, fileOffset + writtenTotal, buffer.data(), static_cast<DWORD>(bytesRead), error)) {
                        return false;
                    }
                    if (bytesRead < chunk) {
                        DWORD zeroSize = chunk - static_cast<DWORD>(bytesRead);
                        stats->unreadableBytes += zeroSize;
                        stats->zeroBytes += zeroSize;
                        if (!WriteZerosAt(file, fileOffset + writtenTotal + static_cast<DWORD>(bytesRead), zeroSize, error)) {
                            return false;
                        }
                    }
                } else {
                    stats->unreadableBytes += chunk;
                    stats->zeroBytes += chunk;
                    if (!WriteZerosAt(file, fileOffset + writtenTotal, chunk, error)) {
                        return false;
                    }
                }
            }

            current += chunk;
            writtenTotal += chunk;
            regionRemaining -= chunk;
        }
    }

    return true;
}

std::wstring SectionName(const IMAGE_SECTION_HEADER& section) {
    char name[IMAGE_SIZEOF_SHORT_NAME + 1]{};
    memcpy(name, section.Name, IMAGE_SIZEOF_SHORT_NAME);

    int required = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    if (required <= 1) {
        return L"<noname>";
    }

    std::wstring wide(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), required);
    return wide;
}

void LogSectionReport(
    const std::wstring& logPath,
    const BYTE* base,
    const IMAGE_NT_HEADERS* nt,
    bool aggressiveRead) {
    DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);

    Log(logPath, L"Section memory report begin.");
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        DWORD virtualOffset = section->VirtualAddress;
        DWORD virtualSize = section->Misc.VirtualSize;
        DWORD rawSize = section->SizeOfRawData;
        if (virtualOffset >= sizeOfImage) {
            continue;
        }

        DWORD nextVirtualOffset = sizeOfImage;
        const IMAGE_SECTION_HEADER* other = IMAGE_FIRST_SECTION(nt);
        for (WORD j = 0; j < nt->FileHeader.NumberOfSections; ++j, ++other) {
            DWORD candidate = other->VirtualAddress;
            if (candidate > virtualOffset && candidate < nextVirtualOffset) {
                nextVirtualOffset = candidate;
            }
        }

        DWORD inferredSize = nextVirtualOffset > virtualOffset ? nextVirtualOffset - virtualOffset : sizeOfImage - virtualOffset;
        DWORD reportSize = virtualSize;
        if (reportSize == 0 || rawSize > reportSize) {
            reportSize = rawSize;
        }
        if (reportSize == 0 || inferredSize > reportSize) {
            reportSize = inferredSize;
        }
        if (reportSize > sizeOfImage - virtualOffset) {
            reportSize = sizeOfImage - virtualOffset;
        }

        MemoryStats stats = CountMemoryBytes(base + virtualOffset, reportSize, aggressiveRead);
        Log(
            logPath,
            L"section=" + SectionName(*section) +
                L" va=" + FormatHex(virtualOffset) +
                L" size=" + std::to_wstring(reportSize) +
                L" nonzero=" + std::to_wstring(stats.nonZeroBytes) +
                L" zero=" + std::to_wstring(stats.zeroBytes) +
                L" cc=" + std::to_wstring(stats.ccBytes) +
                L" unreadable=" + std::to_wstring(stats.unreadableBytes));
    }
    Log(logPath, L"Section memory report end.");
}

std::wstring BuildSidecarDirectory(const std::wstring& dumpPath, const wchar_t* suffix) {
    std::wstring path = dumpPath;
    constexpr wchar_t kDumpExtension[] = L".dmp";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, kDumpExtension) == 0) {
        path.resize(path.size() - 4);
    }

    return path + suffix;
}

std::wstring MemoryTypeName(DWORD type) {
    switch (type) {
        case MEM_IMAGE:
            return L"image";
        case MEM_MAPPED:
            return L"mapped";
        case MEM_PRIVATE:
            return L"private";
        default:
            return L"unknown";
    }
}

void DumpExecutableRegions(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead) {
    std::wstring outputDir = BuildSidecarDirectory(dumpPath, L".exec_regions");
    if (!CreateDirectoryW(outputDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log(logPath, L"Failed to create exec region directory. error=" + std::to_wstring(GetLastError()) + L" path=" + outputDir);
        return;
    }

    auto mainBase = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mainBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(mainBase + dos->e_lfanew);
    const BYTE* mainEnd = mainBase + nt->OptionalHeader.SizeOfImage;

    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);

    const BYTE* current = reinterpret_cast<const BYTE*>(systemInfo.lpMinimumApplicationAddress);
    const BYTE* maxAddress = reinterpret_cast<const BYTE*>(systemInfo.lpMaximumApplicationAddress);
    DWORD index = 0;
    ULONGLONG totalBytes = 0;

    Log(logPath, L"Executable region dump begin. dir=" + outputDir);
    while (current < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T querySize = VirtualQuery(current, &mbi, sizeof(mbi));
        if (querySize == 0) {
            break;
        }

        const BYTE* base = reinterpret_cast<const BYTE*>(mbi.BaseAddress);
        const BYTE* end = base + mbi.RegionSize;
        bool isMainModule = base < mainEnd && end > mainBase;

        if (!isMainModule &&
            IsExecutableMemory(mbi) &&
            (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)) {
            wchar_t fileName[256]{};
            swprintf_s(
                fileName,
                L"%04lu_%s_%p_%llu.bin",
                index,
                MemoryTypeName(mbi.Type).c_str(),
                mbi.BaseAddress,
                static_cast<unsigned long long>(mbi.RegionSize));

            std::wstring filePath = outputDir + L"\\" + fileName;
            HANDLE file = CreateFileW(
                filePath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            DWORD error = ERROR_SUCCESS;
            ReconstructionStats stats{};
            if (file == INVALID_HANDLE_VALUE) {
                Log(logPath, L"exec_region failed_open path=" + filePath + L" error=" + std::to_wstring(GetLastError()));
            } else {
                bool ok = WriteImageMemoryAt(
                    file,
                    0,
                    base,
                    static_cast<DWORD>(mbi.RegionSize),
                    aggressiveRead,
                    &stats,
                    &error);
                CloseHandle(file);

                if (ok) {
                    ++index;
                    totalBytes += mbi.RegionSize;
                    Log(
                        logPath,
                        L"exec_region path=" + filePath +
                            L" base=" + FormatHexPtr(mbi.BaseAddress) +
                            L" size=" + std::to_wstring(static_cast<unsigned long long>(mbi.RegionSize)) +
                            L" type=" + MemoryTypeName(mbi.Type) +
                            L" protect=" + FormatHex(mbi.Protect) +
                            L" zero_bytes=" + std::to_wstring(stats.zeroBytes) +
                            L" unreadable_bytes=" + std::to_wstring(stats.unreadableBytes) +
                            L" read_failure_bytes=" + std::to_wstring(stats.readFailureBytes));
                } else {
                    Log(logPath, L"exec_region failed_write path=" + filePath + L" error=" + std::to_wstring(error));
                }
            }
        }

        if (end <= current) {
            break;
        }
        current = end;
    }

    Log(logPath, L"Executable region dump end. count=" + std::to_wstring(index) + L" total_bytes=" + std::to_wstring(totalBytes));
}

void DumpLoadedModules(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead) {
    std::wstring outputDir = BuildSidecarDirectory(dumpPath, L".modules");
    if (!CreateDirectoryW(outputDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log(logPath, L"Failed to create module directory. error=" + std::to_wstring(GetLastError()) + L" path=" + outputDir);
        return;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        Log(logPath, L"CreateToolhelp32Snapshot failed. error=" + std::to_wstring(GetLastError()));
        return;
    }

    const auto mainBase = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    DWORD count = 0;
    DWORD failed = 0;
    ULONGLONG totalBytes = 0;

    Log(logPath, L"Loaded module reconstruction begin. dir=" + outputDir);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            const auto moduleBase = reinterpret_cast<const BYTE*>(entry.modBaseAddr);
            if (moduleBase == nullptr || moduleBase == mainBase) {
                continue;
            }

            std::wstring moduleName = SanitizeFileName(entry.szModule);
            wchar_t fileName[512]{};
            swprintf_s(fileName, L"%04lu_%p_%s", count, entry.modBaseAddr, moduleName.c_str());
            std::wstring modulePath = outputDir + L"\\" + fileName;

            ReconstructionStats stats{};
            DWORD error = ERROR_SUCCESS;
            if (WriteReconstructedImage(moduleBase, modulePath, logPath, moduleName, aggressiveRead, &stats, &error)) {
                std::wstring plainModulePath = outputDir + L"\\" + moduleName;
                CopyFileW(modulePath.c_str(), plainModulePath.c_str(), FALSE);

                ++count;
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (GetFileAttributesExW(modulePath.c_str(), GetFileExInfoStandard, &data)) {
                    ULARGE_INTEGER size{};
                    size.HighPart = data.nFileSizeHigh;
                    size.LowPart = data.nFileSizeLow;
                    totalBytes += size.QuadPart;
                }

                Log(
                    logPath,
                    L"module_reconstructed name=" + moduleName +
                        L" base=" + FormatHexPtr(moduleBase) +
                        L" path=" + modulePath +
                        L" zero_bytes=" + std::to_wstring(stats.zeroBytes) +
                        L" unreadable_bytes=" + std::to_wstring(stats.unreadableBytes) +
                        L" read_failure_bytes=" + std::to_wstring(stats.readFailureBytes) +
                        L" protect_recovered_bytes=" + std::to_wstring(stats.protectRecoveredBytes));
            } else {
                ++failed;
                Log(
                    logPath,
                    L"module_reconstruct_failed name=" + moduleName +
                        L" base=" + FormatHexPtr(moduleBase) +
                        L" error=" + std::to_wstring(error) +
                        L" " + FormatWin32Error(error));
            }
        } while (Module32NextW(snapshot, &entry));
    } else {
        Log(logPath, L"Module32FirstW failed. error=" + std::to_wstring(GetLastError()));
    }

    CloseHandle(snapshot);
    Log(
        logPath,
        L"Loaded module reconstruction end. count=" + std::to_wstring(count) +
            L" failed=" + std::to_wstring(failed) +
            L" total_bytes=" + std::to_wstring(totalBytes));
}

bool SetEndOfFileAt(HANDLE file, DWORD fileSize, DWORD* error) {
    LARGE_INTEGER offset{};
    offset.QuadPart = fileSize;
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        *error = GetLastError();
        return false;
    }

    if (!SetEndOfFile(file)) {
        *error = GetLastError();
        return false;
    }

    return true;
}

bool WriteReconstructedImage(
    const BYTE* base,
    const std::wstring& imagePath,
    const std::wstring& logPath,
    const std::wstring& imageName,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error) {
    if (base == nullptr) {
        *error = ERROR_INVALID_ADDRESS;
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        *error = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    DWORD ntOffset = static_cast<DWORD>(dos->e_lfanew);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
         nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > 96) {
        *error = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    Log(logPath, L"Image reconstruction begin. name=" + imageName + L" base=" + FormatHexPtr(base) + L" path=" + imagePath);
    LogSectionReport(logPath, base, nt, aggressiveRead);

    HANDLE file = CreateFileW(
        imagePath.c_str(),
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

    bool ok = true;
    DWORD localError = ERROR_SUCCESS;
    DWORD sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    DWORD fileAlignment = nt->OptionalHeader.FileAlignment;

    if (fileAlignment == 0 || fileAlignment > 0x10000 || (fileAlignment & (fileAlignment - 1)) != 0) {
        fileAlignment = 0x200;
    }

    if (sizeOfHeaders == 0 || sizeOfHeaders > sizeOfImage) {
        ok = false;
        localError = ERROR_BAD_EXE_FORMAT;
    }

    std::vector<BYTE> headers;
    if (ok) {
        headers.assign(base, base + sizeOfHeaders);
    }

    IMAGE_NT_HEADERS* mutableNt = nullptr;
    IMAGE_SECTION_HEADER* mutableSection = nullptr;
    if (ok) {
        if (ntOffset + sizeof(IMAGE_NT_HEADERS) > headers.size()) {
            ok = false;
            localError = ERROR_BAD_EXE_FORMAT;
        } else {
            mutableNt = reinterpret_cast<IMAGE_NT_HEADERS*>(headers.data() + ntOffset);
            mutableSection = IMAGE_FIRST_SECTION(mutableNt);
        }
    }

    DWORD nextRawOffset = AlignUp(sizeOfHeaders, fileAlignment);
    DWORD finalFileSize = nextRawOffset;
    for (WORD i = 0; ok && i < nt->FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER& section = mutableSection[i];
        DWORD virtualOffset = section.VirtualAddress;
        DWORD virtualSize = section.Misc.VirtualSize;
        DWORD oldRawSize = section.SizeOfRawData;

        if (virtualOffset >= sizeOfImage) {
            continue;
        }

        DWORD nextVirtualOffset = sizeOfImage;
        for (WORD j = 0; j < nt->FileHeader.NumberOfSections; ++j) {
            DWORD candidate = mutableSection[j].VirtualAddress;
            if (candidate > virtualOffset && candidate < nextVirtualOffset) {
                nextVirtualOffset = candidate;
            }
        }

        DWORD inferredSize = nextVirtualOffset > virtualOffset ? nextVirtualOffset - virtualOffset : sizeOfImage - virtualOffset;
        DWORD available = sizeOfImage - virtualOffset;
        DWORD memorySize = virtualSize;
        if (memorySize == 0 || oldRawSize > memorySize) {
            memorySize = oldRawSize;
        }
        if (memorySize == 0 || inferredSize > memorySize) {
            memorySize = inferredSize;
        }
        if (memorySize == 0) {
            section.PointerToRawData = 0;
            section.SizeOfRawData = 0;
            continue;
        }

        if (available < memorySize) {
            memorySize = available;
        }

        DWORD rawSize = AlignUp(memorySize, fileAlignment);
        if (rawSize < memorySize || nextRawOffset + rawSize < nextRawOffset) {
            ok = false;
            localError = ERROR_BAD_EXE_FORMAT;
            break;
        }

        section.PointerToRawData = nextRawOffset;
        section.SizeOfRawData = rawSize;
        section.Misc.VirtualSize = memorySize;
        nextRawOffset += rawSize;
        finalFileSize = nextRawOffset;
    }

    if (ok) {
        ok = WriteBufferAt(file, 0, headers.data(), static_cast<DWORD>(headers.size()), &localError);
    }

    mutableSection = ok ? IMAGE_FIRST_SECTION(mutableNt) : nullptr;
    for (WORD i = 0; ok && i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& section = mutableSection[i];
        DWORD rawOffset = section.PointerToRawData;
        DWORD rawSize = section.SizeOfRawData;
        DWORD virtualOffset = section.VirtualAddress;
        DWORD virtualSize = section.Misc.VirtualSize;

        if (rawOffset == 0 || rawSize == 0 || virtualOffset >= sizeOfImage) {
            continue;
        }

        DWORD bytesToCopy = virtualSize == 0 ? rawSize : virtualSize;
        DWORD available = sizeOfImage - virtualOffset;
        if (bytesToCopy > available) {
            bytesToCopy = available;
        }
        if (bytesToCopy > rawSize) {
            bytesToCopy = rawSize;
        }

        if (bytesToCopy > 0) {
            ok = WriteImageMemoryAt(file, rawOffset, base + virtualOffset, bytesToCopy, aggressiveRead, stats, &localError);
        }

        if (ok && rawSize > bytesToCopy) {
            stats->zeroBytes += rawSize - bytesToCopy;
            ok = WriteZerosAt(file, rawOffset + bytesToCopy, rawSize - bytesToCopy, &localError);
        }
    }

    if (ok) {
        ok = SetEndOfFileAt(file, finalFileSize, &localError);
    }

    CloseHandle(file);
    *error = ok ? ERROR_SUCCESS : localError;
    if (ok) {
        Log(logPath, L"Image reconstruction completed. name=" + imageName + L" size=" + GetFileSizeString(imagePath));
    }
    return ok;
}

bool WriteReconstructedExe(
    const std::wstring& exePath,
    const std::wstring& logPath,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error) {
    auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    return WriteReconstructedImage(base, exePath, logPath, GetProcessName() + L".exe", aggressiveRead, stats, error);
}

void WriteStatusFile(
    const std::wstring& dumpPath,
    DWORD dumpError,
    const std::wstring& exePath,
    DWORD exeError) {
    std::wstring statusPath = dumpPath + L".status.txt";
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

DWORD WINAPI DumpThread(void*) {
    EnsureConsole();

    std::wstring dumpPath = BuildDumpPath();
    std::wstring logPath = BuildLogPath(dumpPath);
    std::wstring exePath = BuildReconstructedExePath(dumpPath);
    DWORD error = ERROR_SUCCESS;
    DWORD exeError = ERROR_SUCCESS;
    DWORD flags = ParseDumpFlags();
    bool aggressiveRead = ShouldAggressivelyReadMemory();
    ReconstructionStats exeStats{};

    Log(logPath, L"DLL loaded into process.");
    Log(logPath, L"process_id=" + std::to_wstring(GetCurrentProcessId()));
    Log(logPath, L"dump_path=" + dumpPath);
    Log(logPath, L"exe_path=" + exePath);
    Log(logPath, L"log_path=" + logPath);
    Log(logPath, L"dump_flags=" + FormatHex(flags));
    Log(logPath, L"aggressive_read=" + std::to_wstring(aggressiveRead ? 1 : 0));

    Log(logPath, L"Writing dump...");
    if (WriteDump(dumpPath, flags, &error)) {
        Log(logPath, L"Dump completed successfully.");
    } else {
        Log(
            logPath,
            L"Dump failed. error=" + std::to_wstring(error) + L" " + FormatWin32Error(error));
    }

    if (ShouldWriteReconstructedExe()) {
        Log(logPath, L"Writing reconstructed EXE...");
        if (WriteReconstructedExe(exePath, logPath, aggressiveRead, &exeStats, &exeError)) {
            Log(logPath, L"Reconstructed EXE completed successfully. size=" + GetFileSizeString(exePath));
            Log(
                logPath,
                L"Reconstructed EXE zero_bytes=" + std::to_wstring(exeStats.zeroBytes) +
                    L" unreadable_bytes=" + std::to_wstring(exeStats.unreadableBytes) +
                    L" read_failure_bytes=" + std::to_wstring(exeStats.readFailureBytes) +
                    L" protect_recovered_bytes=" + std::to_wstring(exeStats.protectRecoveredBytes));
        } else {
            Log(
                logPath,
                L"Reconstructed EXE failed. error=" + std::to_wstring(exeError) + L" " + FormatWin32Error(exeError));
        }
    } else {
        exeError = ERROR_CANCELLED;
        Log(logPath, L"Skipping reconstructed EXE because IPD_WRITE_EXE is disabled.");
    }

    if (ShouldDumpExecutableRegions()) {
        DumpExecutableRegions(dumpPath, logPath, aggressiveRead);
    }

    if (ShouldDumpModules()) {
        DumpLoadedModules(dumpPath, logPath, aggressiveRead);
    }

    WriteStatusFile(dumpPath, error, exePath, exeError);
    Log(logPath, L"status_path=" + dumpPath + L".status.txt");

    if (ShouldUnload() && g_module != nullptr) {
        Log(logPath, L"Unloading DLL.");
        FreeLibraryAndExitThread(g_module, error);
    }

    Log(logPath, L"Leaving DLL loaded because IPD_UNLOAD is disabled.");
    return error;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI DumpCurrentProcess() {
    EnsureConsole();

    DWORD error = ERROR_SUCCESS;
    std::wstring dumpPath = BuildDumpPath();
    std::wstring logPath = BuildLogPath(dumpPath);
    std::wstring exePath = BuildReconstructedExePath(dumpPath);
    DWORD exeError = ERROR_SUCCESS;
    DWORD flags = ParseDumpFlags();
    bool aggressiveRead = ShouldAggressivelyReadMemory();
    ReconstructionStats exeStats{};

    Log(logPath, L"DumpCurrentProcess called.");
    Log(logPath, L"dump_path=" + dumpPath);
    Log(logPath, L"exe_path=" + exePath);
    Log(logPath, L"log_path=" + logPath);
    Log(logPath, L"aggressive_read=" + std::to_wstring(aggressiveRead ? 1 : 0));

    if (WriteDump(dumpPath, flags, &error)) {
        Log(logPath, L"Dump completed successfully.");
    } else {
        Log(
            logPath,
            L"Dump failed. error=" + std::to_wstring(error) + L" " + FormatWin32Error(error));
    }

    if (ShouldWriteReconstructedExe()) {
        Log(logPath, L"Writing reconstructed EXE...");
        if (WriteReconstructedExe(exePath, logPath, aggressiveRead, &exeStats, &exeError)) {
            Log(logPath, L"Reconstructed EXE completed successfully. size=" + GetFileSizeString(exePath));
            Log(
                logPath,
                L"Reconstructed EXE zero_bytes=" + std::to_wstring(exeStats.zeroBytes) +
                    L" unreadable_bytes=" + std::to_wstring(exeStats.unreadableBytes) +
                    L" read_failure_bytes=" + std::to_wstring(exeStats.readFailureBytes) +
                    L" protect_recovered_bytes=" + std::to_wstring(exeStats.protectRecoveredBytes));
        } else {
            Log(
                logPath,
                L"Reconstructed EXE failed. error=" + std::to_wstring(exeError) + L" " + FormatWin32Error(exeError));
        }
    } else {
        exeError = ERROR_CANCELLED;
        Log(logPath, L"Skipping reconstructed EXE because IPD_WRITE_EXE is disabled.");
    }

    if (ShouldDumpExecutableRegions()) {
        DumpExecutableRegions(dumpPath, logPath, aggressiveRead);
    }

    if (ShouldDumpModules()) {
        DumpLoadedModules(dumpPath, logPath, aggressiveRead);
    }

    WriteStatusFile(dumpPath, error, exePath, exeError);
    return error;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        OutputDebugStringW(L"[IPD] DLL_PROCESS_ATTACH\r\n");
        g_module = module;
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, DumpThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
