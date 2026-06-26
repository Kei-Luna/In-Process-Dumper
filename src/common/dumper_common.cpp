#include "dumper_internal.h"

#include <dbghelp.h>

#include <array>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ipd {
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
std::wstring SanitizeFileName(std::wstring name);

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
    return directory + L"\\InProcessDumper.json";
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

std::wstring StripFileExtension(std::wstring path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        path.resize(dot);
    }

    return path;
}

std::wstring BuildDefaultOutputBasePath() {
    std::wstring directory = GetDefaultDumpDirectory();
    TrimTrailingSlashes(directory);
    return directory + L"\\" + SanitizeFileName(GetProcessName()) + L"_dump";
}

std::wstring ReplaceFileExtension(const std::wstring& path, const wchar_t* extension) {
    return StripFileExtension(path) + extension;
}

std::wstring AddSuffixBeforeExtension(const std::wstring& fileName, const std::wstring& suffix) {
    size_t slash = fileName.find_last_of(L"\\/");
    size_t dot = fileName.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        return fileName.substr(0, dot) + suffix + fileName.substr(dot);
    }

    return fileName + suffix;
}

std::wstring AddDumpSuffixBeforeExtension(const std::wstring& fileName) {
    return AddSuffixBeforeExtension(fileName, L"_dump");
}

std::wstring BuildDumpPath() {
    std::wstring configuredName = GetSettingString(L"IPD_DUMP_NAME", L"dump_name");
    if (!configuredName.empty()) {
        return configuredName;
    }

    return BuildDefaultOutputBasePath() + L".dmp";
}

std::wstring BuildLogPath(const std::wstring& dumpPath) {
    std::wstring configured = GetSettingString(L"IPD_LOG_NAME", L"log_name");
    if (!configured.empty()) {
        return configured;
    }

    return ReplaceFileExtension(dumpPath, L".log.txt");
}

std::wstring BuildReconstructedExePath(const std::wstring& dumpPath) {
    std::wstring configured = GetSettingString(L"IPD_EXE_NAME", L"exe_name");
    if (!configured.empty()) {
        return configured;
    }

    return ReplaceFileExtension(dumpPath, L".exe");
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

DWORD ParseDumpDelaySeconds() {
    std::wstring value = GetSettingString(L"IPD_DUMP_DELAY_SECONDS", L"dump_delay_seconds");
    if (value.empty()) {
        return 0;
    }

    wchar_t* end = nullptr;
    unsigned long seconds = wcstoul(value.c_str(), &end, 0);
    if (end == value.c_str()) {
        return 0;
    }

    constexpr unsigned long kMaxSleepSeconds = MAXDWORD / 1000;
    if (seconds > kMaxSleepSeconds) {
        return kMaxSleepSeconds;
    }

    return static_cast<DWORD>(seconds);
}

DWORD ParseUnityMetadataScanSeconds() {
    std::wstring value = GetSettingString(L"IPD_UNITY_METADATA_SCAN_SECONDS", L"unity_metadata_scan_seconds");
    if (value.empty()) {
        return 15;
    }

    wchar_t* end = nullptr;
    unsigned long seconds = wcstoul(value.c_str(), &end, 0);
    if (end == value.c_str()) {
        return 15;
    }

    constexpr unsigned long kMaxScanSeconds = 120;
    if (seconds > kMaxScanSeconds) {
        return kMaxScanSeconds;
    }

    return static_cast<DWORD>(seconds);
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

bool ShouldDumpUnityMetadata() {
    return ParseBoolSetting(GetSettingString(L"IPD_DUMP_UNITY_METADATA", L"dump_unity_metadata"), true);
}

bool ShouldWatchUnityMetadataFile() {
    return ParseBoolSetting(GetSettingString(L"IPD_WATCH_UNITY_METADATA_FILE", L"watch_unity_metadata_file"), false);
}

bool ShouldInlineWatchUnityMetadataFile() {
    return ParseBoolSetting(GetSettingString(L"IPD_INLINE_WATCH_UNITY_METADATA_FILE", L"inline_watch_unity_metadata_file"), false);
}

}  // namespace ipd

