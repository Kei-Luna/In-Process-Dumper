#include "dumper_internal.h"

#include <array>
#include <string>
#include <vector>

namespace ipd {
DWORD ReadLe32(const BYTE* data) {
    return static_cast<DWORD>(data[0]) |
           (static_cast<DWORD>(data[1]) << 8) |
           (static_cast<DWORD>(data[2]) << 16) |
           (static_cast<DWORD>(data[3]) << 24);
}

bool TryGetUnityMetadataSize(const BYTE* candidate, bool aggressiveRead, DWORD* metadataSize, DWORD* version) {
    constexpr DWORD kUnityMetadataMagic = 0xFAB11BAF;
    constexpr DWORD kProbeSize = 4096;
    constexpr DWORD kMinMetadataSize = 0x100;
    constexpr DWORD kMaxMetadataSize = 512 * 1024 * 1024;

    std::array<BYTE, kProbeSize> header{};
    SIZE_T bytesRead = 0;
    if (!ReadMemoryBlock(candidate, header.data(), static_cast<DWORD>(header.size()), aggressiveRead, &bytesRead)) {
        return false;
    }

    if (ReadLe32(header.data()) != kUnityMetadataMagic) {
        return false;
    }

    DWORD metadataVersion = ReadLe32(header.data() + 4);
    if (metadataVersion < 16 || metadataVersion > 31) {
        return false;
    }

    DWORD firstOffset = ReadLe32(header.data() + 8);
    if (firstOffset < 0x20 || firstOffset > kProbeSize || (firstOffset % 4) != 0) {
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
        const BYTE* pair = header.data() + 8 + (i * 8);
        DWORD offset = ReadLe32(pair);
        DWORD size = ReadLe32(pair + 4);
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

    if (nonEmptyPairs < 4 || maxEnd < kMinMetadataSize || maxEnd > kMaxMetadataSize) {
        return false;
    }

    *metadataSize = maxEnd;
    *version = metadataVersion;
    return true;
}

bool WriteUnityMetadataBlob(
    const BYTE* base,
    DWORD size,
    const std::wstring& outputPath,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error) {
    HANDLE file = CreateFileW(
        outputPath.c_str(),
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

    bool ok = WriteImageMemoryAt(file, 0, base, size, aggressiveRead, stats, error);
    CloseHandle(file);
    return ok;
}

void DumpUnityMetadata(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead) {
    constexpr DWORD kUnityMetadataMagic = 0xFAB11BAF;
    constexpr SIZE_T kScanChunkSize = 64 * 1024;

    std::wstring outputDir = BuildSidecarDirectory(dumpPath, L".unity_metadata");
    if (!CreateDirectoryW(outputDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log(logPath, L"Failed to create Unity metadata directory. error=" + std::to_wstring(GetLastError()) + L" path=" + outputDir);
        return;
    }

    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);

    std::vector<BYTE> buffer(kScanChunkSize + 3);
    const BYTE* current = reinterpret_cast<const BYTE*>(systemInfo.lpMinimumApplicationAddress);
    const BYTE* maxAddress = reinterpret_cast<const BYTE*>(systemInfo.lpMaximumApplicationAddress);
    DWORD index = 0;
    ULONGLONG totalBytes = 0;

    Log(logPath, L"Unity metadata dump begin. dir=" + outputDir);
    while (current < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T querySize = VirtualQuery(current, &mbi, sizeof(mbi));
        if (querySize == 0) {
            break;
        }

        const BYTE* base = reinterpret_cast<const BYTE*>(mbi.BaseAddress);
        const BYTE* end = base + mbi.RegionSize;
        if (IsReadableMemory(mbi) || (aggressiveRead && mbi.State == MEM_COMMIT)) {
            SIZE_T regionOffset = 0;
            while (regionOffset < mbi.RegionSize) {
                const BYTE* chunkBase = base + regionOffset;
                SIZE_T remaining = mbi.RegionSize - regionOffset;
                DWORD chunk = remaining > kScanChunkSize ? static_cast<DWORD>(kScanChunkSize) : static_cast<DWORD>(remaining);
                SIZE_T bytesRead = 0;
                bool readOk = false;
                if (IsReadableMemory(mbi)) {
                    readOk = ReadProcessMemory(GetCurrentProcess(), chunkBase, buffer.data(), chunk, &bytesRead) == TRUE;
                } else {
                    readOk = ReadMemoryBlock(chunkBase, buffer.data(), chunk, aggressiveRead, &bytesRead);
                }

                if (readOk && bytesRead >= 8) {
                    for (SIZE_T i = 0; i + 8 <= bytesRead; ++i) {
                        if (ReadLe32(buffer.data() + i) != kUnityMetadataMagic) {
                            continue;
                        }

                        const BYTE* candidate = chunkBase + i;
                        DWORD metadataSize = 0;
                        DWORD version = 0;
                        if (!TryGetUnityMetadataSize(candidate, aggressiveRead, &metadataSize, &version)) {
                            continue;
                        }

                        std::wstring fileName = index == 0 ?
                            L"global-metadata_dump.dat" :
                            AddSuffixBeforeExtension(L"global-metadata_dump.dat", L"_" + std::to_wstring(index));
                        std::wstring outputPath = BuildUniqueFilePath(outputDir, fileName);
                        ReconstructionStats stats{};
                        DWORD error = ERROR_SUCCESS;
                        if (WriteUnityMetadataBlob(candidate, metadataSize, outputPath, aggressiveRead, &stats, &error)) {
                            ++index;
                            totalBytes += metadataSize;
                            Log(
                                logPath,
                                L"unity_metadata path=" + outputPath +
                                    L" base=" + FormatHexPtr(candidate) +
                                    L" size=" + std::to_wstring(metadataSize) +
                                    L" version=" + std::to_wstring(version) +
                                    L" zero_bytes=" + std::to_wstring(stats.zeroBytes) +
                                    L" unreadable_bytes=" + std::to_wstring(stats.unreadableBytes) +
                                    L" read_failure_bytes=" + std::to_wstring(stats.readFailureBytes) +
                                    L" protect_recovered_bytes=" + std::to_wstring(stats.protectRecoveredBytes));
                        } else {
                            Log(logPath, L"unity_metadata failed_write path=" + outputPath + L" error=" + std::to_wstring(error));
                        }
                    }
                }

                if (chunk == 0 || remaining <= kScanChunkSize) {
                    break;
                }
                regionOffset += kScanChunkSize - 3;
            }
        }

        if (end <= current) {
            break;
        }
        current = end;
    }

    Log(logPath, L"Unity metadata dump end. count=" + std::to_wstring(index) + L" total_bytes=" + std::to_wstring(totalBytes));
}
}  // namespace ipd
