#pragma once

#include <filesystem>
#include <string>

namespace seismic {

// Candidate regular survey layout derived from a small number of real trace
// headers. This is SAMPLED verification only: it never claims to have verified
// every trace. Every trace located through the rule must be re-validated
// against its real header before its samples are trusted (see SgyVolume).
struct SgyRuleLayout {
    bool valid = false;

    int firstInline = 0;
    int lastInline = 0;
    int inlineStep = 0; // signed: direction of the inline numbering
    int firstXline = 0;
    int lastXline = 0;
    int xlineStep = 0;

    int inlineCount = 0;
    int xlineCount = 0;
    bool inlineMajor = true; // true: xline runs fastest inside one inline
    int traceCount = 0;

    int probeCount = 0;
    int matchedProbes = 0;
    std::string rejectionReason;

    // Formula-only trace lookup. Callers must validate the returned trace
    // header before using its samples.
    int TraceIndexFor(int inlineNo, int xlineNo) const;
    bool ExpectedInlineXline(int traceIndex, int& inlineNo, int& xlineNo) const;

    std::string Describe() const;
};

// Reads a bounded number of trace headers (head, middle, tail and deterministic
// probes) and derives/validates the layout. Cost is a few dozen header reads.
bool ProbeSgyRuleLayout(
    const std::filesystem::path& path,
    int traceCount,
    SgyRuleLayout& outLayout,
    std::string& errorMessage,
    int probeBudget = 24);

} // namespace seismic
