// 层：视图
// 综合图道·地质图案工厂（岩性/沉积相纹理）——自 wellcompositetrack.cpp 拆出（方向 66，行为零变更）
#include "wellcompositetrack.h"
#include "patterncatalog.h"
#include "../../domain/faciescatalog.h"
#include <QSvgRenderer>
#include <QPainterPath>

namespace WellComposite
{
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
  const auto path = FaciesCatalog::fillPath(patternTypeOrName);
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

} // namespace WellComposite
