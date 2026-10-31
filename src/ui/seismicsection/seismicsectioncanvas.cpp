// 层：视图
// 方向 65：画布主体 TU——构造 + 事件入口（主题切换重绘 / 键盘导航）+ 数据入口
// （剖面 / 时间切片 / 瓦片渐进流 / 井 / 时深 / 色标）。
// D2.1 纹理缓存键的内容指纹见 seismicsectioncanvas_internal.h（键在本 TU 的
// setSectionData 与渲染核 TU 的 rebuildImage 两处组装，语义注释随指纹走）。
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "domain/seismic/sectionaxis.h"
#include "ui/seismicsection/seismicsectioncanvas_internal.h"

#include <QKeyEvent>
#include <QPaintEvent>
#include <QHelpEvent>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

SeismicSectionCanvas::SeismicSectionCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(200, 150);  // #846：原 400x300 直接成为 dock 硬下限
    setToolTip(tr("左键拖拽平移；滚轮平移（Shift+滚轮横向）；Ctrl+滚轮缩放；双击适应窗口\n"
                  "方向键平移；+/- 缩放；PgUp/PgDn 步进切片"));
}

bool SeismicSectionCanvas::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        for (const auto &label : wellPinLabels()) {
            if (!label.badge.contains(help->pos())) continue;
            const auto &well = m_wells[label.wellIndex];
            QString detail = tr("井名：%1\n偏移距离：%2 m").arg(label.name).arg(well.offsetDistanceM, 0, 'f', 1);
            if (!well.alignmentStatus.isEmpty()) detail += '\n' + well.alignmentStatus;
            QToolTip::showText(help->globalPos(), QStringLiteral("<qt>%1</qt>")
                .arg(detail.toHtmlEscaped().replace('\n', QStringLiteral("<br>"))), this, label.badge.toAlignedRect());
            return true;
        }
    }
    // 主题切换（palette 风暴）时重绘：ruler/角标/文字等 chrome 色在 paint 里
    // 现取 tokens()，这里只负责触发重绘。
    if (event->type() == QEvent::ApplicationPaletteChange) {
        update();
    }
    return QWidget::event(event);
}

