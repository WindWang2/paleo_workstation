// 层：视图
#include "ui/seismicsection/seismicsectioncanvas.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFontDatabase>
#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

namespace {

inline unsigned char ToByte(float v) {
    const int val = static_cast<int>(std::round(v * 255.0f));
    return static_cast<unsigned char>(std::clamp(val, 0, 255));
}

// D2.8：colormap 采样（t ∈ [-1,1] → RGB），预设 8 档
inline QRgb SampleColorMap(SectionColorMapType type, float t) {
    switch (type) {
    case SectionColorMapType::RedWhiteBlue: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(ToByte(0.85f * k), ToByte(0.90f * k), 255, 255);
        return qRgba(255, ToByte(0.88f * k), ToByte(0.84f * k), 255);
    }
    case SectionColorMapType::BlueWhiteRed: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(255, ToByte(0.88f * k), ToByte(0.84f * k), 255);
        return qRgba(ToByte(0.85f * k), ToByte(0.90f * k), 255, 255);
    }
    case SectionColorMapType::Grayscale: {
        const unsigned char g = ToByte((t + 1.0f) * 0.5f);
        return qRgba(g, g, g, 255);
    }
    case SectionColorMapType::BlackWhiteBlue: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(ToByte(0.85f * k), ToByte(0.90f * k), 255, 255);
        const unsigned char g = ToByte(k);
        return qRgba(g, g, g, 255); // Peak→Black
    }
    case SectionColorMapType::RedWhiteBlack: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(ToByte(0.22f * k + 0.78f * k), ToByte(0.22f * k + 0.78f * k),
                         ToByte(0.22f * k + 0.78f * k), 255); // Trough→Black
        return qRgba(255, ToByte(0.88f * k), ToByte(0.84f * k), 255);
    }
    case SectionColorMapType::GreenWhiteMagenta: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(255, ToByte(0.80f * k), 255, 255);
        return qRgba(ToByte(0.20f * k), ToByte(0.78f * k), ToByte(0.30f * k), 255);
    }
    case SectionColorMapType::CyanWhiteOrange: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(255, ToByte(0.60f * k), ToByte(0.13f * k), 255);
        return qRgba(ToByte(0.10f * k), ToByte(0.75f * k), 255, 255);
    }
    case SectionColorMapType::Rainbow:
    default: {
        const float u = std::clamp((t + 1.0f) * 0.5f, 0.0f, 1.0f);
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (u < 0.25f) {
            const float f = u / 0.25f;
            b = 1.0f; g = f;
        } else if (u < 0.5f) {
            const float f = (u - 0.25f) / 0.25f;
            g = 1.0f; b = 1.0f - f;
        } else if (u < 0.75f) {
            const float f = (u - 0.5f) / 0.25f;
            g = 1.0f; r = f;
        } else {
            const float f = (u - 0.75f) / 0.25f;
            r = 1.0f; g = 1.0f - f;
        }
        return qRgba(ToByte(r), ToByte(g), ToByte(b), 255);
    }
    }
}

// D2.1 纹理缓存键：内容指纹（值域 + 全值 FNV）× 显示参数
qint64 SliceFingerprint(const SgySliceImage &img) {
    qint64 h = 1469598103934665603ll;
    const auto mix = [&h](double v) {
        const quint64 bits = *reinterpret_cast<const quint64 *>(&v);
        h ^= static_cast<qint64>(bits);
        h *= 1099511628211ll;
    };
    mix(img.valueMin);
    mix(img.valueMax);
    const std::size_t stride = std::max<std::size_t>(1, img.values.size() / 4096);
    for (std::size_t i = 0; i < img.values.size(); i += stride) {
        const double v = img.values[i];
        mix(std::isnan(v) ? -99999.0 : static_cast<double>(v));
    }
    return h;
}

} // namespace

SeismicSectionCanvas::SeismicSectionCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(400, 300);
}

void SeismicSectionCanvas::setSectionData(
    const SgySliceImage &image,
    float sampleIntervalMs,
    double startSampleMs,
    const std::vector<float> &columnDistancesM,
    const std::vector<glm::dvec2> &mapCoords)
{
    m_orientation = SectionOrientation::Vertical;
    m_slice = image;
    m_traces = image.width;
    m_samples = image.height;
    m_dtMs = sampleIntervalMs > 0.001f ? sampleIntervalMs : 2.0f;
    m_t0Ms = startSampleMs;
    m_columnDistances = columnDistancesM;
    m_mapCoords = mapCoords;
    m_noDataReason.clear(); // 有数据即清原因态

    if (m_columnDistances.size() != static_cast<std::size_t>(m_traces)) {
        m_columnDistances.resize(m_traces);
        for (int i = 0; i < m_traces; ++i) {
            m_columnDistances[i] = static_cast<float>(i * 25.0); // Default 25m trace spacing
        }
    }

    rebuildDisplayValues();

    // D2.1 切片纹理缓存：(内容指纹, 增益, colormap, 反转, 阈值, 极性, AGC) 键控
    // ——同参数下切回已看过的线即出（不重算逐像素上色）
    const qint64 key = SliceFingerprint(m_slice) ^ (qint64(m_gain * 100.0) << 8) ^
                       (qint64(m_contrast * 100.0) << 20) ^
                       (qint64(static_cast<int>(m_colorMap)) << 36) ^
                       (qint64(m_cmapInverted ? 1 : 0) << 40) ^
                       (qint64(int(m_threshold * 100)) << 41) ^
                       (qint64(m_polarityInverted ? 1 : 0) << 49) ^
                       (qint64(m_agcEnabled ? m_agcWindowMs : 0) << 50);
    for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
        if (it->key == key && it->image.size() == QSize(m_traces, m_samples)) {
            m_cachedImage = it->image; // 命中：即出
            m_textureCache.erase(it);
            m_textureCache.push_back({key, m_cachedImage}); // LRU 置顶
            m_lodStride = 0;
            fitToWindow();
            update();
            return;
        }
    }

    rebuildImage();
    fitToWindow();
    update();
}

void SeismicSectionCanvas::setTimeSliceData(
    const SgySliceImage &image,
    double twtMs,
    int inlineMin, int inlineMax,
    int xlineMin, int xlineMax)
{
    m_orientation = SectionOrientation::TimeSlice;
    m_slice = image;
    m_traces = image.width;    // XL count
    m_samples = image.height;  // IL count
    m_currentTimeMs = twtMs;
    m_inlineMin = inlineMin;
    m_inlineMax = inlineMax;
    m_xlineMin = xlineMin;
    m_xlineMax = xlineMax;
    m_columnDistances.clear();
    m_mapCoords.clear();

    rebuildImage();
    fitToWindow();
    update();
}

void SeismicSectionCanvas::beginTimeSliceTiled(
    int xlineCount, int inlineCount, double twtMs,
    int inlineMin, int inlineMax, int xlineMin, int xlineMax)
{
    m_orientation = SectionOrientation::TimeSlice;
    m_traces = std::max(1, xlineCount);    // XL count
    m_samples = std::max(1, inlineCount);  // IL count
    m_currentTimeMs = twtMs;
    m_inlineMin = inlineMin;
    m_inlineMax = inlineMax;
    m_xlineMin = xlineMin;
    m_xlineMax = xlineMax;
    m_columnDistances.clear();
    m_mapCoords.clear();

    // 全网格 NaN 底图：未到的瓦片显示为无数据灰，绝不冒充零振幅。
    m_slice = SgySliceImage{};
    m_slice.width = m_traces;
    m_slice.height = m_samples;
    m_slice.values.assign(static_cast<std::size_t>(m_traces) * m_samples,
                          std::numeric_limits<float>::quiet_NaN());
    m_slice.valueMin = 0.0f;
    m_slice.valueMax = 1.0f;

    rebuildImage();
    fitToWindow();
    update();
}

void SeismicSectionCanvas::appendTimeSliceTile(const SgySliceImage &tile, int x, int y)
{
    if (tile.width <= 0 || tile.height <= 0 ||
        x < 0 || y < 0 || x + tile.width > m_traces || y + tile.height > m_samples ||
        tile.values.size() < static_cast<std::size_t>(tile.width) * tile.height) {
        return;
    }

    for (int ty = 0; ty < tile.height; ++ty) {
        const std::size_t srcRow = static_cast<std::size_t>(ty) * tile.width;
        const std::size_t dstRow = static_cast<std::size_t>(y + ty) * m_traces + x;
        for (int tx = 0; tx < tile.width; ++tx) {
            m_slice.values[dstRow + tx] = tile.values[srcRow + tx];
        }
    }
    // 动态范围随瓦片到达累积（有限值才算数）
    for (float v : tile.values) {
        if (std::isfinite(v)) {
            m_slice.valueMin = std::min(m_slice.valueMin, v);
            m_slice.valueMax = std::max(m_slice.valueMax, v);
        }
    }

    paintValueRegion(x, y, tile.width, tile.height);
    update();
}

