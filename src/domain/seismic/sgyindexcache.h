#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "domain/seismic/sgyindex.h"

namespace seismic {

// On-disk geometry index cache.
//
// Identity: normalized path, file size, high-resolution mtime and a sampling
// fingerprint over the first/last 32 KB. The fingerprint is explicitly NOT a
// full-file checksum; it makes accidental mismatches (edited/replaced files)
// extremely unlikely without reading tens of gigabytes.
//
// Format: explicit little-endian fields with a magic, a cache format version
// and an algorithm version. STL memory layouts are never serialized. Loads are
// bounded and verified with a payload hash, so a truncated or corrupted file
// falls back to a full scan instead of producing a wrong index.
class SgyIndexCache {
public:
    static constexpr std::uint32_t kCacheFormatVersion = 3;
    static constexpr std::uint32_t kAlgorithmVersion = 2;

    // %LOCALAPPDATA%\SeismicF3Viewer\index-cache (override: SEISMIC_INDEX_CACHE_DIR).
    static std::filesystem::path CacheDirectory();
    static std::filesystem::path CachePathFor(const std::filesystem::path& sgyPath);

    // Returns the cached index, or nullptr with a human-readable reason
    // (missing / invalidated / corrupted / incompatible).
    static SgyIndexPtr Load(const std::filesystem::path& sgyPath, std::string& reason);

    // Atomic publish: temp file -> close -> re-read and verify -> replace.
    // Refuses to publish when the source file changed while indexing.
    static bool Save(const SgyIndexPtr& index, std::string& errorMessage);

    // Force-rebuild helper: deletes the cache for this file.
    static bool Remove(const std::filesystem::path& sgyPath, std::string& errorMessage);

    // 64-bit FNV-1a over the first/last 32 KB plus the file size.
    static std::uint64_t FingerprintFile(const std::filesystem::path& path, std::uintmax_t fileSize);
};

} // namespace seismic
