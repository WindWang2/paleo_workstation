// 层：视图
// token 例外：DESIGN 数据符号例外：岩性与沉积相纹理定义，色值是可保存/导出的地质图例数据。（tools/ui-token-exceptions.json 精确计数）。
#include "patterncatalog.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>

#include <cmath>

namespace WellComposite
{

namespace {

// ----------------------------------------------------------------------------
// 30+ 种岩性花纹定义（D4.2）：key / 名字关键词 / 底色 / 纹色 / 图例分组
// 色板遵循「数据符号色由域规则管理」——地质惯例底色，不占 UI token。
// ----------------------------------------------------------------------------
QList<PatternDef> buildLithologyPatterns()
{
  QList<PatternDef> defs;
  const auto add = [&defs](const char *key, const QStringList &hints, const char *bg,
                           const char *fg, const char *cat) {
    PatternDef d;
    d.key = QLatin1String(key);
    d.nameHints = hints;
    d.bg = QColor(QLatin1String(bg));
    d.fg = QColor(QLatin1String(fg));
    d.category = QString::fromUtf8(cat);
    defs << d;
  };

  // —— 碎屑岩 ——
  add("sandstone", {QStringLiteral("砂岩"), QStringLiteral("砂")}, "#FFF9C4", "#F57F17", "碎屑岩");
  add("fine_sandstone", {QStringLiteral("细砂岩")}, "#FFFDE7", "#F57F17", "碎屑岩");
  add("coarse_sandstone", {QStringLiteral("粗砂岩"), QStringLiteral("含砾砂岩")}, "#FFF3C4", "#EF6C00", "碎屑岩");
  add("siltstone", {QStringLiteral("粉砂岩"), QStringLiteral("粉砂")}, "#FFFDE7", "#8D6E63", "碎屑岩");
  add("mudstone", {QStringLiteral("泥岩"), QStringLiteral("泥")}, "#ECEFF1", "#546E7A", "碎屑岩");
  add("shale", {QStringLiteral("页岩"), QStringLiteral("页理")}, "#E0E7EB", "#37474F", "碎屑岩");
  add("sandy_mudstone", {QStringLiteral("砂质泥岩"), QStringLiteral("含砂泥岩")}, "#F1F0E4", "#6D6E2A", "碎屑岩");
  add("argillaceous_sandstone", {QStringLiteral("泥质砂岩")}, "#FAF0D8", "#9E7C2E", "碎屑岩");
  add("conglomerate", {QStringLiteral("砾岩"), QStringLiteral("砾")}, "#FFF8E1", "#6D4C41", "碎屑岩");
  add("breccia", {QStringLiteral("角砾岩"), QStringLiteral("角砾")}, "#F6EBD9", "#5D4037", "碎屑岩");
  add("glauconitic_sandstone", {QStringLiteral("海绿石砂岩"), QStringLiteral("绿砂")}, "#E8F5E9", "#2E7D32", "碎屑岩");

  // —— 碳酸盐岩 ——
  add("limestone", {QStringLiteral("灰岩"), QStringLiteral("石灰岩")}, "#E0F7FA", "#00838F", "碳酸盐岩");
  add("oolitic_limestone", {QStringLiteral("鲕粒灰岩"), QStringLiteral("鲕状")}, "#DFF4F8", "#00695C", "碳酸盐岩");
  add("bioclastic_limestone", {QStringLiteral("生物灰岩"), QStringLiteral("生物碎屑")}, "#D9F0F4", "#006064", "碳酸盐岩");
  add("reef_limestone", {QStringLiteral("礁灰岩"), QStringLiteral("礁")}, "#C8ECF2", "#004D40", "碳酸盐岩");
  add("chalk", {QStringLiteral("白垩"), QStringLiteral("白垩系灰岩")}, "#F5FDFE", "#455A64", "碳酸盐岩");
  add("marl", {QStringLiteral("泥灰岩"), QStringLiteral("泥灰")}, "#EDF2F0", "#546E7A", "碳酸盐岩");
  add("dolomite", {QStringLiteral("白云岩"), QStringLiteral("云岩")}, "#F3E5F5", "#6A1B9A", "碳酸盐岩");
  add("dolomitic_limestone", {QStringLiteral("白云质灰岩")}, "#EDF0F8", "#4527A0", "碳酸盐岩");
  add("calcirudite", {QStringLiteral("灰砾岩"), QStringLiteral("砾屑灰岩")}, "#E2F1F3", "#00838F", "碳酸盐岩");
  // 滩相
  add("shoal_grainstone", {QStringLiteral("颗粒滩"), QStringLiteral("滩灰岩"), QStringLiteral("滩")}, "#E8F6FA", "#0277BD", "碳酸盐岩");

  // —— 蒸发岩 ——
  add("gypsum", {QStringLiteral("石膏"), QStringLiteral("膏岩")}, "#FDF6E3", "#B45309", "蒸发岩");
  add("anhydrite", {QStringLiteral("硬石膏")}, "#FBF3DF", "#92400E", "蒸发岩");
  add("salt_rock", {QStringLiteral("盐岩"), QStringLiteral("石盐")}, "#FFF8F9", "#C2185B", "蒸发岩");

  // —— 有机岩 ——
  add("coal", {QStringLiteral("煤")}, "#CFD8DC", "#212121", "有机岩");
  add("oil_shale", {QStringLiteral("油页岩")}, "#EFEBE9", "#4E342E", "有机岩");
  add("carbonaceous_mudstone", {QStringLiteral("碳质泥岩"), QStringLiteral("炭质")}, "#E5E7E5", "#263238", "有机岩");

  // —— 火成岩/火山碎屑 ——
  add("tuff", {QStringLiteral("凝灰岩"), QStringLiteral("凝灰")}, "#ECEFF1", "#5D4037", "火成岩");
  add("tuffaceous_sandstone", {QStringLiteral("凝灰质砂岩")}, "#F1F2EA", "#6D4C41", "火成岩");
  add("basalt", {QStringLiteral("玄武岩")}, "#E3E7EA", "#37474F", "火成岩");
  add("andesite", {QStringLiteral("安山岩")}, "#EDEFF1", "#455A64", "火成岩");
  add("rhyolite", {QStringLiteral("流纹岩")}, "#F4F5F7", "#546E7A", "火成岩");
  add("granite", {QStringLiteral("花岗岩"), QStringLiteral("基岩")}, "#F1EDE9", "#616161", "火成岩");
  add("pyroclastic", {QStringLiteral("火山碎屑岩")}, "#EAE9E5", "#4E342E", "火成岩");

  // —— 其他沉积/特殊 ——
  add("phosphorite", {QStringLiteral("磷块岩"), QStringLiteral("磷矿")}, "#EDE7F6", "#5E35B1", "其他");
  add("chert", {QStringLiteral("硅质岩"), QStringLiteral("燧石")}, "#ECEFF1", "#37474F", "其他");
  add("ironstone", {QStringLiteral("铁质岩"), QStringLiteral("赤铁矿层")}, "#FBE9E7", "#B71C1C", "其他");
  add("diatomite", {QStringLiteral("硅藻土")}, "#F7F9F8", "#78909C", "其他");

  return defs;
}

// ----------------------------------------------------------------------------
// 程序化纹理绘制（16/20px tile；未覆盖键回退已有 Factory 的 7 种基本纹）
// ----------------------------------------------------------------------------
void drawNewPattern(QPainter &p, const QString &key, const QColor &fg, int size)
{
  p.setPen(QPen(fg, 1.0));
  p.setBrush(Qt::NoBrush);
  const int S = size;

  if (key == QLatin1String("fine_sandstone"))
  {
    for (int y = 3; y < S; y += 5)
      for (int x = 3; x < S; x += 5)
        p.drawPoint(x, y);
  }
  else if (key == QLatin1String("coarse_sandstone"))
  {
    p.setPen(QPen(fg, 1.6));
    p.drawPoint(4, 4); p.drawPoint(12, 6); p.drawPoint(6, 12); p.drawPoint(14, 13);
  }
  else if (key == QLatin1String("shale"))
  {
    // 页理：密集水平细线 + 断错
    for (int y = 3; y < S; y += 3)
      p.drawLine(0, y, S, y);
    p.drawLine(8, 0, 8, 3); p.drawLine(4, 6, 4, 9);
  }
  else if (key == QLatin1String("sandy_mudstone") || key == QLatin1String("argillaceous_sandstone"))
  {
    for (int y = 3; y < S; y += 6)
      p.drawLine(1, y, S - 4, y);
    p.drawPoint(13, 5); p.drawPoint(3, 13);
  }
  else if (key == QLatin1String("breccia"))
  {
    // 角砾：棱角多边形
    QPolygonF a;
    a << QPointF(2, 3) << QPointF(7, 2) << QPointF(8, 7) << QPointF(3, 8);
    p.drawPolygon(a);
    QPolygonF b;
    b << QPointF(9, 9) << QPointF(14, 8) << QPointF(15, 14) << QPointF(10, 15);
    p.drawPolygon(b);
  }
  else if (key == QLatin1String("glauconitic_sandstone"))
  {
    p.setBrush(fg);
    p.drawEllipse(QPointF(4, 4), 1.4, 1.4);
    p.drawEllipse(QPointF(11, 8), 1.4, 1.4);
    p.drawEllipse(QPointF(6, 13), 1.4, 1.4);
    p.drawPoint(14, 3);
  }
  else if (key == QLatin1String("oolitic_limestone"))
  {
    // 鲕粒：同心小圆
    p.drawEllipse(QPointF(4, 4), 2.0, 2.0);
    p.drawEllipse(QPointF(11, 6), 2.0, 2.0);
    p.drawEllipse(QPointF(6, 12), 2.0, 2.0);
    p.drawEllipse(QPointF(13, 13), 1.6, 1.6);
  }
  else if (key == QLatin1String("bioclastic_limestone") || key == QLatin1String("calcirudite"))
  {
    // 生物碎屑：弧形壳片
    p.drawArc(1, 2, 8, 6, 20 * 16, 120 * 16);
    p.drawArc(8, 9, 8, 6, 160 * 16, 120 * 16);
    p.drawArc(4, 10, 6, 4, 0, 90 * 16);
  }
  else if (key == QLatin1String("reef_limestone"))
  {
    // 礁：格架网状
    p.drawLine(2, 2, 8, 6); p.drawLine(8, 6, 14, 2);
    p.drawLine(2, 10, 8, 6); p.drawLine(8, 6, 14, 10);
    p.drawPoint(8, 13); p.drawPoint(3, 14);
  }
  else if (key == QLatin1String("chalk"))
  {
    p.setPen(QPen(fg, 0.8));
    for (int y = 4; y < S; y += 6)
      for (int x = 2; x < S; x += 4)
        p.drawPoint(x, y + (x % 8 == 0 ? 2 : 0));
  }
  else if (key == QLatin1String("marl"))
  {
    for (int y = 4; y < S; y += 5)
      p.drawLine(0, y, S, y);
    p.drawPoint(4, 7); p.drawPoint(12, 12);
  }
  else if (key == QLatin1String("dolomitic_limestone"))
  {
    // 砖纹 + 斜线复合
    p.drawLine(0, 8, S, 8);
    p.drawLine(6, 0, 6, 8); p.drawLine(11, 8, 11, S);
    p.drawLine(2, 12, 8, 8);
  }
  else if (key == QLatin1String("shoal_grainstone"))
  {
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(5, 5), 1.8, 1.8);
    p.drawEllipse(QPointF(12, 9), 1.8, 1.8);
    p.drawEllipse(QPointF(7, 13), 1.8, 1.8);
    p.drawPoint(2, 11);
  }
  else if (key == QLatin1String("gypsum") || key == QLatin1String("anhydrite"))
  {
    // 膏岩：斜向长条晶
    p.drawLine(1, 6, 9, 1); p.drawLine(7, 15, 15, 10);
    p.drawLine(2, 12, 6, 9);
  }
  else if (key == QLatin1String("salt_rock"))
  {
    // 石盐：立方晶格
    p.drawRect(QRectF(3, 3, 5, 5));
    p.drawRect(QRectF(9, 9, 5, 5));
  }
  else if (key == QLatin1String("oil_shale"))
  {
    // 油页岩：水平层理 + 油滴
    for (int y = 3; y < S; y += 4)
      p.drawLine(0, y, S, y);
    p.setBrush(fg);
    p.drawEllipse(QPointF(4, 8), 1.2, 1.2);
    p.drawEllipse(QPointF(12, 14), 1.2, 1.2);
  }
  else if (key == QLatin1String("carbonaceous_mudstone"))
  {
    for (int y = 4; y < S; y += 5)
      p.drawLine(0, y, S, y);
    p.drawLine(3, 2, 5, 8); p.drawLine(11, 8, 13, 14);
  }
  else if (key == QLatin1String("tuff") || key == QLatin1String("pyroclastic"))
  {
    // 凝灰：细点 + 小棱块
    p.drawPoint(3, 3); p.drawPoint(9, 5); p.drawPoint(5, 10); p.drawPoint(13, 12);
    QPolygonF a;
    a << QPointF(9, 2) << QPointF(13, 3) << QPointF(12, 7);
    p.drawPolygon(a);
  }
  else if (key == QLatin1String("tuffaceous_sandstone"))
  {
    p.drawPoint(4, 4); p.drawPoint(11, 4); p.drawPoint(7, 9); p.drawPoint(13, 13);
    p.drawLine(2, 12, 8, 12);
  }
  else if (key == QLatin1String("basalt"))
  {
    // 玄武岩：柱状节理
    p.drawLine(5, 0, 5, S); p.drawLine(11, 0, 11, S);
    p.drawLine(0, 6, 5, 6); p.drawLine(5, 12, 11, 12); p.drawLine(11, 5, S, 5);
  }
  else if (key == QLatin1String("andesite") || key == QLatin1String("rhyolite"))
  {
    // 流纹/安山：流面弧线
    QPainterPath path;
    path.moveTo(0, 5);
    path.cubicTo(5, 2, 10, 9, S, 5);
    p.drawPath(path);
    QPainterPath path2;
    path2.moveTo(0, 13);
    path2.cubicTo(6, 10, 9, 16, S, 12);
    p.drawPath(path2);
  }
  else if (key == QLatin1String("granite"))
  {
    // 花岗：三向节理交错
    p.drawLine(2, 2, 14, 14); p.drawLine(2, 14, 14, 2);
    p.drawPoint(8, 8); p.drawPoint(3, 8); p.drawPoint(13, 8);
  }
  else if (key == QLatin1String("phosphorite"))
  {
    p.drawEllipse(QPointF(5, 5), 1.5, 1.5);
    p.drawEllipse(QPointF(11, 11), 1.5, 1.5);
    p.drawLine(2, 13, 9, 13);
  }
  else if (key == QLatin1String("chert"))
  {
    // 硅质：贝壳状断口弧
    p.drawArc(2, 4, 10, 8, 30 * 16, 100 * 16);
    p.drawArc(6, 2, 8, 12, 200 * 16, 80 * 16);
  }
  else if (key == QLatin1String("ironstone"))
  {
    p.setBrush(fg);
    p.drawEllipse(QPointF(5, 5), 2.0, 2.0);
    p.drawEllipse(QPointF(11, 11), 2.0, 2.0);
    p.drawPoint(8, 8);
  }
  else if (key == QLatin1String("diatomite"))
  {
    p.setPen(QPen(fg, 0.8));
    for (int i = 0; i < 8; ++i)
      p.drawPoint(2 + (i * 5) % (S - 3), 3 + (i * 7) % (S - 5));
  }
  else
  {
    p.drawLine(0, S / 2, S, S / 2);
  }
}

} // namespace

