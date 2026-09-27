// 层：数据
#include "domain/seismic/sgyindex.h"

#include <algorithm>
#include <cmath>

namespace seismic {

std::uint64_t SgyIndex::MakeKey(int inlineNo, int xlineNo) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(inlineNo)) << 32) |
           static_cast<std::uint32_t>(xlineNo);
}

int SgyIndex::FindTraceIndex(int inlineNo, int xlineNo) const {
    if(ruleBased) {
        return rule.TraceIndexFor(inlineNo, xlineNo);
    }
    const auto it = traceByInlineXline.find(MakeKey(inlineNo, xlineNo));
    return it == traceByInlineXline.end() ? -1 : it->second;
}

int SgyIndex::FindNearestInlineValue(float inlineNo) const {
    if(inlineValues.empty()) {
        return 0;
    }
    const auto it = std::lower_bound(
        inlineValues.begin(), inlineValues.end(), static_cast<int>(std::round(inlineNo)));
    if(it == inlineValues.begin()) {
        return *it;
    }
    if(it == inlineValues.end()) {
        return inlineValues.back();
    }
    const int upper = *it;
    const int lower = *(it - 1);
    return std::abs(inlineNo - static_cast<float>(lower)) <= std::abs(static_cast<float>(upper) - inlineNo)
        ? lower
        : upper;
}

int SgyIndex::FindNearestXlineValue(float xlineNo) const {
    if(xlineValues.empty()) {
        return 0;
    }
    const auto it = std::lower_bound(
        xlineValues.begin(), xlineValues.end(), static_cast<int>(std::round(xlineNo)));
    if(it == xlineValues.begin()) {
        return *it;
    }
    if(it == xlineValues.end()) {
        return xlineValues.back();
    }
    const int upper = *it;
    const int lower = *(it - 1);
    return std::abs(xlineNo - static_cast<float>(lower)) <= std::abs(static_cast<float>(upper) - xlineNo)
        ? lower
        : upper;
}

std::size_t SgyIndex::MemoryBytes() const {
    std::size_t bytes = sizeof(SgyIndex);
    bytes += traces.capacity() * sizeof(SgyTraceRef);
    bytes += traceByInlineXline.size() * (sizeof(std::uint64_t) + sizeof(int) + 2 * sizeof(void*));
    bytes += (inlineValues.capacity() + xlineValues.capacity()) * sizeof(int);
    if(ruleBased) {
        bytes += static_cast<std::size_t>(rule.inlineCount + rule.xlineCount) * sizeof(int);
    }
    return bytes;
}

AxisDescriptor SgyIndex::InlineAxis() const {
    if(ruleBased && rule.valid && rule.inlineCount > 0 && rule.inlineStep != 0) {
        return AxisDescriptor::Uniform(rule.firstInline, rule.inlineStep, rule.inlineCount);
    }
    if(!inlineValues.empty()) {
        return AxisDescriptor::FromValues(inlineValues);
    }
    if(inlineMax >= inlineMin) {
        return AxisDescriptor::Uniform(inlineMin, 1, inlineMax - inlineMin + 1);
    }
    return AxisDescriptor();
}

AxisDescriptor SgyIndex::XlineAxis() const {
    if(ruleBased && rule.valid && rule.xlineCount > 0 && rule.xlineStep != 0) {
        return AxisDescriptor::Uniform(rule.firstXline, rule.xlineStep, rule.xlineCount);
    }
    if(!xlineValues.empty()) {
        return AxisDescriptor::FromValues(xlineValues);
    }
    if(xlineMax >= xlineMin) {
        return AxisDescriptor::Uniform(xlineMin, 1, xlineMax - xlineMin + 1);
    }
    return AxisDescriptor();
}

} // namespace seismic
