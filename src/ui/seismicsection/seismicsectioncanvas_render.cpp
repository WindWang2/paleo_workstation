// 层：视图
// 方向 65：画布渲染与缓存核 TU——上色路径、D2.8 色标 LUT、属性/层段叠加烘焙、
// D2.6 LOD 抽稀、D2.2 wiggle 变面积。
// 注：D2.1 纹理缓存键的内容指纹来自 seismicsectioncanvas_internal.h（键在主体
// TU 的 setSectionData 与本 TU 的 rebuildImage 两处组装，语义注释随指纹走）。
// token 例外：DESIGN 数据符号例外：地震振幅密度/wiggle 图像、拾取置信度/断层/井曲线及图像上交互标记，保持地震数据视觉映射。（tools/ui-token-exceptions.json 精确计数）。
#include <QPainterPath>
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "../seismic3d/seismic3d_internal.h"
#include "ui/seismicsection/seismicsectioncanvas_internal.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace seismic {

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
    const qint64 key = TextureCacheKey(m_slice, m_traces, m_samples, m_t0Ms, m_dtMs, m_gain,
                                       m_contrast, static_cast<int>(m_colorMap), m_cmapInverted,
                                       m_threshold, m_polarityInverted,
                                       m_agcEnabled ? m_agcWindowMs : 0,
                                       GainCurveFingerprint(m_gainCurve)); // PW-096
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
    // #286：显示缓冲只在与当前网格逐位一致时生效——失配直通原始值，
    // 防换网格后的越界读（上游入口已负责重建，这里是最后一道兜底）。
    if (m_displayValues.size() ==
        static_cast<std::size_t>(m_traces) * static_cast<std::size_t>(m_samples))
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

void SeismicSectionCanvas::setAttrOverlay(const SgySliceImage &attr) {
    // 几何必须与当前剖面逐位一致（服务保证同切片提取布局）
    if (attr.width != m_traces || attr.height != m_samples || !hasData()) {
        qWarning("setAttrOverlay: 尺寸不匹配（attr %dx%d vs 剖面 %dx%d），忽略",
                 attr.width, attr.height, m_traces, m_samples);
        return;
    }
    m_attrOverlay = attr;
    rebuildAttrImage();
    update();
}

void SeismicSectionCanvas::setAttrOverlayAlpha(double alpha) {
    const double clamped = std::clamp(alpha, 0.0, 1.0);
    if (std::abs(clamped - m_attrAlpha) < 1e-9)
        return;
    m_attrAlpha = clamped;
    update();
}

void SeismicSectionCanvas::clearAttrOverlay() {
    if (m_attrImage.isNull() && m_attrOverlay.values.empty())
        return;
    m_attrOverlay = SgySliceImage{};
    m_attrImage = QImage();
    update();
}

// 烘焙叠加层 → RGBA（NaN=透明）。两种入图行序契约不同（S5 review 修正）：
//  - attr 属性叠加：服务层扫描输出 vendor 行序（行 0=最深样）→ flipRows=true
//    翻转对齐画布升序空间；
//  - zone 层段叠加：属性建模 IJK 投影（stratgrid ZoneGrid）k=0 是最浅层
//    → 本就是升序，直通不翻（翻转会把相带垂直镜像）。
QImage SeismicSectionCanvas::bakeOverlay(const SgySliceImage &src, bool flipRows) {
    if (src.values.empty() || src.width <= 0 || src.height <= 0)
        return {};
    if (m_colorLut.empty())
        rebuildColorLut();
    const SgySliceImage img = flipRows ? ToAscendingSampleRows(src) : src;
    QImage image(img.width, img.height, QImage::Format_ARGB32_Premultiplied);
    const double lo = img.valueMin;
    const double hi = img.valueMax;
    const double span = (hi > lo) ? (hi - lo) : 1.0; // 退化值域：中档色
    for (int y = 0; y < img.height; ++y) {
        auto *scan = reinterpret_cast<QRgb *>(image.scanLine(y));
        const float *row = img.values.data() + std::size_t(y) * img.width;
        for (int x = 0; x < img.width; ++x) {
            const float v = row[x];
            if (std::isnan(v)) {
                scan[x] = 0; // 缺失=透明（不盖底图）
                continue;
            }
            int idx = int((double(v) - lo) / span * 255.0 + 0.5);
            idx = std::clamp(idx, 0, 255);
            scan[x] = m_colorLut[std::size_t(idx)];
        }
    }
    return image;
}

void SeismicSectionCanvas::rebuildAttrImage() {
    m_attrImage = bakeOverlay(m_attrOverlay, /*flipRows=*/true);
}

