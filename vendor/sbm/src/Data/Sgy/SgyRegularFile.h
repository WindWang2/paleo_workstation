#pragma once

#include <filesystem>
#include <system_error>

namespace seismic {

// POSIX streams may open directories successfully. Check before reading, with
// the non-throwing filesystem overload; callers retain their own open order,
// diagnostic and fallback policy.
inline bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

} // namespace seismic
