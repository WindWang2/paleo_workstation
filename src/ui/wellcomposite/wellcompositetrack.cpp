#include "wellcompositetrack.h"

#include <QBitmap>
#include <cmath>

namespace WellComposite
{

// 二分查找插值曲线在目标深度处的数值
float CurveData::valueAtDepth(float d) const
{
  if (depths.size() < 2 || values.size() < 2)
    return depths.isEmpty() ? 0.0f : values.first();

  if (d <= depths.first())
    return values.first();
  if (d >= depths.last())
    return values.last();

  auto it = std::lower_bound(depths.begin(), depths.end(), d);
  if (it == depths.end())
    return values.last();

  const int idx = static_cast<int>(std::distance(depths.begin(), it));
  if (idx == 0)
    return values.first();

  const float d0 = depths.at(idx - 1);
  const float d1 = depths.at(idx);
  const float v0 = values.at(idx - 1);
  const float v1 = values.at(idx);

  if (!std::isfinite(v0) || !std::isfinite(v1))
    return std::isfinite(v0) ? v0 : (std::isfinite(v1) ? v1 : 0.0f);

  const float t = (d - d0) / qMax(1e-5f, (d1 - d0));
  return v0 + t * (v1 - v0);
}

// ----------------------------------------------------------------------------
// LithologyPatternFactory: 生成标准石油地质岩性填充纹理
// ----------------------------------------------------------------------------
QPixmap LithologyPatternFactory::createPatternPixmap(const QString &patternType, const QColor &bg, const QColor &fg)
{
  const int size = 16;
  QPixmap pm(size, size);
  pm.fill(bg);

  QPainter p(&pm);
  p.setPen(QPen(fg, 1.0));

  if (patternType == QLatin1String("sandstone")) // 砂岩：细均匀点
  {
    p.drawPoint(3, 3);
    p.drawPoint(11, 3);
    p.drawPoint(7, 7);
    p.drawPoint(3, 11);
    p.drawPoint(11, 11);
    p.drawPoint(15, 7);
  }
  else if (patternType == QLatin1String("mudstone")) // 泥岩：横向平行短细线
  {
    p.drawLine(1, 4, 7, 4);
    p.drawLine(9, 8, 15, 8);
    p.drawLine(1, 12, 7, 12);
  }
  else if (patternType == QLatin1String("limestone")) // 灰岩：错缝砖形纹
  {
    p.drawLine(0, 7, 16, 7);
    p.drawLine(0, 15, 16, 15);
    p.drawLine(7, 0, 7, 7);
    p.drawLine(15, 8, 15, 15);
  }
  else if (patternType == QLatin1String("dolomite")) // 白云岩：倾斜错缝斜砖纹
  {
    p.drawLine(0, 0, 16, 16);
    p.drawLine(0, 8, 8, 16);
    p.drawLine(8, 0, 16, 8);
  }
  else if (patternType == QLatin1String("siltstone")) // 粉砂岩：点+短虚线
  {
    p.drawPoint(4, 3);
    p.drawLine(8, 3, 13, 3);
    p.drawPoint(12, 9);
    p.drawLine(2, 9, 7, 9);
    p.drawPoint(6, 14);
    p.drawLine(10, 14, 15, 14);
  }
  else if (patternType == QLatin1String("conglomerate")) // 砾岩：小圆圈
  {
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(2, 2, 5, 4);
    p.drawEllipse(9, 8, 6, 5);
  }
  else if (patternType == QLatin1String("coal")) // 煤层：密条纹
  {
    p.fillRect(0, 2, 16, 3, fg);
    p.fillRect(0, 9, 16, 3, fg);
  }
  else // 缺省细网格
  {
    p.drawLine(0, 8, 16, 8);
  }

  p.end();
  return pm;
}

QBrush LithologyPatternFactory::getBrush(const QString &lithoName, const QColor &baseBg)
{
  QString pat = QStringLiteral("mudstone");
  QColor bg = baseBg.isValid() ? baseBg : QColor(QStringLiteral("#ECEFF1"));
  QColor fg = QColor(QStringLiteral("#455A64"));

  if (lithoName.contains(QStringLiteral("砂岩")) || lithoName.contains(QStringLiteral("砂")))
  {
    if (lithoName.contains(QStringLiteral("粉砂")))
    {
      pat = QStringLiteral("siltstone");
      if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFFDE7"));
      fg = QColor(QStringLiteral("#8D6E63"));
    }
    else if (lithoName.contains(QStringLiteral("砾")))
    {
      pat = QStringLiteral("conglomerate");
      if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFF8E1"));
      fg = QColor(QStringLiteral("#6D4C41"));
    }
    else
    {
      pat = QStringLiteral("sandstone");
      if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFF9C4")); // 砂岩浅黄
      fg = QColor(QStringLiteral("#F57F17"));
    }
  }
  else if (lithoName.contains(QStringLiteral("泥岩")) || lithoName.contains(QStringLiteral("泥")))
  {
    pat = QStringLiteral("mudstone");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#ECEFF1")); // 泥岩浅灰
    fg = QColor(QStringLiteral("#546E7A"));
  }
  else if (lithoName.contains(QStringLiteral("灰岩")))
  {
    pat = QStringLiteral("limestone");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#E0F7FA")); // 灰岩天青蓝
    fg = QColor(QStringLiteral("#00838F"));
  }
  else if (lithoName.contains(QStringLiteral("白云岩")))
  {
    pat = QStringLiteral("dolomite");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#F3E5F5")); // 白云岩淡紫
    fg = QColor(QStringLiteral("#6A1B9A"));
  }
  else if (lithoName.contains(QStringLiteral("煤")))
  {
    pat = QStringLiteral("coal");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#CFD8DC"));
    fg = QColor(QStringLiteral("#212121"));
  }

  QPixmap pm = createPatternPixmap(pat, bg, fg);
  return QBrush(pm);
}

// ----------------------------------------------------------------------------
// 1. 标尺道 (DepthScaleTrack)
// ----------------------------------------------------------------------------
DepthScaleTrack::DepthScaleTrack(qreal width)
  : m_width(width)
{
  m_title = QStringLiteral("深度 (m)");
}

void DepthScaleTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);

  // 背景
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  // 标头标题
  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont fTitle = painter.font();
  fTitle.setPointSize(9);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(headerRect.adjusted(2, 6, -2, -24), Qt::AlignCenter, title());

  // 比例尺标识（如 1:500）
  painter.setPen(QColor(QStringLiteral("#5D6E80")));
  QFont fRatio = painter.font();
  fRatio.setPointSize(8);
  fRatio.setBold(false);
  painter.setFont(fRatio);
  painter.drawText(headerRect.adjusted(2, headerRect.height() - 22, -2, -4),
                   Qt::AlignCenter, m_scaleRatio);

  painter.restore();
}

void DepthScaleTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                               double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);

  // 背景
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  const double minPixelSpacing = 36.0;
  const double targetStep = minPixelSpacing / pxPerMeter;
  static const double niceSteps[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
  double majorStep = 1000.0;
  for (double s : niceSteps)
  {
    if (s >= targetStep)
    {
      majorStep = s;
      break;
    }
  }
  const double minorStep = majorStep / 5.0;

  QFont numFont = painter.font();
  numFont.setFamily(QStringLiteral("JetBrains Mono, Noto Sans SC, monospace"));
  numFont.setPointSize(8);
  painter.setFont(numFont);

  // 绘制次刻度与主刻度
  const double firstMinor = std::floor(topDepth / minorStep) * minorStep;
  for (double d = firstMinor; d <= bottomDepth; d += minorStep)
  {
    if (d < topDepth) continue;
    const qreal y = bodyRect.top() + (d - topDepth) * pxPerMeter;
    const bool isMajor = (std::fmod(std::abs(d) + 1e-4, majorStep) < 1e-3);

    if (isMajor)
    {
      painter.setPen(QColor(QStringLiteral("#24303E")));
      painter.drawLine(QPointF(bodyRect.right() - 8, y), QPointF(bodyRect.right(), y));

      const QString lbl = QString::number(static_cast<int>(std::round(d)));
      const QRectF textRect(bodyRect.left() + 2, y - 8, bodyRect.width() - 12, 16);
      painter.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, lbl);
    }
    else
    {
      painter.setPen(QColor(QStringLiteral("#9AA7B4")));
      painter.drawLine(QPointF(bodyRect.right() - 4, y), QPointF(bodyRect.right(), y));
    }
  }

  painter.restore();
}

// ----------------------------------------------------------------------------
// 2. 文本道 (TextTrack)
// ----------------------------------------------------------------------------
TextTrack::TextTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title;
}

void TextTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont font = painter.font();
  font.setPointSize(9);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(headerRect, Qt::AlignCenter, title());
  painter.restore();
}

void TextTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont textFont = painter.font();
  textFont.setPointSize(8);
  painter.setFont(textFont);

  for (const auto &it : m_intervals)
  {
    if (it.bottomDepth < topDepth || it.topDepth > bottomDepth)
      continue;

    const qreal y0 = bodyRect.top() + (it.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (it.bottomDepth - topDepth) * pxPerMeter;
    const QRectF blockRect(bodyRect.left(), y0, bodyRect.width(), qMax<qreal>(12.0, y1 - y0));

    if (it.bgColor.alpha() > 0)
      painter.fillRect(blockRect, it.bgColor);

    // 边界分割线
    painter.setPen(QPen(QColor(QStringLiteral("#CFD8DC")), 1.0, Qt::DashLine));
    painter.drawLine(blockRect.topLeft(), blockRect.topRight());
    painter.drawLine(blockRect.bottomLeft(), blockRect.bottomRight());

    // 绘制文字
    painter.setPen(QColor(QStringLiteral("#24303E")));
    QString fullText = it.category.isEmpty() ? it.text : QStringLiteral("[%1] %2").arg(it.category, it.text);
    painter.drawText(blockRect.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, fullText);
  }

  painter.restore();
}

// ----------------------------------------------------------------------------
// 3. 地层道 (FormationTrack)
// ----------------------------------------------------------------------------
FormationTrack::FormationTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title;
}

void FormationTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont font = painter.font();
  font.setPointSize(9);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(headerRect, Qt::AlignCenter, title());
  painter.restore();
}

void FormationTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                               double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FAFAFA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont font = painter.font();
  font.setPointSize(8);
  font.setBold(true);
  painter.setFont(font);

  for (const auto &it : m_intervals)
  {
    if (it.bottomDepth < topDepth || it.topDepth > bottomDepth)
      continue;

    const qreal y0 = bodyRect.top() + (it.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (it.bottomDepth - topDepth) * pxPerMeter;
    const QRectF blockRect(bodyRect.left(), y0, bodyRect.width(), qMax<qreal>(4.0, y1 - y0));

    // 地层色块填充
    painter.fillRect(blockRect, it.color);

    // 上下实线界线
    painter.setPen(QPen(QColor(QStringLiteral("#24303E")), 1.0));
    painter.drawLine(blockRect.topLeft(), blockRect.topRight());
    painter.drawLine(blockRect.bottomLeft(), blockRect.bottomRight());

    // 居中绘制地层名称
    if (blockRect.height() > 14.0)
    {
      painter.setPen(QColor(QStringLiteral("#1A237E")));
      painter.drawText(blockRect.adjusted(2, 2, -2, -2), Qt::AlignCenter | Qt::TextWordWrap, it.name);
    }
  }

  painter.restore();
}

// ----------------------------------------------------------------------------
// 4. 岩性道 (LithologyTrack)
// ----------------------------------------------------------------------------
LithologyTrack::LithologyTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title;
}

void LithologyTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont font = painter.font();
  font.setPointSize(9);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(headerRect, Qt::AlignCenter, title());
  painter.restore();
}

void LithologyTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                               double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont font = painter.font();
  font.setPointSize(8);
  painter.setFont(font);

  for (const auto &it : m_intervals)
  {
    if (it.bottomDepth < topDepth || it.topDepth > bottomDepth)
      continue;

    const qreal y0 = bodyRect.top() + (it.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (it.bottomDepth - topDepth) * pxPerMeter;
    const QRectF blockRect(bodyRect.left(), y0, bodyRect.width(), qMax<qreal>(3.0, y1 - y0));

    // 使用标准地质岩性图案画刷填充
    const QBrush brush = LithologyPatternFactory::getBrush(it.lithoName, it.baseColor);
    painter.fillRect(blockRect, brush);

    // 岩性分界线
    painter.setPen(QPen(QColor(QStringLiteral("#78909C")), 1.0, Qt::SolidLine));
    painter.drawLine(blockRect.topLeft(), blockRect.topRight());
    painter.drawLine(blockRect.bottomLeft(), blockRect.bottomRight());

    // 高度允许时在右侧半透明贴注岩性名
    if (blockRect.height() >= 18.0)
    {
      const QRectF tagRect(blockRect.left() + 4, blockRect.top() + 2, blockRect.width() - 8, 14);
      painter.fillRect(tagRect, QColor(255, 255, 255, 180));
      painter.setPen(QColor(QStringLiteral("#263238")));
      painter.drawText(tagRect, Qt::AlignCenter, it.lithoName);
    }
  }

  painter.restore();
}

// ----------------------------------------------------------------------------
// 5. 取芯道 (CoreTrack)
// ----------------------------------------------------------------------------
CoreTrack::CoreTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title;
}

void CoreTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont font = painter.font();
  font.setPointSize(9);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(headerRect.adjusted(2, 4, -2, -18), Qt::AlignCenter, title());

  font.setPointSize(7);
  font.setBold(false);
  painter.setFont(font);
  painter.setPen(QColor(QStringLiteral("#5D6E80")));
  painter.drawText(headerRect.adjusted(2, headerRect.height() - 16, -2, -2), Qt::AlignCenter, QStringLiteral("筒号|收获率"));
  painter.restore();
}

void CoreTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont font = painter.font();
  font.setPointSize(8);
  painter.setFont(font);

  for (const auto &b : m_barrels)
  {
    if (b.bottomDepth < topDepth || b.topDepth > bottomDepth)
      continue;

    const qreal y0 = bodyRect.top() + (b.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (b.bottomDepth - topDepth) * pxPerMeter;
    const qreal h = qMax<qreal>(16.0, y1 - y0);
    const QRectF barrelRect(bodyRect.left() + 2, y0, bodyRect.width() - 4, h);

    // 外框：取芯进尺段
    painter.setPen(QPen(QColor(QStringLiteral("#2E7D32")), 1.5));
    painter.setBrush(QColor(QStringLiteral("#E8F5E9")));
    painter.drawRect(barrelRect);

    // 筒号（左侧）
    painter.setPen(QColor(QStringLiteral("#1B5E20")));
    const QRectF noRect(barrelRect.left() + 1, barrelRect.top(), 20, barrelRect.height());
    painter.drawText(noRect, Qt::AlignCenter, b.barrelNo.isEmpty() ? QStringLiteral("C") : b.barrelNo);

    // 心长收获率条带（右侧双色柱）
    const qreal barLeft = barrelRect.left() + 22;
    const qreal barWidth = barrelRect.width() - 24;
    const float rate = qBound(0.0f, b.recoveryRate > 0.0f ? b.recoveryRate : (b.cutLength > 0.0f ? (b.recoveredLength / b.cutLength * 100.0f) : 100.0f), 100.0f);
    const qreal recWidth = barWidth * (rate / 100.0);

    // 填充实测心长部分
    painter.fillRect(QRectF(barLeft, barrelRect.top() + 2, recWidth, barrelRect.height() - 4), QColor(QStringLiteral("#43A047")));

    // 百分比文字
    if (barrelRect.height() >= 14.0)
    {
      painter.setPen(QColor(QStringLiteral("#24303E")));
      const QRectF rateTextRect(barLeft, barrelRect.top(), barWidth, barrelRect.height());
      painter.drawText(rateTextRect, Qt::AlignCenter, QStringLiteral("%1%").arg(qRound(rate)));
    }
  }

  painter.restore();
}

// ----------------------------------------------------------------------------
// 6. 图片道 (ImageTrack)
// ----------------------------------------------------------------------------
ImageTrack::ImageTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title;
}

void ImageTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont font = painter.font();
  font.setPointSize(9);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(headerRect, Qt::AlignCenter, title());
  painter.restore();
}

void ImageTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FAFAFA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  for (const auto &item : m_items)
  {
    if (item.bottomDepth < topDepth || item.topDepth > bottomDepth)
      continue;

    const qreal y0 = bodyRect.top() + (item.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (item.bottomDepth - topDepth) * pxPerMeter;
    const qreal h = qMax<qreal>(20.0, y1 - y0);
    const QRectF imgRect(bodyRect.left() + 2, y0, bodyRect.width() - 4, h);

    if (!item.pixmap.isNull())
    {
      painter.drawPixmap(imgRect.toRect(), item.pixmap);
    }
    else
    {
      painter.fillRect(imgRect, QColor(QStringLiteral("#E0E0E0")));
      painter.setPen(QColor(QStringLiteral("#757575")));
      painter.drawText(imgRect, Qt::AlignCenter, item.caption.isEmpty() ? QStringLiteral("照片") : item.caption);
    }
    painter.setPen(QColor(QStringLiteral("#B0BEC5")));
    painter.drawRect(imgRect);
  }

  painter.restore();
}

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

void CurveTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth)
{
  painter.save();
  painter.setClipRect(headerRect);

  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  // 主标题
  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont fTitle = painter.font();
  fTitle.setPointSize(9);
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
    fCurve.setFamily(QStringLiteral("JetBrains Mono, Noto Sans SC, monospace"));
    fCurve.setPointSize(7);
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
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));

  // 浅灰色垂直等分网格线（4等分）
  painter.setPen(QPen(QColor(QStringLiteral("#F1F3F5")), 1.0, Qt::DashLine));
  for (int div = 1; div < 4; ++div)
  {
    const qreal gx = bodyRect.left() + bodyRect.width() * (div / 4.0);
    painter.drawLine(QPointF(gx, bodyRect.top()), QPointF(gx, bodyRect.bottom()));
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
        painter.drawEllipse(QPointF(x, y), 3.0, 3.0);
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
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
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
  painter.save();
  painter.setClipRect(headerRect);
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont font = painter.font();
  font.setPointSize(9);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(headerRect, Qt::AlignCenter, title());
  painter.restore();
}

void SymbolTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                           double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
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
