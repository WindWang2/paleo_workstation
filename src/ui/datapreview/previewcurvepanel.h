// 层：视图
// token 例外：DESIGN 数据符号例外：测井曲线默认道色（setCurve #1B73D0）；图表 chrome 已转 token。（tools/ui-token-exceptions.json 精确计数）。
#pragma once

// 方向97（datapreviewtabs 拆分）：测井道曲线面板——自 datapreviewtabs_internal.h
// 逐字节迁出（650 行单类组件，超出 internal.h「多分支共用的短工具与小组件」章程）。
// 消费方：datapreviewtabs.cpp（time_depth 正文）、datapreviewtabwelllog.cpp
//（well_log 单道检视）；internal.h 组合导出，族内 TU 无需直接 include。

#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPen>
#include <QPointF>
#include <QRect>
#include <QScrollBar>
#include <QString>
#include <QVector>
#include <QWidget>
#include <QWheelEvent>

#include <algorithm>   // std::lower_bound（hover 插值）
#include <cmath>
#include <functional>
#include <limits>

#include "../paleotheme.h"

namespace paleo::datapreview_detail {

struct CurveData
{
  QString name;
  QString unit;
  QColor color;
  QVector<QPointF> pts; // (x = value, y = depth)
  QPair<double, double> vRange{0, 1};
  bool visible = true;
  double hoverValue = std::numeric_limits<double>::quiet_NaN();
};

// 测井道曲线面板：支持多曲线叠合、深度缩放、拖拽平移、标尺与光标读数
class CurvePanel : public QWidget
{
public:
  explicit CurvePanel(QWidget *parent = nullptr) : QWidget(parent)
  {
    setMinimumSize(280, 320);
    setMouseTracking(true);
    m_vScroll = new QScrollBar(Qt::Vertical, this);
    m_vScroll->setVisible(false);
    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int val) {
      if (m_updatingScroll || m_zoomFactor <= 1.0)
        return;
      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = totalSpan / m_zoomFactor;
      const double maxScroll = m_dRange.second - visibleSpan;
      if (maxScroll <= m_dRange.first)
        return;
      const int maxVal = m_vScroll->maximum();
      const double frac = maxVal > 0 ? static_cast<double>(val) / maxVal : 0.0;
      m_scrollDepth = m_dRange.first + frac * (maxScroll - m_dRange.first);
      update();
    });
  }

  void setEmptyText(const QString &text)
  {
    m_emptyText = text;
    update();
  }

  void clearCurves()
  {
    m_curves.clear();
    m_dRange = {0, 1};
    m_scrollDepth = 0.0;
    m_zoomFactor = 1.0;
    updateScrollBar();
    update();
  }

  void addCurve(const QString &name, const QString &unit,
                const QVector<double> &values, const QVector<double> &depths,
                const QColor &color, bool visible = true)
  {
    CurveData cd;
    cd.name = name;
    cd.unit = unit;
    cd.color = color;
    cd.visible = visible;

    double vMin = std::numeric_limits<double>::max(), vMax = std::numeric_limits<double>::lowest();
    double dMin = m_curves.isEmpty() ? std::numeric_limits<double>::max() : m_dRange.first;
    double dMax = m_curves.isEmpty() ? std::numeric_limits<double>::lowest() : m_dRange.second;

    const int n = qMin(values.size(), depths.size());
    for (int i = 0; i < n; ++i)
    {
      const double v = values.at(i);
      const double d = depths.at(i);
      if (std::isnan(v) || std::isnan(d) || v <= -9999.0)
        continue;
      cd.pts.append(QPointF(v, d));
      vMin = qMin(vMin, v);
      vMax = qMax(vMax, v);
      dMin = qMin(dMin, d);
      dMax = qMax(dMax, d);
    }

    if (!cd.pts.isEmpty())
    {
      cd.vRange = {vMin == vMax ? vMin - 1.0 : vMin, vMax == vMin ? vMax + 1.0 : vMax};
      m_dRange = {dMin, dMax};
      m_scrollDepth = m_dRange.first;
    }
    m_curves.append(cd);
    updateScrollBar();
    update();
  }

  // 单道兼容接口（time_depth 或旧代码调用）
  void setCurve(const QString &name, const QString &unit,
                const QVector<double> &values, const QVector<double> &depths)
  {
    clearCurves();
    addCurve(name, unit, values, depths, QColor(QStringLiteral("#1B73D0")), true);
  }

  void setCurveVisible(const QString &name, bool visible)
  {
    for (CurveData &c : m_curves)
    {
      if (c.name.compare(name, Qt::CaseInsensitive) == 0)
      {
        c.visible = visible;
        break;
      }
    }
    update();
  }

  bool isCurveVisible(const QString &name) const
  {
    for (const CurveData &c : m_curves)
      if (c.name.compare(name, Qt::CaseInsensitive) == 0)
        return c.visible;
    return false;
  }

  int pointCount() const
  {
    int count = 0;
    for (const CurveData &c : m_curves)
      count += c.pts.size();
    return count;
  }

  double zoomFactor() const { return m_zoomFactor; }

  void setZoom(double z, double anchorDepth = -1.0)
  {
    const double clampedZ = qBound(1.0, z, 50.0);
    if (qFuzzyCompare(clampedZ, m_zoomFactor) && anchorDepth < 0)
      return;

    const double totalSpan = m_dRange.second - m_dRange.first;
    if (totalSpan <= 0)
      return;

    const double oldSpan = totalSpan / m_zoomFactor;
    const double newSpan = totalSpan / clampedZ;

    if (anchorDepth < 0)
      anchorDepth = m_scrollDepth + oldSpan * 0.5;

    const double anchorFrac = oldSpan > 0 ? (anchorDepth - m_scrollDepth) / oldSpan : 0.5;
    m_scrollDepth = anchorDepth - anchorFrac * newSpan;
    m_zoomFactor = clampedZ;

    const double maxScroll = m_dRange.second - newSpan;
    m_scrollDepth = qBound(m_dRange.first, m_scrollDepth, qMax(m_dRange.first, maxScroll));

    updateScrollBar();
    update();
    if (onZoomChanged)
      onZoomChanged(m_zoomFactor);
  }

  void zoomIn() { setZoom(m_zoomFactor * 1.5); }
  void zoomOut() { setZoom(m_zoomFactor / 1.5); }
  void resetZoom() { setZoom(1.0); }

  std::function<void(double)> onZoomChanged;
  std::function<void(double depth, const QString &info)> onHoverChanged;

  double depthAtY(int y) const
  {
    const QRect pRect = plotRect();
    if (pRect.height() <= 0)
      return m_dRange.first;
    const double visibleSpan = (m_dRange.second - m_dRange.first) / m_zoomFactor;
    const double frac = static_cast<double>(y - pRect.top()) / pRect.height();
    return m_scrollDepth + frac * visibleSpan;
  }

  int yAtDepth(double d) const
  {
    const QRect pRect = plotRect();
    if (pRect.height() <= 0)
      return 0;
    const double visibleSpan = (m_dRange.second - m_dRange.first) / m_zoomFactor;
    if (visibleSpan <= 0)
      return pRect.top();
    const double frac = (d - m_scrollDepth) / visibleSpan;
    return pRect.top() + qRound(frac * pRect.height());
  }

  int calculateHeaderHeight() const
  {
    int visibleCount = 0;
    for (const CurveData &c : m_curves)
      if (c.visible) visibleCount++;
    if (visibleCount <= 2)
      return 28;
    if (visibleCount <= 4)
      return 46;
    return 64;
  }

  QRect plotRect() const
  {
    const int kRulerW = 50;
    const int headerH = calculateHeaderHeight();
    const int scrollW = (m_zoomFactor > 1.0) ? 14 : 0;
    return QRect(kRulerW, headerH, qMax(20, width() - kRulerW - scrollW - 6),
                 qMax(20, height() - headerH - 8));
  }

