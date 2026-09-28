#include "Data/Sgy/SgyVolume.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <QFile>
#include <QString>

#include <segyio/segy.h>

#include "Data/Sgy/SgyIndexBuilder.h"
#include "Data/Sgy/SgyIo.h"

namespace seismic {
namespace {

bool Check(int err, const char* op, std::string& errorMessage) {
    if(err == SEGY_OK) {
        return true;
    }
    std::ostringstream oss;
    oss << op << " failed, segyio error code = " << err;
    errorMessage = oss.str();
    return false;
}

unsigned char ToByte(float value) {
    return static_cast<unsigned char>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

const std::vector<int>& EmptyIntVector() {
    static const std::vector<int> empty;
    return empty;
}

void ColorizeValues(SgySliceImage& image, float fixedAbsMax = 0.0f) {
    image.rgba.clear();
    if(image.width <= 0 || image.height <= 0 || image.values.empty()) {
        image.valueMin = 0.0f;
        image.valueMax = 1.0f;
        return;
    }

    image.valueMin = std::numeric_limits<float>::max();
    image.valueMax = std::numeric_limits<float>::lowest();
    for(float value : image.values) {
        if(std::isfinite(value)) {
            image.valueMin = std::min(image.valueMin, value);
            image.valueMax = std::max(image.valueMax, value);
        }
    }
    if(image.valueMin > image.valueMax) {
        image.valueMin = 0.0f;
        image.valueMax = 1.0f;
    }

    const float absMax = std::isfinite(fixedAbsMax) && fixedAbsMax > 0.0f ? fixedAbsMax
        : std::max(std::abs(image.valueMin), std::abs(image.valueMax));
    const float scale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;
    const std::size_t pixelCount =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    image.rgba.resize(pixelCount * 4, 255);
    for(std::size_t index = 0; index < pixelCount; ++index) {
        const std::size_t offset = index * 4;
        const float rawValue = index < image.values.size() ? image.values[index]
                                                           : std::numeric_limits<float>::quiet_NaN();
        if(!std::isfinite(rawValue)) {
            // "Not loaded / not valid" placeholder - never zero amplitude.
            image.rgba[offset + 0] = 48;
            image.rgba[offset + 1] = 49;
            image.rgba[offset + 2] = 49;
            image.rgba[offset + 3] = 255;
            continue;
        }

        float value = rawValue * scale;
        constexpr float contrast = 1.45f;
        constexpr float gamma = 0.82f;
        value = std::clamp(value * contrast, -1.0f, 1.0f);
        const float magnitude = std::pow(std::abs(value), gamma);
        if(value < 0.0f) {
            const float k = 1.0f - magnitude;
            image.rgba[offset + 0] = ToByte(0.88f * k);
            image.rgba[offset + 1] = ToByte(0.92f * k);
            image.rgba[offset + 2] = 255;
        } else {
            const float k = 1.0f - magnitude;
            image.rgba[offset + 0] = 255;
            image.rgba[offset + 1] = ToByte(0.90f * k);
            image.rgba[offset + 2] = ToByte(0.86f * k);
        }
        image.rgba[offset + 3] = 255;
    }
}

void FinishImage(std::vector<float>&& values, int width, int height, SgySliceImage& image) {
    image = {};
    image.width = width;
    image.height = height;
    image.values = std::move(values);
    ColorizeValues(image);
}

} // namespace

bool SgySliceImage::Valid(int x, int y) const {
    if(x < 0 || y < 0 || x >= width || y >= height) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x);
    return index < values.size() && std::isfinite(values[index]);
}

float SgySliceImage::Value(int x, int y) const {
    if(!Valid(x, y)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x);
    return values[index];
}

void SgyVolume::Recolorize(SgySliceImage& image, float fixedAbsMax) {
    ColorizeValues(image, fixedAbsMax);
}

bool SgyVolume::Load(
    const std::filesystem::path& path,
    std::string& errorMessage,
    const LoadProgressCallback& progress) {
    SgyIndexPtr built;
    if(!SgyIndexBuilder::Build(path, built, errorMessage, progress)) {
        return false;
    }
    index_ = std::move(built);
    timeGridCache_ = std::make_shared<TimeGridCache>();
    return true;
}

void SgyVolume::AdoptIndex(SgyIndexPtr index) {
    index_ = std::move(index);
    timeGridCache_ = std::make_shared<TimeGridCache>();
}

const std::filesystem::path& SgyVolume::Path() const {
    static const std::filesystem::path empty;
    return index_ ? index_->path : empty;
}

const std::vector<int>& SgyVolume::InlineValues() const {
    return index_ ? index_->inlineValues : EmptyIntVector();
}

const std::vector<int>& SgyVolume::XlineValues() const {
    return index_ ? index_->xlineValues : EmptyIntVector();
}

AxisDescriptor SgyVolume::InlineAxis() const {
    return index_ ? index_->InlineAxis() : AxisDescriptor();
}

AxisDescriptor SgyVolume::XlineAxis() const {
    return index_ ? index_->XlineAxis() : AxisDescriptor();
}

bool SgyVolume::ExactInlineValue(int requested, int& value) const {
    int index = 0;
    if(!InlineAxis().ExactIndexOf(requested, index)) {
        return false;
    }
    value = InlineAxis().ValueAt(index);
    return true;
}

bool SgyVolume::ExactXlineValue(int requested, int& value) const {
    int index = 0;
    if(!XlineAxis().ExactIndexOf(requested, index)) {
        return false;
    }
    value = XlineAxis().ValueAt(index);
    return true;
}

int SgyVolume::FindNearestInlineValue(float inlineNo) const {
    return index_ ? index_->FindNearestInlineValue(inlineNo) : 0;
}

int SgyVolume::FindNearestXlineValue(float xlineNo) const {
    return index_ ? index_->FindNearestXlineValue(xlineNo) : 0;
}

int SgyVolume::FindTraceIndex(int inlineNo, int xlineNo) const {
    return index_ ? index_->FindTraceIndex(inlineNo, xlineNo) : -1;
}

const SgyRuleLayout& SgyVolume::Rule() const {
    static const SgyRuleLayout empty;
    return index_ ? index_->rule : empty;
}

void SgyVolume::EnsureTimeSliceGrid() const {
    if(!timeGridCache_) {
        timeGridCache_ = std::make_shared<TimeGridCache>();
    }
    std::lock_guard<std::mutex> lock(timeGridCache_->mutex);
    if(!timeGridCache_->traceIndices.empty()) {
        return;
    }
    const int inlCount = InlineCount();
    const int xlCount = XlineCount();
    if(inlCount <= 0 || xlCount <= 0) {
        return;
    }
    const std::size_t totalPixels = static_cast<std::size_t>(inlCount * xlCount);
    timeGridCache_->traceIndices.assign(totalPixels, -1);

    const auto& inlVals = InlineValues();
    const auto& xlVals = XlineValues();

    for(int i = 0; i < inlCount; ++i) {
        const int row = inlCount - 1 - i;
        const int inlineNo = inlVals[static_cast<std::size_t>(i)];
        for(int x = 0; x < xlCount; ++x) {
            const int xlineNo = xlVals[static_cast<std::size_t>(x)];
            timeGridCache_->traceIndices[static_cast<std::size_t>(row * xlCount + x)] =
                FindTraceIndex(inlineNo, xlineNo);
        }
    }
}

bool SgyVolume::ValidateRuleTrace(segy_datasource* file, int traceIndex, std::string& errorMessage) const {
    if(!index_ || !index_->ruleBased) {
        return true;
    }
    int expectedInline = 0;
    int expectedXline = 0;
    if(!index_->rule.ExpectedInlineXline(traceIndex, expectedInline, expectedXline)) {
        errorMessage = "rule layout validation failed: trace index out of the sampled layout";
        return false;
    }

    std::array<char, SEGY_TRACE_HEADER_SIZE> header{};
    if(segy_read_standard_traceheader(file, traceIndex, header.data()) != SEGY_OK) {
        errorMessage = "rule layout validation failed: trace header is unreadable";
        return false;
    }
    // P5: same trace-key words (incl. the paleo @8/@20 fallback) the layout was
    // probed with, so validation never contradicts the probe.
    const sgyio::SgyTraceKeyWords words = sgyio::ReadTraceKeyWords(header.data());
    if(words.inlineNo != expectedInline || words.xlineNo != expectedXline) {
        std::ostringstream oss;
        oss << "rule layout validation failed at trace " << traceIndex << ": expected " << expectedInline << "/"
            << expectedXline << " but found " << words.inlineNo << "/" << words.xlineNo
            << ". The sampled layout does not match the real file; use the full index instead.";
        errorMessage = oss.str();
        return false;
    }
    return true;
}

bool SgyVolume::ReadTraceAsFloat(int traceIndex, std::vector<float>& samples, std::string& errorMessage) const {
    if(!index_) {
        errorMessage = "SGY volume has no index.";
        return false;
    }
    sgyio::Handle file(sgyio::OpenReadOnly(index_->path));
    if(!file) {
        errorMessage = "segy_open failed while reading trace.";
        return false;
    }
    if(!Check(segy_collect_metadata(file.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }
    return ReadTraceAsFloat(file.Get(), traceIndex, samples, errorMessage);
}

bool SgyVolume::ReadTraceAsFloat(segy_datasource* file, int traceIndex, std::vector<float>& samples, std::string& errorMessage) const {
    const int sampleCount = SampleCount();
    const int formatCode = FormatCode();
    const int formatSizeBytes = index_ ? index_->formatSizeBytes : 0;

    if(!ValidateRuleTrace(file, traceIndex, errorMessage)) {
        return false;
    }

    samples.assign(static_cast<std::size_t>(sampleCount), 0.0f);
    if(formatSizeBytes == 4 && (formatCode == SEGY_IEEE_FLOAT_4_BYTE || formatCode == SEGY_IBM_FLOAT_4_BYTE)) {
        if(!Check(segy_readtrace(file, traceIndex, samples.data()), "segy_readtrace", errorMessage)) {
            return false;
        }
        if(!Check(segy_to_native(formatCode, sampleCount, samples.data()), "segy_to_native", errorMessage)) {
            return false;
        }
        return true;
    }

    if(formatSizeBytes == 2) {
        std::vector<std::int16_t> raw(static_cast<std::size_t>(sampleCount));
        if(!Check(segy_readtrace(file, traceIndex, raw.data()), "segy_readtrace", errorMessage)) {
            return false;
        }
        if(!Check(segy_to_native(formatCode, sampleCount, raw.data()), "segy_to_native", errorMessage)) {
            return false;
        }
        for(int i = 0; i < sampleCount; ++i) {
            samples[static_cast<std::size_t>(i)] = static_cast<float>(raw[static_cast<std::size_t>(i)]);
        }
        return true;
    }

    errorMessage = "Unsupported SGY sample format for first slice version.";
    return false;
}

bool SgyVolume::ReadSampleAsFloat(
    segy_datasource* file,
    int traceIndex,
    int sampleIndex,
    float& value,
    std::string& errorMessage) const {
    if(sampleIndex < 0 || sampleIndex >= SampleCount()) {
        errorMessage = "SGY sample index is outside the trace.";
        return false;
    }
    if(!ValidateRuleTrace(file, traceIndex, errorMessage)) {
        return false;
    }

    const int formatCode = FormatCode();
    const int formatSizeBytes = index_ ? index_->formatSizeBytes : 0;
    if(formatSizeBytes == 4 &&
       (formatCode == SEGY_IEEE_FLOAT_4_BYTE || formatCode == SEGY_IBM_FLOAT_4_BYTE)) {
        float raw = 0.0f;
        if(!Check(segy_readsubtr(file, traceIndex, sampleIndex, sampleIndex + 1, 1, &raw, nullptr),
                  "segy_readsubtr", errorMessage)) {
            return false;
        }
        if(!Check(segy_to_native(formatCode, 1, &raw), "segy_to_native", errorMessage)) {
            return false;
        }
        value = raw;
        return true;
    }

    if(formatSizeBytes == 2) {
        std::int16_t raw = 0;
        if(!Check(segy_readsubtr(file, traceIndex, sampleIndex, sampleIndex + 1, 1, &raw, nullptr),
                  "segy_readsubtr", errorMessage)) {
            return false;
        }
        if(!Check(segy_to_native(formatCode, 1, &raw), "segy_to_native", errorMessage)) {
            return false;
        }
        value = static_cast<float>(raw);
        return true;
    }

    errorMessage = "Unsupported SGY sample format for time-slice preview.";
    return false;
}

bool SgyVolume::ExtractSlice(
    SgySliceType type,
    int requestedIndex,
    SgySliceImage& image,
    std::string& errorMessage,
    const std::function<bool(int processed, int total)>& progress) const {
    if(!IsLoaded()) {
        errorMessage = "SGY volume index is not complete.";
        return false;
    }

    const int sampleCount = SampleCount();
    std::vector<float> values;
    std::vector<float> traceSamples;
    sgyio::Handle file(sgyio::OpenReadOnly(index_->path));
    if(!file) {
        errorMessage = "segy_open failed while extracting slice.";
        return false;
    }
    if(!Check(segy_collect_metadata(file.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }

    if(type == SgySliceType::Inline) {
        int inlineNo = 0;
        if(!ExactInlineValue(requestedIndex, inlineNo)) {
            errorMessage = "inline " + std::to_string(requestedIndex) +
                           " is not present in this file (no nearest-trace substitution)";
            return false;
        }
        values.assign(static_cast<std::size_t>(XlineCount() * sampleCount), std::numeric_limits<float>::quiet_NaN());
        for(int x = 0; x < XlineCount(); ++x) {
            if(progress && (x % 64 == 0) && !progress(x, XlineCount())) {
                errorMessage = "Slice extraction cancelled by caller.";
                return false;
            }
            const int traceIndex = FindTraceIndex(inlineNo, XlineValues()[static_cast<std::size_t>(x)]);
            if(traceIndex < 0 || !ReadTraceAsFloat(file.Get(), traceIndex, traceSamples, errorMessage)) {
                continue;
            }
            for(int s = 0; s < sampleCount; ++s) {
                const int row = sampleCount - 1 - s;
                values[static_cast<std::size_t>(row * XlineCount() + x)] = traceSamples[static_cast<std::size_t>(s)];
            }
        }
        FinishImage(std::move(values), XlineCount(), sampleCount, image);
        return true;
    }

    if(type == SgySliceType::Xline) {
        int xlineNo = 0;
        if(!ExactXlineValue(requestedIndex, xlineNo)) {
            errorMessage = "xline " + std::to_string(requestedIndex) +
                           " is not present in this file (no nearest-trace substitution)";
            return false;
        }
        values.assign(static_cast<std::size_t>(InlineCount() * sampleCount), std::numeric_limits<float>::quiet_NaN());
        for(int i = 0; i < InlineCount(); ++i) {
            if(progress && (i % 64 == 0) && !progress(i, InlineCount())) {
                errorMessage = "Slice extraction cancelled by caller.";
                return false;
            }
            const int traceIndex = FindTraceIndex(InlineValues()[static_cast<std::size_t>(i)], xlineNo);
            if(traceIndex < 0 || !ReadTraceAsFloat(file.Get(), traceIndex, traceSamples, errorMessage)) {
                continue;
            }
            for(int s = 0; s < sampleCount; ++s) {
                const int row = sampleCount - 1 - s;
                values[static_cast<std::size_t>(row * InlineCount() + i)] = traceSamples[static_cast<std::size_t>(s)];
            }
        }
        FinishImage(std::move(values), InlineCount(), sampleCount, image);
        return true;
    }

    const int sampleIndex = std::clamp(requestedIndex, 0, sampleCount - 1);
    const int inlCount = InlineCount();
    const int xlCount = XlineCount();
    const std::size_t totalPixels = static_cast<std::size_t>(inlCount * xlCount);

    EnsureTimeSliceGrid();
    const auto& gridTraceIndices = timeGridCache_->traceIndices;

    const unsigned long long trace0 = file.Get()->metadata.trace0;
    const int formatCode = FormatCode();
    const int formatSizeBytes = index_ ? index_->formatSizeBytes : 0;
    const std::size_t traceSize = static_cast<std::size_t>(240 + sampleCount * formatSizeBytes);

    bool fastDone = false;
    QFile qfile(QString::fromStdString(sgyio::ToUtf8Path(index_->path)));
    if(formatSizeBytes > 0 && trace0 >= 3600 && traceSize > 240 && qfile.open(QIODevice::ReadOnly)) {
        const qint64 fileSize = qfile.size();
        const uchar* mapped = qfile.map(0, fileSize);
        if(mapped) {
            if(progress && !progress(0, static_cast<int>(totalPixels))) {
                qfile.unmap(const_cast<uchar*>(mapped));
                qfile.close();
                errorMessage = "Slice extraction cancelled by caller.";
                return false;
            }

            values.assign(totalPixels, std::numeric_limits<float>::quiet_NaN());

            const unsigned int nThreads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
            const std::size_t chunkSize = (totalPixels + nThreads - 1) / nThreads;
            std::vector<std::thread> workers;
            workers.reserve(nThreads);

            for(unsigned int th = 0; th < nThreads; ++th) {
                const std::size_t startIdx = th * chunkSize;
                const std::size_t endIdx = std::min(totalPixels, startIdx + chunkSize);
                if(startIdx >= endIdx) break;

                workers.emplace_back([&, startIdx, endIdx]() {
                    for(std::size_t idx = startIdx; idx < endIdx; ++idx) {
                        const int traceIdx = gridTraceIndices[idx];
                        if(traceIdx < 0) continue;

                        const std::size_t offset = static_cast<std::size_t>(trace0) +
                                                  static_cast<std::size_t>(traceIdx) * traceSize +
                                                  240 +
                                                  static_cast<std::size_t>(sampleIndex) * static_cast<std::size_t>(formatSizeBytes);
                        if(offset + static_cast<std::size_t>(formatSizeBytes) <= static_cast<std::size_t>(fileSize)) {
                            if(formatSizeBytes == 4 && (formatCode == SEGY_IEEE_FLOAT_4_BYTE || formatCode == SEGY_IBM_FLOAT_4_BYTE)) {
                                float val = 0.0f;
                                std::memcpy(&val, mapped + offset, 4);
                                segy_to_native(formatCode, 1, &val);
                                values[idx] = val;
                            } else if(formatSizeBytes == 2) {
                                std::int16_t raw = 0;
                                std::memcpy(&raw, mapped + offset, 2);
                                segy_to_native(formatCode, 1, &raw);
                                values[idx] = static_cast<float>(raw);
                            } else if(formatSizeBytes == 1) {
                                signed char raw = static_cast<signed char>(mapped[offset]);
                                values[idx] = static_cast<float>(raw);
                            } else if(formatSizeBytes == 4 && formatCode == SEGY_SIGNED_INTEGER_4_BYTE) {
                                std::int32_t raw = 0;
                                std::memcpy(&raw, mapped + offset, 4);
                                segy_to_native(formatCode, 1, &raw);
                                values[idx] = static_cast<float>(raw);
                            }
                        }
                    }
                });
            }
            for(auto& w : workers) {
                w.join();
            }

            qfile.unmap(const_cast<uchar*>(mapped));
            fastDone = true;

            if(progress && !progress(static_cast<int>(totalPixels), static_cast<int>(totalPixels))) {
                qfile.close();
                errorMessage = "Slice extraction cancelled by caller.";
                return false;
            }
        }
        qfile.close();
    }

    if(!fastDone) {
        values.assign(totalPixels, std::numeric_limits<float>::quiet_NaN());
        for(int i = 0; i < inlCount; ++i) {
            if(progress && (i % 16 == 0) && !progress(i, inlCount)) {
                errorMessage = "Slice extraction cancelled by caller.";
                return false;
            }
            for(int x = 0; x < xlCount; ++x) {
                const int traceIndex = FindTraceIndex(
                    InlineValues()[static_cast<std::size_t>(i)],
                    XlineValues()[static_cast<std::size_t>(x)]);
                if(traceIndex < 0) {
                    continue;
                }
                const int row = inlCount - 1 - i;
                if(!ReadTraceAsFloat(file.Get(), traceIndex, traceSamples, errorMessage)) {
                    continue;
                }
                values[static_cast<std::size_t>(row * xlCount + x)] = traceSamples[static_cast<std::size_t>(sampleIndex)];
            }
        }
    }
    FinishImage(std::move(values), xlCount, inlCount, image);
    return true;
}

bool SgyVolume::ExtractPreviewSlice(
    SgySliceType type,
    int requestedIndex,
    int maxColumns,
    SgySliceImage& image,
    std::string& errorMessage,
    int& columnsRead,
    int& totalColumns,
    bool compact,
    const std::function<bool(int, int)>& progress) const {
    columnsRead = 0;
    totalColumns = 0;
    image = {};
    if(!IsLoaded()) {
        errorMessage = "SGY volume index is not complete.";
        return false;
    }
    if(type == SgySliceType::Time) {
        errorMessage = "Time slices are not generated implicitly; request them explicitly.";
        return false;
    }
    {
        int exact = 0;
        const bool present = (type == SgySliceType::Inline)
            ? ExactInlineValue(requestedIndex, exact)
            : ExactXlineValue(requestedIndex, exact);
        if(!present) {
            errorMessage = (type == SgySliceType::Inline ? "inline " : "xline ") +
                           std::to_string(requestedIndex) +
                           " is not present in this file (no nearest-trace substitution)";
            return false;
        }
    }
    if(maxColumns < 1) {
        maxColumns = 1;
    }

    const int sampleCount = SampleCount();
    sgyio::Handle file(sgyio::OpenReadOnly(index_->path));
    if(!file) {
        errorMessage = "segy_open failed while extracting preview.";
        return false;
    }
    if(!Check(segy_collect_metadata(file.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }

    const std::vector<int>& lineValues =
        (type == SgySliceType::Inline) ? XlineValues() : InlineValues();
    totalColumns = static_cast<int>(lineValues.size());
    if(totalColumns <= 0) {
        errorMessage = "SGY volume has no lines to preview.";
        return false;
    }

    std::vector<int> selected;
    if(totalColumns <= maxColumns) {
        selected.reserve(static_cast<std::size_t>(totalColumns));
        for(int column = 0; column < totalColumns; ++column) {
            selected.push_back(column);
        }
    } else {
        selected.reserve(static_cast<std::size_t>(maxColumns));
        for(int c = 0; c < maxColumns; ++c) {
            const int column = static_cast<int>(
                (static_cast<long long>(c) * (totalColumns - 1)) / std::max(1, maxColumns - 1));
            if(selected.empty() || selected.back() != column) {
                selected.push_back(column);
            }
        }
    }

    // Snap to a line that really exists: regular surveys often use steps > 1,
    // so the numeric midpoint of the range may not be a valid line number.
    const int lineNo = (type == SgySliceType::Inline)
        ? FindNearestInlineValue(static_cast<float>(requestedIndex))
        : FindNearestXlineValue(static_cast<float>(requestedIndex));

    const int outputWidth = compact ? static_cast<int>(selected.size()) : totalColumns;
    std::vector<float> values(
        static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(sampleCount),
        std::numeric_limits<float>::quiet_NaN());
    std::vector<unsigned char> sampledColumns(static_cast<std::size_t>(totalColumns), 0);
    std::vector<int> loadedColumns;
    loadedColumns.reserve(selected.size());
    std::vector<float> traceSamples;
    int selectedIndex = 0;
    for(int column : selected) {
        if(progress && !progress(selectedIndex, static_cast<int>(selected.size()))) {
            errorMessage = "preview cancelled";
            return false;
        }
        const int outputColumn = compact ? selectedIndex : column;
        ++selectedIndex;
        sampledColumns[static_cast<std::size_t>(column)] = 1;
        const int traceIndex = (type == SgySliceType::Inline)
            ? FindTraceIndex(lineNo, lineValues[static_cast<std::size_t>(column)])
            : FindTraceIndex(lineValues[static_cast<std::size_t>(column)], lineNo);
        if(traceIndex < 0) {
            continue;
        }
        // Rule-based indexes abort here when the real header contradicts the
        // sampled layout; the caller then relies on the full index instead.
        if(!ReadTraceAsFloat(file.Get(), traceIndex, traceSamples, errorMessage)) {
            return false;
        }
        for(int s = 0; s < sampleCount; ++s) {
            const int row = sampleCount - 1 - s;
            values[static_cast<std::size_t>(row * outputWidth + outputColumn)] =
                traceSamples[static_cast<std::size_t>(s)];
        }
        loadedColumns.push_back(column);
        ++columnsRead;
    }

    if(columnsRead <= 0) {
        errorMessage = "No preview traces could be read.";
        return false;
    }

    // The preview texture still spans the complete survey plane. Leaving every
    // unsampled column as NaN made 90%+ of a large-volume first view the grey
    // missing-data colour. Expand only for display by repeating the nearest
    // real sampled trace. Exact sampled columns are untouched, and a selected
    // column whose trace is genuinely missing stays NaN.
    if(!compact && columnsRead < totalColumns) {
        std::vector<int> nearestLoaded(static_cast<std::size_t>(totalColumns), loadedColumns.front());
        std::size_t nearestIndex = 0;
        for(int column = 0; column < totalColumns; ++column) {
            while(nearestIndex + 1 < loadedColumns.size() &&
                  std::abs(loadedColumns[nearestIndex + 1] - column) <
                      std::abs(loadedColumns[nearestIndex] - column)) {
                ++nearestIndex;
            }
            nearestLoaded[static_cast<std::size_t>(column)] = loadedColumns[nearestIndex];
        }
        for(int row = 0; row < sampleCount; ++row) {
            float* rowValues = values.data() + static_cast<std::size_t>(row) * totalColumns;
            for(int column = 0; column < totalColumns; ++column) {
                if(sampledColumns[static_cast<std::size_t>(column)] != 0) {
                    continue;
                }
                rowValues[column] = rowValues[nearestLoaded[static_cast<std::size_t>(column)]];
            }
        }
    }

    if(progress && !progress(selectedIndex, static_cast<int>(selected.size()))) {
        errorMessage = "preview cancelled";
        return false;
    }
    FinishImage(std::move(values), outputWidth, sampleCount, image);
    return true;
}

bool SgyVolume::ExtractTimeSlicePreview(
    int sampleIndex,
    int maxInlineRows,
    int maxXlineColumns,
    SgySliceImage& image,
    std::string& errorMessage,
    const std::function<bool(int processed, int total)>& progress,
    SgyTimePreviewCache* cache) const {
    image = {};
    if(!IsLoaded()) {
        errorMessage = "SGY volume index is not complete.";
        return false;
    }
    if(sampleIndex < 0 || sampleIndex >= SampleCount()) {
        errorMessage = "SGY sample index is outside the volume.";
        return false;
    }
    if(InlineCount() <= 0 || XlineCount() <= 0) {
        errorMessage = "SGY volume has no inline/xline grid to preview.";
        return false;
    }

    const int outputHeight = std::min(InlineCount(), std::max(1, maxInlineRows));
    const int outputWidth = std::min(XlineCount(), std::max(1, maxXlineColumns));
    std::unique_lock<std::mutex> cacheLock;
    const std::size_t pixels = static_cast<std::size_t>(outputWidth) * outputHeight;
    if(cache) {
        cacheLock = std::unique_lock<std::mutex>(cache->mutex);
        const std::size_t capacity = (64u * 1024u * 1024u) / sizeof(float);
        if(pixels > capacity) {
            errorMessage = "Time preview exceeds 64 MiB cache budget.";
            return false;
        }
        const int span = static_cast<int>(std::min<std::size_t>(256, capacity / pixels));
        const int begin = sampleIndex / span * span;
        const int count = std::min(span, SampleCount() - begin);
        if(cache->index != index_ || cache->width != outputWidth || cache->height != outputHeight ||
           cache->begin != begin || cache->count != count) {
            cache->index = index_;
            cache->width = outputWidth; cache->height = outputHeight;
            cache->begin = begin; cache->count = count;
            cache->values.assign(pixels * count, std::numeric_limits<float>::quiet_NaN());
            cache->filled.assign(pixels, 0);
        }
        if(std::all_of(cache->filled.begin(), cache->filled.end(), [](unsigned char v){ return v != 0; })) {
            if(progress && !progress(1, 1)) { errorMessage = "Time-slice preview cancelled by caller."; return false; }
            std::vector<float> values(pixels);
            for(std::size_t p = 0; p < pixels; ++p)
                values[p] = cache->values[p * count + sampleIndex - begin];
            FinishImage(std::move(values), outputWidth, outputHeight, image);
            return true;
        }
    }
    std::vector<int> inlineIndices;
    std::vector<int> xlineIndices;
    inlineIndices.reserve(static_cast<std::size_t>(outputHeight));
    xlineIndices.reserve(static_cast<std::size_t>(outputWidth));
    for(int row = 0; row < outputHeight; ++row) {
        inlineIndices.push_back(static_cast<int>(
            (static_cast<long long>(row) * (InlineCount() - 1)) /
            std::max(1, outputHeight - 1)));
    }
    for(int column = 0; column < outputWidth; ++column) {
        xlineIndices.push_back(static_cast<int>(
            (static_cast<long long>(column) * (XlineCount() - 1)) /
            std::max(1, outputWidth - 1)));
    }

    struct PreviewRead {
        int traceIndex = -1;
        std::size_t outputIndex = 0;
    };
    std::vector<PreviewRead> reads;
    reads.reserve(static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight));
    for(int row = 0; row < outputHeight; ++row) {
        const int inlineIndex = inlineIndices[static_cast<std::size_t>(row)];
        const int inlineNo = InlineValues()[static_cast<std::size_t>(inlineIndex)];
        const int outputRow = outputHeight - 1 - row;
        for(int column = 0; column < outputWidth; ++column) {
            const int xlineIndex = xlineIndices[static_cast<std::size_t>(column)];
            const int xlineNo = XlineValues()[static_cast<std::size_t>(xlineIndex)];
            const int traceIndex = FindTraceIndex(inlineNo, xlineNo);
            if(traceIndex >= 0) {
                reads.push_back({
                    traceIndex,
                    static_cast<std::size_t>(outputRow) * static_cast<std::size_t>(outputWidth) +
                        static_cast<std::size_t>(column)});
            }
        }
    }
    // Physical trace order is the cheapest access order for both NVMe and HDD,
    // independent of whether inline or crossline is the file's fast axis.
    std::sort(reads.begin(), reads.end(), [](const PreviewRead& a, const PreviewRead& b) {
        return a.traceIndex < b.traceIndex;
    });

    sgyio::Handle file(sgyio::OpenReadOnly(index_->path));
    if(!file) {
        errorMessage = "segy_open failed while extracting time-slice preview.";
        return false;
    }
    if(!Check(segy_collect_metadata(file.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }

    std::vector<float> values(
        static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight),
        std::numeric_limits<float>::quiet_NaN());
    const int total = static_cast<int>(reads.size());
    int completed = 0;
    std::vector<float> raw(cache ? cache->count : 0);
    std::vector<std::int16_t> raw16(cache ? cache->count : 0);
    for(const PreviewRead& read : reads) {
        if(progress && (completed % 64 == 0) && !progress(completed, std::max(1, total))) {
            errorMessage = "Time-slice preview cancelled by caller.";
            return false;
        }
        float value = std::numeric_limits<float>::quiet_NaN();
        if(cache) {
            if(!cache->filled[read.outputIndex]) {
                if(!ValidateRuleTrace(file.Get(), read.traceIndex, errorMessage)) return false;
                if(index_->formatSizeBytes == 4 &&
                   (FormatCode() == SEGY_IEEE_FLOAT_4_BYTE || FormatCode() == SEGY_IBM_FLOAT_4_BYTE)) {
                    if(!Check(segy_readsubtr(file.Get(), read.traceIndex, cache->begin,
                        cache->begin + cache->count, 1, raw.data(), nullptr), "segy_readsubtr", errorMessage) ||
                       !Check(segy_to_native(FormatCode(), cache->count, raw.data()), "segy_to_native", errorMessage)) return false;
                } else if(index_->formatSizeBytes == 2) {
                    if(!Check(segy_readsubtr(file.Get(), read.traceIndex, cache->begin,
                        cache->begin + cache->count, 1, raw16.data(), nullptr), "segy_readsubtr", errorMessage) ||
                       !Check(segy_to_native(FormatCode(), cache->count, raw16.data()), "segy_to_native", errorMessage)) return false;
                    std::copy(raw16.begin(), raw16.end(), raw.begin());
                } else { errorMessage = "Unsupported SGY sample format for time-slice preview."; return false; }
                std::copy(raw.begin(), raw.end(), cache->values.begin() + read.outputIndex * cache->count);
                cache->filled[read.outputIndex] = 1;
                ++cache->traceReads;
            }
            value = cache->values[read.outputIndex * cache->count + sampleIndex - cache->begin];
        } else if(!ReadSampleAsFloat(file.Get(), read.traceIndex, sampleIndex, value, errorMessage)) {
            return false;
        }
        values[read.outputIndex] = value;
        ++completed;
    }
    if(progress && !progress(completed, std::max(1, total))) {
        errorMessage = "Time-slice preview cancelled by caller.";
        return false;
    }
    if(completed <= 0) {
        errorMessage = "No traces could be read for the time-slice preview.";
        return false;
    }

    // Unavailable grid coordinates remain NaN, but are not pending reads.
    if(cache) std::fill(cache->filled.begin(), cache->filled.end(), 1);

    FinishImage(std::move(values), outputWidth, outputHeight, image);
    return true;
}

bool SgyVolume::ExtractLineSlice(
    int startInline,
    int startXline,
    int endInline,
    int endXline,
    SgySliceImage& image,
    int& sampleColumns,
    std::string& errorMessage) const {
    sampleColumns = 0;
    if(!IsLoaded()) {
        errorMessage = "SGY volume index is not complete.";
        return false;
    }

    const int sampleCount = SampleCount();
    startInline = std::clamp(startInline, InlineMin(), InlineMax());
    endInline = std::clamp(endInline, InlineMin(), InlineMax());
    startXline = std::clamp(startXline, XlineMin(), XlineMax());
    endXline = std::clamp(endXline, XlineMin(), XlineMax());

    const int inlineSpan = std::abs(endInline - startInline);
    const int xlineSpan = std::abs(endXline - startXline);
    sampleColumns = std::clamp(std::max(inlineSpan, xlineSpan) + 1, 2, 1024);

    sgyio::Handle file(sgyio::OpenReadOnly(index_->path));
    if(!file) {
        errorMessage = "segy_open failed while extracting line section.";
        return false;
    }
    if(!Check(segy_collect_metadata(file.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }

    std::vector<float> values(
        static_cast<std::size_t>(sampleColumns * sampleCount),
        std::numeric_limits<float>::quiet_NaN());
    std::vector<float> traceSamples;
    std::unordered_map<int, std::vector<float>> traceCache;
    traceCache.reserve(static_cast<std::size_t>(sampleColumns));

    for(int col = 0; col < sampleColumns; ++col) {
        const float t = sampleColumns <= 1 ? 0.0f : static_cast<float>(col) / static_cast<float>(sampleColumns - 1);
        const float inlineValue = static_cast<float>(startInline) + (static_cast<float>(endInline - startInline) * t);
        const float xlineValue = static_cast<float>(startXline) + (static_cast<float>(endXline - startXline) * t);
        const int nearestInline = FindNearestInlineValue(inlineValue);
        const int nearestXline = FindNearestXlineValue(xlineValue);
        const int traceIndex = FindTraceIndex(nearestInline, nearestXline);
        if(traceIndex < 0) {
            continue;
        }

        auto cacheIt = traceCache.find(traceIndex);
        if(cacheIt == traceCache.end()) {
            if(!ReadTraceAsFloat(file.Get(), traceIndex, traceSamples, errorMessage)) {
                continue;
            }
            cacheIt = traceCache.emplace(traceIndex, traceSamples).first;
        }

        const std::vector<float>& samples = cacheIt->second;
        for(int s = 0; s < sampleCount; ++s) {
            const int row = sampleCount - 1 - s;
            values[static_cast<std::size_t>(row * sampleColumns + col)] = samples[static_cast<std::size_t>(s)];
        }
    }

    FinishImage(std::move(values), sampleColumns, sampleCount, image);
    return true;
}

bool SgyVolume::ExtractLineSlice(
    const std::vector<glm::ivec2>& pathPoints,
    SgySliceImage& image,
    int& sampleColumns,
    std::string& errorMessage,
    bool keepOutsideColumns,
    int maxColumns) const {
    sampleColumns = 0;
    if(!IsLoaded()) {
        errorMessage = "SGY volume index is not complete.";
        return false;
    }
    if(pathPoints.size() < 2) {
        errorMessage = "Line section needs at least two path points.";
        return false;
    }

    const int sampleCount = SampleCount();
    std::vector<float> segmentLengths;
    segmentLengths.reserve(pathPoints.size() - 1);
    float totalLength = 0.0f;
    for(size_t i = 1; i < pathPoints.size(); ++i) {
        const float di = static_cast<float>(pathPoints[i].x - pathPoints[i - 1].x);
        const float dx = static_cast<float>(pathPoints[i].y - pathPoints[i - 1].y);
        const float length = std::sqrt(di * di + dx * dx);
        if(length <= 1e-4f) {
            segmentLengths.push_back(0.0f);
            continue;
        }
        segmentLengths.push_back(length);
        totalLength += length;
    }
    if(totalLength <= 1e-4f) {
        errorMessage = "Line section path is too short.";
        return false;
    }
    sampleColumns = std::clamp(static_cast<int>(std::round(totalLength)) + 1, 2, std::clamp(maxColumns, 2, 8192));

    sgyio::Handle file(sgyio::OpenReadOnly(index_->path));
    if(!file) {
        errorMessage = "segy_open failed while extracting polyline section.";
        return false;
    }
    if(!Check(segy_collect_metadata(file.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }

    std::vector<float> values(
        static_cast<std::size_t>(sampleColumns * sampleCount),
        std::numeric_limits<float>::quiet_NaN());
    std::vector<float> traceSamples;
    std::unordered_map<int, std::vector<float>> traceCache;
    traceCache.reserve(static_cast<std::size_t>(sampleColumns));

    size_t segmentIndex = 0;
    float segmentStartDistance = 0.0f;
    for(int col = 0; col < sampleColumns; ++col) {
        const float targetDistance = sampleColumns <= 1
            ? 0.0f
            : (static_cast<float>(col) / static_cast<float>(sampleColumns - 1)) * totalLength;
        while(segmentIndex + 1 < segmentLengths.size() &&
            targetDistance > segmentStartDistance + segmentLengths[segmentIndex]) {
            segmentStartDistance += segmentLengths[segmentIndex];
            ++segmentIndex;
        }

        const float segmentLength = std::max(segmentLengths[segmentIndex], 1e-4f);
        const float t = std::clamp((targetDistance - segmentStartDistance) / segmentLength, 0.0f, 1.0f);
        const glm::ivec2& a = pathPoints[segmentIndex];
        const glm::ivec2& b = pathPoints[segmentIndex + 1];
        const float inlineValue = static_cast<float>(a.x) + static_cast<float>(b.x - a.x) * t;
        const float xlineValue = static_cast<float>(a.y) + static_cast<float>(b.y - a.y) * t;
        if(keepOutsideColumns &&
            (inlineValue < static_cast<float>(InlineMin()) ||
                inlineValue > static_cast<float>(InlineMax()) ||
                xlineValue < static_cast<float>(XlineMin()) ||
                xlineValue > static_cast<float>(XlineMax()))) {
            continue;
        }
        const int nearestInline = FindNearestInlineValue(inlineValue);
        const int nearestXline = FindNearestXlineValue(xlineValue);
        const int traceIndex = FindTraceIndex(nearestInline, nearestXline);
        if(traceIndex < 0) {
            continue;
        }

        auto cacheIt = traceCache.find(traceIndex);
        if(cacheIt == traceCache.end()) {
            if(!ReadTraceAsFloat(file.Get(), traceIndex, traceSamples, errorMessage)) {
                continue;
            }
            cacheIt = traceCache.emplace(traceIndex, traceSamples).first;
        }

        const std::vector<float>& samples = cacheIt->second;
        for(int s = 0; s < sampleCount; ++s) {
            const int row = sampleCount - 1 - s;
            values[static_cast<std::size_t>(row * sampleColumns + col)] = samples[static_cast<std::size_t>(s)];
        }
    }

    FinishImage(std::move(values), sampleColumns, sampleCount, image);
    return true;
}

} // namespace seismic
