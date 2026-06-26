#include "dumper_internal.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace ipd {
DWORD ReadLe32(const BYTE* data) {
    return static_cast<DWORD>(data[0]) |
           (static_cast<DWORD>(data[1]) << 8) |
           (static_cast<DWORD>(data[2]) << 16) |
           (static_cast<DWORD>(data[3]) << 24);
}

bool TryGetUnityMetadataSize(
    const BYTE* candidate,
    bool aggressiveRead,
    DWORD* metadataSize,
    DWORD* version,
    bool* repairMagic) {
    constexpr DWORD kUnityMetadataMagic = 0xFAB11BAF;
    constexpr DWORD kProbeSize = 4096;
    constexpr DWORD kMinMetadataSize = 0x100;
    constexpr DWORD kMaxMetadataSize = 512 * 1024 * 1024;

    std::array<BYTE, kProbeSize> header{};
    SIZE_T bytesRead = 0;
    if (!ReadMemoryBlock(candidate, header.data(), static_cast<DWORD>(header.size()), aggressiveRead, &bytesRead)) {
        return false;
    }

    DWORD magic = ReadLe32(header.data());
    if (magic != kUnityMetadataMagic && magic != 0) {
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

    if (magic == 0 && maxEnd < 1024 * 1024) {
        return false;
    }

    *metadataSize = maxEnd;
    *version = metadataVersion;
    *repairMagic = magic == 0;
    return true;
}

bool WriteUnityMetadataBlob(
    const BYTE* base,
    DWORD size,
    const std::wstring& outputPath,
    bool aggressiveRead,
    ReconstructionStats* stats,
    DWORD* error,
    bool repairMagic = false) {
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
    if (ok && repairMagic) {
        constexpr DWORD kUnityMetadataMagic = 0xFAB11BAF;
        DWORD written = 0;
        LARGE_INTEGER zero{};
        SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
        ok = WriteFile(file, &kUnityMetadataMagic, sizeof(kUnityMetadataMagic), &written, nullptr) == TRUE &&
             written == sizeof(kUnityMetadataMagic);
        if (!ok) {
            *error = GetLastError();
        }
    }
    CloseHandle(file);
    return ok;
}

void DeleteFileIfExists(const std::wstring& path) {
    DeleteFileW(path.c_str());
}

bool IsPoorRepairedMetadataDump(DWORD size, const ReconstructionStats& stats) {
    if (size == 0) {
        return true;
    }

    ULONGLONG allowedBadBytes = static_cast<ULONGLONG>(size) / 20;
    return stats.unreadableBytes > allowedBadBytes || stats.zeroBytes > allowedBadBytes;
}

struct MetadataMarker {
    const char* text;
    const wchar_t* label;
};

constexpr MetadataMarker kManagedMetadataMarkers[] = {
    {"Assembly-CSharp.dll", L"assembly-csharp"},
    {"Assembly-CSharp-firstpass.dll", L"assembly-csharp-firstpass"},
    {"HybridCLR.Runtime.dll", L"hybridclr-runtime"},
    {"HybridCLR.RuntimeApi::LoadMetadataForAOTAssembly", L"hybridclr-loadmetadata"},
    {"global-metadata.dat", L"global-metadata-name"},
};

SIZE_T MaxMarkerOverlap() {
    SIZE_T maxLength = 3;
    for (const auto& marker : kManagedMetadataMarkers) {
        SIZE_T length = std::strlen(marker.text);
        if (length > maxLength) {
            maxLength = length;
        }
    }

    return maxLength - 1;
}

bool ContainsMarker(const BYTE* data, SIZE_T dataSize, const MetadataMarker** matchedMarker, SIZE_T* markerOffset) {
    for (const auto& marker : kManagedMetadataMarkers) {
        SIZE_T markerLength = std::strlen(marker.text);
        if (markerLength == 0 || markerLength > dataSize) {
            continue;
        }

        for (SIZE_T i = 0; i + markerLength <= dataSize; ++i) {
            if (std::memcmp(data + i, marker.text, markerLength) == 0) {
                *matchedMarker = &marker;
                *markerOffset = i;
                return true;
            }
        }
    }

    return false;
}

bool WasWindowDumped(const std::vector<const BYTE*>& dumpedStarts, const BYTE* start) {
    for (const BYTE* dumped : dumpedStarts) {
        if (dumped == start) {
            return true;
        }
    }

    return false;
}

bool TryGetDotNetMetadataSize(const BYTE* candidate, bool aggressiveRead, DWORD* metadataSize) {
    constexpr DWORD kDotNetMetadataMagic = 0x424A5342;
    constexpr DWORD kProbeSize = 4096;
    constexpr DWORD kMaxMetadataSize = 128 * 1024 * 1024;

    std::array<BYTE, kProbeSize> header{};
    SIZE_T bytesRead = 0;
    if (!ReadMemoryBlock(candidate, header.data(), static_cast<DWORD>(header.size()), aggressiveRead, &bytesRead)) {
        return false;
    }

    if (ReadLe32(header.data()) != kDotNetMetadataMagic) {
        return false;
    }

    DWORD versionLength = ReadLe32(header.data() + 12);
    if (versionLength == 0 || versionLength > 512 || 16 + versionLength + 4 > kProbeSize) {
        return false;
    }

    DWORD streamHeaderOffset = AlignUp(16 + versionLength, 4);
    if (streamHeaderOffset + 4 > kProbeSize) {
        return false;
    }

    WORD streamCount = static_cast<WORD>(header[streamHeaderOffset + 2] | (header[streamHeaderOffset + 3] << 8));
    if (streamCount == 0 || streamCount > 16) {
        return false;
    }

    DWORD cursor = streamHeaderOffset + 4;
    DWORD maxEnd = cursor;
    for (WORD i = 0; i < streamCount; ++i) {
        if (cursor + 8 > kProbeSize) {
            return false;
        }

        DWORD offset = ReadLe32(header.data() + cursor);
        DWORD size = ReadLe32(header.data() + cursor + 4);
        cursor += 8;

        DWORD nameStart = cursor;
        while (cursor < kProbeSize && header[cursor] != 0) {
            ++cursor;
        }

        if (cursor == nameStart || cursor >= kProbeSize) {
            return false;
        }

        ++cursor;
        cursor = AlignUp(cursor, 4);

        if (offset > kMaxMetadataSize || size > kMaxMetadataSize - offset) {
            return false;
        }

        DWORD end = offset + size;
        if (end > maxEnd) {
            maxEnd = end;
        }
    }

    if (maxEnd < 0x100 || maxEnd > kMaxMetadataSize) {
        return false;
    }

    *metadataSize = maxEnd;
    return true;
}

DWORD DumpDotNetMetadataRoot(
    const BYTE* candidate,
    const std::wstring& outputDir,
    const std::wstring& logPath,
    bool aggressiveRead,
    std::vector<const BYTE*>* dumpedRoots) {
    if (WasWindowDumped(*dumpedRoots, candidate)) {
        return 0;
    }

    DWORD metadataSize = 0;
    if (!TryGetDotNetMetadataSize(candidate, aggressiveRead, &metadataSize)) {
        return 0;
    }

    dumpedRoots->push_back(candidate);
    std::wstring fileName = L"dotnet-metadata_" + std::to_wstring(dumpedRoots->size() - 1) + L".bin";
    std::wstring outputPath = BuildUniqueFilePath(outputDir, fileName);

    ReconstructionStats stats{};
    DWORD error = ERROR_SUCCESS;
    if (!WriteUnityMetadataBlob(candidate, metadataSize, outputPath, aggressiveRead, &stats, &error)) {
        Log(logPath, L"dotnet_metadata failed_write path=" + outputPath + L" error=" + std::to_wstring(error));
        return 0;
    }

    Log(
        logPath,
        L"dotnet_metadata path=" + outputPath +
            L" base=" + FormatHexPtr(candidate) +
            L" size=" + std::to_wstring(metadataSize) +
            L" zero_bytes=" + std::to_wstring(stats.zeroBytes) +
            L" unreadable_bytes=" + std::to_wstring(stats.unreadableBytes) +
            L" read_failure_bytes=" + std::to_wstring(stats.readFailureBytes) +
            L" protect_recovered_bytes=" + std::to_wstring(stats.protectRecoveredBytes));
    return 1;
}

DWORD DumpManagedMetadataWindow(
    const MEMORY_BASIC_INFORMATION& mbi,
    const BYTE* markerAddress,
    const MetadataMarker& marker,
    const std::wstring& outputDir,
    const std::wstring& logPath,
    bool aggressiveRead,
    std::vector<const BYTE*>* dumpedStarts) {
    constexpr SIZE_T kWindowBefore = 16 * 1024 * 1024;
    constexpr SIZE_T kWindowSize = 96 * 1024 * 1024;

    const BYTE* regionBase = reinterpret_cast<const BYTE*>(mbi.BaseAddress);
    const BYTE* regionEnd = regionBase + mbi.RegionSize;
    const BYTE* windowStart = markerAddress > regionBase + kWindowBefore ? markerAddress - kWindowBefore : regionBase;
    SIZE_T available = regionEnd > windowStart ? static_cast<SIZE_T>(regionEnd - windowStart) : 0;
    SIZE_T windowSize = available > kWindowSize ? kWindowSize : available;
    if (windowSize == 0 || windowSize > MAXDWORD || WasWindowDumped(*dumpedStarts, windowStart)) {
        return 0;
    }

    dumpedStarts->push_back(windowStart);
    std::wstring fileName =
        L"managed-metadata-region_" + std::to_wstring(dumpedStarts->size() - 1) +
        L"_" + marker.label + L".bin";
    std::wstring outputPath = BuildUniqueFilePath(outputDir, fileName);

    ReconstructionStats stats{};
    DWORD error = ERROR_SUCCESS;
    if (!WriteUnityMetadataBlob(windowStart, static_cast<DWORD>(windowSize), outputPath, aggressiveRead, &stats, &error)) {
        Log(logPath, L"managed_metadata_region failed_write path=" + outputPath + L" error=" + std::to_wstring(error));
        return 0;
    }

    Log(
        logPath,
        L"managed_metadata_region path=" + outputPath +
            L" marker=" + marker.label +
            L" marker_address=" + FormatHexPtr(markerAddress) +
            L" base=" + FormatHexPtr(windowStart) +
            L" size=" + std::to_wstring(windowSize) +
            L" zero_bytes=" + std::to_wstring(stats.zeroBytes) +
            L" unreadable_bytes=" + std::to_wstring(stats.unreadableBytes) +
            L" read_failure_bytes=" + std::to_wstring(stats.readFailureBytes) +
            L" protect_recovered_bytes=" + std::to_wstring(stats.protectRecoveredBytes));
    return 1;
}

DWORD DumpUnityMetadata(const std::wstring& dumpPath, const std::wstring& logPath, bool aggressiveRead) {
    constexpr DWORD kUnityMetadataMagic = 0xFAB11BAF;
    constexpr SIZE_T kScanChunkSize = 64 * 1024;

    std::wstring outputDir = BuildSidecarDirectory(dumpPath, L".unity_metadata");
    if (!CreateDirectoryW(outputDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log(logPath, L"Failed to create Unity metadata directory. error=" + std::to_wstring(GetLastError()) + L" path=" + outputDir);
        return 0;
    }

    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);

    SIZE_T overlap = MaxMarkerOverlap();
    std::vector<BYTE> buffer(kScanChunkSize + overlap);
    const BYTE* current = reinterpret_cast<const BYTE*>(systemInfo.lpMinimumApplicationAddress);
    const BYTE* maxAddress = reinterpret_cast<const BYTE*>(systemInfo.lpMaximumApplicationAddress);
    DWORD index = 0;
    DWORD managedRegionIndex = 0;
    ULONGLONG totalBytes = 0;
    ULONGLONG scannedBytes = 0;
    ULONGLONG scannedRegions = 0;
    ULONGLONG rawMagicCandidates = 0;
    ULONGLONG zeroMagicCandidates = 0;
    ULONGLONG invalidHeaderCandidates = 0;
    ULONGLONG managedMarkerCandidates = 0;
    ULONGLONG dotNetMetadataCandidates = 0;
    static std::vector<const BYTE*> dumpedManagedStarts;
    static std::vector<const BYTE*> dumpedDotNetRoots;

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
            ++scannedRegions;
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
                    scannedBytes += bytesRead;
                    const MetadataMarker* matchedMarker = nullptr;
                    SIZE_T markerOffset = 0;
                    if (ContainsMarker(buffer.data(), bytesRead, &matchedMarker, &markerOffset)) {
                        ++managedMarkerCandidates;
                        managedRegionIndex += DumpManagedMetadataWindow(
                            mbi,
                            chunkBase + markerOffset,
                            *matchedMarker,
                            outputDir,
                            logPath,
                            aggressiveRead,
                            &dumpedManagedStarts);
                    }

                    for (SIZE_T i = 0; i + 8 <= bytesRead; ++i) {
                        if (ReadLe32(buffer.data() + i) == 0x424A5342) {
                            dotNetMetadataCandidates += DumpDotNetMetadataRoot(
                                chunkBase + i,
                                outputDir,
                                logPath,
                                aggressiveRead,
                                &dumpedDotNetRoots);
                        }

                        DWORD candidateMagic = ReadLe32(buffer.data() + i);
                        if (candidateMagic != kUnityMetadataMagic) {
                            if (candidateMagic != 0 ||
                                (reinterpret_cast<UINT_PTR>(chunkBase + i) % 4) != 0 ||
                                i + 12 > bytesRead ||
                                ReadLe32(buffer.data() + i + 4) < 16 ||
                                ReadLe32(buffer.data() + i + 4) > 31 ||
                                ReadLe32(buffer.data() + i + 8) < 0x20 ||
                                ReadLe32(buffer.data() + i + 8) > 4096 ||
                                (ReadLe32(buffer.data() + i + 8) % 4) != 0) {
                                continue;
                            }
                            ++zeroMagicCandidates;
                        } else {
                            ++rawMagicCandidates;
                        }

                        const BYTE* candidate = chunkBase + i;
                        DWORD metadataSize = 0;
                        DWORD version = 0;
                        bool repairMagic = false;
                        if (!TryGetUnityMetadataSize(candidate, aggressiveRead, &metadataSize, &version, &repairMagic)) {
                            ++invalidHeaderCandidates;
                            continue;
                        }

                        std::wstring fileName = index == 0 ?
                            L"global-metadata_dump.dat" :
                            AddSuffixBeforeExtension(L"global-metadata_dump.dat", L"_" + std::to_wstring(index));
                        std::wstring outputPath = BuildUniqueFilePath(outputDir, fileName);
                        ReconstructionStats stats{};
                        DWORD error = ERROR_SUCCESS;
                        if (WriteUnityMetadataBlob(candidate, metadataSize, outputPath, aggressiveRead, &stats, &error, repairMagic)) {
                            if (repairMagic && IsPoorRepairedMetadataDump(metadataSize, stats)) {
                                DeleteFileIfExists(outputPath);
                                Log(
                                    logPath,
                                    L"unity_metadata rejected_poor_repaired path=" + outputPath +
                                        L" base=" + FormatHexPtr(candidate) +
                                        L" size=" + std::to_wstring(metadataSize) +
                                        L" version=" + std::to_wstring(version) +
                                        L" zero_bytes=" + std::to_wstring(stats.zeroBytes) +
                                        L" unreadable_bytes=" + std::to_wstring(stats.unreadableBytes));
                                continue;
                            }

                            ++index;
                            totalBytes += metadataSize;
                            Log(
                                logPath,
                                L"unity_metadata path=" + outputPath +
                                    L" base=" + FormatHexPtr(candidate) +
                                    L" size=" + std::to_wstring(metadataSize) +
                                    L" version=" + std::to_wstring(version) +
                                    L" repaired_magic=" + std::to_wstring(repairMagic ? 1 : 0) +
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
                regionOffset += kScanChunkSize - overlap;
            }
        }

        if (end <= current) {
            break;
        }
        current = end;
    }

    if (index == 0) {
        Log(
            logPath,
            L"No valid in-memory Unity global-metadata.dat header found. raw_magic_candidates=" +
                std::to_wstring(rawMagicCandidates) +
                L" zero_magic_candidates=" + std::to_wstring(zeroMagicCandidates) +
                L" invalid_header_candidates=" + std::to_wstring(invalidHeaderCandidates) +
                L" managed_marker_candidates=" + std::to_wstring(managedMarkerCandidates) +
                L" managed_regions=" + std::to_wstring(managedRegionIndex) +
                L" dotnet_metadata=" + std::to_wstring(dotNetMetadataCandidates));
    }

    Log(
        logPath,
        L"Unity metadata dump end. count=" + std::to_wstring(index) +
            L" managed_regions=" + std::to_wstring(managedRegionIndex) +
            L" dotnet_metadata=" + std::to_wstring(dotNetMetadataCandidates) +
            L" total_bytes=" + std::to_wstring(totalBytes) +
            L" scanned_regions=" + std::to_wstring(scannedRegions) +
            L" scanned_bytes=" + std::to_wstring(scannedBytes));
    return index;
}
}  // namespace ipd