protected:
  void updateScrollBar()
  {
    if (!m_vScroll)
      return;
    const QRect pRect = plotRect();
    m_vScroll->setGeometry(width() - 14, pRect.top(), 14, pRect.height());

    if (m_zoomFactor <= 1.0)
    {
      m_vScroll->setVisible(false);
      return;
    }
    m_vScroll->setVisible(true);

    const double totalSpan = m_dRange.second - m_dRange.first;
    const double visibleSpan = totalSpan / m_zoomFactor;
    const double maxScroll = m_dRange.second - visibleSpan;

    m_updatingScroll = true;
    const int kRange = 10000;
    const int pageStep = qMax(1, qRound(kRange / m_zoomFactor));
    m_vScroll->setRange(0, kRange - pageStep);
    m_vScroll->setPageStep(pageStep);
    const double frac = (maxScroll > m_dRange.first)
                            ? (m_scrollDepth - m_dRange.first) / (maxScroll - m_dRange.first)
                            : 0.0;
    m_vScroll->setValue(qRound(frac * (kRange - pageStep)));
    m_updatingScroll = false;
  }

  void updateHoverValues()
  {
    for (CurveData &c : m_curves)
    {
      if (!c.visible || c.pts.isEmpty() || std::isnan(m_hoverDepth))
      {
        c.hoverValue = std::numeric_limits<double>::quiet_NaN();
        continue;
      }

      auto it = std::lower_bound(c.pts.begin(), c.pts.end(), m_hoverDepth,
                                 [](const QPointF &pt, double d) { return pt.y() < d; });
      if (it == c.pts.end())
      {
        c.hoverValue = c.pts.last().x();
      }
      else if (it == c.pts.begin())
      {
        c.hoverValue = c.pts.first().x();
      }
      else
      {
        const QPointF &p0 = *(it - 1);
        const QPointF &p1 = *it;
        if (qAbs(p1.y() - p0.y()) > 1e-4)
        {
          const double t = (m_hoverDepth - p0.y()) / (p1.y() - p0.y());
          c.hoverValue = p0.x() + t * (p1.x() - p0.x());
        }
        else
        {
          c.hoverValue = p1.x();
        }
      }
    }
    if (onHoverChanged)
    {
      if (std::isnan(m_hoverDepth))
      {
        onHoverChanged(-1.0, QString());
      }
      else
      {
        QString info = QString::asprintf("MD: %.1f m", m_hoverDepth);
        for (const CurveData &c : m_curves)
        {
          if (c.visible && !std::isnan(c.hoverValue))
          {
            info += QStringLiteral(" | ") + c.name + QString::asprintf(": %.2f", c.hoverValue);
            if (!c.unit.isEmpty())
              info += QStringLiteral(" ") + c.unit;
          }
        }
        onHoverChanged(m_hoverDepth, info);
      }
    }
  }

  void mousePressEvent(QMouseEvent *e) override
  {
    const QRect pRect = plotRect();
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
    {
      if (m_zoomFactor > 1.0 && pRect.contains(e->pos()))
      {
        m_dragging = true;
        m_dragStartY = e->pos().y();
        m_dragStartScrollDepth = m_scrollDepth;
        setCursor(Qt::ClosedHandCursor);
      }
    }
  }

  void mouseMoveEvent(QMouseEvent *e) override
  {
    const QRect pRect = plotRect();
    if (pRect.contains(e->pos()))
    {
      m_hoverDepth = depthAtY(e->pos().y());
      updateHoverValues();
    }
    else
    {
      m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
    }

    if (m_dragging)
    {
      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = totalSpan / m_zoomFactor;
      const double dy = e->pos().y() - m_dragStartY;
      const double dDepth = (dy / static_cast<double>(pRect.height())) * visibleSpan;
      const double maxScroll = m_dRange.second - visibleSpan;
      m_scrollDepth = qBound(m_dRange.first, m_dragStartScrollDepth - dDepth, qMax(m_dRange.first, maxScroll));
      updateScrollBar();
      update();
    }
    else
    {
      if (m_zoomFactor > 1.0 && pRect.contains(e->pos()))
        setCursor(Qt::OpenHandCursor);
      else
        unsetCursor();
      update();
    }
  }

  void mouseReleaseEvent(QMouseEvent *) override
  {
    if (m_dragging)
    {
      m_dragging = false;
      if (m_zoomFactor > 1.0 && plotRect().contains(mapFromGlobal(QCursor::pos())))
        setCursor(Qt::OpenHandCursor);
      else
        unsetCursor();
    }
  }

  void mouseDoubleClickEvent(QMouseEvent *e) override
  {
    if (plotRect().contains(e->pos()))
    {
      resetZoom();
    }
  }

  void wheelEvent(QWheelEvent *e) override
  {
    const QRect pRect = plotRect();
    if (!pRect.contains(e->position().toPoint()))
    {
      e->ignore();
      return;
    }

    if (e->modifiers() & Qt::ControlModifier)
    {
      const double f = e->angleDelta().y() > 0 ? 1.25 : 1.0 / 1.25;
      setZoom(m_zoomFactor * f, depthAtY(e->position().y()));
      e->accept();
      return;
    }

    if (m_zoomFactor > 1.0)
    {
      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = totalSpan / m_zoomFactor;
      const double step = visibleSpan * 0.12 * (e->angleDelta().y() > 0 ? -1.0 : 1.0);
      const double maxScroll = m_dRange.second - visibleSpan;
      m_scrollDepth = qBound(m_dRange.first, m_scrollDepth + step, qMax(m_dRange.first, maxScroll));
      updateScrollBar();
      update();
      e->accept();
      return;
    }

    e->ignore();
  }

  void leaveEvent(QEvent *) override
  {
    m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
    for (CurveData &c : m_curves)
      c.hoverValue = std::numeric_limits<double>::quiet_NaN();
    unsetCursor();
    update();
    if (onHoverChanged)
      onHoverChanged(-1.0, QString());
  }

  void resizeEvent(QResizeEvent *) override
  {
    updateScrollBar();
  }

  void drawHeader(QPainter &p, const QRect &pRect)
  {
    const int headerTop = 4;
    int curX = pRect.left() + 4;
    int curY = headerTop;
    const int rowHeight = 18;

    QFont fName = font();
    fName.setPointSize(PaleoTheme::tokens().labelPt);
    fName.setBold(true);

    QFont fMono = PaleoTheme::monoFont();
    fMono.setPointSize(PaleoTheme::tokens().labelPt);

    for (const CurveData &c : m_curves)
    {
      if (!c.visible)
        continue;

      // Sample line
      p.setPen(QPen(c.color, 2.5, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(curX, curY + rowHeight / 2, curX + 12, curY + rowHeight / 2);
      curX += 16;

      // Curve Name
      p.setFont(fName);
      p.setPen(PaleoTheme::tokens().text);
      const QString nameStr = c.name;
      p.drawText(curX, curY + rowHeight - 4, nameStr);
      curX += fontMetrics().horizontalAdvance(nameStr) + 4;

      // Scale range & unit: e.g. "0–150 API"
      p.setFont(fMono);
      p.setPen(PaleoTheme::tokens().textMuted);
      QString scaleStr;
      if (!std::isnan(c.hoverValue))
      {
        scaleStr = QString::asprintf(": %.1f", c.hoverValue);
        if (!c.unit.isEmpty())
          scaleStr += QStringLiteral(" ") + c.unit;
        scaleStr += QString::asprintf(" (%.0f–%.0f)", c.vRange.first, c.vRange.second);
      }
      else
      {
        scaleStr = QString::asprintf("[%.0f–%.0f", c.vRange.first, c.vRange.second);
        if (!c.unit.isEmpty())
          scaleStr += QStringLiteral(" ") + c.unit;
        scaleStr += QStringLiteral("]");
      }

      p.drawText(curX, curY + rowHeight - 4, scaleStr);
      curX += QFontMetrics(fMono).horizontalAdvance(scaleStr) + 12;

      if (curX > pRect.right() - 80)
      {
        curX = pRect.left() + 4;
        curY += rowHeight;
      }
    }
  }

  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.fillRect(rect(), PaleoTheme::tokens().surface);

    const QRect pRect = plotRect();
    const int kRulerW = pRect.left();

    // Draw ruler background
    const QRect rulerRect(0, pRect.top(), kRulerW, pRect.height());
    p.fillRect(rulerRect, PaleoTheme::tokens().surfaceAlt);
    p.setPen(PaleoTheme::tokens().border);
    p.drawLine(kRulerW, pRect.top(), kRulerW, pRect.bottom());

    // Ruler title "MD (m)"
    QFont fCaption = font();
    fCaption.setPointSize(PaleoTheme::tokens().labelPt);
    p.setFont(fCaption);
    p.setPen(PaleoTheme::tokens().textMuted);
    p.drawText(QRect(2, 4, kRulerW - 4, pRect.top() - 4), Qt::AlignCenter | Qt::AlignVCenter,
               QStringLiteral("MD (m)"));

    p.drawRect(pRect);

    int totalPoints = 0;
    int visibleCurves = 0;
    for (const CurveData &c : m_curves)
    {
      totalPoints += c.pts.size();
      if (c.visible)
        visibleCurves++;
    }

    if (totalPoints == 0)
    {
      p.setPen(PaleoTheme::tokens().textMuted);
      p.drawText(pRect, Qt::AlignCenter, m_emptyText);
      return;
    }

    if (visibleCurves == 0)
    {
      p.setPen(PaleoTheme::tokens().textMuted);
      p.drawText(pRect, Qt::AlignCenter, tr("未勾选任何曲线 — 在上方选择要显示的曲线"));
      return;
    }

    const double totalSpan = m_dRange.second - m_dRange.first;
    const double visibleSpan = (totalSpan > 0 && m_zoomFactor >= 1.0)
                                   ? totalSpan / m_zoomFactor
                                   : 1.0;
    const double dTop = m_scrollDepth;
    const double dBottom = m_scrollDepth + visibleSpan;

    const int targetTicks = qBound(4, pRect.height() / 45, 12);
    const double rawInterval = visibleSpan / targetTicks;
    double niceInterval = 100.0;
    const double intervals[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
    for (double iv : intervals)
    {
      niceInterval = iv;
      if (iv >= rawInterval)
        break;
    }

    const double firstTick = std::ceil(dTop / niceInterval) * niceInterval;
    QFont fMono = PaleoTheme::monoFont();
    fMono.setPointSize(PaleoTheme::tokens().labelPt);

    p.setFont(fMono);
    for (double d = firstTick; d <= dBottom; d += niceInterval)
    {
      const int y = yAtDepth(d);
      if (y < pRect.top() || y > pRect.bottom())
        continue;

      p.setPen(PaleoTheme::tokens().textMuted);
      p.drawLine(kRulerW - 5, y, kRulerW, y);

      p.setPen(QPen(PaleoTheme::tokens().surfaceAltRaised, 1, Qt::DashLine));
      p.drawLine(pRect.left(), y, pRect.right(), y);

      p.setPen(PaleoTheme::tokens().textMuted);
      const QString dText = QString::number(d, 'f', (niceInterval < 1.0 ? 1 : 0));
      p.drawText(QRect(2, y - 8, kRulerW - 9, 16), Qt::AlignRight | Qt::AlignVCenter, dText);
    }

    p.setPen(QPen(PaleoTheme::tokens().surfaceAltRaised, 1, Qt::DotLine));
    for (int i = 1; i <= 3; ++i)
    {
      const int vx = pRect.left() + (pRect.width() * i) / 4;
      p.drawLine(vx, pRect.top(), vx, pRect.bottom());
    }

    p.setClipRect(pRect);
    for (const CurveData &c : m_curves)
    {
      if (!c.visible || c.pts.isEmpty())
        continue;

      const double vSpan = c.vRange.second - c.vRange.first;
      if (vSpan <= 0)
        continue;

      const auto mapX = [&](double v) {
        const double f = (v - c.vRange.first) / vSpan;
        return pRect.left() + f * pRect.width();
      };

      const QPen dataPen(c.color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      const QColor halo = PaleoTheme::dataHaloColor(c.color);

      bool first = true;
      QPointF prev;
      for (const QPointF &pt : c.pts)
      {
        const double v = pt.x();
        const double d = pt.y();

        if (d < dTop - niceInterval || d > dBottom + niceInterval)
        {
          first = true;
          continue;
        }

        const QPointF mapped(mapX(v), yAtDepth(d));
        if (!first)
        {
          if (halo.isValid())
          {
            p.setPen(QPen(halo, dataPen.widthF() + 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(prev, mapped);
          }
          p.setPen(dataPen);
          p.drawLine(prev, mapped);
        }
        prev = mapped;
        first = false;
      }
    }
    p.setClipping(false);

    drawHeader(p, pRect);

    if (!std::isnan(m_hoverDepth) && pRect.contains(mapFromGlobal(QCursor::pos())))
    {
      const int hy = yAtDepth(m_hoverDepth);
      if (hy >= pRect.top() && hy <= pRect.bottom())
      {
        p.setPen(QPen(PaleoTheme::tokens().primaryText, 1, Qt::DashLine));
        p.drawLine(pRect.left(), hy, pRect.right(), hy);

        const QString hText = QString::number(m_hoverDepth, 'f', 1);
        p.setFont(fMono);
        const QRect badgeRect(2, hy - 8, kRulerW - 4, 16);
        p.fillRect(badgeRect, PaleoTheme::tokens().text);
        p.setPen(PaleoTheme::tokens().surface);
        p.drawText(badgeRect, Qt::AlignCenter, hText);
      }
    }
  }

private:
  QVector<CurveData> m_curves;
  QPair<double, double> m_dRange{0, 1};
  double m_zoomFactor = 1.0;
  double m_scrollDepth = 0.0;
  QScrollBar *m_vScroll = nullptr;
  bool m_updatingScroll = false;
  double m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
  bool m_dragging = false;
  int m_dragStartY = 0;
  double m_dragStartScrollDepth = 0.0;
  QString m_emptyText = QObject::tr("无有效采样");
};

} // namespace paleo::datapreview_detail
