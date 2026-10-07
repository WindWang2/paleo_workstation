// 层：视图
// 方向 65：画布显示参数与叠加数据入口 TU——D2.3 阈值/极性、D2.4 AGC/增益曲线、
// D2.5 双刻度、D2.7 纵向拉伸、D2.8 色标反转，以及 D4 解释与 D5.3/D5.4/D5.7 的
// 叠加 setter（画布只画不存，模型归 dock）。
#include "ui/seismicsection/seismicsectioncanvas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

// ---- D2.8 反转 ----
void SeismicSectionCanvas::setColorMapInverted(bool inverted) {
    if (m_cmapInverted != inverted) {
        m_cmapInverted = inverted;
        rebuildColorLut();
        rebuildImage();
        rebuildAttrImage();
        if (!m_zoneOverlay.values.empty())
            rebuildZoneImage();
        update();
    }
}

// ---- D2.2 显示三模 ----
void SeismicSectionCanvas::setDisplayMode(SectionDisplayMode mode) {
    if (m_displayMode != mode) {
        m_displayMode = mode;
        update();
    }
}

// ---- D2.3 密度阈值（低于阈值*absMax 的振幅按 0 显示）----
void SeismicSectionCanvas::setAmplitudeThreshold(float threshold) {
    const float clamped = std::clamp(threshold, 0.0f, 0.95f);
    if (std::abs(m_threshold - clamped) > 1e-3f) {
        m_threshold = clamped;
        rebuildImage();
        update();
    }
}

// ---- D2.3 极性反转 ----
void SeismicSectionCanvas::setPolarityInverted(bool inverted) {
    if (m_polarityInverted != inverted) {
        m_polarityInverted = inverted;
        rebuildDisplayValues();
        rebuildImage();
        update();
    }
}

// ---- D2.4 AGC：滑动窗 RMS 归一 ----
void SeismicSectionCanvas::setAgcEnabled(bool enabled, int windowMs) {
    const int clampedWin = std::clamp(windowMs, 20, 5000);
    if (m_agcEnabled != enabled || m_agcWindowMs != clampedWin) {
        m_agcEnabled = enabled;
        m_agcWindowMs = clampedWin;
        rebuildDisplayValues();
        rebuildImage();
        update();
    }
}

// ---- D2.4 手动增益曲线 ----
void SeismicSectionCanvas::setGainCurve(const std::vector<SectionGainNode> &nodes) {
    m_gainCurve = nodes;
    std::sort(m_gainCurve.begin(), m_gainCurve.end(),
              [](const SectionGainNode &a, const SectionGainNode &b) { return a.twtMs < b.twtMs; });
    if (!m_gainCurve.empty()) {
        rebuildDisplayValues();
        rebuildImage();
        update();
    }
}

float SeismicSectionCanvas::gainAtTwt(double twtMs) const {
    if (m_gainCurve.empty())
        return 1.0f;
    if (twtMs <= m_gainCurve.front().twtMs)
        return static_cast<float>(m_gainCurve.front().gain);
    if (twtMs >= m_gainCurve.back().twtMs)
        return static_cast<float>(m_gainCurve.back().gain);
    for (std::size_t i = 1; i < m_gainCurve.size(); ++i) {
        if (twtMs <= m_gainCurve[i].twtMs) {
            const double t = (twtMs - m_gainCurve[i - 1].twtMs) /
                             std::max(1e-9, m_gainCurve[i].twtMs - m_gainCurve[i - 1].twtMs);
            return static_cast<float>(m_gainCurve[i - 1].gain +
                                      t * (m_gainCurve[i].gain - m_gainCurve[i - 1].gain));
        }
    }
    return 1.0f;
}

// ---- D2.5 双刻度 ----
void SeismicSectionCanvas::setDualScaleEnabled(bool enabled) {
    if (m_dualScale != enabled) {
        m_dualScale = enabled;
        update();
    }
}

// ---- D2.12 书签视口状态 ----
SectionViewState SeismicSectionCanvas::viewState() const {
    return SectionViewState{m_zoomX, m_zoomY, m_panX, m_panY};
}

