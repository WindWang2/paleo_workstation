// 层：数据
#pragma once

#include <string>

#include "domain/seismic/sgyindex.h"

namespace seismic {

// Affine fit (inline, xline) -> (X, Y) derived from sampled trace coordinates:
//   x = a * inline + b * xline + c
//   y = d * inline + e * xline + f
struct SgyAffineFit {
    bool valid = false;
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 0.0;
    double e = 1.0;
    double f = 0.0;
    int sampleCount = 0;
    double rmsResidual = 0.0;
    double maxResidual = 0.0;
    std::string rejectionReason;
};

// Maps between survey geometry and trace coordinates using the fitted affine.
//
// Honest limits (documented in the UI as well):
//   * it only works when the file actually carries coordinates,
//   * it assumes a single projected CRS; well coordinates must be in the same
//     CRS - no reprojection is attempted,
//   * the fit is validated by its residuals; a large residual makes the mapper
//     invalid instead of pretending to be precise,
//   * a fit says nothing about traces that were never sampled.
class SgyCoordinateMapper {
public:
    // maxRmsResidual is in the coordinate unit (usually metres).
    static SgyCoordinateMapper Fit(const SgyIndex& index, double maxRmsResidual = 25.0);

    bool valid() const { return fit_.valid; }
    const SgyAffineFit& fit() const { return fit_; }

    bool MapInlineXline(double inlineNo, double xlineNo, double& x, double& y) const;
    bool MapXY(double x, double y, double& inlineNo, double& xlineNo) const;
    // True when the mapped position falls inside the indexed survey range.
    bool InCoverage(double x, double y, double marginLines = 0.0) const;

    std::string Describe() const;

private:
    SgyAffineFit fit_;
    double inlineMin_ = 0.0;
    double inlineMax_ = 0.0;
    double xlineMin_ = 0.0;
    double xlineMax_ = 0.0;
};

} // namespace seismic
