// 层：视图
// 方向 65：画布交互控制 TU——鼠标按下/移动/释放（平移、拾取、断层拖拽、卷帘
// 拖动）、滚轮缩放、双击适应窗口、leave 与 hover 读数上报。
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "domain/seismic/sectionaxis.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

void SeismicSectionCanvas::mousePressEvent(QMouseEvent *event) {
    // D4.1：种子拾取模式——点击放点（列号+TWT 交由 dock 解析测线号）
    if (m_pickMode == SectionPickMode::Seed && event->button() == Qt::LeftButton &&
        viewportRect().contains(event->pos()) && hasData()) {
        const double twt = (m_orientation == SectionOrientation::TimeSlice)
            ? m_currentTimeMs : pixelToTime(event->pos().y());
        const int col = std::clamp(static_cast<int>(std::round(pixelToTrace(event->pos().x()))), 0, m_traces - 1);
        emit pickPlaced(col, twt);
        return;
    }
    // D4.4：断层模式——按下开始画折线
    if (m_pickMode == SectionPickMode::Fault && event->button() == Qt::LeftButton &&
        viewportRect().contains(event->pos()) && hasData()) {
        m_faultDraft.clear();
        m_faultDraft.append({pixelToTrace(event->pos().x()), pixelToTime(event->pos().y())});
        m_isPanning = false;
        return;
    }
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
        m_pressPos = event->pos();
        m_dragged = false;
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
    // D4.4：断层折线跟踪
    if (m_pickMode == SectionPickMode::Fault && !m_faultDraft.isEmpty()) {
        m_faultDraft.append({pixelToTrace(event->pos().x()), pixelToTime(event->pos().y())});
        update();
        return;
    }
    if (m_isPanning) {
      if ((event->pos() - m_pressPos).manhattanLength() > 4)
        m_dragged = true;
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
        // D4.4：断层折线收笔（≥2 点才算线）
        if (m_pickMode == SectionPickMode::Fault && m_faultDraft.size() >= 2) {
            QVector<QPair<double, double>> fracPts;
            for (const auto &pt : m_faultDraft)
                fracPts.append({pt.first / std::max(1, m_traces - 1), pt.second});
            emit faultDrawn(fracPts);
            m_faultDraft.clear();
            update();
            return;
        }
        m_faultDraft.clear();
        // D2.11：左键原地点击（未拖动）→ 道拾取事件（道头卡/解释拾取入口）；
        // wave/sections 的 updateHoverInfo(pos, clicked) 路径由此等价覆盖
        if (event->button() == Qt::LeftButton && !m_pressMoved && hasData() &&
            viewportRect().contains(event->pos()) && m_pickMode == SectionPickMode::None) {
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

    if (event->modifiers() & Qt::ControlModifier) {
        // Ctrl+滚轮：以光标为锚缩放（与 correlationpanel 惯例对齐）
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
        return;
    }

    // 裸滚轮平移（Shift+滚轮横向）
    const double stepPx = (delta / 120.0) * 48.0;
    if (event->modifiers() & Qt::ShiftModifier) {
        m_panX += stepPx;
    } else {
        m_panY += stepPx;
    }
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

void SeismicSectionCanvas::updateHoverInfo(const QPoint &pos, bool clicked) {
  if (!hasData() || !viewportRect().contains(pos))
    return;

  if (m_orientation == SectionOrientation::TimeSlice) {
    const double trace = pixelToTrace(pos.x());
    const double ilVal = pixelToInline(pos.y());
    const int colIdx =
        std::clamp(static_cast<int>(std::round(trace)), 0, m_traces - 1);
    const double spanIl = std::max(1, m_inlineMax - m_inlineMin);
    const int rowIdx = std::clamp(
        static_cast<int>(std::round((static_cast<double>(m_inlineMax) - ilVal) /
                                    spanIl * std::max(1, m_samples - 1))),
        0, m_samples - 1);

    const float amp =
        (colIdx >= 0 && colIdx < m_traces && rowIdx >= 0 && rowIdx < m_samples)
            ? m_slice.Value(colIdx, rowIdx)
            : 0.0f;

    const double xlVal =
        m_xlineMin + (static_cast<double>(colIdx) / std::max(1, m_traces - 1)) *
                         (m_xlineMax - m_xlineMin);
    const double depth = m_tdModel.TwtMsToDepth(m_currentTimeMs);

    if (clicked)
      emit traceClicked(colIdx, m_currentTimeMs, depth, amp, xlVal, ilVal);
    else
      emit traceHovered(colIdx, m_currentTimeMs, depth, amp, xlVal, ilVal);
    return;
  }

  const double trace = pixelToTrace(pos.x());
  const double twt = pixelToTime(pos.y());
  const int traceIdx =
      std::clamp(static_cast<int>(std::round(trace)), 0, m_traces - 1);
  const int sampleIdx = std::clamp(
      static_cast<int>(std::round((twt - m_t0Ms) / m_dtMs)), 0, m_samples - 1);

  const float amp = (traceIdx >= 0 && traceIdx < m_traces && sampleIdx >= 0 &&
                     sampleIdx < m_samples)
                        ? m_slice.Value(traceIdx, sampleIdx)
                        : 0.0f;

  const double depth = m_tdModel.TwtMsToDepth(twt);

  double mapX = qQNaN(), mapY = qQNaN();
  if (traceIdx < static_cast<int>(m_mapCoords.size())) {
    mapX = m_mapCoords[traceIdx].x;
    mapY = m_mapCoords[traceIdx].y;
  }

  if (clicked)
    emit traceClicked(traceIdx, twt, depth, amp, mapX, mapY);
  else
    emit traceHovered(traceIdx, twt, depth, amp, mapX, mapY);
}

} // namespace seismic