void SeismicSectionCanvas::finishTimeSliceTiled(const SgySliceImage &full)
{
    if (full.width <= 0 || full.height <= 0) {
        return;
    }
    m_slice = full;
    m_traces = full.width;
    m_samples = full.height;
    rebuildImage();
    update();
}

void SeismicSectionCanvas::setOrientation(SectionOrientation orientation) {
    if (m_orientation != orientation) {
        m_orientation = orientation;
        fitToWindow();
        update();
    }
}

void SeismicSectionCanvas::clearData() {
    m_slice = {};
    m_traces = 0;
    m_samples = 0;
    m_columnDistances.clear();
    m_mapCoords.clear();
    m_cachedImage = QImage();
    m_displayValues.clear();
    m_lodStride = 0;
    update();
}

// ---- D2.14 空数据原因态 ----
void SeismicSectionCanvas::setNoDataReason(const QString &reason) {
    m_noDataReason = reason;
    update();
}

void SeismicSectionCanvas::setWells(const std::vector<SectionWellInfo> &wells) {
    m_wells = wells;
    update();
}

void SeismicSectionCanvas::setTimeDepthModel(const TimeDepthModel &model) {
    m_tdModel = model;
    // Re-calibrate well tops & curves with new model
    for (auto &w : m_wells) {
        for (auto &top : w.tops) {
            const double depth = top.tvd > 0.0 ? top.tvd : top.md;
            top.twtMs = m_tdModel.DepthToTwtMs(depth);
        }
        for (auto &curve : w.curves) {
            curve.twtMs.clear();
            curve.twtMs.reserve(curve.depthsM.size());
            for (double d : curve.depthsM) {
                curve.twtMs.push_back(m_tdModel.DepthToTwtMs(d));
            }
        }
    }
    update();
}

void SeismicSectionCanvas::setColorMap(SectionColorMapType type) {
    if (m_colorMap != type) {
        m_colorMap = type;
        rebuildColorLut();
        rebuildImage();
        update();
    }
}

