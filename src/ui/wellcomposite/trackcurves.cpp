// 层：视图
// 综合图道·曲线充填与符号（CurveTrack 1-4 根合并 / SymbolTrack）——自 wellcompositetrack.cpp 拆出（方向 66，行为零变更）
#include "wellcompositetrack.h"
#include "depthtools.h"
#include "ui/paleotheme.h"
#include <algorithm>
#include <cmath>

namespace WellComposite
{
// ----------------------------------------------------------------------------
// 7. 曲线道 (CurveTrack) — 支持合并显示 1 至 4 根曲线
// ----------------------------------------------------------------------------
CurveTrack::CurveTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title;
}

bool CurveTrack::addCurve(const CurveData &curve)
{
  // 严格执行限制：一个曲线道最多合并显示 4 根曲线
  if (m_curves.size() >= 4)
    return false;

  m_curves.append(curve);
  return true;
}

void CurveTrack::setCurves(const QVector<CurveData> &curves)
{
  m_curves.clear();
  for (int i = 0; i < qMin(4, curves.size()); ++i)
    m_curves.append(curves.at(i));
}

bool CurveTrack::setCurveAt(int idx, const CurveData &curve)
{
  if (idx < 0 || idx >= m_curves.size())
    return false;
  m_curves[idx] = curve;
  return true;
}

bool CurveTrack::removeCurveAt(int idx)
{
  if (idx < 0 || idx >= m_curves.size())
    return false;
  m_curves.removeAt(idx);
  return true;
}

QString CurveTrack::headerScaleText() const
{
  if (m_curves.isEmpty())
    return QString();
  const auto &c = m_curves.first();
  const auto fmt = [](float v) {
    if (std::abs(v) >= 100.0f) return QString::number(static_cast<int>(std::round(v)));
    return QString::number(v, 'f', 1);
  };
  QString t = QStringLiteral("%1~%2").arg(fmt(c.minScale), fmt(c.maxScale));
  if (m_curves.size() > 1)
    t += QStringLiteral(" +%1").arg(m_curves.size() - 1);
  return t;
}

QString CurveTrack::headerUnitText() const
{
  if (m_curves.isEmpty())
    return QString();
  const QString u = m_curves.first().unit;
  return u.isEmpty() ? QStringLiteral("—") : u;
}

QString CurveTrack::trackToolTip(double depth) const
{
  if (m_curves.isEmpty())
    return title();
  QStringList rows;
  rows.reserve(m_curves.size() + 1);
  rows << title();
  const auto fmt = [](float v) {
    if (!std::isfinite(v)) return QStringLiteral("NaN");
    if (std::abs(v) >= 10000.0f || (std::abs(v) < 0.01f && v != 0.0f))
      return QString::number(v, 'g', 3);
    if (std::abs(v) >= 100.0f) return QString::number(static_cast<int>(std::round(v)));
    return QString::number(v, 'f', 2);
  };
  for (const auto &c : m_curves)
  {
    if (c.isEmpty()) continue;
    const float val = c.valueAtDepth(static_cast<float>(depth));
    rows << QStringLiteral("%1 = %2%3   [%4~%5]")
                .arg(c.name, fmt(val), c.unit.isEmpty() ? QString() : QStringLiteral(" ") + c.unit,
                     fmt(c.minScale), fmt(c.maxScale));
  }
  return rows.join(QLatin1Char('\n'));
}

void CurveTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth)
{
  painter.save();
  painter.setClipRect(headerRect);

  painter.fillRect(headerRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surfaceAlt);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  // 主标题
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
  QFont fTitle = painter.font();
  fTitle.setPointSize(PaleoTheme::tokens().bodyPt);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(QRectF(headerRect.left() + 2, headerRect.top() + 2, headerRect.width() - 4, 16),
                   Qt::AlignCenter, title());

  // 绘制 1 至 4 根曲线的标头（色标、刻度范围 [min-max] 及实时光标读数）
  const int count = m_curves.size();
  if (count > 0)
  {
    const qreal slotH = (headerRect.height() - 20) / static_cast<qreal>(count);
    QFont fCurve = painter.font();
    fCurve = PaleoTheme::monoFont(fCurve.pointSize());
    fCurve.setStyleHint(QFont::TypeWriter);
    fCurve.setPointSize(PaleoTheme::tokens().labelPt);
    fCurve.setBold(false);
    painter.setFont(fCurve);

    for (int i = 0; i < count; ++i)
    {
      const auto &c = m_curves.at(i);
      const qreal ySlot = headerRect.top() + 18 + i * slotH;
      const QRectF rowRect(headerRect.left() + 3, ySlot, headerRect.width() - 6, slotH);

      // 色标线
      painter.setPen(QPen(c.color, 2.0, c.penStyle));
      painter.drawLine(QPointF(rowRect.left() + 2, rowRect.center().y()),
                       QPointF(rowRect.left() + 16, rowRect.center().y()));

      // 曲线名与单位
      painter.setPen(c.color);
      const QString nameUnit = c.unit.isEmpty() ? c.name : QStringLiteral("%1(%2)").arg(c.name, c.unit);
      painter.drawText(QRectF(rowRect.left() + 20, rowRect.top(), rowRect.width() * 0.5, rowRect.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, nameUnit);

      // 量程刻度范围与实时读数
      const auto formatVal = [](float v) {
        if (!std::isfinite(v)) return QStringLiteral("NaN");
        if (std::abs(v) >= 10000.0f || (std::abs(v) < 0.01f && v != 0.0f))
          return QString::number(v, 'g', 3);
        if (std::abs(v) >= 100.0f)
          return QString::number(static_cast<int>(std::round(v)));
        return QString::number(v, 'f', 1);
      };
      QString scaleText = QStringLiteral("%1-%2").arg(formatVal(c.minScale), formatVal(c.maxScale));
      if (currentDepth > 0.0 && !c.isEmpty())
      {
        const float val = c.valueAtDepth(static_cast<float>(currentDepth));
        scaleText = QStringLiteral("%1 [%2]").arg(formatVal(val), scaleText);
      }
      painter.drawText(QRectF(rowRect.left() + rowRect.width() * 0.45, rowRect.top(), rowRect.width() * 0.53, rowRect.height()),
                       Qt::AlignRight | Qt::AlignVCenter, scaleText);
    }
  }

  painter.restore();
}

void CurveTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                           double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);

  // 纸面网格：浅灰实线，主次线均在曲线下方；深度线锚定实际深度，滚动不漂移。
  if (m_showGrid && m_gridDensity > 0 && std::isfinite(pxPerMeter) && pxPerMeter > 0.0 &&
      std::isfinite(topDepth) && std::isfinite(bottomDepth) && bottomDepth >= topDepth)
  {
    const auto &paper = PaleoTheme::tokens(PaleoTheme::Theme::Light);
    const int divisions = m_gridDensity == 1 ? 2 : (m_gridDensity == 2 ? 4 : 10);
    const bool antialias = painter.testRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (int div = 1; div < divisions; ++div)
    {
      const bool minor = m_gridDensity == 3 && div != 5;
      painter.setPen(QPen(minor ? paper.surfaceAlt : paper.border, 1.0));
      const qreal x = bodyRect.left() + bodyRect.width() * div / divisions;
      painter.drawLine(QPointF(x, bodyRect.top()), QPointF(x, bodyRect.bottom()));
    }
    double majorStep = DepthTools::niceStepFor(pxPerMeter, 36.0);
    if (majorStep * pxPerMeter < 36.0)
      majorStep *= std::pow(10.0, std::ceil(std::log10(36.0 / (majorStep * pxPerMeter))));
    const double minorStep = majorStep / (m_gridDensity == 3 ? 5.0 : 1.0);
    const double first = std::ceil(topDepth / minorStep);
    const double last = std::floor(bottomDepth / minorStep);
    // 只迭代可见高度，极小/极大比例尺也不生成无界网格。
    const int count = qBound(0, static_cast<int>(qMin(last - first + 1.0, bodyRect.height() + 1.0)), 10000);
    for (int i = 0; i < count; ++i)
    {
      const double d = (first + i) * minorStep;
      const bool major = std::abs(d / majorStep - std::round(d / majorStep)) < 1e-6;
      painter.setPen(QPen(major ? paper.border : paper.surfaceAlt, 1.0));
      const qreal y = bodyRect.top() + (d - topDepth) * pxPerMeter;
      painter.drawLine(QPointF(bodyRect.left(), y), QPointF(bodyRect.right(), y));
    }
    painter.setRenderHint(QPainter::Antialiasing, antialias);
  }

  // 遍历绘制道内的 1 至 4 根曲线
  for (const auto &c : m_curves)
  {
    if (c.isEmpty())
      continue;

    const float minVal = c.minScale;
    const float maxVal = qMax(minVal + 1e-4f, c.maxScale);
    const float valSpan = maxVal - minVal;

    painter.setPen(QPen(c.color, c.penWidth, c.penStyle));
    painter.setBrush(Qt::NoBrush);

    // #412：depths/values 不等长属坏数据——按短边裁，release 下 at() 越界
    // 不兜底（CurveData::isEmpty 只查非空，不查对齐）。
    const int n = qMin(c.depths.size(), c.values.size());

    if (c.mode == CurveDisplayMode::Continuous)
    {
      QVector<QPointF> polyPoints;
      polyPoints.reserve(1024);

      // #557：深度采样有序（domain CurveData::valueAtDepth 二分插值同一
      // 前提）→ 二分定位可见窗口起点，每帧 O(可见样点) 而非 O(总样点)。
      // 仅在端点有限且首 ≤ 末时走二分（越下窗即 break）；坏数据（NaN/逆序
      // 深度）退回原全量线性扫描，绘制结果与改前逐点一致。起点比窗口下界
      // 多取一个样点，但该样点仍按原 ±20 m 深度窗谓词过滤——不改线段集合。
      const double winTop = topDepth - 20.0;
      const double winBottom = bottomDepth + 20.0;
      const bool ordered = n > 0 && std::isfinite(c.depths.first()) &&
                           std::isfinite(c.depths.at(n - 1)) &&
                           c.depths.first() <= c.depths.at(n - 1);
      int begin = 0;
      if (ordered)
      {
        begin = static_cast<int>(std::lower_bound(c.depths.cbegin(),
                                                  c.depths.cbegin() + n,
                                                  winTop) -
                                 c.depths.cbegin());
        if (begin > 0)
          --begin; // 窗口上缘多取一个样点，避免边界处线段断裂
      }

      for (int i = begin; i < n; ++i)
      {
        const float d = c.depths.at(i);
        const float v = c.values.at(i);

        if (ordered && d > winBottom)
          break; // 深度有序：其后样点必然越窗，无需继续扫描

        if (d < winTop || d > winBottom)
          continue;

        // 深度或值任一 NaN 都断笔（opt-02）：深度列 NULL 行的 NaN 比较恒
        // false，过不了深度窗过滤——不断笔会产出 NaN 顶点（与值 NaN 同口径）。
        if (!std::isfinite(v) || !std::isfinite(d))
        {
          if (polyPoints.size() >= 2)
            painter.drawPolyline(polyPoints.constData(), static_cast<int>(polyPoints.size()));
          polyPoints.clear();
          continue;
        }

        const float normX = qBound(0.0f, (v - minVal) / valSpan, 1.0f);
        const qreal x = bodyRect.left() + normX * bodyRect.width();
        const qreal y = bodyRect.top() + (d - topDepth) * pxPerMeter;
        polyPoints.append(QPointF(x, y));
      }

      if (polyPoints.size() >= 2)
        painter.drawPolyline(polyPoints.constData(), static_cast<int>(polyPoints.size()));
    }
    else if (c.mode == CurveDisplayMode::Discrete)
    {
      painter.setBrush(c.color);
      for (int i = 0; i < n; ++i)
      {
        const float d = c.depths.at(i);
        const float v = c.values.at(i);
        if (d < topDepth || d > bottomDepth || !std::isfinite(v))
          continue;

        const float normX = qBound(0.0f, (v - minVal) / valSpan, 1.0f);
        const qreal x = bodyRect.left() + normX * bodyRect.width();
        const qreal y = bodyRect.top() + (d - topDepth) * pxPerMeter;
        painter.drawEllipse(QPointF(x, y), PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);
      }
    }
    else if (c.mode == CurveDisplayMode::Histogram)
    {
      painter.setBrush(QColor(c.color.red(), c.color.green(), c.color.blue(), 140));
      for (int i = 0; i < n - 1; ++i)
      {
        const float d0 = c.depths.at(i);
        const float d1 = c.depths.at(i + 1);
        const float v = c.values.at(i);
        if (d1 < topDepth || d0 > bottomDepth || !std::isfinite(v))
          continue;

        const float normX = qBound(0.0f, (v - minVal) / valSpan, 1.0f);
        const qreal barW = normX * bodyRect.width();
        const qreal y0 = bodyRect.top() + (d0 - topDepth) * pxPerMeter;
        const qreal y1 = bodyRect.top() + (d1 - topDepth) * pxPerMeter;
        painter.drawRect(QRectF(bodyRect.left(), y0, barW, qMax<qreal>(2.0, y1 - y0)));
      }
    }
  }

  // 右侧分界线
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  painter.restore();
}