void SeismicSectionCanvas::keyPressEvent(QKeyEvent *event) {
    constexpr double kPanStepPx = 32.0;
    switch (event->key()) {
    case Qt::Key_Left:
        m_panX += kPanStepPx;
        break;
    case Qt::Key_Right:
        m_panX -= kPanStepPx;
        break;
    case Qt::Key_Up:
        m_panY += kPanStepPx;
        break;
    case Qt::Key_Down:
        m_panY -= kPanStepPx;
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        event->accept();
        return;
    case Qt::Key_Minus:
        zoomOut();
        event->accept();
        return;
    case Qt::Key_PageUp:
        emit sliceStepRequested(-1);
        event->accept();
        return;
    case Qt::Key_PageDown:
        emit sliceStepRequested(+1);
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    update();
    event->accept();
}

void SeismicSectionCanvas::setSectionData(
    const SgySliceImage &image,
    float sampleIntervalMs,
    double startSampleMs,
    const std::vector<float> &columnDistancesM,
    const std::vector<glm::dvec2> &mapCoords)
{
    m_orientation = SectionOrientation::Vertical;
    // 入图行序转换：vendor 行 0=最深 → 画布行 y=采样 y 升序（标尺/hover/
    // 拾取/井分层全链升序；契约见 seismicsectioncanvas_internal.h）。
    m_slice = ToAscendingSampleRows(image);
    m_traces = image.width;
    m_samples = image.height;
    m_dtMs = sampleIntervalMs > 0.001f ? sampleIntervalMs : 2.0f;
    m_t0Ms = startSampleMs;
    m_columnDistances = columnDistancesM;
    m_mapCoords = mapCoords;
    m_noDataReason.clear(); // 有数据即清原因态
    if (hasAttrOverlay() &&
        (image.width != m_attrOverlay.width || image.height != m_attrOverlay.height))
        clearAttrOverlay(); // 换剖面：旧属性层几何失配，防错位
    if (hasZoneOverlay() &&
        (image.width != m_zoneOverlay.width || image.height != m_zoneOverlay.height))
        clearZoneOverlay();

    if (m_columnDistances.size() != static_cast<std::size_t>(m_traces)) {
        m_columnDistances.resize(m_traces);
        for (int i = 0; i < m_traces; ++i) {
            m_columnDistances[i] = static_cast<float>(i * 25.0); // Default 25m trace spacing
        }
    }

    rebuildDisplayValues();

    // D2.1 切片纹理缓存：(内容指纹, 增益, colormap, 反转, 阈值, 极性, AGC) 键控
    // ——同参数下切回已看过的线即出（不重算逐像素上色）
    const qint64 key = TextureCacheKey(m_slice, m_traces, m_samples, m_t0Ms, m_dtMs, m_gain,
                                       m_contrast, static_cast<int>(m_colorMap), m_cmapInverted,
                                       m_threshold, m_polarityInverted,
                                       m_agcEnabled ? m_agcWindowMs : 0,
                                       GainCurveFingerprint(m_gainCurve)); // PW-096
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
    if (hasZoneOverlay() &&
        (image.width != m_zoneOverlay.width || image.height != m_zoneOverlay.height))
        clearZoneOverlay();

    // #286：显示缓冲（AGC/增益曲线/极性）随切片内容重建——旧实现沿用
    // 上一条剖面的缓冲，按新网格索引会显示旧数据甚至越界读。
    rebuildDisplayValues();
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
    // PW-093：不再伪造 [0,1] 种子（会把 absMax 钉在 ≥1，小幅值数据整片灰）。
    m_slice.valueMin = 0.0f;
    m_slice.valueMax = 0.0f;
    m_tileRangeEmpty = true;
    m_tileScaleAbsMax = 0.0f;

    // #286：与 setTimeSliceData 同口径——瓦片流先建全网格显示缓冲，
    // 瓦片到达时在 appendTimeSliceTile 里同步改写对应区域。
    rebuildDisplayValues();
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
            if (m_tileRangeEmpty) {
                m_slice.valueMin = m_slice.valueMax = v; // PW-093：首值落种
                m_tileRangeEmpty = false;
            } else {
                m_slice.valueMin = std::min(m_slice.valueMin, v);
                m_slice.valueMax = std::max(m_slice.valueMax, v);
            }
        }
    }

    // #286：显示缓冲与 m_slice 逐位同步——时间切片方向 AGC/增益曲线
    // 已关（见 rebuildDisplayValues），显示值 = 极性处理后的原始值。
    if (m_displayValues.size() ==
        static_cast<std::size_t>(m_traces) * static_cast<std::size_t>(m_samples)) {
        for (int ty = 0; ty < tile.height; ++ty) {
            const std::size_t srcRow = static_cast<std::size_t>(ty) * tile.width;
            const std::size_t dstRow = static_cast<std::size_t>(y + ty) * m_traces + x;
            for (int tx = 0; tx < tile.width; ++tx) {
                float v = tile.values[srcRow + tx];
                if (m_polarityInverted && std::isfinite(v))
                    v = -v;
                m_displayValues[dstRow + tx] = v;
            }
        }
    } else {
        m_displayValues.clear(); // 尺寸失配兜底：直通原始值
    }

    // 数据变了——LOD 缓存（zoomY<1 缩小态用的缩略图）随瓦片失效，
    // 否则新 tile 在键控参数变化/整体重建前永不上屏。
    m_lodStride = 0;
    // PW-093：absMax 相对上次整图上色变化超 5% → 整图按新尺度重绘（时间切片
    // 仅 IL×XL，代价小），否则只上色新瓦片——消除瓦片间色标接缝。
    const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
    const float prev = m_tileScaleAbsMax;
    if (prev <= 0.0f || std::abs(absMax - prev) > 0.05f * prev) {
        m_tileScaleAbsMax = absMax;
        paintValueRegion(0, 0, m_traces, m_samples);
    } else {
        paintValueRegion(x, y, tile.width, tile.height);
    }
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
    // #286：整体替换同样重建显示缓冲（setTimeSliceData 同口径）。
    rebuildDisplayValues();
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
    clearZoneOverlay();
    clearAttrOverlay(); // #224：换体/清剖面时属性叠加随数据作废（同尺寸新体不得沿用）
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
      if (w.calibrated)
        continue;
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
        rebuildAttrImage();
        if (!m_zoneOverlay.values.empty())
            rebuildZoneImage();
        // 卷帘 B 图用当前 LUT 一次性烤死（setCompareData 内），换色标不重烘
        // 会让帘右保留旧色标——对比恰是换色标的主要场景（setGain 同款）。
        if (m_compareEnabled && !m_compareSlice.values.empty())
            setCompareData(m_compareSlice, m_compareLabel);
        update();
    }
}


} // namespace seismic