// ---- D2.8 反转 ----
void SeismicSectionCanvas::setColorMapInverted(bool inverted) {
    if (m_cmapInverted != inverted) {
        m_cmapInverted = inverted;
        rebuildColorLut();
        rebuildImage();
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

// ---- D2.10 卷帘对比 ----
void SeismicSectionCanvas::setCompareData(const SgySliceImage &image, const QString &label) {
    m_compareSlice = image;
    m_compareLabel = label;
    m_compareImage = QImage();
    if (m_compareEnabled && image.width > 0 && image.height > 0) {
        // B 图按当前显示参数上色（复用 LUT 路径）
        m_compareImage = QImage(image.width, image.height, QImage::Format_ARGB32_Premultiplied);
        const float absMax = std::max(std::abs(image.valueMin), std::abs(image.valueMax));
        const float baseScale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;
        for (int y = 0; y < image.height; ++y) {
            auto *scan = reinterpret_cast<QRgb *>(m_compareImage.scanLine(y));
            for (int x = 0; x < image.width; ++x) {
                const float raw = image.Value(x, y);
                if (!std::isfinite(raw)) {
                    scan[x] = qRgba(48, 49, 49, 255);
                    continue;
                }
                const float val = std::clamp(raw * baseScale * m_gain * m_contrast, -1.0f, 1.0f);
                scan[x] = m_colorLut[static_cast<std::size_t>(
                    std::clamp(int((val + 1.0f) * 127.5f), 0, 255))];
            }
        }
    }
    update();
}

void SeismicSectionCanvas::setCompareEnabled(bool enabled) {
    if (m_compareEnabled != enabled) {
        m_compareEnabled = enabled;
        if (enabled)
            setCompareData(m_compareSlice, m_compareLabel); // 重建 B 图纹理
        update();
    }
}

void SeismicSectionCanvas::setCurtainPos(double frac) {
    const double clamped = std::clamp(frac, 0.02, 0.98);
    if (std::abs(m_curtainPos - clamped) > 1e-4) {
        m_curtainPos = clamped;
        emit curtainMoved(m_curtainPos);
        update();
    }
}

// ---- 直接导航（测试与外部驱动）----
void SeismicSectionCanvas::setZoom(double zx, double zy) {
    m_zoomX = std::clamp(zx, 0.12, 64.0);
    if (zy > 0.0)
        m_zoomY = std::clamp(zy, 0.12, 64.0);
    emit zoomChanged(m_zoomX);
    update();
}

void SeismicSectionCanvas::panBy(int dx, int dy) {
    m_panX += dx;
    m_panY += dy;
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

// ---- D2.9 导出 PNG / D2.13 抓图 ----
QImage SeismicSectionCanvas::grabCanvasImage(double scale) {
    QImage img(std::max(1, int(width() * scale)), std::max(1, int(height() * scale)),
               QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter painter(&img);
    painter.scale(scale, scale);
    render(&painter);
    painter.end();
    return img;
}

bool SeismicSectionCanvas::exportPng(const QString &filePath, double scale) {
    return grabCanvasImage(scale).save(filePath, "PNG");
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

void SeismicSectionCanvas::zoomIn() {
    const double factor = 1.25;
    const QRect vp = viewportRect();
    const double centerX = vp.center().x();
    const double centerY = vp.center().y();

    const double traceCenter = pixelToTrace(centerX);
    const double timeCenter = pixelToTime(centerY);

    m_zoomX = std::clamp(m_zoomX * factor, 0.12, 64.0);
    m_zoomY = std::clamp(m_zoomY * factor, 0.12, 64.0);

    m_panX = centerX - m_leftMargin - traceCenter * m_zoomX;
    m_panY = centerY - m_topMargin - ((timeCenter - m_t0Ms) / m_dtMs) * m_zoomY;

    emit zoomChanged(m_zoomX);
    update();
}

void SeismicSectionCanvas::zoomOut() {
    const double factor = 0.8;
    const QRect vp = viewportRect();
    const double centerX = vp.center().x();
    const double centerY = vp.center().y();

    const double traceCenter = pixelToTrace(centerX);
    const double timeCenter = pixelToTime(centerY);

    m_zoomX = std::clamp(m_zoomX * factor, 0.12, 64.0);
    m_zoomY = std::clamp(m_zoomY * factor, 0.12, 64.0);

    m_panX = centerX - m_leftMargin - traceCenter * m_zoomX;
    m_panY = centerY - m_topMargin - ((timeCenter - m_t0Ms) / m_dtMs) * m_zoomY;

    emit zoomChanged(m_zoomX);
    update();
}

void SeismicSectionCanvas::resetZoom() {
    fitToWindow();
}

void SeismicSectionCanvas::fitToWindow() {
    if (m_traces <= 0 || m_samples <= 0) {
        m_zoomX = 1.0;
        m_zoomY = 1.0;
        m_panX = 0.0;
        m_panY = 0.0;
        update();
        return;
    }

    const QRect vp = viewportRect();
    if (vp.width() <= 10 || vp.height() <= 10)
        return;

    m_zoomX = static_cast<double>(vp.width()) / static_cast<double>(m_traces);
    m_zoomY = static_cast<double>(vp.height()) / static_cast<double>(m_samples) * m_vExag; // D2.7
    m_panX = 0.0;
    m_panY = 0.0;

    emit zoomChanged(m_zoomX);
    update();
}

double SeismicSectionCanvas::totalDistanceM() const {
    if (m_columnDistances.empty())
        return m_traces > 0 ? static_cast<double>(m_traces - 1) * 25.0 : 0.0;
    return static_cast<double>(m_columnDistances.back());
}

double SeismicSectionCanvas::traceToPixelX(double trace) const {
    return static_cast<double>(m_leftMargin) + m_panX + trace * m_zoomX;
}

double SeismicSectionCanvas::timeToPixelY(double twtMs) const {
    const double sampleIdx = (twtMs - m_t0Ms) / m_dtMs;
    return static_cast<double>(m_topMargin) + m_panY + sampleIdx * m_zoomY;
}

double SeismicSectionCanvas::pixelToTrace(double px) const {
    if (std::abs(m_zoomX) < 1e-6)
        return 0.0;
    return (px - static_cast<double>(m_leftMargin) - m_panX) / m_zoomX;
}

double SeismicSectionCanvas::pixelToTime(double py) const {
    if (std::abs(m_zoomY) < 1e-6)
        return m_t0Ms;
    const double sampleIdx = (py - static_cast<double>(m_topMargin) - m_panY) / m_zoomY;
    return m_t0Ms + sampleIdx * m_dtMs;
}

double SeismicSectionCanvas::inlineToPixelY(double inlineNo) const {
    const double span = std::max(1, m_inlineMax - m_inlineMin);
    const double rowIdx = (static_cast<double>(m_inlineMax) - inlineNo) / span * std::max(1, m_samples - 1);
    return static_cast<double>(m_topMargin) + m_panY + rowIdx * m_zoomY;
}

double SeismicSectionCanvas::pixelToInline(double py) const {
    if (std::abs(m_zoomY) < 1e-6)
        return m_inlineMin;
    const double rowIdx = (py - static_cast<double>(m_topMargin) - m_panY) / m_zoomY;
    const double norm = std::clamp(rowIdx / std::max(1, m_samples - 1), 0.0, 1.0);
    return static_cast<double>(m_inlineMax) - norm * (m_inlineMax - m_inlineMin);
}

QRect SeismicSectionCanvas::viewportRect() const {
    // D2.5：双刻度时右缘多留 32px 给深度轴（色标仍占最右 m_rightMargin）
    const int right = m_rightMargin + (m_dualScale ? 32 : 0);
    return QRect(m_leftMargin, m_topMargin,
                 std::max(1, width() - m_leftMargin - right),
                 std::max(1, height() - m_topMargin));
}

void SeismicSectionCanvas::rebuildImage() {
    if (m_colorLut.empty())
        rebuildColorLut();
    if (m_traces <= 0 || m_samples <= 0 || m_slice.values.empty()) {
        m_cachedImage = QImage();
        m_lodStride = 0;
        return;
    }

    m_cachedImage = QImage(m_traces, m_samples, QImage::Format_ARGB32_Premultiplied);
    paintValueRegion(0, 0, m_traces, m_samples);
    m_lodStride = 0; // 数据变了，LOD 缓存失效

    // D2.1 纹理缓存入列（LRU ≤4；键在 setSectionData 中计算过——此处重建键）
    const qint64 key = SliceFingerprint(m_slice) ^ (qint64(m_gain * 100.0) << 8) ^
                       (qint64(m_contrast * 100.0) << 20) ^
                       (qint64(static_cast<int>(m_colorMap)) << 36) ^
                       (qint64(m_cmapInverted ? 1 : 0) << 40) ^
                       (qint64(int(m_threshold * 100)) << 41) ^
                       (qint64(m_polarityInverted ? 1 : 0) << 49) ^
                       (qint64(m_agcEnabled ? m_agcWindowMs : 0) << 50);
    for (auto it = m_textureCache.begin(); it != m_textureCache.end();) {
        it = (it->key == key) ? m_textureCache.erase(it) : it + 1;
    }
    m_textureCache.push_back({key, m_cachedImage});
    while (m_textureCache.size() > 4)
        m_textureCache.erase(m_textureCache.begin());
}

// D2.4：AGC / 增益曲线 / 极性处理后的显示值（空 = 直通原始值；hover 读原始值）
void SeismicSectionCanvas::rebuildDisplayValues() {
    m_displayValues.clear();
    if (m_traces <= 0 || m_samples <= 0 || m_slice.values.empty())
        return;
    const bool needAgc = m_agcEnabled && m_orientation == SectionOrientation::Vertical;
    const bool needCurve = !m_gainCurve.empty() && m_orientation == SectionOrientation::Vertical;
    if (!needAgc && !needCurve && !m_polarityInverted)
        return; // 直通

    m_displayValues.assign(m_slice.values.begin(), m_slice.values.end());
    const std::size_t n = m_displayValues.size();

    if (m_polarityInverted) {
        for (std::size_t i = 0; i < n; ++i) {
            const float v = m_displayValues[i];
            m_displayValues[i] = std::isfinite(v) ? -v : v;
        }
    }

    if (needCurve) {
        for (int y = 0; y < m_samples; ++y) {
            const double twt = m_t0Ms + static_cast<double>(y) * m_dtMs;
            const float g = gainAtTwt(twt);
            if (std::abs(g - 1.0f) < 1e-4f)
                continue;
            const std::size_t row = static_cast<std::size_t>(y) * m_traces;
            for (int x = 0; x < m_traces; ++x) {
                const float v = m_displayValues[row + x];
                m_displayValues[row + x] = std::isfinite(v) ? v * g : v;
            }
        }
    }

    if (needAgc) {
        // 滑动窗 RMS：道内时间窗，窗口内 RMS 归一并截幅到 ±5×窗均值（防 0 除与炸点）
        const int halfW = std::max(1, static_cast<int>(m_agcWindowMs * 0.5 / m_dtMs));
        std::vector<float> trace(m_samples);
        std::vector<double> prefix(m_samples + 1, 0.0);
        for (int x = 0; x < m_traces; ++x) {
            bool anyFinite = false;
            for (int y = 0; y < m_samples; ++y) {
                const float v = m_displayValues[static_cast<std::size_t>(y) * m_traces + x];
                trace[y] = std::isfinite(v) ? v : 0.0f;
                anyFinite = anyFinite || std::isfinite(v);
            }
            if (!anyFinite)
                continue;
            for (int y = 0; y < m_samples; ++y)
                prefix[y + 1] = prefix[y] + static_cast<double>(trace[y]) * trace[y];
            for (int y = 0; y < m_samples; ++y) {
                const int lo = std::max(0, y - halfW);
                const int hi = std::min(m_samples - 1, y + halfW);
                const double count = hi - lo + 1;
                const double energy = prefix[hi + 1] - prefix[lo];
                const double rms = std::sqrt(energy / std::max(1.0, count));
                const float scale = rms > 1e-8 ? static_cast<float>(1.0 / rms) : 1.0f;
                const float v = m_displayValues[static_cast<std::size_t>(y) * m_traces + x];
                if (std::isfinite(v))
                    m_displayValues[static_cast<std::size_t>(y) * m_traces + x] =
                        std::clamp(v * scale, -5.0f, 5.0f);
            }
        }
    }
}

float SeismicSectionCanvas::displayValue(int traceIdx, int sampleIdx) const {
    if (traceIdx < 0 || traceIdx >= m_traces || sampleIdx < 0 || sampleIdx >= m_samples)
        return std::numeric_limits<float>::quiet_NaN();
    if (!m_displayValues.empty())
        return m_displayValues[static_cast<std::size_t>(sampleIdx) * m_traces + traceIdx];
    return m_slice.Value(traceIdx, sampleIdx);
}

// D2.8：256 档 LUT（colormap × 反转）
void SeismicSectionCanvas::rebuildColorLut() {
    m_colorLut.resize(256);
    for (int i = 0; i < 256; ++i) {
        float t = static_cast<float>(i) / 127.5f - 1.0f;
        if (m_cmapInverted)
            t = -t;
        m_colorLut[static_cast<std::size_t>(i)] = SampleColorMap(m_colorMap, t);
    }
}

void SeismicSectionCanvas::paintValueRegion(int x0, int y0, int w, int h) {
    if (m_cachedImage.isNull() || m_slice.values.empty()) {
        return;
    }
    if (m_colorLut.empty())
        rebuildColorLut();
    x0 = std::clamp(x0, 0, m_traces);
    y0 = std::clamp(y0, 0, m_samples);
    w = std::clamp(w, 0, m_traces - x0);
    h = std::clamp(h, 0, m_samples - y0);
    if (w <= 0 || h <= 0) {
        return;
    }

    const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
    const float baseScale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;
    const float effectiveScale = baseScale * m_gain;
    const bool hasDisplay = !m_displayValues.empty();

    for (int y = y0; y < y0 + h; ++y) {
        auto *scanLine = reinterpret_cast<QRgb *>(m_cachedImage.scanLine(y));
        const int rowOffset = y * m_traces;
        for (int x = x0; x < x0 + w; ++x) {
            // D2.4：显示值（AGC/增益曲线/极性已处理）；无处理时直通原始值
            const float raw = hasDisplay
                ? m_displayValues[static_cast<std::size_t>(rowOffset) + x]
                : m_slice.values[static_cast<std::size_t>(rowOffset) + x];
            if (!std::isfinite(raw)) {
                // NaN: Dark neutral gray per DESIGN.md (#303131)
                scanLine[x] = qRgba(48, 49, 49, 255);
                continue;
            }

            float val = std::clamp(raw * effectiveScale * m_contrast, -1.0f, 1.0f);
            // D2.3：密度阈值——|val| 低于阈值的振幅压为 0（白），压噪声底
            if (m_threshold > 0.0f && std::abs(val) < m_threshold)
                val = 0.0f;
            scanLine[x] = m_colorLut[static_cast<std::size_t>(
                std::clamp(int((val + 1.0f) * 127.5f), 0, 255))];
        }
    }
}

// D2.6 LOD 抽稀：zoomY<1（多样本/像素）时按步长取带内最大绝对值（保幅保事件）
const QImage &SeismicSectionCanvas::lodImage() const {
    const int stride = std::clamp(int(1.0 / std::max(1e-6, m_zoomY)), 1, 64);
    if (stride <= 1) {
        m_lodStride = 0;
        return m_cachedImage;
    }
    if (m_lodStride == stride && !m_lodCache.isNull())
        return m_lodCache;

    const int outRows = (m_samples + stride - 1) / stride;
    m_lodCache = QImage(m_traces, outRows, QImage::Format_ARGB32_Premultiplied);
    const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
    const float baseScale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;
    const bool hasDisplay = !m_displayValues.empty();
    for (int r = 0; r < outRows; ++r) {
        auto *scan = reinterpret_cast<QRgb *>(m_lodCache.scanLine(r));
        const int yBegin = r * stride;
        const int yEnd = std::min(m_samples, yBegin + stride);
        for (int x = 0; x < m_traces; ++x) {
            float best = std::numeric_limits<float>::quiet_NaN();
            float bestAbs = -1.0f;
            for (int y = yBegin; y < yEnd; ++y) {
                const float raw = hasDisplay
                    ? m_displayValues[static_cast<std::size_t>(y) * m_traces + x]
                    : m_slice.values[static_cast<std::size_t>(y) * m_traces + x];
                if (!std::isfinite(raw))
                    continue;
                const float a = std::abs(raw);
                if (a > bestAbs) {
                    bestAbs = a;
                    best = raw;
                }
            }
            if (!std::isfinite(best)) {
                scan[x] = qRgba(48, 49, 49, 255);
                continue;
            }
            float val = std::clamp(best * baseScale * m_gain * m_contrast, -1.0f, 1.0f);
            if (m_threshold > 0.0f && std::abs(val) < m_threshold)
                val = 0.0f;
            scan[x] = m_colorLut[static_cast<std::size_t>(
                std::clamp(int((val + 1.0f) * 127.5f), 0, 255))];
        }
    }
    m_lodStride = stride;
    return m_lodCache;
}

// D2.2 wiggle 变面积：正相位涂黑 + 波形线；缩小时按像素步长抽稀
void SeismicSectionCanvas::paintWiggleOverlay(QPainter &p, const QRect &vp, double alpha) {
    if (!hasData())
        return;
    p.save();
    p.setClipRect(vp);
    p.setOpacity(alpha);

    const double spp = 1.0 / std::max(1e-6, m_zoomY); // 每像素样本数
    const int step = std::max(1, static_cast<int>(spp));
    const double halfW = std::max(2.0, m_zoomX * 0.42); // 波形半宽 = 0.84 道距
    const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
    const float baseScale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;

    const int t0 = std::clamp(static_cast<int>(std::floor(pixelToTrace(vp.left() - halfW))) - 1, 0, m_traces - 1);
    const int t1 = std::clamp(static_cast<int>(std::ceil(pixelToTrace(vp.right() + halfW))) + 1, 0, m_traces - 1);

    QPen wavePen(QColor(30, 38, 48), 1.0);
    QBrush fillBrush(QColor(24, 30, 40));
    for (int t = t0; t <= t1; ++t) {
        const double cx = traceToPixelX(t);
        if (cx < vp.left() - halfW - 8 || cx > vp.right() + halfW + 8)
            continue;

        QPolygonF wave;
        QPolygonF fill;
        bool fillOpen = false;
        for (int y = 0; y < m_samples; y += step) {
            const float raw = displayValue(t, y);
            if (!std::isfinite(raw))
                continue;
            const double py = timeToPixelY(m_t0Ms + static_cast<double>(y) * m_dtMs);
            if (py < vp.top() - 10 || py > vp.bottom() + 10)
                continue;
            const double amp = std::clamp(static_cast<double>(raw) * baseScale * m_gain * m_contrast, -1.0, 1.0);
            const double px = cx + amp * halfW;
            wave.append(QPointF(px, py));
            if (amp > 0.0) {
                if (!fillOpen) {
                    fill.append(QPointF(cx, py)); // 从中线起笔
                    fillOpen = true;
                }
                fill.append(QPointF(px, py));
            } else if (fillOpen) {
                fill.append(QPointF(cx, py)); // 回到中线封口
                fillOpen = false;
            }
        }
        if (wave.size() >= 2) {
            // 中线（道位置参考线）
            p.setPen(QPen(QColor(223, 229, 236), 1.0));
            p.drawLine(QPointF(cx, vp.top()), QPointF(cx, vp.bottom()));
            if (fill.size() >= 3) {
                p.setPen(Qt::NoPen);
                p.setBrush(fillBrush);
                p.drawPolygon(fill);
            }
            p.setPen(wavePen);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(wave);
        }
    }
    p.restore();
}

void SeismicSectionCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect vp = viewportRect();
    const QFont monoFont(QStringLiteral("JetBrains Mono"), 8);
    const QFont bodyFont(QStringLiteral("Noto Sans SC"), 8);

    // 1. Clear background
    p.fillRect(rect(), QColor(QStringLiteral("#FFFFFF")));

    // 2. Render seismic image inside viewport（D2.6 缩小时走 LOD 抽稀图）
    p.save();
    p.setClipRect(vp);

    if (hasData() && !m_cachedImage.isNull()) {
        const double x0 = traceToPixelX(0.0);
        const double y0 = timeToPixelY(m_t0Ms);
        const double x1 = traceToPixelX(static_cast<double>(m_traces));
        const double y1 = timeToPixelY(m_t0Ms + static_cast<double>(m_samples) * m_dtMs);
        const QRectF imgDest(x0, y0, x1 - x0, y1 - y0);

        const bool drawDensity = m_displayMode != SectionDisplayMode::WiggleVA ||
                                 m_compareEnabled;
        if (drawDensity) {
            if (m_displayMode == SectionDisplayMode::Mixed)
                p.setOpacity(0.42); // 混合：密度淡显打底
            const QImage &img = (m_zoomY < 1.0) ? lodImage() : m_cachedImage;
            p.drawImage(imgDest, img); // 抽稀保全程，目标矩形不变
            p.setOpacity(1.0);
        } else {
            // 纯 wiggle：白底
            p.fillRect(vp, QColor(QStringLiteral("#FFFFFF")));
        }

        // D2.10 卷帘对比：帘左 A（当前）/ 帘右 B（对比图）
        if (m_compareEnabled && !m_compareImage.isNull()) {
            const double curtainPx = vp.left() + m_curtainPos * vp.width();
            p.save();
            p.setClipRect(QRectF(vp.left(), vp.top(), curtainPx - vp.left(), vp.height()));
            const QImage &imgA = (m_zoomY < 1.0) ? lodImage() : m_cachedImage;
            p.drawImage(imgDest, imgA);
            p.restore();

            p.save();
            p.setClipRect(QRectF(curtainPx, vp.top(), vp.right() - curtainPx, vp.height()));
            const double by1 = y0 + (y1 - y0); // B 图与 A 同几何
            p.drawImage(QRectF(x0, y0, x1 - x0, by1 - y0), m_compareImage);
            p.restore();

            // 分割线 + 拖拽柄 + A/B 标签
            p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 2.0));
            p.drawLine(QPointF(curtainPx, vp.top()), QPointF(curtainPx, vp.bottom()));
            p.setBrush(QColor(QStringLiteral("#1B73D0")));
            p.drawPolygon(QPolygonF({
                QPointF(curtainPx - 7.0, vp.center().y() - 9.0),
                QPointF(curtainPx + 7.0, vp.center().y() - 9.0),
                QPointF(curtainPx + 2.0, vp.center().y()),
                QPointF(curtainPx + 7.0, vp.center().y() + 9.0),
                QPointF(curtainPx - 7.0, vp.center().y() + 9.0),
                QPointF(curtainPx - 2.0, vp.center().y())}));
            p.setFont(bodyFont);
            p.setPen(QColor(QStringLiteral("#24303E")));
            p.drawText(QRectF(vp.left() + 4.0, vp.top() + 2.0, 120.0, 16.0),
                       Qt::AlignLeft, tr("A · %1").arg(m_compareLabel.isEmpty() ? tr("当前") : tr("当前")));
            p.drawText(QRectF(vp.right() - 160.0, vp.top() + 2.0, 156.0, 16.0),
                       Qt::AlignRight, tr("B · %1").arg(m_compareLabel));
        }

        // D2.2 wiggle 叠加（WiggleVA 全强 / Mixed 全强叠在淡密度上）
        if (m_displayMode != SectionDisplayMode::Density && !m_compareEnabled)
            paintWiggleOverlay(p, vp, 1.0);
    } else {
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.setFont(bodyFont);
        // D2.14：有原因态时展示原因（如「线号 1234 不在测网…」），否则通用占位
        const QString placeholder = m_noDataReason.isEmpty()
            ? tr("未加载地震剖面数据\n（支持拖拽测线或从地图生成连井/任意剖面）")
            : m_noDataReason;
        p.drawText(vp, Qt::AlignCenter | Qt::TextWordWrap, placeholder);
    }

    // 3. Render Wellbores, Tops, and Curves inside viewport
    if (m_showWells && !m_wells.empty() && hasData()) {
        if (m_orientation == SectionOrientation::TimeSlice) {
            // Horizontal slice wellhead markers & penetrated tops
            for (const auto &well : m_wells) {
                double wellXl = 0.0;
                double wellIl = 0.0;
                if (well.surfaceX >= m_xlineMin && well.surfaceX <= m_xlineMax &&
                    well.surfaceY >= m_inlineMin && well.surfaceY <= m_inlineMax) {
                    wellXl = well.surfaceX;
                    wellIl = well.surfaceY;
                } else {
                    const double norm = std::clamp(well.tracePosition / std::max(1, m_traces - 1), 0.0, 1.0);
                    wellXl = m_xlineMin + norm * (m_xlineMax - m_xlineMin);
                    wellIl = (m_inlineMin + m_inlineMax) * 0.5;
                }

                const double colIdx = (wellXl - m_xlineMin) / std::max(1, m_xlineMax - m_xlineMin) * std::max(1, m_traces - 1);
                const double wx = traceToPixelX(colIdx);
                const double wy = inlineToPixelY(wellIl);

                if (wx < vp.left() - 40 || wx > vp.right() + 40 || wy < vp.top() - 40 || wy > vp.bottom() + 40)
                    continue;

                // Borehole target marker (halo, blue circle, white crosshair)
                p.setPen(QPen(QColor(QStringLiteral("#FFFFFF")), 4.0));
                p.setBrush(QColor(QStringLiteral("#1B73D0")));
                p.drawEllipse(QPointF(wx, wy), 5.5, 5.5);

                p.setPen(QPen(QColor(QStringLiteral("#FFFFFF")), 1.5));
                p.drawLine(QPointF(wx - 4.0, wy), QPointF(wx + 4.0, wy));
                p.drawLine(QPointF(wx, wy - 4.0), QPointF(wx, wy + 4.0));

                // Well tag badge
                p.setFont(monoFont);
                QString wellTag = well.wellName;
                QString nearTop;
                double minDiff = 1e9;
                for (const auto &top : well.tops) {
                    const double diff = std::abs(top.twtMs - m_currentTimeMs);
                    if (diff < minDiff && diff <= 35.0) {
                        minDiff = diff;
                        nearTop = top.topName;
                    }
                }
                if (!nearTop.isEmpty()) {
                    wellTag += QStringLiteral(" [%1]").arg(nearTop);
                }

                const QFontMetrics fm(monoFont);
                const int tw = fm.horizontalAdvance(wellTag);
                const QRectF tagRect(wx + 8.0, wy - 9.0, tw + 8.0, 18.0);

                p.setBrush(QColor(QStringLiteral("#FFFFFF")));
                p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.0));
                p.drawRoundedRect(tagRect, 3.0, 3.0);

                p.setPen(QColor(QStringLiteral("#1B73D0")));
                p.drawText(tagRect, Qt::AlignCenter, wellTag);
            }
        } else {
            for (const auto &well : m_wells) {
                if (!well.isWithinBuffer)
                    continue;

                const double wx = traceToPixelX(well.tracePosition);
                if (wx < vp.left() - 60 || wx > vp.right() + 60)
                    continue;

                // Draw vertical wellbore trajectory line (1px white halo under 2px #1B73D0)
                const double bottomTwt = well.totalDepth > 0.0
                    ? m_tdModel.DepthToTwtMs(well.totalDepth)
                    : (m_t0Ms + m_samples * m_dtMs);
                const double wellTopY = timeToPixelY(m_t0Ms);
                const double wellBotY = std::min(timeToPixelY(bottomTwt), static_cast<double>(vp.bottom()));

                p.setPen(QPen(QColor(QStringLiteral("#FFFFFF")), 4.0));
                p.drawLine(QPointF(wx, wellTopY), QPointF(wx, wellBotY));

                p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 2.0));
                p.drawLine(QPointF(wx, wellTopY), QPointF(wx, wellBotY));

                // Formation tops
                if (m_showTops) {
                    for (const auto &top : well.tops) {
                        const double ty = timeToPixelY(top.twtMs);
                        if (ty < vp.top() || ty > vp.bottom())
                            continue;

                        // Horizontal top cross tick
                        p.setPen(QPen(QColor(QStringLiteral("#FFFFFF")), 4.0));
                        p.drawLine(QPointF(wx - 8.0, ty), QPointF(wx + 8.0, ty));
                        p.setPen(QPen(top.color.isValid() ? top.color : QColor(QStringLiteral("#1B73D0")), 2.0));
                        p.drawLine(QPointF(wx - 8.0, ty), QPointF(wx + 8.0, ty));

                        // Marker label
                        p.setFont(monoFont);
                        const QString tagText = top.topName;
                        const QFontMetrics fm(monoFont);
                        const int tw = fm.horizontalAdvance(tagText);
                        const QRectF tagRect(wx + 10.0, ty - 8.0, tw + 8.0, 16.0);

                        p.setBrush(QColor(QStringLiteral("#E8F0FE")));
                        p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.0));
                        p.drawRoundedRect(tagRect, 3.0, 3.0);

                        p.setPen(QColor(QStringLiteral("#1B73D0")));
                        p.drawText(tagRect, Qt::AlignCenter, tagText);
                    }
                }

                // Well log curves
                if (m_showCurves && !well.curves.empty()) {
                    const double trackWidth = 36.0;
                    for (const auto &curve : well.curves) {
                        if (curve.values.empty() || curve.twtMs.size() != curve.values.size())
                            continue;

                        const float span = std::max(1e-4f, curve.maxVal - curve.minVal);
                        QPolygonF poly;
                        poly.reserve(static_cast<int>(curve.values.size()));

                        for (std::size_t i = 0; i < curve.values.size(); ++i) {
                            const float val = curve.values[i];
                            if (!std::isfinite(val))
                                continue;
                            const double cy = timeToPixelY(curve.twtMs[i]);
                            if (cy < vp.top() - 10 || cy > vp.bottom() + 10)
                                continue;
                            const double norm = std::clamp(static_cast<double>((val - curve.minVal) / span), 0.0, 1.0);
                            const double cx = wx + 4.0 + norm * trackWidth;
                            poly.append(QPointF(cx, cy));
                        }

                        if (poly.size() >= 2) {
                            p.setPen(QPen(curve.color.isValid() ? curve.color : QColor(QStringLiteral("#43A047")), 1.5));
                            p.setBrush(Qt::NoBrush);
                            p.drawPolyline(poly);
                        }
                    }
                }
            }
        }
    }

    // 4. Crosshairs inside viewport
    if (m_hasHover && vp.contains(m_currentMousePos)) {
        p.setPen(QPen(QColor(27, 115, 208, 140), 1.0, Qt::DashLine));
        p.drawLine(QPointF(vp.left(), m_currentMousePos.y()), QPointF(vp.right(), m_currentMousePos.y()));
        p.drawLine(QPointF(m_currentMousePos.x(), vp.top()), QPointF(m_currentMousePos.x(), vp.bottom()));
    }

    p.restore();

    // 5. Render Top Horizontal Ruler (Distance & Well Pins)
    const QRect topRulerRect(m_leftMargin, 0, vp.width(), m_topMargin);
    p.fillRect(topRulerRect, QColor(QStringLiteral("#F5F7FA")));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawLine(QPoint(m_leftMargin, m_topMargin), QPoint(vp.right(), m_topMargin));

    if (hasData()) {
        if (m_orientation == SectionOrientation::TimeSlice) {
            // Horizontal Crossline (XL) ruler
            const double minTrace = pixelToTrace(m_leftMargin);
            const double maxTrace = pixelToTrace(vp.right());
            const double spanXl = std::max(1, m_xlineMax - m_xlineMin);
            const double minXl = m_xlineMin + (minTrace / std::max(1.0, static_cast<double>(m_traces - 1))) * spanXl;
            const double maxXl = m_xlineMin + (maxTrace / std::max(1.0, static_cast<double>(m_traces - 1))) * spanXl;

            const auto ticks = NiceStep::GenerateTicks(minXl, maxXl, m_leftMargin, vp.right(), 8, QStringLiteral("%.0f"));

            p.setFont(monoFont);
            for (const auto &tk : ticks) {
                if (tk.pixelPos < m_leftMargin || tk.pixelPos > vp.right())
                    continue;

                p.setPen(QColor(QStringLiteral("#5D6E80")));
                if (tk.isMajor) {
                    p.drawLine(QPointF(tk.pixelPos, m_topMargin - 12.0), QPointF(tk.pixelPos, m_topMargin));
                    const QString xlLabel = QStringLiteral("XL %1").arg(qRound(tk.value));
                    const QFontMetrics fm(monoFont);
                    const int tw = fm.horizontalAdvance(xlLabel);
                    p.setPen(QColor(QStringLiteral("#24303E")));
                    p.drawText(QPointF(tk.pixelPos - tw * 0.5, m_topMargin - 16.0), xlLabel);
                } else {
                    p.drawLine(QPointF(tk.pixelPos, m_topMargin - 6.0), QPointF(tk.pixelPos, m_topMargin));
                }
            }
        } else {
            // Distance ticks for vertical profile
            const double minTrace = pixelToTrace(m_leftMargin);
            const double maxTrace = pixelToTrace(vp.right());

            // Distance ticks
            const double totalDist = totalDistanceM();
            const double traceToDistFactor = m_traces > 1 ? totalDist / static_cast<double>(m_traces - 1) : 25.0;
            const double minDistM = minTrace * traceToDistFactor;
            const double maxDistM = maxTrace * traceToDistFactor;

            const auto ticks = NiceStep::GenerateTicks(minDistM, maxDistM, m_leftMargin, vp.right(), 8, QStringLiteral("%.0f"));

            p.setFont(monoFont);
            for (const auto &tk : ticks) {
                if (tk.pixelPos < m_leftMargin || tk.pixelPos > vp.right())
                    continue;

                p.setPen(QColor(QStringLiteral("#5D6E80")));
                if (tk.isMajor) {
                    p.drawLine(QPointF(tk.pixelPos, m_topMargin - 12.0), QPointF(tk.pixelPos, m_topMargin));
                    QString distLabel;
                    if (std::abs(tk.value) >= 1000.0) {
                        distLabel = QStringLiteral("%1 km").arg(tk.value / 1000.0, 0, 'f', 1);
                    } else {
                        distLabel = QStringLiteral("%1 m").arg(qRound(tk.value));
                    }
                    const QFontMetrics fm(monoFont);
                    const int tw = fm.horizontalAdvance(distLabel);
                    p.setPen(QColor(QStringLiteral("#24303E")));
                    p.drawText(QPointF(tk.pixelPos - tw * 0.5, m_topMargin - 16.0), distLabel);
                } else {
                    p.drawLine(QPointF(tk.pixelPos, m_topMargin - 6.0), QPointF(tk.pixelPos, m_topMargin));
                }
            }

            // Well indicator flags on top ruler
            if (m_showWells) {
                for (const auto &well : m_wells) {
                    if (!well.isWithinBuffer)
                        continue;
                    const double wx = traceToPixelX(well.tracePosition);
                    if (wx < m_leftMargin || wx > vp.right())
                        continue;

                    // Triangle pin pointing down
                    const QPolygonF triangle({
                        QPointF(wx - 5.0, m_topMargin - 8.0),
                        QPointF(wx + 5.0, m_topMargin - 8.0),
                        QPointF(wx, m_topMargin - 1.0)
                    });
                    p.setBrush(QColor(QStringLiteral("#1B73D0")));
                    p.setPen(Qt::NoPen);
                    p.drawPolygon(triangle);

                    // Capsule label
                    p.setFont(bodyFont);
                    const QString pinText = std::abs(well.offsetDistanceM) > 1.0
                        ? QStringLiteral("%1 (%2m)").arg(well.wellName).arg(qRound(well.offsetDistanceM))
                        : well.wellName;
                    const QFontMetrics fm(bodyFont);
                    const int tw = fm.horizontalAdvance(pinText);
                    const QRectF badge(wx - tw * 0.5 - 4.0, 4.0, tw + 8.0, 18.0);

                    p.setBrush(QColor(QStringLiteral("#E8F0FE")));
                    p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.0));
                    p.drawRoundedRect(badge, 4.0, 4.0);

                    p.setPen(QColor(QStringLiteral("#1B73D0")));
                    p.drawText(badge, Qt::AlignCenter, pinText);
                }
            }
        }
    }

    // 6. Render Left Vertical Ruler (TWT ms or Depth m, or Inline for TimeSlice)
    const QRect leftRulerRect(0, m_topMargin, m_leftMargin, vp.height());
    p.fillRect(leftRulerRect, QColor(QStringLiteral("#F5F7FA")));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawLine(QPoint(m_leftMargin, m_topMargin), QPoint(m_leftMargin, height()));

    if (hasData()) {
        if (m_orientation == SectionOrientation::TimeSlice) {
            // Vertical Inline (IL) ruler
            const double topIl = pixelToInline(m_topMargin);
            const double botIl = pixelToInline(height());

            const auto ticks = NiceStep::GenerateTicks(std::min(topIl, botIl), std::max(topIl, botIl),
                                                       m_topMargin, height(), 8, QStringLiteral("%.0f"));

            p.setFont(monoFont);
            for (const auto &tk : ticks) {
                const double py = inlineToPixelY(tk.value);
                if (py < m_topMargin || py > height())
                    continue;

                p.setPen(QColor(QStringLiteral("#5D6E80")));
                if (tk.isMajor) {
                    p.drawLine(QPointF(m_leftMargin - 10.0, py), QPointF(m_leftMargin, py));
                    const QString label = QStringLiteral("IL %1").arg(qRound(tk.value));
                    const QFontMetrics fm(monoFont);
                    const int tw = fm.horizontalAdvance(label);
                    p.setPen(QColor(QStringLiteral("#24303E")));
                    p.drawText(QPointF(m_leftMargin - 14.0 - tw, py + 4.0), label);
                } else {
                    p.drawLine(QPointF(m_leftMargin - 5.0, py), QPointF(m_leftMargin, py));
                }
            }

            // Top-left corner box
            p.fillRect(QRect(0, 0, m_leftMargin, m_topMargin), QColor(QStringLiteral("#EDF1F5")));
            p.setPen(QColor(QStringLiteral("#DFE5EC")));
            p.drawRect(QRect(0, 0, m_leftMargin, m_topMargin));

            p.setFont(bodyFont);
            p.setPen(QColor(QStringLiteral("#1B73D0")));
            const QString cornerStr = m_vertUnit == SectionVerticalUnit::TwoWayTimeMs
                ? QStringLiteral("时间切片\n%1 ms").arg(m_currentTimeMs, 0, 'f', 1)
                : QStringLiteral("深度切片\n%1 m").arg(m_tdModel.TwtMsToDepth(m_currentTimeMs), 0, 'f', 1);
            p.drawText(QRect(2, 2, m_leftMargin - 4, m_topMargin - 4), Qt::AlignCenter, cornerStr);
        } else {
            const double minTime = pixelToTime(m_topMargin);
            const double maxTime = pixelToTime(height());

            p.setFont(monoFont);
            if (m_vertUnit == SectionVerticalUnit::TwoWayTimeMs) {
                const auto ticks = NiceStep::GenerateTicks(minTime, maxTime, m_topMargin, height(), 8, QStringLiteral("%.0f"));
                for (const auto &tk : ticks) {
                    if (tk.pixelPos < m_topMargin || tk.pixelPos > height())
                        continue;

                    p.setPen(QColor(QStringLiteral("#5D6E80")));
                    if (tk.isMajor) {
                        p.drawLine(QPointF(m_leftMargin - 10.0, tk.pixelPos), QPointF(m_leftMargin, tk.pixelPos));
                        const QString label = QStringLiteral("%1").arg(qRound(tk.value));
                        const QFontMetrics fm(monoFont);
                        const int tw = fm.horizontalAdvance(label);
                        p.setPen(QColor(QStringLiteral("#24303E")));
                        p.drawText(QPointF(m_leftMargin - 14.0 - tw, tk.pixelPos + 4.0), label);
                    } else {
                        p.drawLine(QPointF(m_leftMargin - 5.0, tk.pixelPos), QPointF(m_leftMargin, tk.pixelPos));
                    }
                }
            } else {
                // Depth unit
                const double minDepth = m_tdModel.TwtMsToDepth(minTime);
                const double maxDepth = m_tdModel.TwtMsToDepth(maxTime);
                const auto ticks = NiceStep::GenerateTicks(minDepth, maxDepth, m_topMargin, height(), 8, QStringLiteral("%.0f"));
                for (const auto &tk : ticks) {
                    if (tk.pixelPos < m_topMargin || tk.pixelPos > height())
                        continue;

                    p.setPen(QColor(QStringLiteral("#5D6E80")));
                    if (tk.isMajor) {
                        p.drawLine(QPointF(m_leftMargin - 10.0, tk.pixelPos), QPointF(m_leftMargin, tk.pixelPos));
                        const QString label = QStringLiteral("%1").arg(qRound(tk.value));
                        const QFontMetrics fm(monoFont);
                        const int tw = fm.horizontalAdvance(label);
                        p.setPen(QColor(QStringLiteral("#24303E")));
                        p.drawText(QPointF(m_leftMargin - 14.0 - tw, tk.pixelPos + 4.0), label);
                    } else {
                        p.drawLine(QPointF(m_leftMargin - 5.0, tk.pixelPos), QPointF(m_leftMargin, tk.pixelPos));
                    }
                }
            }

            // Axis unit label in top-left corner box
            p.fillRect(QRect(0, 0, m_leftMargin, m_topMargin), QColor(QStringLiteral("#EDF1F5")));
            p.setPen(QColor(QStringLiteral("#DFE5EC")));
            p.drawRect(QRect(0, 0, m_leftMargin, m_topMargin));

            p.setFont(bodyFont);
            p.setPen(QColor(QStringLiteral("#5D6E80")));
            const QString unitStr = m_vertUnit == SectionVerticalUnit::TwoWayTimeMs
                ? QStringLiteral("TWT (ms)")
                : QStringLiteral("深度 (m)");
            p.drawText(QRect(2, 2, m_leftMargin - 4, m_topMargin - 4), Qt::AlignCenter, unitStr);
        }
    }

    // 7. Render Right Color Bar (色标)
    const int colorBarX = width() - m_rightMargin;
    const QRect rightBarRect(colorBarX, 0, m_rightMargin, height());
    p.fillRect(rightBarRect, QColor(QStringLiteral("#F5F7FA")));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawLine(QPoint(colorBarX, 0), QPoint(colorBarX, height()));

    // Title at the top of the color bar
    p.setFont(QFont(QStringLiteral("Noto Sans SC"), 8, QFont::Bold));
    p.setPen(QColor(QStringLiteral("#24303E")));
    p.drawText(QRect(colorBarX, 8, m_rightMargin, 16), Qt::AlignCenter, tr("色标"));

    p.setFont(QFont(QStringLiteral("Noto Sans SC"), 7));
    p.setPen(QColor(QStringLiteral("#5D6E80")));
    p.drawText(QRect(colorBarX, 24, m_rightMargin, 14), Qt::AlignCenter, tr("振幅"));

    if (hasData()) {
        const int barW = 12;
        const int barLeft = colorBarX + 8;
        const int barTop = m_topMargin + 12;
        const int barBottom = height() - 24;
        const int barH = std::max(20, barBottom - barTop);

        // D2.8：色标渐变直接采样当前 LUT（8 预设 + 反转全同步）
        if (m_colorLut.empty())
            rebuildColorLut();
        QLinearGradient grad(barLeft, barTop, barLeft, barBottom);
        for (int i = 0; i <= 8; ++i) {
            const int idx = 255 - i * 255 / 8; // 顶=+Peak
            grad.setColorAt(i / 8.0, QColor(m_colorLut[static_cast<std::size_t>(idx)]));
        }

        const QRectF colorBarRect(barLeft, barTop, barW, barH);
        p.setBrush(grad);
        p.setPen(QPen(QColor(QStringLiteral("#DFE5EC")), 1.0));
        p.drawRoundedRect(colorBarRect, 2.0, 2.0);

        // Labels next to the bar
        p.setFont(QFont(QStringLiteral("JetBrains Mono"), 7));
        p.setPen(QColor(QStringLiteral("#24303E")));

        const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
        const QString maxStr = absMax >= 10000.0f
            ? QStringLiteral("+%1k").arg(absMax / 1000.0f, 0, 'f', 0)
            : (absMax >= 1000.0f ? QStringLiteral("+%1k").arg(absMax / 1000.0f, 0, 'f', 1)
                                 : QStringLiteral("+%1").arg(qRound(absMax)));
        const QString minStr = absMax >= 10000.0f
            ? QStringLiteral("-%1k").arg(absMax / 1000.0f, 0, 'f', 0)
            : (absMax >= 1000.0f ? QStringLiteral("-%1k").arg(absMax / 1000.0f, 0, 'f', 1)
                                 : QStringLiteral("-%1").arg(qRound(absMax)));

        // Top tick (+Max)
        p.drawLine(QPointF(barLeft + barW, barTop), QPointF(barLeft + barW + 3, barTop));
        p.drawText(QRectF(barLeft + barW + 4, barTop - 7, m_rightMargin - barW - 12, 14), Qt::AlignLeft | Qt::AlignVCenter, maxStr);

        // Mid tick (0)
        const double midY = barTop + barH * 0.5;
        p.drawLine(QPointF(barLeft + barW, midY), QPointF(barLeft + barW + 3, midY));
        p.drawText(QRectF(barLeft + barW + 4, midY - 7, m_rightMargin - barW - 12, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));

        // Bottom tick (-Max)
        p.drawLine(QPointF(barLeft + barW, barBottom), QPointF(barLeft + barW + 3, barBottom));
        p.drawText(QRectF(barLeft + barW + 4, barBottom - 7, m_rightMargin - barW - 12, 14), Qt::AlignLeft | Qt::AlignVCenter, minStr);

        // Peak / Trough text annotations
        p.setFont(QFont(QStringLiteral("Noto Sans SC"), 7));
        if (m_colorMap == SectionColorMapType::RedWhiteBlue) {
            p.setPen(QColor(220, 38, 38));
            p.drawText(QRectF(colorBarX, barTop - 13, m_rightMargin - 6, 12), Qt::AlignRight, tr("波峰+"));
            p.setPen(QColor(25, 118, 210));
            p.drawText(QRectF(colorBarX, barBottom + 3, m_rightMargin - 6, 12), Qt::AlignRight, tr("波谷-"));
        }
    }

    // D2.5 双刻度：右缘深度轴（TWT 主轴在左）——时深模型无效时自动隐藏
    if (m_dualScale && m_orientation == SectionOrientation::Vertical && hasData() &&
        m_tdModel.isValid()) {
        const int depthAxisX = vp.right() + 4;
        p.setFont(monoFont);
        const double minTime = pixelToTime(m_topMargin);
        const double maxTime = pixelToTime(height());
        const double minDepth = m_tdModel.TwtMsToDepth(minTime);
        const double maxDepth = m_tdModel.TwtMsToDepth(maxTime);
        const auto depthTicks = NiceStep::GenerateTicks(std::min(minDepth, maxDepth),
                                                         std::max(minDepth, maxDepth),
                                                         m_topMargin, height(), 8, QStringLiteral("%.0f"));
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.drawText(QRect(depthAxisX, m_topMargin - 16, m_rightMargin - 8, 14),
                   Qt::AlignLeft, tr("深度(m)"));
        for (const auto &tk : depthTicks) {
            if (tk.pixelPos < m_topMargin || tk.pixelPos > height())
                continue;
            // 反算该深度对应 TWT 的像素位置（非线性时深时贴准）
            const double twt = m_tdModel.DepthToTwtMs(tk.value);
            const double py = timeToPixelY(twt);
            if (py < m_topMargin || py > height())
                continue;
            p.setPen(QColor(QStringLiteral("#43A047")));
            p.drawLine(QPointF(depthAxisX, py), QPointF(depthAxisX + 4.0, py));
            p.setPen(QColor(QStringLiteral("#5D6E80")));
            p.drawText(QRectF(depthAxisX + 5.0, py - 6.0, 26.0, 12.0), Qt::AlignLeft,
                       QStringLiteral("%1").arg(qRound(tk.value)));
        }
    }}