// ----------------------------------------------------------------------------
// 8. 符号道 (SymbolTrack)
// ----------------------------------------------------------------------------
SymbolTrack::SymbolTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title;
}

void SymbolTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  paintHeaderChrome(painter, headerRect, QStringLiteral("段/点符号"), QString());
}

void SymbolTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                           double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  for (const auto &sym : m_items)
  {
    if (sym.bottomDepth < topDepth || sym.topDepth > bottomDepth)
      continue;

    const qreal y0 = bodyRect.top() + (sym.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (sym.bottomDepth - topDepth) * pxPerMeter;
    const qreal h = qMax<qreal>(6.0, y1 - y0);

    if (sym.kind == SymbolKind::Perforation) // 射孔段：行业标准梳齿状
    {
      const qreal cx = bodyRect.center().x();
      painter.setPen(QPen(QColor(QStringLiteral("#D32F2F")), 2.0));
      painter.drawLine(QPointF(cx, y0), QPointF(cx, y1)); // 主干竖线

      // 梳状齿
      painter.setPen(QPen(QColor(QStringLiteral("#D32F2F")), 1.5));
      const qreal toothStep = 5.0;
      for (qreal ty = y0; ty <= y1; ty += toothStep)
      {
        painter.drawLine(QPointF(cx, ty), QPointF(cx + 8, ty));
      }
    }
    else if (sym.kind == SymbolKind::OilShow) // 油层：实心红水滴/红圆
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(QStringLiteral("#D32F2F")));
      painter.drawEllipse(QPointF(bodyRect.center().x(), y0 + h * 0.5), 5.0, 5.0);
    }
    else if (sym.kind == SymbolKind::GasShow) // 气层：红白相间
    {
      painter.setPen(QPen(QColor(QStringLiteral("#D32F2F")), 1.5));
      painter.setBrush(QColor(QStringLiteral("#FFEBEE")));
      painter.drawEllipse(QPointF(bodyRect.center().x(), y0 + h * 0.5), 5.0, 5.0);
    }
    else if (sym.kind == SymbolKind::WaterShow) // 水层：纯蓝水滴
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(QStringLiteral("#1976D2")));
      painter.drawEllipse(QPointF(bodyRect.center().x(), y0 + h * 0.5), 5.0, 5.0);
    }
    else if (sym.kind == SymbolKind::PressureTest) // 测压取样：菱形
    {
      painter.setPen(QPen(QColor(QStringLiteral("#7B1FA2")), 1.5));
      painter.setBrush(QColor(QStringLiteral("#E1BEE7")));
      const QPointF c(bodyRect.center().x(), y0 + h * 0.5);
      const QPolygonF diamond({QPointF(c.x(), c.y() - 5), QPointF(c.x() + 5, c.y()),
                               QPointF(c.x(), c.y() + 5), QPointF(c.x() - 5, c.y())});
      painter.drawPolygon(diamond);
    }
    else if (sym.kind == SymbolKind::PositiveCycle) // 正旋回：向上变细 (正粒序，底宽顶窄喇叭/三角)
    {
      const qreal cx = bodyRect.center().x();
      const qreal halfW = qMin<qreal>(14.0, bodyRect.width() * 0.38);
      const QPolygonF triangle({QPointF(cx - halfW, y1), QPointF(cx + halfW, y1), QPointF(cx, y0)});
      painter.setPen(QPen(QColor(QStringLiteral("#D97706")), 1.5));
      painter.setBrush(QColor(QStringLiteral("#FEF3C7")));
      painter.drawPolygon(triangle);
    }
    else if (sym.kind == SymbolKind::NegativeCycle) // 反旋回：向上变粗 (逆粒序，底窄顶宽漏斗/倒三角)
    {
      const qreal cx = bodyRect.center().x();
      const qreal halfW = qMin<qreal>(14.0, bodyRect.width() * 0.38);
      const QPolygonF triangle({QPointF(cx, y1), QPointF(cx - halfW, y0), QPointF(cx + halfW, y0)});
      painter.setPen(QPen(QColor(QStringLiteral("#2563EB")), 1.5));
      painter.setBrush(QColor(QStringLiteral("#DBEAFE")));
      painter.drawPolygon(triangle);
    }
  }

  painter.restore();
}

} // namespace WellComposite
