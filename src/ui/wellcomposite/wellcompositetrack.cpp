// 层：视图
#include "wellcompositetrack.h"
#include "patterncatalog.h"
#include "../../domain/faciescatalog.h"
#include <QCoreApplication>
#include <QSvgRenderer>

#include <QBitmap>
#include <QPainterPath>
#include <cmath>

namespace WellComposite
{

// ----------------------------------------------------------------------------
// WellTrack 公共道头三行区（D1.11）：标题 | 刻度 | 单位，随道宽自适应截断
// ----------------------------------------------------------------------------
void WellTrack::paintHeaderChrome(QPainter &painter, const QRectF &headerRect,
                                  const QString &scaleText, const QString &unitText) const
{
  painter.save();
  painter.setClipRect(headerRect);

  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  const bool hasScale = !scaleText.isEmpty();
  const bool hasUnit = !unitText.isEmpty();

  // 三行布局：标题 26px 置顶，刻度/单位行按存在性均分余高；无副行时标题纵向居中
  const qreal titleBandH = hasScale || hasUnit ? qMin<qreal>(28.0, headerRect.height() * 0.45)
                                               : headerRect.height();
  const QRectF titleBand(headerRect.left() + 2, headerRect.top() + 4,
                         headerRect.width() - 4, titleBandH - 6);
  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont fTitle = painter.font();
  fTitle.setPointSize(9);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(titleBand, Qt::AlignCenter | Qt::TextWrapAnywhere,
                   elideTitle(painter, title(), titleBand.width()));

  QFont fSub = painter.font();
  fSub.setPointSize(8);
  fSub.setBold(false);
  painter.setFont(fSub);

  qreal rowTop = headerRect.top() + titleBandH;
  const qreal rowH = (headerRect.height() - titleBandH) / (hasScale + hasUnit ? (hasScale && hasUnit ? 2.0 : 1.0) : 1.0);
  const QFontMetrics fm(fSub);
  const auto drawRow = [&](const QString &raw, bool isUnit) {
    if (raw.isEmpty()) return;
    const QRectF rowRect(headerRect.left() + 3, rowTop, headerRect.width() - 6, rowH);
    painter.setPen(QColor(isUnit ? QStringLiteral("#5D6E80") : QStringLiteral("#455A64")));
    painter.drawText(rowRect, Qt::AlignCenter,
                     fm.elidedText(raw, Qt::ElideRight, static_cast<int>(rowRect.width())));
    rowTop += rowH;
  };
  drawRow(scaleText, false);
  drawRow(unitText, true);

  painter.restore();
}

QString WellTrack::elideTitle(const QPainter &painter, const QString &text, qreal widthPx) const
{
  const QFontMetrics fm(painter.font());
  if (fm.horizontalAdvance(text) <= widthPx)
    return text;
  // 两行机会：先按最长行宽折半截断（竖排语义由窄道自适配合并到一行截断）
  return fm.elidedText(text, Qt::ElideMiddle, static_cast<int>(widthPx));
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
  // D4.2 花纹库扩充：30+ 种岩性先经 PatternCatalog 关键词最长匹配
  PatternDef def;
  if (PatternCatalog::lookupLithology(lithoName, &def))
  {
    const QColor bg = baseBg.isValid() ? baseBg : def.bg;
    QPixmap pm = createPatternPixmap(def.key, bg, def.fg);
    if (pm.isNull())
      pm = PatternCatalog::createLithoPattern(def.key, bg, def.fg);
    return QBrush(pm);
  }

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
// FaciesPatternFactory: 生成标准石油地质沉积相填充纹理 (相/亚相/微相)
// ----------------------------------------------------------------------------
QPixmap FaciesPatternFactory::createPatternPixmap(const QString &patternType, const QColor &bg, const QColor &fg)
{
  const int size = 20;
  QPixmap pm(size, size);
  pm.fill(bg);

  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(QPen(fg, 1.0));

  if (patternType == QLatin1String("distributary_channel")) // 水下分流河道：前积交错层理细弧线 + 砂粒散点
  {
    p.setBrush(Qt::NoBrush);
    p.drawArc(-4, 0, 24, 16, 20 * 16, 110 * 16);
    p.drawArc(6, 6, 20, 14, 20 * 16, 110 * 16);
    p.setPen(QPen(fg, 1.2));
    p.drawPoint(5, 15);
    p.drawPoint(12, 17);
    p.drawPoint(16, 4);
    p.drawPoint(3, 7);
  }
  else if (patternType == QLatin1String("mouth_bar")) // 河口坝：向上凸起的坝状双透镜微弧线 + 密砂点
  {
    p.setBrush(Qt::NoBrush);
    p.drawArc(-2, 4, 24, 12, 0, 180 * 16);
    p.drawArc(2, 12, 20, 10, 0, 180 * 16);
    p.setPen(QPen(fg, 1.2));
    p.drawPoint(10, 3);
    p.drawPoint(4, 9);
    p.drawPoint(16, 9);
    p.drawPoint(10, 15);
  }
  else if (patternType == QLatin1String("sheet_sand")) // 席状砂：稀疏细水平薄层线 + 均匀散点
  {
    p.drawLine(0, 5, 20, 5);
    p.drawLine(0, 15, 20, 15);
    p.setPen(QPen(fg, 1.2));
    p.drawPoint(5, 10);
    p.drawPoint(15, 10);
    p.drawPoint(10, 1);
    p.drawPoint(10, 19);
  }
  else if (patternType == QLatin1String("interdistributary_bay")) // 分流间湾：缓波浪形泥质微纹
  {
    p.setBrush(Qt::NoBrush);
    QPainterPath path1;
    path1.moveTo(0, 6);
    path1.cubicTo(5, 3, 10, 9, 20, 6);
    p.drawPath(path1);

    QPainterPath path2;
    path2.moveTo(0, 16);
    path2.cubicTo(5, 13, 10, 19, 20, 16);
    p.drawPath(path2);
  }
  else if (patternType == QLatin1String("delta_front")) // 三角洲前缘：45度斜向交错砂泥互层纹
  {
    p.drawLine(0, 10, 10, 0);
    p.drawLine(0, 20, 20, 0);
    p.drawLine(10, 20, 20, 10);
    p.setPen(QPen(fg, 1.2));
    p.drawPoint(5, 5);
    p.drawPoint(15, 15);
  }
  else if (patternType == QLatin1String("delta_plain")) // 三角洲平原：垂直植物根迹/炭质线与杂基点
  {
    p.drawLine(5, 2, 5, 10);
    p.drawLine(15, 8, 15, 18);
    p.setPen(QPen(fg, 1.2));
    p.drawPoint(5, 14);
    p.drawPoint(15, 4);
    p.drawPoint(10, 10);
  }
  else if (patternType == QLatin1String("prodelta")) // 前三角洲：密集的水平平直极薄页理线
  {
    p.drawLine(0, 4, 20, 4);
    p.drawLine(0, 9, 20, 9);
    p.drawLine(0, 14, 20, 14);
    p.drawLine(0, 19, 20, 19);
  }
  else if (patternType == QLatin1String("shallow_marine")) // 浅海陆棚：波状水流波痕与微波浪纹
  {
    p.setBrush(Qt::NoBrush);
    QPainterPath pWave;
    pWave.moveTo(0, 7);
    pWave.quadTo(5, 2, 10, 7);
    pWave.quadTo(15, 12, 20, 7);
    p.drawPath(pWave);

    QPainterPath pWave2;
    pWave2.moveTo(0, 17);
    pWave2.quadTo(5, 12, 10, 17);
    pWave2.quadTo(15, 22, 20, 17);
    p.drawPath(pWave2);
  }
  else if (patternType == QLatin1String("turbidite")) // 浊积砂体/重力流：底粗顶细的正粒序递变点阵
  {
    p.setPen(QPen(fg, 1.0));
    p.drawPoint(3, 3);
    p.drawPoint(10, 4);
    p.drawPoint(17, 3);

    p.setPen(QPen(fg, 1.5));
    p.drawPoint(6, 10);
    p.drawPoint(14, 11);

    p.setPen(QPen(fg, 2.2));
    p.drawPoint(4, 17);
    p.drawPoint(10, 16);
    p.drawPoint(16, 17);
  }
  else if (patternType == QLatin1String("channel_lag")) // 滞留沉积：椭圆砾石 + 粗砂粒
  {
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(2, 3, 6, 4);
    p.drawEllipse(11, 11, 7, 5);
    p.setPen(QPen(fg, 1.5));
    p.drawPoint(14, 4);
    p.drawPoint(4, 14);
  }
  else if (patternType == QLatin1String("tidal_flat")) // 潮坪：人字形双向羽状交错层理
  {
    p.drawLine(2, 4, 8, 8);
    p.drawLine(8, 8, 14, 4);
    p.drawLine(6, 14, 12, 18);
    p.drawLine(12, 18, 18, 14);
  }
  else // 默认沉积微纹理：点线相间
  {
    p.drawLine(0, 10, 20, 10);
    p.setPen(QPen(fg, 1.2));
    p.drawPoint(5, 4);
    p.drawPoint(15, 4);
    p.drawPoint(5, 16);
    p.drawPoint(15, 16);
  }

  p.end();
  return pm;
}

QBrush FaciesPatternFactory::getBrush(const QString &patternTypeOrName, const QColor &baseBg)
{
  const auto path = FaciesCatalog::resourcePath(patternTypeOrName);
  if (!path.isEmpty()) {
    QPixmap tile(96, 48);
    tile.fill(baseBg.isValid() ? baseBg : Qt::white);
    QSvgRenderer svg(path);
    QPainter painter(&tile);
    svg.render(&painter);
    painter.end();
    return QBrush(tile);
  }

  QString pat = QStringLiteral("distributary_channel");
  QColor bg = baseBg.isValid() ? baseBg : QColor(QStringLiteral("#FFE082"));
  QColor fg = QColor(QStringLiteral("#E65100"));

  const QString key = patternTypeOrName.trimmed();

  // D4.3 相名→花纹外置映射（内置 + 用户 JSON 覆盖）优先
  const QString mapped = PatternCatalog::faciesPatternKey(key);
  if (!mapped.isEmpty())
  {
    pat = mapped;
    if (!baseBg.isValid())
      bg = QColor(QStringLiteral("#FFE082"));
    fg = QColor(QStringLiteral("#E65100"));
    QPixmap pm = createPatternPixmap(pat, bg, fg);
    if (pm.isNull())
      pm = PatternCatalog::createLithoPattern(pat, bg, fg);
    return QBrush(pm);
  }

  if (key == QLatin1String("distributary_channel") || key.contains(QStringLiteral("分流河道")) || key.contains(QStringLiteral("水下河道")))
  {
    pat = QStringLiteral("distributary_channel");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFE082"));
    fg = QColor(QStringLiteral("#E65100"));
  }
  else if (key == QLatin1String("mouth_bar") || key.contains(QStringLiteral("河口坝")) || key.contains(QStringLiteral("沙坝")))
  {
    pat = QStringLiteral("mouth_bar");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFF176"));
    fg = QColor(QStringLiteral("#F57F17"));
  }
  else if (key == QLatin1String("sheet_sand") || key.contains(QStringLiteral("席状砂")) || key.contains(QStringLiteral("远砂坝")))
  {
    pat = QStringLiteral("sheet_sand");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFF9C4"));
    fg = QColor(QStringLiteral("#F9A825"));
  }
  else if (key == QLatin1String("interdistributary_bay") || key.contains(QStringLiteral("间湾")) || key.contains(QStringLiteral("分流间")))
  {
    pat = QStringLiteral("interdistributary_bay");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#C8E6C9"));
    fg = QColor(QStringLiteral("#2E7D32"));
  }
  else if (key == QLatin1String("delta_front") || key.contains(QStringLiteral("前缘")))
  {
    pat = QStringLiteral("delta_front");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFE0B2"));
    fg = QColor(QStringLiteral("#D84315"));
  }
  else if (key == QLatin1String("delta_plain") || key.contains(QStringLiteral("平原")) || key.contains(QStringLiteral("沼泽")))
  {
    pat = QStringLiteral("delta_plain");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#E6EE9C"));
    fg = QColor(QStringLiteral("#33691E"));
  }
  else if (key == QLatin1String("prodelta") || key.contains(QStringLiteral("前三角洲")) || key.contains(QStringLiteral("半深湖")) || key.contains(QStringLiteral("深湖")))
  {
    pat = QStringLiteral("prodelta");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#B0BEC5"));
    fg = QColor(QStringLiteral("#37474F"));
  }
  else if (key == QLatin1String("shallow_marine") || key.contains(QStringLiteral("浅海")) || key.contains(QStringLiteral("陆棚")) || key.contains(QStringLiteral("滨海")) || key.contains(QStringLiteral("台地")))
  {
    pat = QStringLiteral("shallow_marine");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#80DEEA"));
    fg = QColor(QStringLiteral("#006064"));
  }
  else if (key == QLatin1String("turbidite") || key.contains(QStringLiteral("浊积")) || key.contains(QStringLiteral("重力流")) || key.contains(QStringLiteral("扇")))
  {
    pat = QStringLiteral("turbidite");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFCC80"));
    fg = QColor(QStringLiteral("#BF360C"));
  }
  else if (key == QLatin1String("channel_lag") || key.contains(QStringLiteral("滞留")))
  {
    pat = QStringLiteral("channel_lag");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#FFE082"));
    fg = QColor(QStringLiteral("#5D4037"));
  }
  else if (key == QLatin1String("tidal_flat") || key.contains(QStringLiteral("潮坪")) || key.contains(QStringLiteral("潮道")))
  {
    pat = QStringLiteral("tidal_flat");
    if (!baseBg.isValid()) bg = QColor(QStringLiteral("#D7CCC8"));
    fg = QColor(QStringLiteral("#4E342E"));
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
  m_title = QCoreApplication::translate("WellCompositeTrack", "深度 (m)");
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
  const QString unitSuffix = m_depthUnitLabel.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_depthUnitLabel);
  painter.drawText(headerRect.adjusted(2, 6, -2, -24), Qt::AlignCenter, title() + unitSuffix);

  // 比例尺标识（如 1:500）
  painter.setPen(QColor(QStringLiteral("#5D6E80")));
  QFont fRatio = painter.font();
  fRatio.setPointSize(8);
  fRatio.setBold(false);
  painter.setFont(fRatio);
  painter.drawText(headerRect.adjusted(2, headerRect.height() - 22, -2, -4),
                   Qt::AlignCenter, m_scaleRatio);

  // 单位行（D6.3 切 ft 时显示换算标注）
  if (!m_depthUnitLabel.isEmpty())
  {
    painter.setPen(QColor(QStringLiteral("#9AA7B4")));
    QFont fUnit = painter.font();
    fUnit.setPointSize(7);
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
    twtFont.setPointSize(7);
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
  paintHeaderChrome(painter, headerRect, QStringLiteral("顶深-底深 m"), QString());
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
  paintHeaderChrome(painter, headerRect, QStringLiteral("岩性花纹"), QString());
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
  paintHeaderChrome(painter, headerRect,
                    QCoreApplication::translate("WellCompositeTrack", "筒号|进尺|心长"),
                    QCoreApplication::translate("WellCompositeTrack", "收获率 %"));
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
  paintHeaderChrome(painter, headerRect, QStringLiteral("深度等比"), QString());
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
      painter.drawText(imgRect, Qt::AlignCenter, item.caption.isEmpty() ? QCoreApplication::translate("WellCompositeTrack", "照片") : item.caption);
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
  paintHeaderChrome(painter, headerRect, QStringLiteral("段/点符号"), QString());
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

// ----------------------------------------------------------------------------
// 9. 地层系统组组合道 (StratigraphyCompoundTrack)
// ----------------------------------------------------------------------------
StratigraphyCompoundTrack::StratigraphyCompoundTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title.isEmpty() ? QCoreApplication::translate("WellCompositeTrack", "地层") : title;
}

void StratigraphyCompoundTrack::setSubColumnWidths(qreal sysW, qreal serW)
{
  m_systemWidth = qMax(20.0, sysW);
  m_seriesWidth = qMax(20.0, serW);
}

void StratigraphyCompoundTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);

  // 背景
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  const qreal topH = std::floor(headerRect.height() * 0.5);
  const qreal botH = headerRect.height() - topH;

  // 顶层合并道头：「地层」
  const QRectF topRect(headerRect.left(), headerRect.top(), headerRect.width(), topH);
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(topRect.bottomLeft(), topRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont fTitle = painter.font();
  fTitle.setPointSize(9);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(topRect, Qt::AlignCenter, title());

  // 底层次级道头：3 列分栏「系 | 统 | 组」
  const qreal col1W = m_systemWidth;
  const qreal col2W = m_seriesWidth;
  const qreal col3W = qMax<qreal>(20.0, headerRect.width() - col1W - col2W);

  const QRectF rSys(headerRect.left(), headerRect.top() + topH, col1W, botH);
  const QRectF rSer(rSys.right(), headerRect.top() + topH, col2W, botH);
  const QRectF rForm(rSer.right(), headerRect.top() + topH, col3W, botH);

  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(rSys.topRight(), rSys.bottomRight());
  painter.drawLine(rSer.topRight(), rSer.bottomRight());

  QFont fSub = painter.font();
  fSub.setPointSize(8);
  fSub.setBold(true);
  painter.setFont(fSub);
  painter.setPen(QColor(QStringLiteral("#5D6E80"))); // text-muted

  painter.drawText(rSys, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "系"));
  painter.drawText(rSer, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "统"));
  painter.drawText(rForm, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "组"));

  painter.restore();
}

void StratigraphyCompoundTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));

  const qreal col1W = m_systemWidth;
  const qreal col2W = m_seriesWidth;
  const qreal col3W = qMax<qreal>(20.0, bodyRect.width() - col1W - col2W);

  const qreal col1X = bodyRect.left();
  const qreal col2X = col1X + col1W;
  const qreal col3X = col2X + col2W;

  // 绘制竖向分割线
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(QPointF(col2X, bodyRect.top()), QPointF(col2X, bodyRect.bottom()));
  painter.drawLine(QPointF(col3X, bodyRect.top()), QPointF(col3X, bodyRect.bottom()));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont fBody = painter.font();
  fBody.setPointSize(8);
  fBody.setBold(true);
  painter.setFont(fBody);

  // 1. 绘制「系」：对连续相同系名称进行跨层合并绘制
  struct MergedSys {
    float topD = 0;
    float botD = 0;
    QString name;
    QColor color;
  };
  QVector<MergedSys> sysGroups;
  for (const auto &it : m_intervals)
  {
    if (!sysGroups.isEmpty() && sysGroups.last().name == it.system)
    {
      sysGroups.last().botD = qMax(sysGroups.last().botD, it.bottomDepth);
    }
    else
    {
      sysGroups.append({it.topDepth, it.bottomDepth, it.system, it.systemColor});
    }
  }

  for (const auto &grp : sysGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col1X, y0, col1W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#78909C")), 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.name.isEmpty())
    {
      painter.setPen(QColor(QStringLiteral("#24303E")));
      QString dt = grp.name;
      if (textBox.height() >= 45.0 && col1W <= 42.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.name) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 2. 绘制「统」：对连续相同统名称进行跨层合并绘制
  struct MergedSer {
    float topD = 0;
    float botD = 0;
    QString name;
    QColor color;
  };
  QVector<MergedSer> serGroups;
  for (const auto &it : m_intervals)
  {
    if (!serGroups.isEmpty() && serGroups.last().name == it.series)
    {
      serGroups.last().botD = qMax(serGroups.last().botD, it.bottomDepth);
    }
    else
    {
      serGroups.append({it.topDepth, it.bottomDepth, it.series, it.seriesColor});
    }
  }

  for (const auto &grp : serGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col2X, y0, col2W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#90A4AE")), 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.name.isEmpty())
    {
      painter.setPen(QColor(QStringLiteral("#24303E")));
      QString dt = grp.name;
      if (textBox.height() >= 45.0 && col2W <= 46.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.name) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 3. 绘制「组」：具体地层分层
  for (const auto &it : m_intervals)
  {
    if (it.bottomDepth < topDepth || it.topDepth > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (it.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (it.bottomDepth - topDepth) * pxPerMeter;
    const QRectF box(col3X, y0, col3W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, it.formationColor);
    painter.setPen(QPen(QColor(QStringLiteral("#24303E")), 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 14.0 && !it.formation.isEmpty())
    {
      painter.setPen(QColor(QStringLiteral("#1A237E")));
      painter.drawText(textBox.adjusted(2, 2, -2, -2), Qt::AlignCenter | Qt::TextWordWrap, it.formation);
    }
  }

  painter.restore();
}

void StratigraphyCompoundTrack::autoDeriveStratigraphy(const QVector<FormationInterval> &formations,
                                                      double minDepth, double maxDepth)
{
  Q_UNUSED(minDepth);
  Q_UNUSED(maxDepth);
  m_intervals.clear();

  if (!formations.isEmpty())
  {
    for (const auto &f : formations)
    {
      StratigraphyInterval si;
      si.topDepth = f.topDepth;
      si.bottomDepth = f.bottomDepth;
      si.formation = f.name;
      si.formationColor = f.color;

      const QString n = f.name.trimmed();
      if (n.contains(QStringLiteral("粤海")) || n.contains(QStringLiteral("万山")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("上新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFF59D"));
      }
      else if (n.contains(QStringLiteral("韩江")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("中新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFE082"));
      }
      else if (n.contains(QStringLiteral("珠江")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("早中新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFD54F"));
      }
      else if (n.contains(QStringLiteral("珠海")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("渐新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFCC80"));
      }
      else if (n.contains(QStringLiteral("恩平")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("始新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFA726"));
      }
      else if (n.contains(QStringLiteral("文昌")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("始新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFA726"));
      }
      else if (n.contains(QStringLiteral("新近")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("中新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFE082"));
      }
      else if (n.contains(QStringLiteral("古近")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("古新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFCC80"));
      }
      else if (n.contains(QStringLiteral("白垩")))
      {
        si.system = QStringLiteral("白垩系");
        si.series = QStringLiteral("上白垩统");
        si.systemColor = QColor(QStringLiteral("#E8F5E9"));
        si.seriesColor = QColor(QStringLiteral("#C8E6C9"));
      }
      // 未识别层名：不臆造系/统，仅显示真实层名
      m_intervals.append(si);
    }
  }
}

// ----------------------------------------------------------------------------
// 10. 沉积相组合道 (FaciesCompoundTrack)
// ----------------------------------------------------------------------------
FaciesCompoundTrack::FaciesCompoundTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title.isEmpty() ? QCoreApplication::translate("WellCompositeTrack", "沉积相") : title;
}

void FaciesCompoundTrack::setSubColumnWidths(qreal majW, qreal subW)
{
  m_majorWidth = qMax(20.0, majW);
  m_subWidth = qMax(20.0, subW);
}

void FaciesCompoundTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);

  // 背景
  painter.fillRect(headerRect, QColor(QStringLiteral("#F5F7FA")));
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  const qreal topH = std::floor(headerRect.height() * 0.5);
  const qreal botH = headerRect.height() - topH;

  // 顶层合并道头：「沉积相」
  const QRectF topRect(headerRect.left(), headerRect.top(), headerRect.width(), topH);
  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(topRect.bottomLeft(), topRect.bottomRight());

  painter.setPen(QColor(QStringLiteral("#24303E")));
  QFont fTitle = painter.font();
  fTitle.setPointSize(9);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(topRect, Qt::AlignCenter, title());

  // 底层次级道头：3 列分栏「相 | 亚 | 微」
  const qreal col1W = m_majorWidth;
  const qreal col2W = m_subWidth;
  const qreal col3W = qMax<qreal>(20.0, headerRect.width() - col1W - col2W);

  const QRectF rMaj(headerRect.left(), headerRect.top() + topH, col1W, botH);
  const QRectF rSub(rMaj.right(), headerRect.top() + topH, col2W, botH);
  const QRectF rMic(rSub.right(), headerRect.top() + topH, col3W, botH);

  painter.setPen(QColor(QStringLiteral("#DFE5EC")));
  painter.drawLine(rMaj.topRight(), rMaj.bottomRight());
  painter.drawLine(rSub.topRight(), rSub.bottomRight());

  QFont fSub = painter.font();
  fSub.setPointSize(8);
  fSub.setBold(true);
  painter.setFont(fSub);
  painter.setPen(QColor(QStringLiteral("#5D6E80"))); // text-muted

  painter.drawText(rMaj, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "相"));
  painter.drawText(rSub, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "亚"));
  painter.drawText(rMic, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "微"));

  painter.restore();
}

void FaciesCompoundTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                                    double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, QColor(QStringLiteral("#FFFFFF")));

  const qreal col1W = m_majorWidth;
  const qreal col2W = m_subWidth;
  const qreal col3W = qMax<qreal>(20.0, bodyRect.width() - col1W - col2W);

  const qreal col1X = bodyRect.left();
  const qreal col2X = col1X + col1W;
  const qreal col3X = col2X + col2W;

  // 绘制竖向分割线（#DFE5EC 在白底上近乎不可见，加深一档）
  painter.setPen(QColor(QStringLiteral("#B0BEC5")));
  painter.drawLine(QPointF(col2X, bodyRect.top()), QPointF(col2X, bodyRect.bottom()));
  painter.drawLine(QPointF(col3X, bodyRect.top()), QPointF(col3X, bodyRect.bottom()));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont fBody = painter.font();
  fBody.setPointSize(9);
  fBody.setBold(true);
  painter.setFont(fBody);

  // 1. 绘制「相」：对连续相同相名称进行跨层合并绘制
  struct MergedMajor {
    float topD = 0;
    float botD = 0;
    QString name;
    QColor color;
  };
  QVector<MergedMajor> majGroups;
  for (const auto &it : m_intervals)
  {
    if (!majGroups.isEmpty() && majGroups.last().name == it.majorFacies)
    {
      majGroups.last().botD = qMax(majGroups.last().botD, it.bottomDepth);
    }
    else
    {
      majGroups.append({it.topDepth, it.bottomDepth, it.majorFacies, it.majorColor});
    }
  }

  for (const auto &grp : majGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col1X, y0, col1W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#546E7A")), 1.4));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.name.isEmpty())
    {
      painter.setPen(QColor(QStringLiteral("#24303E")));
      QString dt = grp.name;
      if (textBox.height() >= 45.0 && col1W <= 52.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.name) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 2. 绘制「亚」：对同一主要相内连续相同亚相名称进行合并绘制
  struct MergedSub {
    float topD = 0;
    float botD = 0;
    QString major;
    QString sub;
    QColor color;
  };
  QVector<MergedSub> subGroups;
  for (const auto &it : m_intervals)
  {
    if (!subGroups.isEmpty() && subGroups.last().major == it.majorFacies && subGroups.last().sub == it.subFacies)
    {
      subGroups.last().botD = qMax(subGroups.last().botD, it.bottomDepth);
    }
    else
    {
      subGroups.append({it.topDepth, it.bottomDepth, it.majorFacies, it.subFacies, it.subColor});
    }
  }

  for (const auto &grp : subGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col2X, y0, col2W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#78909C")), 1.2));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.sub.isEmpty())
    {
      painter.setPen(QColor(QStringLiteral("#24303E")));
      QString dt = grp.sub;
      if (textBox.height() >= 55.0 && col2W <= 56.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.sub) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 3. 绘制「微」（微相）：地质纹理全填充，带半透明高对比胶囊文字保证极致可读性
  for (const auto &it : m_intervals)
  {
    if (it.bottomDepth < topDepth || it.topDepth > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (it.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (it.bottomDepth - topDepth) * pxPerMeter;
    const QRectF box(col3X, y0, col3W, qMax<qreal>(4.0, y1 - y0));

    // 使用地质沉积相纹理画刷填充
    const QBrush brush = FaciesPatternFactory::getBrush(
        it.patternType.isEmpty() ? it.microFacies : it.patternType, it.microColor);
    painter.fillRect(box, brush);

    // 上下边界线
    painter.setPen(QPen(QColor(QStringLiteral("#24303E")), 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    // 绘制微相名称：采用半透明白色胶囊底衬，确保任何复杂纹理下文字 100% 极佳清晰度
    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 15.0 && !it.microFacies.isEmpty())
    {
      QFontMetrics fm(painter.font());
      const int tw = fm.horizontalAdvance(it.microFacies);
      const int th = fm.height();
      const qreal pillW = qMin(box.width() - 4.0, static_cast<qreal>(tw + 10));
      const qreal pillH = qMin(textBox.height() - 4.0, static_cast<qreal>(th + 4));

      const QRectF pill(box.center().x() - pillW * 0.5,
                        textBox.center().y() - pillH * 0.5,
                        pillW, pillH);

      painter.fillRect(pill, QColor(255, 255, 255, 220));
      painter.setPen(QPen(QColor(QStringLiteral("#B0BEC5")), 0.8));
      painter.drawRoundedRect(pill, 3.0, 3.0);

      painter.setPen(QColor(QStringLiteral("#1A237E")));
      painter.drawText(pill, Qt::AlignCenter, it.microFacies);
    }
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

int FaciesCompoundTrack::intervalIndexAtDepth(float depth) const
{
  for (int i = 0; i < m_intervals.size(); ++i)
  {
    if (depth >= m_intervals.at(i).topDepth && depth <= m_intervals.at(i).bottomDepth)
      return i;
  }
  return -1;
}

bool FaciesCompoundTrack::replaceIntervalAt(int idx, const FaciesInterval &interval)
{
  if (idx < 0 || idx >= m_intervals.size())
    return false;
  m_intervals[idx] = interval;
  return true;
}

QString FaciesCompoundTrack::trackToolTip(double depth) const
{
  const int idx = intervalIndexAtDepth(depth);
  if (idx < 0)
    return title();
  const auto &fi = m_intervals.at(idx);
  return QStringLiteral("%1\n相: %2\n亚: %3\n微: %4")
      .arg(title(), fi.majorFacies, fi.subFacies, fi.microFacies);
}

} // namespace WellComposite
