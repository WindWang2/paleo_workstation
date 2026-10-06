// 层：视图
// 综合图道·曲线充填与符号（CurveTrack 1-4 根合并 / SymbolTrack）——自 wellcompositetrack.cpp 拆出（方向 66，行为零变更）
#include "wellcompositetrack.h"
#include "ui/paleotheme.h"
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

  // D4.11/D1.7 网格系统：密度可配（0 无 / 1 两分 / 2 四分 / 3 十分含次网格），
  // 次网格（1/10）仅在密度 3 时叠加；重叠网格开关关闭时完全不画。
  if (m_showGrid && m_gridDensity > 0)
  {
    const int divisions = m_gridDensity == 1 ? 2 : (m_gridDensity == 2 ? 4 : 10);
    for (int div = 1; div < divisions; ++div)
    {
      const bool isMinor = (m_gridDensity == 3 && div % 5 != 0 && div != 5);
      painter.setPen(QPen(isMinor ? QColor(QStringLiteral("#F8FAFB")) : QColor(QStringLiteral("#F1F3F5")),
                          1.0, isMinor ? Qt::DotLine : Qt::DashLine));
      const qreal gx = bodyRect.left() + bodyRect.width() * (div / static_cast<qreal>(divisions));
      painter.drawLine(QPointF(gx, bodyRect.top()), QPointF(gx, bodyRect.bottom()));
    }
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

    if (c.mode == CurveDisplayMode::Continuous)
    {
      QVector<QPointF> polyPoints;
      polyPoints.reserve(1024);

      for (int i = 0; i < c.depths.size(); ++i)
      {
        const float d = c.depths.at(i);
        const float v = c.values.at(i);

        if (d < topDepth - 20.0 || d > bottomDepth + 20.0)
          continue;

        if (!std::isfinite(v))
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
      for (int i = 0; i < c.depths.size(); ++i)
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
      for (int i = 0; i < c.depths.size() - 1; ++i)
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
