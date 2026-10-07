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
    setMinimumSize(400, 300);
    setToolTip(tr("左键拖拽平移；滚轮平移（Shift+滚轮横向）；Ctrl+滚轮缩放；双击适应窗口\n"
                  "方向键平移；+/- 缩放；PgUp/PgDn 步进切片"));
}

bool SeismicSectionCanvas::event(QEvent *event) {
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
    m_slice = image;
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
    if (hasZoneOverlay() &&
        (image.width != m_zoneOverlay.width || image.height != m_zoneOverlay.height))
        clearZoneOverlay();

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
        update();
    }
}


} // namespace seismic
