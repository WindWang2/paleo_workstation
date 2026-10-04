// 层：QGIS 封装
#include "geopatterns.h"

#include "../domain/faciescatalog.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QVariantMap>

#include <qgsfillsymbollayer.h> // QgsSVGFill/LinePattern 填充层（QGIS 4 合一头）
#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgslinesymbollayer.h> // QgsMarkerLineSymbolLayer
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgssymbollayer.h>

namespace
{
  // 词表条目（id/词面/资源/参数）。id 稳定：入库 customProperty 与跨版本
  // round-trip 都锚它；title 走 NOOP 翻译面；出处 catalog.json
  //（specification 字段，Q/HS 1011—2016 附录 F/O）。
  struct PatternDef
  {
    const char *id;
    const char *title;
    const char *texture; // qrc 相对名（textures/… 或 strata/…_fill）
    double widthMm;      // 平铺尺寸
  };

  struct LineDef
  {
    const char *id;
    const char *title;
    const char *color;
    double widthMm;
    const char *style; // solid | dash | dot
  };

  // 岩性常用集（textures/ 附录 F.1—F.3；granular 亚类/油砂等按需后扩）。
  const PatternDef kLithologies[] = {
    { "sandstone", QT_TRANSLATE_NOOP("GeoPatterns", "砂岩"), "textures/tex_sandstone_medium.svg", 10.0 },
    { "sandstone_coarse", QT_TRANSLATE_NOOP("GeoPatterns", "粗砂岩"), "textures/tex_sandstone_coarse.svg", 12.0 },
    { "sandstone_fine", QT_TRANSLATE_NOOP("GeoPatterns", "细砂岩"), "textures/tex_sandstone_fine.svg", 8.0 },
    { "sandstone_pebbly", QT_TRANSLATE_NOOP("GeoPatterns", "砾质砂岩"), "textures/tex_sandstone_pebbly.svg", 12.0 },
    { "siltstone", QT_TRANSLATE_NOOP("GeoPatterns", "粉砂岩"), "textures/tex_siltstone.svg", 8.0 },
    { "mudstone", QT_TRANSLATE_NOOP("GeoPatterns", "泥岩"), "textures/tex_mudstone.svg", 10.0 },
    { "mudstone_silty", QT_TRANSLATE_NOOP("GeoPatterns", "含粉砂泥岩"), "textures/tex_mudstone_silty.svg", 10.0 },
    { "shale", QT_TRANSLATE_NOOP("GeoPatterns", "页岩"), "textures/tex_shale_thin.svg", 10.0 },
    { "limestone", QT_TRANSLATE_NOOP("GeoPatterns", "灰岩"), "textures/tex_limestone_micritic.svg", 12.0 },
    { "dolomite", QT_TRANSLATE_NOOP("GeoPatterns", "白云岩"), "textures/tex_dolomite.svg", 12.0 },
    { "conglomerate", QT_TRANSLATE_NOOP("GeoPatterns", "砾岩"), "textures/tex_conglomerate_pebble.svg", 14.0 },
    { "coal", QT_TRANSLATE_NOOP("GeoPatterns", "煤层"), "textures/tex_coal.svg", 10.0 },
    { "gypsum", QT_TRANSLATE_NOOP("GeoPatterns", "石膏"), "textures/tex_gypsum.svg", 10.0 },
    { "salt", QT_TRANSLATE_NOOP("GeoPatterns", "岩盐"), "textures/tex_salt.svg", 10.0 },
  };
  constexpr int kLithologyCount = sizeof(kLithologies) / sizeof(kLithologies[0]);