void SeismicSectionCanvas::resizeEvent(QResizeEvent *) {
    update();
}

// 卷帘分割线像素位置（-1 = 未启用）
double SeismicSectionCanvas::curtainPixelX() const {
    if (!m_compareEnabled)
        return -1.0;
    const QRect vp = viewportRect();
    return vp.left() + m_curtainPos * vp.width();
}

void SeismicSectionCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        // D2.10：靠近卷帘分割线 → 拖帘，否则平移
        const double curtainX = curtainPixelX();
        const bool nearCurtain = curtainX > 0 && std::abs(event->pos().x() - curtainX) <= 7.0;
        m_pressPos = event->pos();
        m_pressMoved = false;
        if (nearCurtain && event->button() == Qt::LeftButton) {
            m_draggingCurtain = true;
        } else {
            m_isPanning = true;
        }
        m_lastMousePos = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void SeismicSectionCanvas::mouseMoveEvent(QMouseEvent *event) {
    m_currentMousePos = event->pos();
    m_hasHover = true;

    if ((event->pos() - m_pressPos).manhattanLength() > 4)
        m_pressMoved = true;

    if (m_draggingCurtain) {
        const QRect vp = viewportRect();
        setCurtainPos(static_cast<double>(event->pos().x() - vp.left()) / std::max(1, vp.width()));
        return;
    }
    if (m_isPanning) {
        const QPoint delta = event->pos() - m_lastMousePos;
        m_panX += delta.x();
        m_panY += delta.y();
        m_lastMousePos = event->pos();
        update();
    } else {
        // 帘附近给 Split 光标提示可拖
        const double curtainX = curtainPixelX();
        if (curtainX > 0 && std::abs(event->pos().x() - curtainX) <= 7.0)
            setCursor(Qt::SplitHCursor);
        else
            setCursor(Qt::CrossCursor);
        updateHoverInfo(event->pos());
        update();
    }
}

void SeismicSectionCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_isPanning = false;
        m_draggingCurtain = false;
        setCursor(Qt::CrossCursor);
        // D2.11：左键原地点击（未拖动）→ 道拾取事件（道头卡/解释拾取入口）
        if (event->button() == Qt::LeftButton && !m_pressMoved && hasData() &&
            viewportRect().contains(event->pos())) {
            const QPoint pos = event->pos();
            if (m_orientation == SectionOrientation::TimeSlice) {
                const double trace = pixelToTrace(pos.x());
                const int colIdx = std::clamp(static_cast<int>(std::round(trace)), 0, m_traces - 1);
                const double xlVal = m_xlineMin + (static_cast<double>(colIdx) / std::max(1, m_traces - 1)) * (m_xlineMax - m_xlineMin);
                const double depth = m_tdModel.TwtMsToDepth(m_currentTimeMs);
                emit traceClicked(colIdx, m_currentTimeMs, depth,
                                  m_slice.Value(colIdx, 0), xlVal, pixelToInline(pos.y()));
            } else {
                const double twt = pixelToTime(pos.y());
                const int traceIdx = std::clamp(static_cast<int>(std::round(pixelToTrace(pos.x()))), 0, m_traces - 1);
                const int sampleIdx = std::clamp(static_cast<int>(std::round((twt - m_t0Ms) / m_dtMs)), 0, m_samples - 1);
                double mapX = 0.0, mapY = 0.0;
                if (traceIdx < static_cast<int>(m_mapCoords.size())) {
                    mapX = m_mapCoords[traceIdx].x;
                    mapY = m_mapCoords[traceIdx].y;
                }
                emit traceClicked(traceIdx, twt, m_tdModel.TwtMsToDepth(twt),
                                  m_slice.Value(traceIdx, sampleIdx), mapX, mapY);
            }
        }
    }
}