void SeismicSectionCanvas::setZoneOverlay(const SgySliceImage &zone) {
    if (zone.width != m_traces || zone.height != m_samples || !hasData()) {
        qWarning("setZoneOverlay: 尺寸不匹配（zone %dx%d vs 剖面 %dx%d），忽略",
                 zone.width, zone.height, m_traces, m_samples);
        return;
    }
    m_zoneOverlay = zone;
    rebuildZoneImage();
    update();
}

void SeismicSectionCanvas::setZoneOverlayAlpha(double alpha) {
    const double clamped = std::clamp(alpha, 0.0, 1.0);
    if (std::abs(clamped - m_zoneAlpha) < 1e-9)
        return;
    m_zoneAlpha = clamped;
    update();
}

void SeismicSectionCanvas::clearZoneOverlay() {
    if (m_zoneImage.isNull() && m_zoneOverlay.values.empty())
        return;
    m_zoneOverlay = SgySliceImage{};
    m_zoneImage = QImage();
    update();
}

void SeismicSectionCanvas::rebuildZoneImage() {
    // zone（stratgrid IJK 投影）k=0=最浅层，本就升序直通（见 bakeOverlay 注释）
    m_zoneImage = bakeOverlay(m_zoneOverlay, /*flipRows=*/false);
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
    // #286：显示缓冲与当前网格逐位一致才启用（失配直通原始值，防越界读）
    const bool hasDisplay = m_displayValues.size() ==
        static_cast<std::size_t>(m_traces) * static_cast<std::size_t>(m_samples);

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

// D2.6 LOD 抽稀：zoom<1（多样本/道 每像素）时按步长取块内最大绝对值（保幅保事件）。
// PW-095：X/Y 双向抽稀（长测线缩小时不再整幅缩放全分辨率图）；步长量化到
// 2 的幂——滚轮每档不再触发整幅重建，只在跨 2× 边界时重算。
namespace {
int quantizedStride(double zoom) {
    const int raw = std::clamp(int(1.0 / std::max(1e-6, zoom)), 1, 64);
    int s = 1;
    while (s * 2 <= raw)
        s *= 2;
    return s;
}
} // namespace

bool SeismicSectionCanvas::needsLod() const {
    return quantizedStride(m_zoomX) > 1 || quantizedStride(m_zoomY) > 1;
}

const QImage &SeismicSectionCanvas::lodImage() const {
    const int strideX = quantizedStride(m_zoomX);
    const int strideY = quantizedStride(m_zoomY);
    if (strideX <= 1 && strideY <= 1) {
        m_lodStride = 0;
        return m_cachedImage;
    }
    const int key = strideX * 1000 + strideY;
    if (m_lodStride == key && !m_lodCache.isNull())
        return m_lodCache;

    const int outRows = (m_samples + strideY - 1) / strideY;
    const int outCols = (m_traces + strideX - 1) / strideX;
    m_lodCache = QImage(outCols, outRows, QImage::Format_ARGB32_Premultiplied);
    const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
    const float baseScale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;
    // #286：同 paintValueRegion——显示缓冲失配时直通原始值，防越界读
    const bool hasDisplay = m_displayValues.size() ==
        static_cast<std::size_t>(m_traces) * static_cast<std::size_t>(m_samples);
    for (int r = 0; r < outRows; ++r) {
        auto *scan = reinterpret_cast<QRgb *>(m_lodCache.scanLine(r));
        const int yBegin = r * strideY;
        const int yEnd = std::min(m_samples, yBegin + strideY);
        for (int c = 0; c < outCols; ++c) {
            const int xBegin = c * strideX;
            const int xEnd = std::min(m_traces, xBegin + strideX);
            float best = std::numeric_limits<float>::quiet_NaN();
            float bestAbs = -1.0f;
            for (int y = yBegin; y < yEnd; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * m_traces;
                for (int x = xBegin; x < xEnd; ++x) {
                    const float raw = hasDisplay ? m_displayValues[row + x] : m_slice.values[row + x];
                    if (!std::isfinite(raw))
                        continue;
                    const float a = std::abs(raw);
                    if (a > bestAbs) {
                        bestAbs = a;
                        best = raw;
                    }
                }
            }
            if (!std::isfinite(best)) {
                scan[c] = qRgba(48, 49, 49, 255);
                continue;
            }
            float val = std::clamp(best * baseScale * m_gain * m_contrast, -1.0f, 1.0f);
            if (m_threshold > 0.0f && std::abs(val) < m_threshold)
                val = 0.0f;
            scan[c] = m_colorLut[static_cast<std::size_t>(
                std::clamp(int((val + 1.0f) * 127.5f), 0, 255))];
        }
    }
    m_lodStride = key;
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

    // #482：纵向与横向同法裁剪——视口上下缘反算可见样本区间，循环域收窄到
    // 该区间（旧实现 continue 只跳过不可见样本却仍迭代全道：窄纵窗下
    // O(可见道 × 总样本)）。反算沿用同一 timeToPixelY 映射的逆
    // （pixelToTime），±1 样本+10px 边距与原先 continue 的判据一致；
    // 变换退化（dt/zoom 非正）时保持全量，语义不倒退。
    int yBegin = 0;
    int yEnd = m_samples - 1;
    if (m_dtMs > 1e-6f && std::abs(m_zoomY) > 1e-6) {
        const double sTop = (pixelToTime(vp.top() - 10) - m_t0Ms) / m_dtMs;
        const double sBot = (pixelToTime(vp.bottom() + 10) - m_t0Ms) / m_dtMs;
        const double lo = std::floor(std::min(sTop, sBot)) - 1.0;
        const double hi = std::ceil(std::max(sTop, sBot)) + 1.0;
        if (hi < 0.0 || lo > static_cast<double>(m_samples - 1)) {
            yBegin = 1; // 视口整体在数据范围外：空循环（无可见样本可画）
            yEnd = 0;
        } else {
            yBegin = std::clamp(static_cast<int>(lo), 0, m_samples - 1);
            yEnd = std::clamp(static_cast<int>(hi), 0, m_samples - 1);
        }
    }

    QPen wavePen(QColor(30, 38, 48), 1.0);
    QBrush fillBrush(QColor(24, 30, 40));
    for (int t = t0; t <= t1; ++t) {
        const double cx = traceToPixelX(t);
        if (cx < vp.left() - halfW - 8 || cx > vp.right() + halfW + 8)
            continue;

        // PW-097：每个正半周瓣独立成多边形，在中线封口——道尾/可视窗尾/NaN
        // 缺口处也封口，不再由 drawPolygon 自动回连首瓣起点画出伪三角，
        // 也不跨缺口桥接；波形线同样在 NaN 处断笔。
        QVector<QPolygonF> waves(1);
        QPainterPath fill;
        QPolygonF lobe;
        double lastPy = 0.0;
        const auto closeLobe = [&] {
            if (lobe.size() >= 2) {
                lobe.append(QPointF(cx, lastPy));
                fill.addPolygon(lobe);
                fill.closeSubpath();
            }
            lobe.clear();
        };
        const auto breakWave = [&] {
            closeLobe();
            if (!waves.last().isEmpty())
                waves.append(QPolygonF());
        };
        for (int y = yBegin; y <= yEnd; y += step) {
            const float raw = displayValue(t, y);
            if (!std::isfinite(raw)) {
                breakWave();
                continue;
            }
            const double py = timeToPixelY(m_t0Ms + static_cast<double>(y) * m_dtMs);
            if (py < vp.top() - 10 || py > vp.bottom() + 10) {
                breakWave();
                continue;
            }
            const double amp = std::clamp(static_cast<double>(raw) * baseScale * m_gain * m_contrast, -1.0, 1.0);
            const double px = cx + amp * halfW;
            waves.last().append(QPointF(px, py));
            if (amp > 0.0) {
                if (lobe.isEmpty())
                    lobe.append(QPointF(cx, py)); // 从中线起笔
                lobe.append(QPointF(px, py));
            } else if (!lobe.isEmpty()) {
                lobe.append(QPointF(cx, py)); // 回到中线封口
                fill.addPolygon(lobe);
                fill.closeSubpath();
                lobe.clear();
            }
            lastPy = py;
        }
        closeLobe(); // 道尾/窗尾仍在正瓣内：中线封口
        int wavePts = 0;
        for (const QPolygonF &w : waves)
            wavePts = std::max(wavePts, int(w.size()));
        if (wavePts >= 2) {
            // 中线（道位置参考线）
            p.setPen(QPen(QColor(223, 229, 236), 1.0));
            p.drawLine(QPointF(cx, vp.top()), QPointF(cx, vp.bottom()));
            if (!fill.isEmpty()) {
                p.setPen(Qt::NoPen);
                p.setBrush(fillBrush);
                p.drawPath(fill);
            }
            p.setPen(wavePen);
            p.setBrush(Qt::NoBrush);
            for (const QPolygonF &w : waves)
                if (w.size() >= 2)
                    p.drawPolyline(w);
        }
    }
    p.restore();
}

} // namespace seismic
