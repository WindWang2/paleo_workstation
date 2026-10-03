#include "Data/Sgy/SgyCoordinateMapper.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace seismic {
namespace {

// Paleo patch P12 (#135): the upstream fit built raw (uncentred) 3x3 normal
// equations and solved them with Cramer's rule. With inline/xline ~1e3 and
// projected coordinates ~1e5..1e6 the determinant is O(1e2) while the
// cofactor products reach O(1e20): catastrophic cancellation. A perfectly
// affine 2x4 survey fitted with rms=2 m (no FMA) or rms=6404 m (FMA
// contraction), and small surveys were rejected as "not a regular grid".
// The fit now centres every variable on its mean first; the slopes come from
// a 2x2 system in deviations (entries O(n * spread^2)), the intercept is
// recovered from the means. Exact for an exactly affine survey up to
// round-off, and independent of the absolute magnitude of the coordinates.
struct CentredSums {
    double sii = 0.0; // sum (di * di)
    double sij = 0.0; // sum (di * dj)
    double sjj = 0.0; // sum (dj * dj)
    double siv = 0.0; // sum (di * dv)
    double sjv = 0.0; // sum (dj * dv)
};

} // namespace

SgyCoordinateMapper SgyCoordinateMapper::Fit(const SgyIndex& index, double maxRmsResidual) {
    SgyCoordinateMapper mapper;
    mapper.inlineMin_ = static_cast<double>(index.inlineMin);
    mapper.inlineMax_ = static_cast<double>(index.inlineMax);
    mapper.xlineMin_ = static_cast<double>(index.xlineMin);
    mapper.xlineMax_ = static_cast<double>(index.xlineMax);

    SgyAffineFit& fit = mapper.fit_;
    fit.sampleCount = static_cast<int>(index.coordinateSamples.size());
    if(!index.coordinateFieldsPresent || fit.sampleCount < 6) {
        fit.rejectionReason = "SEG-Y trace coordinates are missing or too few (need >= 6 samples)";
        return mapper;
    }

    const double n = static_cast<double>(fit.sampleCount);
    double meanIl = 0.0;
    double meanXl = 0.0;
    double meanX = 0.0;
    double meanY = 0.0;
    for(const SgyCoordinateSample& sample : index.coordinateSamples) {
        meanIl += static_cast<double>(sample.inlineNo);
        meanXl += static_cast<double>(sample.xlineNo);
        meanX += sample.x;
        meanY += sample.y;
    }
    meanIl /= n;
    meanXl /= n;
    meanX /= n;
    meanY /= n;

    CentredSums sx;
    CentredSums sy;
    for(const SgyCoordinateSample& sample : index.coordinateSamples) {
        const double di = static_cast<double>(sample.inlineNo) - meanIl;
        const double dj = static_cast<double>(sample.xlineNo) - meanXl;
        const double dx = sample.x - meanX;
        const double dy = sample.y - meanY;
        sx.sii += di * di;
        sx.sij += di * dj;
        sx.sjj += dj * dj;
        sx.siv += di * dx;
        sx.sjv += dj * dx;
        sy.siv += di * dy;
        sy.sjv += dj * dy;
    }
    sy.sii = sx.sii;
    sy.sij = sx.sij;
    sy.sjj = sx.sjj;

    // Degeneracy is judged relative to the spread of the sample grid: the
    // determinant of the centred 2x2 Gram matrix is >= 0 and equals
    // sii*sjj*(1 - corr^2). Collinear samples (single line, or inline and
    // xline moving together) drive corr^2 -> 1.
    const double det = sx.sii * sx.sjj - sx.sij * sx.sij;
    if(!(sx.sii > 0.0) || !(sx.sjj > 0.0) || !(det > 1e-9 * sx.sii * sx.sjj)) {
        fit.rejectionReason = "coordinate samples are degenerate (collinear survey?)";
        return mapper;
    }

    fit.a = (sx.siv * sx.sjj - sx.sjv * sx.sij) / det;
    fit.b = (sx.sjv * sx.sii - sx.siv * sx.sij) / det;
    fit.c = meanX - fit.a * meanIl - fit.b * meanXl;
    fit.d = (sy.siv * sy.sjj - sy.sjv * sy.sij) / det;
    fit.e = (sy.sjv * sy.sii - sy.siv * sy.sij) / det;
    fit.f = meanY - fit.d * meanIl - fit.e * meanXl;

    double sumSq = 0.0;
    fit.maxResidual = 0.0;
    for(const SgyCoordinateSample& sample : index.coordinateSamples) {
        // Residuals evaluated in the centred frame: avoids subtracting two
        // ~1e6 numbers to get a sub-metre residual.
        const double di = static_cast<double>(sample.inlineNo) - meanIl;
        const double dj = static_cast<double>(sample.xlineNo) - meanXl;
        const double dx = (fit.a * di + fit.b * dj) - (sample.x - meanX);
        const double dy = (fit.d * di + fit.e * dj) - (sample.y - meanY);
        const double residual = std::sqrt(dx * dx + dy * dy);
        sumSq += residual * residual;
        fit.maxResidual = std::max(fit.maxResidual, residual);
    }
    fit.rmsResidual = std::sqrt(sumSq / static_cast<double>(fit.sampleCount));

    if(fit.rmsResidual > maxRmsResidual) {
        std::ostringstream oss;
        oss << "affine fit residual too large: rms=" << fit.rmsResidual
            << ", max=" << fit.maxResidual << " (limit " << maxRmsResidual
            << "); the survey is not a regular grid in this CRS";
        fit.rejectionReason = oss.str();
        return mapper;
    }

    fit.valid = true;
    return mapper;
}

bool SgyCoordinateMapper::MapInlineXline(double inlineNo, double xlineNo, double& x, double& y) const {
    if(!fit_.valid) {
        return false;
    }
    x = fit_.a * inlineNo + fit_.b * xlineNo + fit_.c;
    y = fit_.d * inlineNo + fit_.e * xlineNo + fit_.f;
    return true;
}

bool SgyCoordinateMapper::MapXY(double x, double y, double& inlineNo, double& xlineNo) const {
    if(!fit_.valid) {
        return false;
    }
    const double det = fit_.a * fit_.e - fit_.b * fit_.d;
    if(std::abs(det) < 1e-12) {
        return false;
    }
    const double px = x - fit_.c;
    const double py = y - fit_.f;
    inlineNo = (px * fit_.e - fit_.b * py) / det;
    xlineNo = (fit_.a * py - px * fit_.d) / det;
    return true;
}

bool SgyCoordinateMapper::InCoverage(double x, double y, double marginLines) const {
    double inlineNo = 0.0;
    double xlineNo = 0.0;
    if(!MapXY(x, y, inlineNo, xlineNo)) {
        return false;
    }
    return inlineNo >= inlineMin_ - marginLines && inlineNo <= inlineMax_ + marginLines &&
           xlineNo >= xlineMin_ - marginLines && xlineNo <= xlineMax_ + marginLines;
}

std::string SgyCoordinateMapper::Describe() const {
    std::ostringstream oss;
    if(!fit_.valid) {
        oss << "coordinate mapping unavailable";
        if(!fit_.rejectionReason.empty()) {
            oss << " (" << fit_.rejectionReason << ")";
        }
        return oss.str();
    }
    oss << "affine coordinate mapping: samples=" << fit_.sampleCount
        << ", rmsResidual=" << fit_.rmsResidual << ", maxResidual=" << fit_.maxResidual;
    return oss.str();
}

} // namespace seismic
