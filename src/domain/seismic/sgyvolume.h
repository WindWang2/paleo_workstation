#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "domain/seismic/sgyindex.h"

struct segy_datasource;

namespace seismic {

// One bounded sparse temporal slab per view. Only complete trace windows are
// retained across cancellation; an incomplete image is never published.
struct SgyTimePreviewCache {
    std::mutex mutex;
    SgyIndexPtr index;
    int width = 0, height = 0, begin = -1, count = 0;
    std::vector<float> values;
    std::vector<unsigned char> filled;
    std::uint64_t traceReads = 0;
};

enum class SgySliceType {
    Inline = 0,
    Xline = 1,
    Time = 2,
};

struct SgySliceImage {
    int width = 0;
    int height = 0;
    float valueMin = 0.0f;
    float valueMax = 0.0f;
    // Raw amplitudes (row-major). NaN marks a cell that is NOT valid data:
    // a missing trace, an unreadable trace, a rule-layout mismatch or a column
    // that has not been read yet. Validity travels with the values through
    // colouring and hover queries; nothing is silently treated as zero.
    std::vector<float> values;
    std::vector<unsigned char> rgba;

    bool Valid(int x, int y) const;
    float Value(int x, int y) const;
};

// Reports indexing progress as (processedTraceHeaders, totalTraceCount).
// Returning false cancels the scan. Called every few thousand trace headers
// and once more when the scan finished.
using LoadProgressCallback = std::function<bool(int processed, int total)>;

// SEG-Y volume accessor. The geometry index is immutable and shared through
// std::shared_ptr<const SgyIndex>, so copying a volume is cheap and never
// duplicates the index. Only a complete (fully scanned) index can extract
// slices; declared-range snapshots are for temporary frames only.
class SgyVolume {
public:
    // Synchronous index build. The GUI uses SgyIndexJob instead so the UI
    // thread is never blocked; the CLI keeps this entry point.
    bool Load(
        const std::filesystem::path& path,
        std::string& errorMessage,
        const LoadProgressCallback& progress = {});

    // Adopts an index built elsewhere (background job or cache).
    void AdoptIndex(SgyIndexPtr index);

    const SgyIndexPtr& Index() const { return index_; }
    bool HasIndex() const { return index_ != nullptr; }
    bool IsIndexComplete() const { return index_ != nullptr && index_->complete; }
    bool IsRuleBased() const { return index_ != nullptr && index_->ruleBased; }
    bool IsDeclaredRangeOnly() const {
        return index_ != nullptr && !index_->complete && index_->fromTextHeader;
    }
    std::size_t IndexMemoryBytes() const { return index_ ? index_->MemoryBytes() : 0; }
    const SgyRuleLayout& Rule() const;

    // progress(processedColumns, totalColumns) is called per line and may
    // cancel the extraction (returns false).
    bool ExtractSlice(
        SgySliceType type,
        int requestedIndex,
        SgySliceImage& image,
        std::string& errorMessage,
        const std::function<bool(int processed, int total)>& progress = {}) const;