const QList<PatternDef> &PatternCatalog::lithologyPatterns()
{
  static const QList<PatternDef> defs = buildLithologyPatterns();
  return defs;
}

int PatternCatalog::lithologyPatternCount()
{
  return lithologyPatterns().size();
}

QPixmap PatternCatalog::createLithoPattern(const QString &key, const QColor &bg, const QColor &fg)
{
  const int size = 16;
  QPixmap pm(size, size);
  pm.fill(bg.isValid() ? bg : QColor(QStringLiteral("#FFFFFF")));
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  drawNewPattern(p, key, fg.isValid() ? fg : QColor(QStringLiteral("#455A64")), size);
  p.end();
  return pm;
}

bool PatternCatalog::lookupLithology(const QString &lithoName, PatternDef *out)
{
  const PatternDef *best = nullptr;
  int bestLen = 0;
  for (const auto &d : lithologyPatterns())
  {
    for (const QString &hint : d.nameHints)
    {
      if (lithoName.contains(hint) && hint.size() > bestLen)
      {
        best = &d;
        bestLen = hint.size();
      }
    }
  }
  if (!best)
    return false;
  if (out)
    *out = *best;
  return true;
}

// ----------------------------------------------------------------------------
// D4.3 相名→花纹 JSON 映射（内置资源 + 用户覆盖）
// ----------------------------------------------------------------------------
namespace {
QHash<QString, QString> g_faciesMap;
bool g_faciesMapLoaded = false;

// 内置映射（外置 JSON 的编译期内嵌等价物；resources/faciespatterns.json 同源）
QHash<QString, QString> builtinFaciesMap()
{
  QHash<QString, QString> m;
  m.insert(QStringLiteral("水下分流河道"), QStringLiteral("distributary_channel"));
  m.insert(QStringLiteral("分流河道"), QStringLiteral("distributary_channel"));
  m.insert(QStringLiteral("河口坝"), QStringLiteral("mouth_bar"));
  m.insert(QStringLiteral("远砂坝"), QStringLiteral("sheet_sand"));
  m.insert(QStringLiteral("席状砂"), QStringLiteral("sheet_sand"));
  m.insert(QStringLiteral("分流间湾"), QStringLiteral("interdistributary_bay"));
  m.insert(QStringLiteral("三角洲前缘"), QStringLiteral("delta_front"));
  m.insert(QStringLiteral("三角洲平原"), QStringLiteral("delta_plain"));
  m.insert(QStringLiteral("前三角洲"), QStringLiteral("prodelta"));
  m.insert(QStringLiteral("浅海陆棚"), QStringLiteral("shallow_marine"));
  m.insert(QStringLiteral("浊积"), QStringLiteral("turbidite"));
  m.insert(QStringLiteral("滞留沉积"), QStringLiteral("channel_lag"));
  m.insert(QStringLiteral("潮坪"), QStringLiteral("tidal_flat"));
  // D4.2 扩充键的新映射
  m.insert(QStringLiteral("颗粒滩"), QStringLiteral("shoal_grainstone"));
  m.insert(QStringLiteral("礁"), QStringLiteral("reef_limestone"));
  m.insert(QStringLiteral("泻湖"), QStringLiteral("marl"));
  m.insert(QStringLiteral("沼泽"), QStringLiteral("carbonaceous_mudstone"));
  return m;
}

void mergeJsonMap(QHash<QString, QString> *map, const QByteArray &json, QString *errOut = nullptr)
{
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
  {
    if (errOut)
      *errOut = err.errorString();
    return;
  }
  const QJsonObject obj = doc.object();
  for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
    map->insert(it.key(), it.value().toString());
}

} // namespace

QString PatternCatalog::userFaciesMapPath()
{
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  return base + QStringLiteral("/wellcomposite/faciespatterns.json");
}

QHash<QString, QString> PatternCatalog::faciesPatternMap()
{
  if (g_faciesMapLoaded)
    return g_faciesMap;

  g_faciesMap = builtinFaciesMap();

  // 用户覆盖（存在才合并）
  QFile f(userFaciesMapPath());
  if (f.exists() && f.open(QIODevice::ReadOnly))
    mergeJsonMap(&g_faciesMap, f.readAll());

  g_faciesMapLoaded = true;
  return g_faciesMap;
}

void PatternCatalog::reloadFaciesMap()
{
  g_faciesMapLoaded = false;
  g_faciesMap.clear();
}

QString PatternCatalog::faciesPatternKey(const QString &faciesName)
{
  return faciesPatternMap().value(faciesName.trimmed());
}

} // namespace WellComposite