  // 相单元（strata/ 附录 O 的 *_fill 可平铺变体；strat_deltaic 相带级、
  // strat_delta 朵体级分立收录）。
  const PatternDef kFacies[] = {
    { "fluvial", QT_TRANSLATE_NOOP("GeoPatterns", "河流相"), "strata/strat_fluvial_fill.svg", 16.0 },
    { "deltaic", QT_TRANSLATE_NOOP("GeoPatterns", "三角洲相"), "strata/strat_deltaic_fill.svg", 16.0 },
    { "delta", QT_TRANSLATE_NOOP("GeoPatterns", "三角洲朵体"), "strata/strat_delta_fill.svg", 16.0 },
    { "lacustrine", QT_TRANSLATE_NOOP("GeoPatterns", "湖泊相"), "strata/strat_lacustrine_fill.svg", 16.0 },
    { "coastal", QT_TRANSLATE_NOOP("GeoPatterns", "滨海相"), "strata/strat_coastal_fill.svg", 16.0 },
    { "alluvial_fan", QT_TRANSLATE_NOOP("GeoPatterns", "洪积相"), "strata/strat_alluvial_fan_fill.svg", 16.0 },
    { "aeolian", QT_TRANSLATE_NOOP("GeoPatterns", "沙漠相"), "strata/strat_aeolian_fill.svg", 16.0 },
    { "glacial", QT_TRANSLATE_NOOP("GeoPatterns", "冰川相"), "strata/strat_glacial_fill.svg", 16.0 },
  };
  constexpr int kFaciesCount = sizeof(kFacies) / sizeof(kFacies[0]);

  // 断层红 #D71414（resources/geology/faults 图式调性，同 fault_cut 类目
  // 既有口径）；相界细灰与测区边界墨色沿 DESIGN.md 地图域例外条款。
  const LineDef kLineStyles[] = {
    { "fault_normal", QT_TRANSLATE_NOOP("GeoPatterns", "正断层"), "#D71414", 0.7, "solid" },
    { "fault_reverse", QT_TRANSLATE_NOOP("GeoPatterns", "逆断层"), "#D71414", 0.7, "solid" },
    { "fault_strike", QT_TRANSLATE_NOOP("GeoPatterns", "走滑断层"), "#D71414", 0.7, "solid" },
    { "fault_inferred", QT_TRANSLATE_NOOP("GeoPatterns", "推测断层"), "#D71414", 0.55, "dash" },
    { "facies_definite", QT_TRANSLATE_NOOP("GeoPatterns", "确定相界"), "#5D6E80", 0.5, "solid" },
    { "facies_inferred", QT_TRANSLATE_NOOP("GeoPatterns", "推测相界"), "#5D6E80", 0.4, "dash" },
    { "facies_transitional", QT_TRANSLATE_NOOP("GeoPatterns", "渐变相带界"), "#5D6E80", 0.4, "dot" },
    { "survey_boundary", QT_TRANSLATE_NOOP("GeoPatterns", "测区边界"), "#24303E", 0.6, "solid" },
  };
  constexpr int kLineStyleCount = sizeof(kLineStyles) / sizeof(kLineStyles[0]);

  const PatternDef *findLithology(const QString &id)
  {
    for (int i = 0; i < kLithologyCount; ++i)
      if (id == QLatin1String(kLithologies[i].id))
        return &kLithologies[i];
    return nullptr;
  }

  // 常见粒级/俗写同义词（字段值常见写法；未知原样返回不吞数据）。
  const QHash<QString, QString> kLithologyAliases = {
    { QStringLiteral("中砂岩"), QStringLiteral("sandstone") },
    { QStringLiteral("石英砂岩"), QStringLiteral("sandstone") },
    { QStringLiteral("长石砂岩"), QStringLiteral("sandstone") },
    { QStringLiteral("岩屑砂岩"), QStringLiteral("sandstone") },
    { QStringLiteral("粗粒砂岩"), QStringLiteral("sandstone_coarse") },
    { QStringLiteral("细粒砂岩"), QStringLiteral("sandstone_fine") },
    { QStringLiteral("含砾砂岩"), QStringLiteral("sandstone_pebbly") },
    { QStringLiteral("砾岩（卵石）"), QStringLiteral("conglomerate") },
    { QStringLiteral("泥晶灰岩"), QStringLiteral("limestone") },
    { QStringLiteral("石灰岩"), QStringLiteral("limestone") },
    { QStringLiteral("薄层页岩"), QStringLiteral("shale") },
    { QStringLiteral("含粉砂泥岩"), QStringLiteral("mudstone_silty") },
    { QStringLiteral("膏岩"), QStringLiteral("gypsum") },
    { QStringLiteral("硬石膏"), QStringLiteral("gypsum") },
    { QStringLiteral("盐"), QStringLiteral("salt") },
    { QStringLiteral("食盐"), QStringLiteral("salt") },
    { QStringLiteral("煤"), QStringLiteral("coal") },
    { QStringLiteral("煤层"), QStringLiteral("coal") },
  };