    // Bounded real preview: reads at most maxColumns traces spread over the
    // requested line. Real sampled columns keep their exact values; unsampled
    // display columns repeat the nearest sampled trace so the texture is a
    // continuous low-resolution preview rather than mostly-NaN grey pixels.
    // A selected trace that is genuinely missing remains NaN.
    // compact=true returns only sampled columns for interactive GPU stretching;
    // unlike full-width previews it is display-only, not an exact grid image.
    bool ExtractPreviewSlice(
        SgySliceType type,
        int requestedIndex,
        int maxColumns,
        SgySliceImage& image,
        std::string& errorMessage,
        int& columnsRead,
        int& totalColumns,
        bool compact = false,
        const std::function<bool(int, int)>& progress = {}) const;
    // Fast, bounded time-slice preview. It reads one sample from a sparse
    // inline/xline grid instead of reading every complete trace in the volume.
    // The returned image is deliberately smaller and is stretched over the
    // complete survey plane by the renderer while the full slice is refined in
    // the background.
    bool ExtractTimeSlicePreview(
        int sampleIndex,
        int maxInlineRows,
        int maxXlineColumns,
        SgySliceImage& image,
        std::string& errorMessage,
        const std::function<bool(int processed, int total)>& progress = {},
        SgyTimePreviewCache* cache = nullptr) const;
    bool ExtractLineSlice(
        int startInline,
        int startXline,
        int endInline,
        int endXline,
        SgySliceImage& image,
        int& sampleColumns,
        std::string& errorMessage) const;
    bool ExtractLineSlice(
        const std::vector<glm::ivec2>& pathPoints,
        SgySliceImage& image,
        int& sampleColumns,
        std::string& errorMessage,
        bool keepOutsideColumns = false,
        int maxColumns = 2048) const;
    bool ReadSampleValue(
        int inlineNo,
        int xlineNo,
        int sampleIndex,
        float& value,
        std::string& errorMessage) const;

    // Re-colours an existing image from its stored values (colour map or
    // contrast changes must not re-read the SEG-Y file).
    static void Recolorize(SgySliceImage& image, float fixedAbsMax = 0.0f);

    bool IsLoaded() const { return IsIndexComplete(); }
    const std::filesystem::path& Path() const;
    int TraceCount() const { return index_ ? index_->traceCount : 0; }
    int ScannedTraceCount() const { return index_ ? index_->scannedTraceCount : 0; }
    int SampleCount() const { return index_ ? index_->sampleCount : 0; }
    int SampleIntervalUs() const { return index_ ? index_->sampleIntervalUs : 0; }
    int FormatCode() const { return index_ ? index_->formatCode : 0; }
    int InlineMin() const { return index_ ? index_->inlineMin : 0; }
    int InlineMax() const { return index_ ? index_->inlineMax : 0; }
    int XlineMin() const { return index_ ? index_->xlineMin : 0; }
    int XlineMax() const { return index_ ? index_->xlineMax : 0; }
    int SampleMin() const { return 0; }
    int SampleMax() const { return std::max(0, SampleCount() - 1); }
    int InlineCount() const { return index_ ? index_->InlineCount() : 0; }
    int XlineCount() const { return index_ ? index_->XlineCount() : 0; }
    const std::vector<int>& InlineValues() const;
    const std::vector<int>& XlineValues() const;
    // UI controls operate on numeric ranges; surveys can have missing lines
    // or increments greater than one. Resolve the displayed number as well as
    // the extracted slice so the label and rendered geometry stay consistent.
    int FindNearestInlineValue(float inlineNo) const;
    // Exact axis models and lookups. A coordinate that is not present must be
    // reported as missing (NotFound) and must never be snapped to a neighbour;
    // FindNearest* is for explicit UI snapping only.
    AxisDescriptor InlineAxis() const;
    AxisDescriptor XlineAxis() const;
    bool ExactInlineValue(int requested, int& value) const;
    bool ExactXlineValue(int requested, int& value) const;
    int FindNearestXlineValue(float xlineNo) const;
    // Trace lookup through the shared index (map or sampled rule layout).
    int FindTraceIndex(int inlineNo, int xlineNo) const;

private:
    bool ReadTraceAsFloat(int traceIndex, std::vector<float>& samples, std::string& errorMessage) const;
    bool ReadTraceAsFloat(segy_datasource* file, int traceIndex, std::vector<float>& samples, std::string& errorMessage) const;
    bool ReadSampleAsFloat(
        segy_datasource* file,
        int traceIndex,
        int sampleIndex,
        float& value,
        std::string& errorMessage) const;
    // Rule-based indexes locate traces by formula; the real header must match
    // the expectation before its samples may be used.
    bool ValidateRuleTrace(segy_datasource* file, int traceIndex, std::string& errorMessage) const;

    SgyIndexPtr index_;
};

} // namespace seismic
