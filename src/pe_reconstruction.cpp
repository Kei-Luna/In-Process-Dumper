#include "dumper_internal.h"

#include <tlhelp32.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace ipd {
struct MemoryStats {
    ULONGLONG zeroBytes = 0;
    ULONGLONG ccBytes = 0;
    ULONGLONG nonZeroBytes = 0;
    ULONGLONG unreadableBytes = 0;
};

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
    return StripFileExtension(dumpPath) + suffix;
}

bool FileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring BuildUniqueFilePath(const std::wstring& directory, const std::wstring& fileName) {
    std::wstring path = directory + L"\\" + fileName;
    if (!FileExists(path)) {
        return path;
    }

    for (DWORD i = 1; i < 1000; ++i) {
        std::wstring uniqueName = AddSuffixBeforeExtension(fileName, L"_" + std::to_wstring(i));
        path = directory + L"\\" + uniqueName;
        if (!FileExists(path)) {
            return path;
        }
    }

    return directory + L"\\" + AddSuffixBeforeExtension(fileName, L"_" + FormatHex(GetTickCount()));
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
            std::wstring moduleDumpName = AddDumpSuffixBeforeExtension(moduleName);
            std::wstring modulePath = BuildUniqueFilePath(outputDir, moduleDumpName);

            ReconstructionStats stats{};
            DWORD error = ERROR_SUCCESS;
            if (WriteReconstructedImage(moduleBase, modulePath, logPath, moduleName, aggressiveRead, &stats, &error)) {
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
}  // namespace ipd

