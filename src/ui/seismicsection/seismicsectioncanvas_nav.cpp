// 层：视图
// 方向 65：画布导航与坐标变换 TU——D2.10 卷帘、缩放/平移、D2.9 导出抓图、
// 像素↔道号/时间/线号换算与视口矩形。
#include "ui/seismicsection/seismicsectioncanvas.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

// ---- D2.10 卷帘对比 ----
void SeismicSectionCanvas::setCompareData(const SgySliceImage &image, const QString &label) {
    m_compareSlice = image;
    m_compareLabel = label;
    m_compareImage = QImage();
    if (m_compareEnabled && image.width > 0 && image.height > 0 &&
        !image.values.empty()) {
        if (m_colorLut.empty())
            rebuildColorLut();
        // B 图按当前显示参数上色（复用 LUT 路径；字节级写 RGBA）
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

} // namespace seismic
