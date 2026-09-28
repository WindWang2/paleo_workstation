#pragma once

#include <filesystem>
#include <string>

struct segy_datasource;

namespace seismic {
namespace sgyio {

// Converts a path to the encoding segyio expects on Windows. segyio 2.x
// converts UTF-8 input to UTF-16 and calls _wfopen, so UTF-8 handles Chinese
// paths directly; the ANSI variant is the fallback for older segyio builds.
std::string ToUtf8Path(const std::filesystem::path& path);
std::string ToAnsiPath(const std::filesystem::path& path);

// Opens a SEG-Y file read-only. Never copies, links or modifies the source.
segy_datasource* OpenReadOnly(const std::filesystem::path& path);

// P5 (paleo_workstation): the production volumes keep the standard INLINE@188 /
// CROSSLINE@192 words zero and encode inline in field record @8 plus crossline
// in CDP @20 (see tools/make_segy_fixture.py and src/io/segyreader.cpp — the
// conventions SegyReader freezes on). The fallback engages ONLY when BOTH
// standard words are zero, so upstream-conformant files keep their exact
// upstream behavior everywhere this helper replaces the raw field reads.
struct SgyTraceKeyWords {
    int inlineNo = 0;
    int xlineNo = 0;
};
SgyTraceKeyWords ReadTraceKeyWords(const char* traceHeader);

// RAII wrapper around a segy_datasource handle.
class Handle {
public:
    Handle() = default;
    explicit Handle(segy_datasource* handle) : handle_(handle) {}
    ~Handle();

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Handle& operator=(Handle&& other) noexcept;

    segy_datasource* Get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }
    void Reset();

private:
    segy_datasource* handle_ = nullptr;
};

} // namespace sgyio
} // namespace seismic