void SeismicSectionCanvas::setViewState(const SectionViewState &state) {
    m_zoomX = std::clamp(state.zoomX, 0.12, 64.0);
    m_zoomY = std::clamp(state.zoomY, 0.12, 64.0);
    m_panX = state.panX;
    m_panY = state.panY;
    emit zoomChanged(m_zoomX);
    update();
}

// ---- D2.7 纵向拉伸系数 ----
void SeismicSectionCanvas::setVerticalExaggeration(double factor) {
    const double clamped = std::clamp(factor, 0.1, 20.0);
    if (std::abs(m_vExag - clamped) > 1e-4) {
        const double ratio = clamped / std::max(1e-6, m_vExag);
        m_vExag = clamped;
        // 以视口中心为锚缩放纵向
        const QRect vp = viewportRect();
        const double centerY = vp.center().y();
        const double timeCenter = pixelToTime(centerY);
        m_zoomY = std::clamp(m_zoomY * ratio, 0.12, 64.0);
        m_panY = centerY - m_topMargin - ((timeCenter - m_t0Ms) / m_dtMs) * m_zoomY;
        update();
    }
}


void SeismicSectionCanvas::setGain(float gain) {
    const float clamped = std::clamp(gain, 0.05f, 20.0f);
    if (std::abs(m_gain - clamped) > 1e-4f) {
        m_gain = clamped;
        rebuildImage();
        update();
    }
}

void SeismicSectionCanvas::setContrast(float contrast) {
    const float clamped = std::clamp(contrast, 0.2f, 5.0f);
    if (std::abs(m_contrast - clamped) > 1e-4f) {
        m_contrast = clamped;
        rebuildImage();
        update();
    }
}

void SeismicSectionCanvas::setVerticalUnit(SectionVerticalUnit unit) {
    if (m_vertUnit != unit) {
        m_vertUnit = unit;
        update();
    }
}

void SeismicSectionCanvas::setShowWells(bool show) {
    if (m_showWells != show) {
        m_showWells = show;
        update();
    }
}

void SeismicSectionCanvas::setShowFormationTops(bool show) {
    if (m_showTops != show) {
        m_showTops = show;
        update();
    }
}

void SeismicSectionCanvas::setShowWellCurves(bool show) {
    if (m_showCurves != show) {
        m_showCurves = show;
        update();
    }
}

void SeismicSectionCanvas::setBufferDistanceM(double bufferM) {
    if (std::abs(m_bufferDistanceM - bufferM) > 0.1) {
        m_bufferDistanceM = bufferM;
        for (auto &w : m_wells) {
            w.isWithinBuffer = (std::abs(w.offsetDistanceM) <= m_bufferDistanceM);
        }
        update();
    }
}


// ---- D5.3/D5.4/D5.7 井轨迹 / 合成记录 / 多井开关 ----
void SeismicSectionCanvas::setWellTrajectories(const std::vector<WellTrajectory> &traj) {
    m_wellTrajectories = traj;
    update();
}

void SeismicSectionCanvas::setSyntheticOverlays(const std::vector<SyntheticOverlay> &overlays) {
    m_syntheticOverlays = overlays;
    update();
}

void SeismicSectionCanvas::setMaxVisibleWells(int n) {
    m_maxVisibleWells = std::max(0, n);
    update();
}

// ---- D4 解释：拾取/断层模式 ----
void SeismicSectionCanvas::setPickMode(SectionPickMode mode) {
    if (m_pickMode != mode) {
        m_pickMode = mode;
        m_faultDraft.clear();
        setCursor(mode == SectionPickMode::None ? Qt::CrossCursor : Qt::PointingHandCursor);
        update();
    }
}

void SeismicSectionCanvas::setPickOverlays(const QList<SeismicPick> &picks,
                                           const QList<SeismicFaultSegment> &faults) {
    m_pickOverlays = picks;
    m_faultOverlays = faults;
    update();
}

void SeismicSectionCanvas::setFaultStickOverlays(
    const QVector<SeismicSectionCanvas::FaultStickDisplay> &sticks) {
    m_faultStickOverlays = sticks;
    update();
}

void SeismicSectionCanvas::setFaultSurfaceCut(
    const SeismicSectionCanvas::FaultSurfaceCutDisplay &cut) {
    m_faultSurfaceCut = cut;
    update();
}

} // namespace seismic