void SeismicSectionCanvas::wheelEvent(QWheelEvent *event) {
    const double delta = event->angleDelta().y();
    if (std::abs(delta) < 1.0)
        return;

    const double zoomFactor = std::pow(1.15, delta / 120.0);
    const QPointF anchor = event->position();

    const double anchorTrace = pixelToTrace(anchor.x());
    const double anchorY = (m_orientation == SectionOrientation::TimeSlice)
        ? pixelToInline(anchor.y())
        : pixelToTime(anchor.y());

    m_zoomX = std::clamp(m_zoomX * zoomFactor, 0.12, 64.0);
    m_zoomY = std::clamp(m_zoomY * zoomFactor, 0.12, 64.0);

    m_panX = anchor.x() - m_leftMargin - anchorTrace * m_zoomX;
    if (m_orientation == SectionOrientation::TimeSlice) {
        const double rowIdx = (static_cast<double>(m_inlineMax) - anchorY) / std::max(1, m_inlineMax - m_inlineMin) * std::max(1, m_samples - 1);
        m_panY = anchor.y() - m_topMargin - rowIdx * m_zoomY;
    } else {
        m_panY = anchor.y() - m_topMargin - ((anchorY - m_t0Ms) / m_dtMs) * m_zoomY;
    }

    emit zoomChanged(m_zoomX);
    updateHoverInfo(event->position().toPoint());
    update();
}

