// 层：视图
// 综合图道·基础六道（标尺/文本/地层/岩性/取芯/图片）与道内数据访问器——自 wellcompositetrack.cpp 拆出（方向 66，行为零变更）
#include "wellcompositetrack.h"
#include "services/imagelod.h"
#include "ui/paleotheme.h"
#include <QCoreApplication>
#include <cmath>

namespace WellComposite
{
// ----------------------------------------------------------------------------
// 1. 标尺道 (DepthScaleTrack)
// ----------------------------------------------------------------------------
DepthScaleTrack::DepthScaleTrack(qreal width)
  : m_width(width)
{
  m_title = QCoreApplication::translate("WellCompositeTrack", "深度 (m)");
}

void DepthScaleTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);

  // 背景
  painter.fillRect(headerRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surfaceAlt);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  // 标头标题
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
  QFont fTitle = painter.font();
  fTitle.setPointSize(PaleoTheme::tokens().bodyPt);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  const QString unitSuffix = m_depthUnitLabel.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_depthUnitLabel);
  painter.drawText(headerRect.adjusted(2, 6, -2, -24), Qt::AlignCenter, title() + unitSuffix);

  // 比例尺标识（如 1:500）
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).textMuted);
  QFont fRatio = painter.font();
  fRatio.setPointSize(PaleoTheme::tokens().labelPt);
  fRatio.setBold(false);
  painter.setFont(fRatio);
  painter.drawText(headerRect.adjusted(2, headerRect.height() - 22, -2, -4),
                   Qt::AlignCenter, m_scaleRatio);

  // 单位行（D6.3 切 ft 时显示换算标注）
  if (!m_depthUnitLabel.isEmpty())
  {
    painter.setPen(QColor(QStringLiteral("#9AA7B4")));
    QFont fUnit = painter.font();
    fUnit.setPointSize(PaleoTheme::tokens().labelPt);
    painter.setFont(fUnit);
    painter.drawText(headerRect.adjusted(2, headerRect.height() - 8, -2, -1),
                     Qt::AlignCenter, m_depthUnitLabel);
  }

  painter.restore();
}

void DepthScaleTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                               double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);

  // 背景
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
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
  numFont = PaleoTheme::monoFont(numFont.pointSize());
  numFont.setStyleHint(QFont::TypeWriter);
  numFont.setPointSize(PaleoTheme::tokens().labelPt);
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
      painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
      painter.drawLine(QPointF(bodyRect.right() - 8, y), QPointF(bodyRect.right(), y));

      const QString lbl = QString::number(static_cast<int>(std::round(d)));
      const QRectF textRect(bodyRect.left() + 2, y - 8, bodyRect.width() - 12, 16);
      painter.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, lbl);
    }
    else
    {
      // D7.4 高对比：次刻度从 disabled 灰提升到 text-muted
      painter.setPen(QColor(m_highContrast ? QStringLiteral("#5D6E80") : QStringLiteral("#9AA7B4")));
      painter.drawLine(QPointF(bodyRect.right() - 4, y), QPointF(bodyRect.right(), y));
    }
  }

  // D6.5 TWT 副刻度列：左半列标注时深值（右侧主刻度不受影响）
  if (!m_twtLabels.isEmpty())
  {
    painter.setPen(QColor(m_highContrast ? QStringLiteral("#24303E") : QStringLiteral("#5D6E80")));
    QFont twtFont = painter.font();
    twtFont.setPointSize(PaleoTheme::tokens().labelPt);
    painter.setFont(twtFont);
    for (const auto &pair : m_twtLabels)
    {
      if (pair.first < topDepth || pair.first > bottomDepth)
        continue;
      const qreal y = bodyRect.top() + (pair.first - topDepth) * pxPerMeter;
      painter.drawText(QRectF(bodyRect.left() + 1, y - 8, bodyRect.width() * 0.45, 16),
                       Qt::AlignLeft | Qt::AlignVCenter, pair.second);
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
  paintHeaderChrome(painter, headerRect, QString(), QStringLiteral("m"));
}

void TextTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont textFont = painter.font();
  textFont.setPointSize(PaleoTheme::tokens().labelPt);
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
    painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
    QString fullText = it.category.isEmpty() ? it.text : QStringLiteral("[%1] %2").arg(it.category, it.text);
    const QRectF textRect = m_keepTextVisible ? blockRect.intersected(bodyRect) : blockRect;
    painter.drawText(textRect.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, fullText);
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
  paintHeaderChrome(painter, headerRect, QStringLiteral("顶深-底深 m"), QString());
}

void FormationTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                               double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FAFAFA")));
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont font = painter.font();
  font.setPointSize(PaleoTheme::tokens().labelPt);
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
    painter.setPen(QPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text, 1.0));
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
  paintHeaderChrome(painter, headerRect, QStringLiteral("岩性花纹"), QString());
}

void LithologyTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                               double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont font = painter.font();
  font.setPointSize(PaleoTheme::tokens().labelPt);
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
  paintHeaderChrome(painter, headerRect,
                    QCoreApplication::translate("WellCompositeTrack", "筒号|进尺|心长"),
                    QCoreApplication::translate("WellCompositeTrack", "收获率 %"));
}

void CoreTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont font = painter.font();
  font.setPointSize(PaleoTheme::tokens().labelPt);
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
      painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
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
  paintHeaderChrome(painter, headerRect, QStringLiteral("深度等比"), QString());
}

void ImageTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true); // minification 质量（方向 79）
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FAFAFA")));
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  const qreal tw = bodyRect.width() - 4.0;
  for (const auto &item : m_items)
  {
    if (item.bottomDepth < topDepth || item.topDepth > bottomDepth)
      continue;

    const qreal anchorY = bodyRect.top() + (item.topDepth - topDepth) * pxPerMeter;
    // 井段照片（top<bottom）纵向按深度区间等比；单点锚照片（top==bottom，
    // 当前唯一数据形态）以锚深为中心、宽度撑满按原图纵横比定高——
    // 任何情况都不做非等比拉伸。
    const qreal spanH = (item.bottomDepth - item.topDepth) * pxPerMeter;
    QRectF imgRect;

    if (!item.pixmap.isNull())
    {
      const QSizeF want = item.pixmap.size();
      if (spanH > 1.0)
      {
        const QSizeF fitted = want.scaled(QSizeF(tw, spanH), Qt::KeepAspectRatio);
        imgRect = QRectF(bodyRect.left() + 2.0 + (tw - fitted.width()) / 2.0,
                         anchorY + (spanH - fitted.height()) / 2.0,
                         fitted.width(), fitted.height());
      }
      else
      {
        // 超高（窄长）照片封顶 4 倍道宽并等比缩窄，避免吃掉整道。
        const qreal naturalH = tw * want.height() / want.width();
        const qreal drawH = qMin(naturalH, tw * 4.0);
        const qreal drawW = tw * (drawH / naturalH);
        imgRect = QRectF(bodyRect.left() + 2.0 + (tw - drawW) / 2.0,
                         anchorY - drawH / 2.0, drawW, drawH);
      }
      // 透明图垫中性灰棋盘底（方向 79：纸面图件口径，不随暗色翻转）。
      if (item.pixmap.hasAlpha())
        painter.fillRect(imgRect, paleo::imagelod::alphaCheckerboard());
      painter.drawPixmap(imgRect, item.pixmap, QRectF(item.pixmap.rect()));
    }
    else
    {
      const qreal h = qMax<qreal>(20.0, spanH);
      imgRect = QRectF(bodyRect.left() + 2.0,
                       anchorY - (spanH > 1.0 ? 0.0 : h / 2.0), tw, h);
      painter.fillRect(imgRect, QColor(QStringLiteral("#E0E0E0")));
      painter.setPen(QColor(QStringLiteral("#757575")));
      painter.drawText(imgRect, Qt::AlignCenter, item.caption.isEmpty() ? QCoreApplication::translate("WellCompositeTrack", "照片") : item.caption);
    }
    painter.setPen(QColor(QStringLiteral("#B0BEC5")));
    painter.drawRect(imgRect);
  }

  painter.restore();
}

// ----------------------------------------------------------------------------
// D1.9/D3.x 道内数据访问器（tooltip 与编辑会话共用）
// ----------------------------------------------------------------------------
QString DepthScaleTrack::trackToolTip(double depth) const
{
  return QStringLiteral("%1: %2%3")
      .arg(title(), QString::number(depth, 'f', 1),
           m_depthUnitLabel.isEmpty() ? QStringLiteral(" m") : QStringLiteral(" ") + m_depthUnitLabel);
}

int FormationTrack::intervalIndexAtDepth(float depth) const
{
  for (int i = 0; i < m_intervals.size(); ++i)
  {
    if (depth >= m_intervals.at(i).topDepth && depth <= m_intervals.at(i).bottomDepth)
      return i;
  }
  return -1;
}

FormationInterval *FormationTrack::intervalAtDepth(float depth)
{
  const int idx = intervalIndexAtDepth(depth);
  return idx >= 0 ? &m_intervals[idx] : nullptr;
}

QString FormationTrack::trackToolTip(double depth) const
{
  const int idx = intervalIndexAtDepth(depth);
  if (idx < 0)
    return title();
  const auto &fi = m_intervals.at(idx);
  return QStringLiteral("%1\n%2\n%3 ~ %4 m")
      .arg(title(), fi.name,
           QString::number(fi.topDepth, 'f', 1), QString::number(fi.bottomDepth, 'f', 1));
}

int LithologyTrack::intervalIndexAtDepth(float depth) const
{
  for (int i = 0; i < m_intervals.size(); ++i)
  {
    if (depth >= m_intervals.at(i).topDepth && depth <= m_intervals.at(i).bottomDepth)
      return i;
  }
  return -1;
}

bool LithologyTrack::replaceIntervalAt(int idx, const LithologyInterval &interval)
{
  if (idx < 0 || idx >= m_intervals.size())
    return false;
  m_intervals[idx] = interval;
  return true;
}

bool LithologyTrack::removeIntervalAt(int idx)
{
  if (idx < 0 || idx >= m_intervals.size())
    return false;
  m_intervals.removeAt(idx);
  return true;
}

QString LithologyTrack::trackToolTip(double depth) const
{
  const int idx = intervalIndexAtDepth(depth);
  if (idx < 0)
    return title();
  const auto &li = m_intervals.at(idx);
  return QStringLiteral("%1\n%2\n%3 ~ %4 m")
      .arg(title(), li.lithoName,
           QString::number(li.topDepth, 'f', 1), QString::number(li.bottomDepth, 'f', 1));
}

} // namespace WellComposite
