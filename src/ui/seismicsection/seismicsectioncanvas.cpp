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
    m_slice = image;
    m_traces = image.width;
    m_samples = image.height;
    m_dtMs = sampleIntervalMs > 0.001f ? sampleIntervalMs : 2.0f;
    m_t0Ms = startSampleMs;
    m_columnDistances = columnDistancesM;
    m_mapCoords = mapCoords;

    if (m_columnDistances.size() != static_cast<std::size_t>(m_traces)) {
        m_columnDistances.resize(m_traces);
        for (int i = 0; i < m_traces; ++i) {
            m_columnDistances[i] = static_cast<float>(i * 25.0); // Default 25m trace spacing
        }
    }

    rebuildImage();
    fitToWindow();
    update();
}

void SeismicSectionCanvas::clearData() {
    m_slice = {};
    m_traces = 0;
    m_samples = 0;
    m_columnDistances.clear();
    m_mapCoords.clear();
    m_cachedImage = QImage();
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
        rebuildImage();
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
    m_zoomY = static_cast<double>(vp.height()) / static_cast<double>(m_samples);
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

QRect SeismicSectionCanvas::viewportRect() const {
    return QRect(m_leftMargin, m_topMargin,
                 std::max(1, width() - m_leftMargin),
                 std::max(1, height() - m_topMargin));
}

void SeismicSectionCanvas::rebuildImage() {
    if (m_traces <= 0 || m_samples <= 0 || m_slice.values.empty()) {
        m_cachedImage = QImage();
        return;
    }

    const int w = m_traces;
    const int h = m_samples;
    m_cachedImage = QImage(w, h, QImage::Format_ARGB32_Premultiplied);

    const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
    const float baseScale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;
    const float effectiveScale = baseScale * m_gain;

    for (int y = 0; y < h; ++y) {
        auto *scanLine = reinterpret_cast<QRgb *>(m_cachedImage.scanLine(y));
        const int rowOffset = y * w;
        for (int x = 0; x < w; ++x) {
            const float raw = m_slice.values[rowOffset + x];
            if (!std::isfinite(raw)) {
                // NaN: Dark neutral gray per DESIGN.md (#303131)
                scanLine[x] = qRgba(48, 49, 49, 255);
                continue;
            }

            float val = std::clamp(raw * effectiveScale * m_contrast, -1.0f, 1.0f);

            if (m_colorMap == SectionColorMapType::RedWhiteBlue) {
                // Peak (val > 0) -> Red; Trough (val < 0) -> Blue; Zero -> Pure white
                const float mag = std::pow(std::abs(val), 0.85f);
                const float k = 1.0f - mag;
                if (val < 0.0f) {
                    // Deep blue to white
                    scanLine[x] = qRgba(ToByte(0.85f * k), ToByte(0.90f * k), 255, 255);
                } else {
                    // White to deep red
                    scanLine[x] = qRgba(255, ToByte(0.88f * k), ToByte(0.84f * k), 255);
                }
            } else if (m_colorMap == SectionColorMapType::Grayscale) {
                // Standard seismic variable density
                const float norm = (val + 1.0f) * 0.5f;
                const unsigned char g = ToByte(norm);
                scanLine[x] = qRgba(g, g, g, 255);
            } else {
                // Rainbow Spectrum (Deep Blue -> Cyan -> Green -> Yellow -> Red)
                const float t = std::clamp((val + 1.0f) * 0.5f, 0.0f, 1.0f);
                float r = 0.0f, g = 0.0f, b = 0.0f;
                if (t < 0.25f) {
                    const float f = t / 0.25f;
                    b = 1.0f; g = f;
                } else if (t < 0.5f) {
                    const float f = (t - 0.25f) / 0.25f;
                    g = 1.0f; b = 1.0f - f;
                } else if (t < 0.75f) {
                    const float f = (t - 0.5f) / 0.25f;
                    g = 1.0f; r = f;
                } else {
                    const float f = (t - 0.75f) / 0.25f;
                    r = 1.0f; g = 1.0f - f;
                }
                scanLine[x] = qRgba(ToByte(r), ToByte(g), ToByte(b), 255);
            }
        }
    }
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

    // 2. Render seismic image inside viewport
    p.save();
    p.setClipRect(vp);

    if (hasData() && !m_cachedImage.isNull()) {
        const double x0 = traceToPixelX(0.0);
        const double y0 = timeToPixelY(m_t0Ms);
        const double x1 = traceToPixelX(static_cast<double>(m_traces));
        const double y1 = timeToPixelY(m_t0Ms + static_cast<double>(m_samples) * m_dtMs);
        const QRectF imgDest(x0, y0, x1 - x0, y1 - y0);

        p.drawImage(imgDest, m_cachedImage);
    } else {
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.setFont(bodyFont);
        p.drawText(vp, Qt::AlignCenter, tr("未加载地震剖面数据\n（支持拖拽测线或从地图生成连井/任意剖面）"));
    }

    // 3. Render Wellbores, Tops, and Curves inside viewport
    if (m_showWells && !m_wells.empty() && hasData()) {
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
    p.drawLine(QPoint(m_leftMargin, m_topMargin), QPoint(width(), m_topMargin));

    if (hasData()) {
        const double minTrace = pixelToTrace(m_leftMargin);
        const double maxTrace = pixelToTrace(width());

        // Distance ticks
        const double totalDist = totalDistanceM();
        const double traceToDistFactor = m_traces > 1 ? totalDist / static_cast<double>(m_traces - 1) : 25.0;
        const double minDistM = minTrace * traceToDistFactor;
        const double maxDistM = maxTrace * traceToDistFactor;

        const auto ticks = NiceStep::GenerateTicks(minDistM, maxDistM, m_leftMargin, width(), 8, QStringLiteral("%.0f"));

        p.setFont(monoFont);
        for (const auto &tk : ticks) {
            if (tk.pixelPos < m_leftMargin || tk.pixelPos > width())
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
                if (wx < m_leftMargin || wx > width())
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

    // 6. Render Left Vertical Ruler (TWT ms or Depth m)
    const QRect leftRulerRect(0, m_topMargin, m_leftMargin, vp.height());
    p.fillRect(leftRulerRect, QColor(QStringLiteral("#F5F7FA")));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawLine(QPoint(m_leftMargin, m_topMargin), QPoint(m_leftMargin, height()));

    if (hasData()) {
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

void SeismicSectionCanvas::resizeEvent(QResizeEvent *) {
    update();
}

void SeismicSectionCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_isPanning = true;
        m_lastMousePos = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void SeismicSectionCanvas::mouseMoveEvent(QMouseEvent *event) {
    m_currentMousePos = event->pos();
    m_hasHover = true;

    if (m_isPanning) {
        const QPoint delta = event->pos() - m_lastMousePos;
        m_panX += delta.x();
        m_panY += delta.y();
        m_lastMousePos = event->pos();
        update();
    } else {
        updateHoverInfo(event->pos());
        update();
    }
}

void SeismicSectionCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_isPanning = false;
        setCursor(Qt::CrossCursor);
    }
}

void SeismicSectionCanvas::wheelEvent(QWheelEvent *event) {
    const double delta = event->angleDelta().y();
    if (std::abs(delta) < 1.0)
        return;

    const double zoomFactor = std::pow(1.15, delta / 120.0);
    const QPointF anchor = event->position();

    const double anchorTrace = pixelToTrace(anchor.x());
    const double anchorTime = pixelToTime(anchor.y());

    m_zoomX = std::clamp(m_zoomX * zoomFactor, 0.12, 64.0);
    m_zoomY = std::clamp(m_zoomY * zoomFactor, 0.12, 64.0);

    m_panX = anchor.x() - m_leftMargin - anchorTrace * m_zoomX;
    m_panY = anchor.y() - m_topMargin - ((anchorTime - m_t0Ms) / m_dtMs) * m_zoomY;

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
