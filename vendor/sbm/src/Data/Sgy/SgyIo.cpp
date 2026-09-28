#include "Data/Sgy/SgyIo.h"

#include <segyio/segy.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace seismic {
namespace sgyio {

std::string ToUtf8Path(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring widePath = path.wstring();
    const int size = WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(size > 0) {
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, result.data(), size, nullptr, nullptr);
        if(!result.empty() && result.back() == '\0') {
            result.pop_back();
        }
        return result;
    }
#endif
    return path.string();
}

std::string ToAnsiPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring widePath = path.wstring();
    const int size = WideCharToMultiByte(CP_ACP, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(size > 0) {
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_ACP, 0, widePath.c_str(), -1, result.data(), size, nullptr, nullptr);
        if(!result.empty() && result.back() == '\0') {
            result.pop_back();
        }
        return result;
    }
#endif
    return path.string();
}

segy_datasource* OpenReadOnly(const std::filesystem::path& path) {
    if(auto* file = segy_open(ToUtf8Path(path).c_str(), "rb")) {
        return file;
    }
    if(auto* file = segy_open(ToAnsiPath(path).c_str(), "rb")) {
        return file;
    }
    return nullptr;
}

Handle::~Handle() {
    Reset();
}

Handle& Handle::operator=(Handle&& other) noexcept {
    if(this != &other) {
        Reset();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void Handle::Reset() {
    if(handle_ != nullptr) {
        segy_close(handle_);
        handle_ = nullptr;
    }
}

} // namespace sgyio
} // namespace seismic