  const PatternDef *findFacies(const QString &id)
  {
    for (int i = 0; i < kFaciesCount; ++i)
      if (id == QLatin1String(kFacies[i].id))
        return &kFacies[i];
    return nullptr;
  }

  const LineDef *findLine(const QString &id)
  {
    for (int i = 0; i < kLineStyleCount; ++i)
      if (id == QLatin1String(kLineStyles[i].id))
        return &kLineStyles[i];
    return nullptr;
  }

  QVariantMap patternMap(const PatternDef &def, const QString &group)
  {
    QVariantMap m;
    m.insert(QStringLiteral("id"), QString::fromLatin1(def.id));
    m.insert(QStringLiteral("title"), QObject::tr(def.title));
    m.insert(QStringLiteral("texture"), QString::fromLatin1(def.texture));
    m.insert(QStringLiteral("widthMm"), def.widthMm);
    m.insert(QStringLiteral("group"), group);
    return m;
  }

  QVariantMap lineMap(const LineDef &def, const QString &group)
  {
    QVariantMap m;
    m.insert(QStringLiteral("id"), QString::fromLatin1(def.id));
    m.insert(QStringLiteral("title"), QObject::tr(def.title));
    m.insert(QStringLiteral("texture"), QString());
    m.insert(QStringLiteral("color"), QString::fromLatin1(def.color));
    m.insert(QStringLiteral("widthMm"), def.widthMm);
    m.insert(QStringLiteral("style"), QString::fromLatin1(def.style));
    m.insert(QStringLiteral("group"), group);
    return m;
  }

  // SVG 平铺填充 + 细灰描边（相多边形描边不与花纹抢视觉；缺资源 → 纯色
  // 兜底，花纹语义仍可从 customProperty 恢复）。
  QgsFillSymbol *svgPatternFill(const PatternDef *def, const QColor &fallback)
  {
    const QString path = def ? FaciesCatalog::resourcePath(QString::fromLatin1(def->texture))
                             : QString();
    if (path.isEmpty())
    {
      QVariantMap props;
      props.insert(QStringLiteral("color"), fallback.name());
      props.insert(QStringLiteral("outline_color"), QStringLiteral("#5D6E80"));
      props.insert(QStringLiteral("outline_width"), QStringLiteral("0.26"));
      return QgsFillSymbol::createSimple(props).release();
    }
    auto *svg = new QgsSVGFillSymbolLayer(path, def->widthMm);
    QVariantMap stroke;
    stroke.insert(QStringLiteral("line_color"), QStringLiteral("#5D6E80"));
    stroke.insert(QStringLiteral("line_width"), QStringLiteral("0.26"));
    svg->setSubSymbol(QgsLineSymbol::createSimple(stroke).release());
    return new QgsFillSymbol(QgsSymbolLayerList{svg});
  }
} // namespace