void SeismicSectionCanvas::mouseDoubleClickEvent(QMouseEvent *) {
    fitToWindow();
}

void SeismicSectionCanvas::leaveEvent(QEvent *) {
    m_hasHover = false;
    update();
}

void SeismicSectionCanvas::updateHoverInfo(const QPoint &pos) {
    if (!hasData())
        return;

    if (m_orientation == SectionOrientation::TimeSlice) {
        const double trace = pixelToTrace(pos.x());
        const double ilVal = pixelToInline(pos.y());
        const int colIdx = std::clamp(static_cast<int>(std::round(trace)), 0, m_traces - 1);
        const double spanIl = std::max(1, m_inlineMax - m_inlineMin);
        const int rowIdx = std::clamp(static_cast<int>(std::round((static_cast<double>(m_inlineMax) - ilVal) / spanIl * std::max(1, m_samples - 1))), 0, m_samples - 1);

        const float amp = (colIdx >= 0 && colIdx < m_traces && rowIdx >= 0 && rowIdx < m_samples)
            ? m_slice.Value(colIdx, rowIdx)
            : 0.0f;

        const double xlVal = m_xlineMin + (static_cast<double>(colIdx) / std::max(1, m_traces - 1)) * (m_xlineMax - m_xlineMin);
        const double depth = m_tdModel.TwtMsToDepth(m_currentTimeMs);

        emit traceHovered(colIdx, m_currentTimeMs, depth, amp, xlVal, ilVal);
        return;
    }

    const double trace = pixelToTrace(pos.x());
    const double twt = pixelToTime(pos.y());
    const int traceIdx = std::clamp(static_cast<int>(std::round(trace)), 0, m_traces - 1);
    const int sampleIdx = std::clamp(static_cast<int>(std::round((twt - m_t0Ms) / m_dtMs)), 0, m_samples - 1);

    const float amp = (traceIdx >= 0 && traceIdx < m_traces && sampleIdx >= 0 && sampleIdx < m_samples)
        ? m_slice.Value(traceIdx, sampleIdx)
        : 0.0f;

    const double depth = m_tdModel.TwtMsToDepth(twt);

    double mapX = 0.0, mapY = 0.0;
    if (traceIdx < static_cast<int>(m_mapCoords.size())) {
        mapX = m_mapCoords[traceIdx].x;
        mapY = m_mapCoords[traceIdx].y;
    }

    emit traceHovered(traceIdx, twt, depth, amp, mapX, mapY);
}

} // namespace seismic
