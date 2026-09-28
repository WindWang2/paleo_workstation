#include "Data/Sgy/SgyCoordinateMapper.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace seismic {
namespace {

// Solves a 3x3 linear system with Cramer's rule.
bool Solve3x3(const double m[3][3], const double rhs[3], double out[3]) {
    const double det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if(std::abs(det) < 1e-12) {
        return false;
    }
    for(int column = 0; column < 3; ++column) {
        double n[3][3];
        for(int r = 0; r < 3; ++r) {
            for(int c = 0; c < 3; ++c) {
                n[r][c] = (c == column) ? rhs[r] : m[r][c];
            }
        }
        const double num =
            n[0][0] * (n[1][1] * n[2][2] - n[1][2] * n[2][1]) -
            n[0][1] * (n[1][0] * n[2][2] - n[1][2] * n[2][0]) +
            n[0][2] * (n[1][0] * n[2][1] - n[1][1] * n[2][0]);
        out[column] = num / det;
    }
    return true;
}

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

    double normal[3][3] = {};
    double rhsX[3] = {};
    double rhsY[3] = {};
    for(const SgyCoordinateSample& sample : index.coordinateSamples) {
        const double basis[3] = { 1.0, static_cast<double>(sample.inlineNo), static_cast<double>(sample.xlineNo) };
        for(int r = 0; r < 3; ++r) {
            for(int c = 0; c < 3; ++c) {
                normal[r][c] += basis[r] * basis[c];
            }
            rhsX[r] += basis[r] * sample.x;
            rhsY[r] += basis[r] * sample.y;
        }
    }

    double solutionX[3] = {};
    double solutionY[3] = {};
    if(!Solve3x3(normal, rhsX, solutionX) || !Solve3x3(normal, rhsY, solutionY)) {
        fit.rejectionReason = "coordinate samples are degenerate (collinear survey?)";
        return mapper;
    }

    fit.c = solutionX[0];
    fit.a = solutionX[1];
    fit.b = solutionX[2];
    fit.f = solutionY[0];
    fit.d = solutionY[1];
    fit.e = solutionY[2];

    double sumSq = 0.0;
    fit.maxResidual = 0.0;
    for(const SgyCoordinateSample& sample : index.coordinateSamples) {
        const double predictedX = fit.a * sample.inlineNo + fit.b * sample.xlineNo + fit.c;
        const double predictedY = fit.d * sample.inlineNo + fit.e * sample.xlineNo + fit.f;
        const double dx = predictedX - sample.x;
        const double dy = predictedY - sample.y;
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