namespace GeoPatterns
{

QVariantList lithologyDefinitions()
{
  QVariantList out;
  for (int i = 0; i < kLithologyCount; ++i)
    out.append(patternMap(kLithologies[i], QStringLiteral("lithology")));
  return out;
}

QVariantList faciesDefinitions()
{
  QVariantList out;
  for (int i = 0; i < kFaciesCount; ++i)
    out.append(patternMap(kFacies[i], QStringLiteral("facies")));
  return out;
}

QVariantList lineStyleDefinitions()
{
  QVariantList out;
  for (int i = 0; i < kLineStyleCount; ++i)
  {
    const QString group = QString::fromLatin1(kLineStyles[i].id).startsWith(
                              QStringLiteral("fault_"))
                              ? QStringLiteral("fault")
                              : QString::fromLatin1(kLineStyles[i].id).startsWith(
                                    QStringLiteral("facies_"))
                                    ? QStringLiteral("facies_boundary")
                                    : QStringLiteral("survey_boundary");
    out.append(lineMap(kLineStyles[i], group));
  }
  return out;
}

QString titleFor(const QString &id)
{
  if (const PatternDef *d = findLithology(id))
    return QObject::tr(d->title);
  if (const PatternDef *d = findFacies(id))
    return QObject::tr(d->title);
  if (const LineDef *d = findLine(id))
    return QObject::tr(d->title);
  return id;
}

QString textureResourcePath(const QString &patternId)
{
  if (const PatternDef *d = findLithology(patternId))
    return FaciesCatalog::resourcePath(QString::fromLatin1(d->texture));
  if (const PatternDef *d = findFacies(patternId))
    return FaciesCatalog::resourcePath(QString::fromLatin1(d->texture));
  return QString();
}

QString normalizeLithology(const QString &raw)
{
  const QString v = raw.trimmed();
  if (v.isEmpty())
    return QString();
  for (int i = 0; i < kLithologyCount; ++i)
    if (v == QLatin1String(kLithologies[i].id) ||
        v == QObject::tr(kLithologies[i].title))
      return QString::fromLatin1(kLithologies[i].id);
  return kLithologyAliases.value(v, v);
}

QString normalizeFaultKind(const QString &raw)
{
  const QString v = raw.trimmed();
  if (v.isEmpty())
    return QString();
  static const QHash<QString, QString> map = {
    { QStringLiteral("normal"), QStringLiteral("normal") },
    { QStringLiteral("正断层"), QStringLiteral("normal") },
    { QStringLiteral("正"), QStringLiteral("normal") },
    { QStringLiteral("reverse"), QStringLiteral("reverse") },
    { QStringLiteral("逆断层"), QStringLiteral("reverse") },
    { QStringLiteral("逆"), QStringLiteral("reverse") },
    { QStringLiteral("thrust"), QStringLiteral("reverse") },
    { QStringLiteral("逆冲断层"), QStringLiteral("reverse") },
    { QStringLiteral("strike"), QStringLiteral("strike") },
    { QStringLiteral("strike_slip"), QStringLiteral("strike") },
    { QStringLiteral("走滑断层"), QStringLiteral("strike") },
    { QStringLiteral("平移断层"), QStringLiteral("strike") },
    { QStringLiteral("inferred"), QStringLiteral("inferred") },
    { QStringLiteral("suspected"), QStringLiteral("inferred") },
    { QStringLiteral("推测断层"), QStringLiteral("inferred") },
    { QStringLiteral("可疑断层"), QStringLiteral("inferred") },
  };
  return map.value(v, v);
}

QStringList valueBuckets(const QString &patternId)
{
  QStringList out;
  const auto addUnique = [&out](const QString &v) {
    if (!v.isEmpty() && !out.contains(v))
      out.append(v);
  };
  addUnique(patternId);
  if (const PatternDef *d = findLithology(patternId))
  {
    addUnique(QObject::tr(d->title));
    for (auto it = kLithologyAliases.cbegin(); it != kLithologyAliases.cend(); ++it)
      if (it.value() == patternId)
        addUnique(it.key());
  }
  else if (const PatternDef *d = findFacies(patternId))
    addUnique(QObject::tr(d->title));
  return out;
}

QgsFillSymbol *lithologyFillSymbol(const QString &patternId)
{
  return svgPatternFill(findLithology(patternId), QColor(QStringLiteral("#D9DEE4")));
}

QgsFillSymbol *faciesFillSymbol(const QString &patternId)
{
  return svgPatternFill(findFacies(patternId), QColor(QStringLiteral("#E4DFD3")));
}

QgsFillSymbol *linePatternFillSymbol(const QColor &color, double distanceMm,
                                     double angleDeg)
{
  auto *layer = new QgsLinePatternFillSymbolLayer();
  layer->setColor(color);
  layer->setDistance(distanceMm);
  layer->setLineAngle(angleDeg);
  QVariantMap stroke;
  stroke.insert(QStringLiteral("line_color"), QStringLiteral("#5D6E80"));
  stroke.insert(QStringLiteral("line_width"), QStringLiteral("0.26"));
  layer->setSubSymbol(QgsLineSymbol::createSimple(stroke).release());
  return new QgsFillSymbol(QgsSymbolLayerList{layer});
}

QgsLineSymbol *faultLineSymbol(const QString &kind)
{
  const bool inferred = kind != QLatin1String("normal") &&
                        kind != QLatin1String("reverse") &&
                        kind != QLatin1String("strike");
  QVariantMap props;
  props.insert(QStringLiteral("line_color"), QStringLiteral("#D71414"));
  props.insert(QStringLiteral("line_width"),
               QString::number(inferred ? 0.55 : 0.7));
  props.insert(QStringLiteral("line_style"),
               inferred ? QStringLiteral("dash") : QStringLiteral("solid"));
  props.insert(QStringLiteral("capstyle"), QStringLiteral("round"));
  std::unique_ptr<QgsLineSymbol> sym(QgsLineSymbol::createSimple(props));
  if (inferred)
    return sym.release();

  // 断层齿（Q/HS 1011—2016 图式：齿挂断层面指示两盘相对运动）：
  // normal/reverse 三角齿反向（尖指下降/上升盘），strike 双向 cross。
  QVariantMap glyph;
  glyph.insert(QStringLiteral("name"), kind == QLatin1String("strike")
                                          ? QStringLiteral("cross2")
                                          : QStringLiteral("triangle"));
  glyph.insert(QStringLiteral("color"), QStringLiteral("#D71414"));
  glyph.insert(QStringLiteral("outline_style"), QStringLiteral("no"));
  glyph.insert(QStringLiteral("size"), QStringLiteral("2.4"));
  glyph.insert(QStringLiteral("angle"),
               QString::number(kind == QLatin1String("reverse") ? 180.0 : 0.0));
  auto *teeth = new QgsMarkerLineSymbolLayer(true /*rotateMarker*/, 8.0 /*interval mm*/);
  teeth->setSubSymbol(QgsMarkerSymbol::createSimple(glyph).release());
  sym->appendSymbolLayer(teeth);
  return sym.release();
}

QgsLineSymbol *faciesBoundaryLineSymbol(const QString &kind)
{
  const LineDef *def = findLine(kind == QLatin1String("definite")
                                    ? QStringLiteral("facies_definite")
                                    : kind == QLatin1String("transitional")
                                          ? QStringLiteral("facies_transitional")
                                          : QStringLiteral("facies_inferred"));
  QVariantMap props;
  props.insert(QStringLiteral("line_color"), QString::fromLatin1(def->color));
  props.insert(QStringLiteral("line_width"), QString::number(def->widthMm));
  props.insert(QStringLiteral("line_style"), QString::fromLatin1(def->style));
  return QgsLineSymbol::createSimple(props).release();
}

QgsLineSymbol *surveyBoundaryLineSymbol()
{
  QVariantMap props;
  props.insert(QStringLiteral("line_color"), QStringLiteral("#24303E"));
  props.insert(QStringLiteral("line_width"), QStringLiteral("0.6"));
  props.insert(QStringLiteral("line_style"), QStringLiteral("solid"));
  props.insert(QStringLiteral("capstyle"), QStringLiteral("round"));
  props.insert(QStringLiteral("joinstyle"), QStringLiteral("round"));
  return QgsLineSymbol::createSimple(props).release();
}

} // namespace GeoPatterns
