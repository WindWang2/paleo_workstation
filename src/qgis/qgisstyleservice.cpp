// 层：QGIS 封装
#include "qgisstyleservice.h"
#include "factorstylewriter.h"
#include "qgiserrors_internal.h"

#include "geopatterns.h"

#include <QColor>
#include <QDir>

#include <algorithm>
#include <QFileInfo>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>

#include <qgsarrowsymbollayer.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsexpression.h>
#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgslinesymbollayer.h> // QgsSimpleLineSymbolLayer（相变渐变带层）
#include <qgsmaplayer.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h> // QgsSimpleMarkerSymbolLayer（QGIS 4 无独立头）
#include <qgspallabeling.h>
#include <qgsrulebasedrenderer.h>
#include <qgsproject.h>
#include <qgsproperty.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

namespace
{
using paleo::qgis_detail::setError;

  // ---- C3：井类别符号构成件（Q/HS 1011—2016 表 K.1 调性）-------------------
  // 规范墨色 #333；流体语义色取 resources/geology/catalog.json 井型表
  //（采油 #00AA00 / 采气 #FF3300 / 未见显示·报废 #999）。尺寸基准 = 通用
  // 探井图式（外细环 6mm + 实心盘 4.4mm ≈ 0.73 外径比，同测区全景口径）。
  QgsMarkerSymbol *wellSymbolLayers(const QList<QVariantMap> &layers)
  {
    std::unique_ptr<QgsMarkerSymbol> sym;
    for (const QVariantMap &props : layers)
    {
      if (!sym)
        sym.reset(QgsMarkerSymbol::createSimple(props).release());
      else
        sym->appendSymbolLayer(QgsSimpleMarkerSymbolLayer::create(props));
    }
    return sym.release();
  }

  QVariantMap ringProps(const QString &outline, double widthMm, double sizeMm,
                        const QString &outlineStyle = QStringLiteral("solid"))
  {
    QVariantMap p;
    p.insert(QStringLiteral("name"), QStringLiteral("circle"));
    p.insert(QStringLiteral("color"), QStringLiteral("255,255,255,0"));
    p.insert(QStringLiteral("outline_color"), outline);
    p.insert(QStringLiteral("outline_width"), QString::number(widthMm));
    p.insert(QStringLiteral("outline_style"), outlineStyle);
    p.insert(QStringLiteral("size"), QString::number(sizeMm));
    return p;
  }

  QVariantMap diskProps(const QString &color, double sizeMm)
  {
    QVariantMap p;
    p.insert(QStringLiteral("name"), QStringLiteral("circle"));
    p.insert(QStringLiteral("color"), color);
    p.insert(QStringLiteral("outline_style"), QStringLiteral("no"));
    p.insert(QStringLiteral("size"), QString::number(sizeMm));
    return p;
  }

  QVariantMap glyphProps(const QString &name, const QString &color, double sizeMm,
                         double angleDeg = 0.0)
  {
    QVariantMap p;
    p.insert(QStringLiteral("name"), name);
    p.insert(QStringLiteral("color"), color);
    p.insert(QStringLiteral("outline_style"), QStringLiteral("no"));
    p.insert(QStringLiteral("size"), QString::number(sizeMm));
    if (angleDeg != 0.0)
      p.insert(QStringLiteral("angle"), QString::number(angleDeg));
    return p;
  }

  // 井别符号（存储 id → 符号）。序 = 表 K.1 类别序（12 类探井）+ 方向 31
  // 开发区块 4 类（oil_prod/gas_prod/water_injection/water_prod——表 K.1 之外
  // 的开发区块图式，按行业惯例近似：生产井实心盘+中心白点、注水井蓝盘白
  // 箭头，色源同 resources/geology/catalog.json 井型表）。未知 id → 通用
  //「探井」。全部类别统一挂 @map_scale 数据定义尺寸（函数尾，比例尺缩放）。
  QgsMarkerSymbol *wellCategorySymbol(const QString &id)
  {
    const QString ink = QStringLiteral("#333333");
    const QString waterBlue = QStringLiteral("#2E86C1");
    QgsMarkerSymbol *sym = nullptr;
    if (id == QLatin1String("wildcat")) // 预探井：双环
      sym = wellSymbolLayers({ ringProps(ink, 0.5, 6.0), ringProps(ink, 0.4, 3.4) });
    else if (id == QLatin1String("appraisal")) // 评价井：单环 + 中点
      sym = wellSymbolLayers({ ringProps(ink, 0.5, 6.0), diskProps(ink, 1.8) });
    else if (id == QLatin1String("drilling")) // 正钻井：环 + 半填盘
      sym = wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("semi_circle"), ink, 4.4) });
    else if (id == QLatin1String("planned")) // 待钻井：虚线环
      sym = wellSymbolLayers({ ringProps(ink, 0.5, 6.0, QStringLiteral("dash")) });
    else if (id == QLatin1String("discovery")) // 发现井：环 + 星
      sym = wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("star"), QStringLiteral("#B8860B"), 4.6) });
    else if (id == QLatin1String("oil_flow")) // 工业油流井：绿盘 + 细环
      sym = wellSymbolLayers(
          { ringProps(ink, 0.4, 6.0), diskProps(QStringLiteral("#00AA00"), 4.4) });
    else if (id == QLatin1String("gas_flow")) // 工业气流井：红环 + 红斜线
      sym = wellSymbolLayers({ ringProps(QStringLiteral("#FF3300"), 0.6, 6.0),
                               glyphProps(QStringLiteral("line"), QStringLiteral("#FF3300"), 6.4, 45.0) });
    else if (id == QLatin1String("parameter")) // 参数井：环内十字
      sym = wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("cross"), ink, 3.6) });
    else if (id == QLatin1String("scientific")) // 科学探索井：环内斜十字
      sym = wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("cross2"), ink, 3.6, 45.0) });
    else if (id == QLatin1String("oil_test")) // 试油井：环内上箭头
      sym = wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("arrow"), ink, 3.4) });
    else if (id == QLatin1String("dry")) // 未见显示井：灰空心环
      sym = wellSymbolLayers({ ringProps(QStringLiteral("#999999"), 0.5, 6.0) });
    else if (id == QLatin1String("abandoned")) // 报废井：灰环 + 灰叉
      sym = wellSymbolLayers(
          { ringProps(QStringLiteral("#999999"), 0.5, 6.0),
            glyphProps(QStringLiteral("cross2"), QStringLiteral("#999999"), 3.6, 45.0) });
    // ---- 方向 31：开发区块井别（行业惯例近似，非表 K.1 序）----
    else if (id == QLatin1String("oil_prod")) // 生产油井：墨环 + 绿盘 + 中心白点
      sym = wellSymbolLayers({ ringProps(ink, 0.5, 6.0),
                               diskProps(QStringLiteral("#00AA00"), 4.4),
                               diskProps(QStringLiteral("#FFFFFF"), 1.4) });
    else if (id == QLatin1String("gas_prod")) // 生产气井：墨环 + 红盘 + 中心白点
      sym = wellSymbolLayers({ ringProps(ink, 0.5, 6.0),
                               diskProps(QStringLiteral("#FF3300"), 4.4),
                               diskProps(QStringLiteral("#FFFFFF"), 1.4) });
    else if (id == QLatin1String("water_injection")) // 注水井：墨环 + 蓝盘 + 白下箭头
      sym = wellSymbolLayers({ ringProps(ink, 0.5, 6.0),
                               diskProps(waterBlue, 4.4),
                               glyphProps(QStringLiteral("arrow"), QStringLiteral("#FFFFFF"), 2.8, 180.0) });
    else if (id == QLatin1String("water_prod")) // 采水井：蓝环 + 蓝十字
      sym = wellSymbolLayers({ ringProps(waterBlue, 0.5, 6.0),
                               glyphProps(QStringLiteral("cross"), waterBlue, 3.6) });
    else // 通用「探井」：外细环 + 实心盘（0.73 外径比）
      sym = wellSymbolLayers({ ringProps(QStringLiteral("#1B73D0"), 0.5, 6.0),
                               diskProps(QStringLiteral("#1B73D0"), 4.4) });
    // 比例尺缩放：小比例尺（出图远景）符号整体收缩，各构成层按比例
    //（setDataDefinedSize 语义），断点 1:25万 → 1:250万 线性 6mm → 3mm。
    sym->setDataDefinedSize(QgsProperty::fromExpression(
        QStringLiteral("clamp(3.0, scale_linear(@map_scale, 250000, 2500000, 6.0, 3.0), 6.0)")));
    return sym;
  }

  struct WellCategoryDef
  {
    const char *id;
    const char *title;
    const char *glyph;
  };

  // 表 K.1 十二类（词面/图式描述；色源与resources/geology 井型目录一致）
  // + 方向 31 开发区块四类（行业惯例近似，注出处 ledger）。
  const WellCategoryDef kWellCategories[] = {
    { "wildcat", QT_TRANSLATE_NOOP("QgisStyleService", "预探井"), "双环" },
    { "appraisal", QT_TRANSLATE_NOOP("QgisStyleService", "评价井"), "单环+中点" },
    { "drilling", QT_TRANSLATE_NOOP("QgisStyleService", "正钻井"), "半填盘" },
    { "planned", QT_TRANSLATE_NOOP("QgisStyleService", "待钻井"), "虚线环" },
    { "discovery", QT_TRANSLATE_NOOP("QgisStyleService", "发现井"), "星标" },
    { "oil_flow", QT_TRANSLATE_NOOP("QgisStyleService", "工业油流井"), "绿盘" },
    { "gas_flow", QT_TRANSLATE_NOOP("QgisStyleService", "工业气流井"), "红环+斜线" },
    { "parameter", QT_TRANSLATE_NOOP("QgisStyleService", "参数井"), "环内十字" },
    { "scientific", QT_TRANSLATE_NOOP("QgisStyleService", "科学探索井"), "环内斜十字" },
    { "oil_test", QT_TRANSLATE_NOOP("QgisStyleService", "试油井"), "环内箭头" },
    { "dry", QT_TRANSLATE_NOOP("QgisStyleService", "未见显示井"), "灰空心环" },
    { "abandoned", QT_TRANSLATE_NOOP("QgisStyleService", "报废井"), "灰环+叉" },
    { "oil_prod", QT_TRANSLATE_NOOP("QgisStyleService", "生产油井"), "绿盘+白点" },
    { "gas_prod", QT_TRANSLATE_NOOP("QgisStyleService", "生产气井"), "红盘+白点" },
    { "water_injection", QT_TRANSLATE_NOOP("QgisStyleService", "注水井"), "蓝盘+白箭头" },
    { "water_prod", QT_TRANSLATE_NOOP("QgisStyleService", "采水井"), "蓝环+十字" },
  };
  constexpr int kWellCategoryCount = sizeof(kWellCategories) / sizeof(kWellCategories[0]);
} // namespace

// qgis/ — QgisStyleService applies named styles to layers.
// Style refs resolve to .qml files under vendor share or project styles/ dir.

QgisStyleService::QgisStyleService(QObject *parent)
  : QObject(parent)
{
}

void QgisStyleService::setStylesRoot(const QString &dir)
{
  m_stylesRoot = dir;
}

bool QgisStyleService::applyStyle(QgsMapLayer *layer, const QString &styleRef, QString *error)
{
  if (!layer)
  {
    setError(error, tr("cannot apply a style to a null layer"));
    return false;
  }
  if (m_stylesRoot.isEmpty())
  {
    setError(error, tr("no styles root configured — call setStylesRoot() first"));
    return false;
  }
  if (styleRef.isEmpty())
  {
    setError(error, tr("cannot apply an empty style reference"));
    return false;
  }

  // styles/<ref>.qml — tolerate a ref that already carries the suffix
  const QString fileName = styleRef.endsWith(QStringLiteral(".qml"), Qt::CaseInsensitive)
    ? styleRef : styleRef + QStringLiteral(".qml");
  const QString path = QDir(m_stylesRoot).filePath(fileName);
  if (!QFileInfo::exists(path))
  {
    setError(error, tr("style '%1' not found at %2").arg(styleRef, path));
    return false;
  }

  bool resultFlag = false;
  const QString status = layer->loadNamedStyle(path, resultFlag);
  if (!resultFlag)
  {
    setError(error, tr("loadNamedStyle('%1') failed: %2")
                      .arg(path, status.isEmpty() ? tr("unknown error") : status));
    return false;
  }
  return true;
}

QStringList QgisStyleService::availableStyles() const
{
  QStringList refs;
  if (m_stylesRoot.isEmpty())
    return refs;

  const QDir dir(m_stylesRoot);
  const QStringList files = dir.entryList({QStringLiteral("*.qml")}, QDir::Files, QDir::Name);
  refs.reserve(files.size());
  for (const QString &f : files)
    refs << QFileInfo(f).completeBaseName(); // basename without .qml
  return refs;
}

void QgisStyleService::applyTrajectoryLayerStyle(QgsVectorLayer *layer)
{
  if (!layer)
    return;
  QVariantMap props;
  props.insert(QStringLiteral("name"), QStringLiteral("line"));
  props.insert(QStringLiteral("line_color"), QStringLiteral("#24303E"));
  props.insert(QStringLiteral("line_width"), QStringLiteral("0.6"));
  props.insert(QStringLiteral("capstyle"), QStringLiteral("round"));
  props.insert(QStringLiteral("joinstyle"), QStringLiteral("round"));
  layer->setRenderer(
      new QgsSingleSymbolRenderer(QgsLineSymbol::createSimple(props).release()));
}

void QgisStyleService::applyWellLayerStyle(QgsVectorLayer *layer)
{
  if (!layer)
    return;
  QVariantMap props;
  props.insert(QStringLiteral("name"), QStringLiteral("circle"));
  props.insert(QStringLiteral("color"), QStringLiteral("#24303E"));
  props.insert(QStringLiteral("outline_color"), QStringLiteral("#FFFFFF"));
  props.insert(QStringLiteral("outline_width"), QStringLiteral("0.4"));
  props.insert(QStringLiteral("size"), QStringLiteral("3"));
  layer->setRenderer(
      new QgsSingleSymbolRenderer(QgsMarkerSymbol::createSimple(props).release()));

  // 标记保持这一套圆点。文字在有沉积相字段时加一行相名，没有则仍只标井名。
  auto columnOrNull = [layer](const QString &name) -> QString {
    if (layer->fields().lookupField(name) < 0)
      return QString();
    return QStringLiteral("nullif(trim(to_string(%1)), '')")
        .arg(QgsExpression::quotedColumnRef(name));
  };
  QStringList faciesExprs;
  for (const QString &field : {QStringLiteral("facies_label"), QStringLiteral("microfacies"),
                               QStringLiteral("subfacies"), QStringLiteral("facies_name"),
                               QStringLiteral("facies"), QStringLiteral("沉积相"),
                               QStringLiteral("微相"), QStringLiteral("亚相"),
                               QStringLiteral("相")})
  {
    const QString expr = columnOrNull(field);
    if (!expr.isEmpty())
      faciesExprs << expr;
  }
  QString nameExpr;
  for (const QString &field : {QStringLiteral("name"), QStringLiteral("well_name"),
                               QStringLiteral("井名")})
  {
    nameExpr = columnOrNull(field);
    if (!nameExpr.isEmpty())
      break;
  }

  QgsPalLayerSettings lbl;
  if (faciesExprs.isEmpty())
  {
    lbl.fieldName = QStringLiteral("name");
    lbl.isExpression = false;
  }
  else
  {
    const QString faciesExpr =
        QStringLiteral("coalesce(%1)").arg(faciesExprs.join(QStringLiteral(", ")));
    lbl.isExpression = true;
    lbl.fieldName = QStringLiteral(
                        "with_variable('nm', %1, with_variable('fc', %2, "
                        "CASE WHEN @nm IS NULL AND @fc IS NULL THEN '' "
                        "WHEN @nm IS NULL THEN @fc "
                        "WHEN @fc IS NULL THEN @nm "
                        "ELSE @nm || '\\n' || @fc END))")
                        .arg(nameExpr.isEmpty() ? QStringLiteral("NULL") : nameExpr, faciesExpr);
    lbl.placement = Qgis::LabelPlacement::OrderedPositionsAroundPoint;
  }
  QgsTextFormat fmt;
  fmt.setSize(9.0);
  fmt.setSizeUnit(Qgis::RenderUnit::Points);
  fmt.setColor(QColor(QStringLiteral("#24303E")));
  if (!faciesExprs.isEmpty())
  {
    QgsTextBufferSettings buffer;
    buffer.setEnabled(true);
    buffer.setSize(0.8);
    buffer.setColor(Qt::white);
    fmt.setBuffer(buffer);
  }
  lbl.setFormat(fmt);
  layer->setLabeling(new QgsVectorLayerSimpleLabeling(lbl));
  layer->setLabelsEnabled(true);
}

void QgisStyleService::applyBoundaryLayerStyle(QgsVectorLayer *layer)
{
  if (!layer)
    return;
  QVariantMap props;
  props.insert(QStringLiteral("style"), QStringLiteral("no")); // 空心填
  props.insert(QStringLiteral("outline_color"), QStringLiteral("#24303E"));
  props.insert(QStringLiteral("outline_width"), QStringLiteral("0.6"));
  props.insert(QStringLiteral("outline_style"), QStringLiteral("solid"));
  layer->setRenderer(
      new QgsSingleSymbolRenderer(QgsFillSymbol::createSimple(props).release()));
}

void QgisStyleService::applyPlannedWellLayerStyle(QgsVectorLayer *layer)
{
  if (!layer)
    return;
  // 空心橙虚线方框：部署建议（非实井），与实井实心圆点同图可分。
  QVariantMap props;
  props.insert(QStringLiteral("name"), QStringLiteral("square"));
  props.insert(QStringLiteral("color"), QStringLiteral("transparent"));
  props.insert(QStringLiteral("outline_color"), QStringLiteral("#F29900"));
  props.insert(QStringLiteral("outline_width"), QStringLiteral("0.5"));
  props.insert(QStringLiteral("outline_style"), QStringLiteral("dash"));
  props.insert(QStringLiteral("size"), QStringLiteral("4.5"));
  layer->setRenderer(
      new QgsSingleSymbolRenderer(QgsMarkerSymbol::createSimple(props).release()));

  if (layer->fields().lookupField(QStringLiteral("name")) < 0)
    return;
  QgsPalLayerSettings lbl;
  lbl.fieldName = QStringLiteral("name");
  lbl.isExpression = false;
  QgsTextFormat fmt;
  fmt.setSize(8.0);
  fmt.setSizeUnit(Qgis::RenderUnit::Points);
  fmt.setColor(QColor(QStringLiteral("#8A5A00")));
  QgsTextBufferSettings buffer;
  buffer.setEnabled(true);
  buffer.setSize(0.8);
  buffer.setColor(Qt::white);
  fmt.setBuffer(buffer);
  lbl.setFormat(fmt);
  layer->setLabeling(new QgsVectorLayerSimpleLabeling(lbl));
  layer->setLabelsEnabled(true);
}

void QgisStyleService::applyHoleLayerStyle(QgsVectorLayer *layer)
{
  if (!layer)
    return;
  // 警示橙半透明填 + 橙虚线描边（与配准临时层同调性）：诊断叠加层，
  // 不遮挡下层井位/因素图。
  QVariantMap props;
  props.insert(QStringLiteral("color"), QStringLiteral("242,153,0,64"));
  props.insert(QStringLiteral("outline_color"), QStringLiteral("#F29900"));
  props.insert(QStringLiteral("outline_width"), QStringLiteral("0.5"));
  props.insert(QStringLiteral("outline_style"), QStringLiteral("dash"));
  layer->setRenderer(
      new QgsSingleSymbolRenderer(QgsFillSymbol::createSimple(props).release()));
}

void QgisStyleService::applyConstraintLayerStyle(QgsVectorLayer *layer)
{
  if (!layer || !layer->isValid())
    return;
  if (layer->geometryType() != Qgis::GeometryType::Line)
    return;
  if (layer->fields().lookupField(QStringLiteral("type")) < 0)
    return; // 无语义字段：不接管渲染器

  // 地图域数据符号色（DESIGN.md 例外条款）：方向线红实线、打断线墨实线、
  // 解释软边界橙虚线、等值线停线灰虚线、制图绕行蓝点划线。
  auto lineSym = [](const QString &color, double widthMm, const QString &style) {
    QVariantMap props;
    props.insert(QStringLiteral("line_color"), color);
    props.insert(QStringLiteral("line_width"), QString::number(widthMm));
    props.insert(QStringLiteral("line_style"), style);
    return QgsLineSymbol::createSimple(props).release();
  };
  QgsCategoryList cats;
  const auto add = [&cats, &lineSym](const char *type, const QString &color,
                                     double w, const QString &style,
                                     const QString &title) {
    cats.append(QgsRendererCategory(QString::fromLatin1(type),
                                    lineSym(color, w, style), title));
  };
  // 入库 type 已被 storageTypeForSemantic 归一化；两种历史拼写同收。
  add("direction_line", QStringLiteral("#C0392B"), 0.7, QStringLiteral("solid"),
      QObject::tr("物源方向线"));
  add("direction_guide", QStringLiteral("#C0392B"), 0.7, QStringLiteral("dash"),
      QObject::tr("方向引导"));
  add("break_line", QStringLiteral("#24303E"), 0.55, QStringLiteral("solid"),
      QObject::tr("打断线"));
  add("hard_barrier", QStringLiteral("#24303E"), 0.55, QStringLiteral("solid"),
      QObject::tr("硬屏障"));
  add("interpretive_boundary", QStringLiteral("#F29900"), 0.5,
      QStringLiteral("dash"), QObject::tr("解释软边界"));
  add("contour_stop", QStringLiteral("#5D6E80"), 0.45, QStringLiteral("dash"),
      QObject::tr("等值线停线"));
  add("cartographic_detour", QStringLiteral("#1B73D0"), 0.5,
      QStringLiteral("dash dot"), QObject::tr("制图绕行线"));
  // 未标语义/旧 type=line 与 all-other 桶：常规细墨线。
  cats.append(QgsRendererCategory(QStringLiteral("line"),
                                  lineSym(QStringLiteral("#24303E"), 0.3,
                                          QStringLiteral("solid")),
                                  QObject::tr("约束线")));
  cats.append(QgsRendererCategory(QVariant(),
                                  lineSym(QStringLiteral("#24303E"), 0.3,
                                          QStringLiteral("solid")),
                                  QObject::tr("约束线")));
  layer->setRenderer(
      new QgsCategorizedSymbolRenderer(QStringLiteral("type"), cats));
}

void QgisStyleService::applyContourLayerStyle(QgsVectorLayer *layer)
{
  FactorStyleWriter::applyContours(layer);
}

// ---- C2（wave/deepen-perf）：相界地质语义符号 --------------------------------

void QgisStyleService::applyFaciesBoundaryStyle(QgsVectorLayer *layer)
{
  if (!layer || !layer->isValid())
    return;
  if (layer->geometryType() != Qgis::GeometryType::Polygon)
    return;
  if (layer->fields().lookupField(QStringLiteral("boundary_kind")) < 0)
    return; // 无语义字段：不接管渲染器（保持现状图面）

  // 面填充走低透明度中性色——相多边形主要承载边界语义，不与栅格因素图
  // 抢视觉（地图域 QGIS 样式，DESIGN.md 例外条款；克制规范）。
  auto boundaryFill = [](const QString &outlineColor, double outlineWidthMm,
                         const QString &outlineStyle = QStringLiteral("solid")) {
    QVariantMap props;
    props.insert(QStringLiteral("color"), QStringLiteral("188,199,209,60"));
    props.insert(QStringLiteral("outline_color"), outlineColor);
    props.insert(QStringLiteral("outline_width"), QString::number(outlineWidthMm));
    props.insert(QStringLiteral("outline_style"), outlineStyle);
    return QgsFillSymbol::createSimple(props).release();
  };

  // 相变（方向 39）：点线 crisp 边（= 方向 31 facies_transitional 线型）之上
  // 叠半透明渐变带层——带宽 data-defined 绑 transition_width 字段（图层
  // CRS 地图单位，渐变带是地质体宽度）；无带/NULL/0 或字段缺失时线型
  // data-defined 落 no（不画带，诚实：无渐变范围不渲染带）。
  auto faciesChangeFill = [boundaryFill](bool withBandField) {
    QgsFillSymbol *sym = boundaryFill(QStringLiteral("#5D6E80"), 0.26,
                                      QStringLiteral("dot"));
    if (!withBandField)
      return sym;
    QVariantMap bandProps;
    bandProps.insert(QStringLiteral("line_color"), QStringLiteral("93,110,128,90"));
    bandProps.insert(QStringLiteral("line_width"), QStringLiteral("0"));
    bandProps.insert(QStringLiteral("line_style"), QStringLiteral("no"));
    auto *band = static_cast<QgsSimpleLineSymbolLayer *>(
        QgsSimpleLineSymbolLayer::create(bandProps));
    band->setWidthUnit(Qgis::RenderUnit::MapUnits);
    band->setDataDefinedProperty(
        QgsSymbolLayer::Property::StrokeWidth,
        QgsProperty::fromExpression(QStringLiteral("coalesce(\"transition_width\", 0)")));
    band->setDataDefinedProperty(
        QgsSymbolLayer::Property::StrokeStyle,
        QgsProperty::fromExpression(
            QStringLiteral("if(coalesce(\"transition_width\", 0) > 0, 'solid', 'no')")));
    sym->insertSymbolLayer(1, band); // 填充描边之上叠加半透明带（alpha 90）
    return sym;
  };

  // 三类新图式复用方向 31 相界线型（geopatterns LineDef 调性，同色
  // #5D6E80 以线型/宽度区分——确定界实线/端部不确定虚线/渐变界点线）：
  //   整合接触 conformable = 细实线（facies_definite）；
  //   尖灭 pinchout = 虚线（端部不确定，沿 facies_inferred 惯例）；
  //   相变 facies_change = 点线 + 可选渐变带（facies_transitional）。
  const bool hasBandField =
      layer->fields().lookupField(QStringLiteral("transition_width")) >= 0;
  QgsCategoryList cats;
  cats.append(QgsRendererCategory(
      QStringLiteral("conformable"), boundaryFill(QStringLiteral("#5D6E80"), 0.5),
      QObject::tr("整合接触")));
  cats.append(QgsRendererCategory(
      QStringLiteral("pinchout"),
      boundaryFill(QStringLiteral("#5D6E80"), 0.5, QStringLiteral("dash")),
      QObject::tr("尖灭")));
  cats.append(QgsRendererCategory(
      QStringLiteral("facies_change"), faciesChangeFill(hasBandField),
      QObject::tr("相变")));
  // 断层切割（首发单类型）：断层红粗描边（#D71414，resources/geology/faults
  // 调性；resources/geology/boundaries/bnd_fault_line.svg 同族图式）。
  cats.append(QgsRendererCategory(
      QStringLiteral("fault_cut"), boundaryFill(QStringLiteral("#D71414"), 1.0),
      QObject::tr("断层切割")));
  // 其余/未分类：常规细灰边（含空串——未标类型的相界落这一类；表外值同
  // 落 all-other 桶走中性样式，不猜类）。
  const QString normalTitle = QObject::tr("常规相界");
  cats.append(QgsRendererCategory(
      QStringLiteral(""), boundaryFill(QStringLiteral("#5D6E80"), 0.26), normalTitle));
  cats.append(QgsRendererCategory(QVariant(),
                                  boundaryFill(QStringLiteral("#5D6E80"), 0.26),
                                  normalTitle)); // all-other 值桶
  layer->setRenderer(new QgsCategorizedSymbolRenderer(QStringLiteral("boundary_kind"), cats));
}

// ---- C3（wave/deepen-perf）：井类别符号（Q/HS 1011—2016 表 K.1）--------------

QVariantList QgisStyleService::wellCategoryDefinitions()
{
  QVariantList out;
  for (int i = 0; i < kWellCategoryCount; ++i)
  {
    QVariantMap m;
    m.insert(QStringLiteral("id"), QString::fromLatin1(kWellCategories[i].id));
    m.insert(QStringLiteral("title"),
             QObject::tr(kWellCategories[i].title)); // NOOP 词面，此类无 QObject 上下文时归本类
    m.insert(QStringLiteral("glyph"), QString::fromLatin1(kWellCategories[i].glyph));
    out.append(m);
  }
  return out;
}

QString QgisStyleService::normalizeWellCategory(const QString &raw)
{
  const QString v = raw.trimmed();
  if (v.isEmpty())
    return QString();
  for (int i = 0; i < kWellCategoryCount; ++i)
    if (v == QLatin1String(kWellCategories[i].id) ||
        v == QObject::tr(kWellCategories[i].title))
      return QString::fromLatin1(kWellCategories[i].id);
  // 常见同义词（数据字段常见写法）。
  static const QHash<QString, QString> aliases = {
    { QStringLiteral("预探"), QStringLiteral("wildcat") },
    { QStringLiteral("初探井"), QStringLiteral("wildcat") },
    { QStringLiteral("野猫井"), QStringLiteral("wildcat") },
    { QStringLiteral("评价"), QStringLiteral("appraisal") },
    { QStringLiteral("正钻"), QStringLiteral("drilling") },
    { QStringLiteral("在钻"), QStringLiteral("drilling") },
    { QStringLiteral("待钻"), QStringLiteral("planned") },
    { QStringLiteral("计划井"), QStringLiteral("planned") },
    { QStringLiteral("发现"), QStringLiteral("discovery") },
    { QStringLiteral("工业油流"), QStringLiteral("oil_flow") },
    { QStringLiteral("油流井"), QStringLiteral("oil_flow") },
    { QStringLiteral("工业气流"), QStringLiteral("gas_flow") },
    { QStringLiteral("气流井"), QStringLiteral("gas_flow") },
    { QStringLiteral("参数"), QStringLiteral("parameter") },
    { QStringLiteral("科探井"), QStringLiteral("scientific") },
    { QStringLiteral("试油"), QStringLiteral("oil_test") },
    { QStringLiteral("干井"), QStringLiteral("dry") },
    { QStringLiteral("无显示"), QStringLiteral("dry") },
    { QStringLiteral("报废"), QStringLiteral("abandoned") },
    // 方向 31：开发区块井别常见写法。
    { QStringLiteral("生产井"), QStringLiteral("oil_prod") },
    { QStringLiteral("油井"), QStringLiteral("oil_prod") },
    { QStringLiteral("开发井"), QStringLiteral("oil_prod") },
    { QStringLiteral("生产油井"), QStringLiteral("oil_prod") },
    { QStringLiteral("气井"), QStringLiteral("gas_prod") },
    { QStringLiteral("生产气井"), QStringLiteral("gas_prod") },
    { QStringLiteral("注水"), QStringLiteral("water_injection") },
    { QStringLiteral("注水井"), QStringLiteral("water_injection") },
    { QStringLiteral("回注井"), QStringLiteral("water_injection") },
    { QStringLiteral("采水井"), QStringLiteral("water_prod") },
    { QStringLiteral("水源井"), QStringLiteral("water_prod") },
  };
  return aliases.value(v, v); // 未知词原样返回（不静默吞数据）
}

void QgisStyleService::applyWellCategoryStyle(QgsVectorLayer *layer, const QString &categoryField)
{
  if (!layer || !layer->isValid())
    return;
  if (layer->geometryType() != Qgis::GeometryType::Point)
    return;

  // 无类别字段 → 通用「探井」（外细环+实心盘，无字段时保持通用符号）。
  const int fieldIdx =
      categoryField.isEmpty() ? -1 : layer->fields().lookupField(categoryField);
  if (fieldIdx < 0)
  {
    layer->setRenderer(new QgsSingleSymbolRenderer(wellCategorySymbol(QString())));
    return;
  }

  // 数据字段驱动：类别桶同时登记规范 id 与中文词面（字段里两种写法都
  // 命中；其余值落通用符号桶）。
  QgsCategoryList cats;
  const QString genericTitle = QObject::tr("探井（通用）");
  for (int i = 0; i < kWellCategoryCount; ++i)
  {
    const QString id = QString::fromLatin1(kWellCategories[i].id);
    const QString title = QObject::tr(kWellCategories[i].title);
    cats.append(QgsRendererCategory(id, wellCategorySymbol(id), title));
    if (title != id)
      cats.append(QgsRendererCategory(title, wellCategorySymbol(id), title));
  }
  cats.append(QgsRendererCategory(QVariant(), wellCategorySymbol(QString()), genericTitle));
  layer->setRenderer(
      new QgsCategorizedSymbolRenderer(layer->fields().field(fieldIdx).name(), cats));
}

// ---- 方向35：演化迁移矢量符号 -----------------------------------------------

namespace
{

// 箭头线符号（进积/退积配色由调用侧给）。直线单头箭头，毫米单位。
std::unique_ptr<QgsLineSymbol> migrationArrowSymbol(const QColor &color, double shaftMm)
{
  QgsArrowSymbolLayer *arrow = new QgsArrowSymbolLayer();
  arrow->setIsCurved(false);
  arrow->setIsRepeated(false);
  arrow->setArrowStartWidth(shaftMm);            // 杆（起点）宽
  arrow->setArrowWidth(std::max(shaftMm * 2.0, 0.4)); // 头部张开宽
  arrow->setArrowWidthUnit(Qgis::RenderUnit::Millimeters);
  arrow->setArrowStartWidthUnit(Qgis::RenderUnit::Millimeters);
  arrow->setHeadLength(std::max(shaftMm * 3.0, 0.6));
  arrow->setHeadLengthUnit(Qgis::RenderUnit::Millimeters);
  arrow->setHeadThickness(std::max(shaftMm * 3.0, 0.6));
  arrow->setHeadThicknessUnit(Qgis::RenderUnit::Millimeters);
  arrow->setColor(color);
  std::unique_ptr<QgsLineSymbol> symbol = std::make_unique<QgsLineSymbol>();
  symbol->changeSymbolLayer(0, arrow);
  return symbol;
}

} // namespace

void QgisStyleService::applyMigrationVectorStyle(QgsVectorLayer *layer)
{
  if (!layer || !layer->isValid())
    return;
  if (layer->geometryType() != Qgis::GeometryType::Line)
    return;
  const bool hasKind = layer->fields().lookupField(QStringLiteral("vector_kind")) >= 0;
  const bool hasAdvance = layer->fields().lookupField(QStringLiteral("advance")) >= 0;
  if (!hasKind || !hasAdvance)
  {
    // 缺字段：只给中性灰箭头，不接管语义分色（字段口径是数据侧契约）。
    layer->setRenderer(new QgsSingleSymbolRenderer(
        migrationArrowSymbol(QColor(QStringLiteral("#5D6E80")), 0.25).release()));
    return;
  }

  // 进积蓝 / 退积红（地图域数据符号；前缘细、质心粗）。
  const QColor advanceColor(QStringLiteral("#1565B8"));
  const QColor retreatColor(QStringLiteral("#C62828"));
  auto *renderer = new QgsRuleBasedRenderer(
      migrationArrowSymbol(QColor(QStringLiteral("#5D6E80")), 0.25).release());
  QgsRuleBasedRenderer::Rule *root = renderer->rootRule();
  const auto addRule = [&root](std::unique_ptr<QgsLineSymbol> symbol, const QString &filter,
                               const QString &label) {
    root->appendChild(new QgsRuleBasedRenderer::Rule(symbol.release(), 0, 0, filter, label));
  };
  addRule(migrationArrowSymbol(advanceColor, 0.8),
          QStringLiteral("vector_kind = 'centroid' AND advance = 1"),
          QObject::tr("进积（质心）"));
  addRule(migrationArrowSymbol(retreatColor, 0.8),
          QStringLiteral("vector_kind = 'centroid' AND advance = 0"),
          QObject::tr("退积（质心）"));
  addRule(migrationArrowSymbol(advanceColor, 0.25),
          QStringLiteral("vector_kind = 'front' AND advance = 1"),
          QObject::tr("进积（前缘）"));
  addRule(migrationArrowSymbol(retreatColor, 0.25),
          QStringLiteral("vector_kind = 'front' AND advance = 0"),
          QObject::tr("退积（前缘）"));
  layer->setRenderer(renderer);
}

// ---- 方向 31（geological-symbols）：花纹/断层线型语义入口 ---------------------

namespace
{
  // 花纹分类渲染共用骨架：词表逐条经 valueBuckets 登记（规范 id/词面/同义
  // 词同桶），末尾 all-other 兜底桶（未命中值走纯色填充，不吞数据）。
  void applyPatternRenderer(QgsVectorLayer *layer, int fieldIdx,
                            const QVariantList &defs,
                            QgsFillSymbol *(*make)(const QString &),
                            const QString &fallbackTitle)
  {
    QgsCategoryList cats;
    for (const auto &v : defs)
    {
      const auto id = v.toMap().value(QStringLiteral("id")).toString();
      const auto title = v.toMap().value(QStringLiteral("title")).toString();
      for (const QString &bucket : GeoPatterns::valueBuckets(id))
        cats.append(QgsRendererCategory(bucket, make(id), title));
    }
    cats.append(QgsRendererCategory(QVariant(), make(QString()), fallbackTitle));
    layer->setRenderer(new QgsCategorizedSymbolRenderer(
        layer->fields().field(fieldIdx).name(), cats));
  }
} // namespace

void QgisStyleService::applyLithologyPatternStyle(QgsVectorLayer *layer,
                                                  const QString &lithologyField)
{
  if (!layer || !layer->isValid())
    return;
  if (layer->geometryType() != Qgis::GeometryType::Polygon)
    return;
  const int fieldIdx =
      lithologyField.isEmpty() ? -1 : layer->fields().lookupField(lithologyField);
  if (fieldIdx < 0)
    return; // 无语义字段：不接管渲染器（保持现状图面）

  applyPatternRenderer(layer, fieldIdx, GeoPatterns::lithologyDefinitions(),
                       GeoPatterns::lithologyFillSymbol, QObject::tr("其他岩性"));
}

void QgisStyleService::applyFaciesPatternStyle(QgsVectorLayer *layer,
                                               const QString &faciesField)
{
  if (!layer || !layer->isValid())
    return;
  if (layer->geometryType() != Qgis::GeometryType::Polygon)
    return;
  const int fieldIdx =
      faciesField.isEmpty() ? -1 : layer->fields().lookupField(faciesField);
  if (fieldIdx < 0)
    return;

  applyPatternRenderer(layer, fieldIdx, GeoPatterns::faciesDefinitions(),
                       GeoPatterns::faciesFillSymbol, QObject::tr("未划分相"));
}

void QgisStyleService::applyFaultLineLayerStyle(QgsVectorLayer *layer,
                                                const QString &kindField)
{
  if (!layer || !layer->isValid())
    return;
  if (layer->geometryType() != Qgis::GeometryType::Line)
    return;
  const int fieldIdx =
      kindField.isEmpty() ? -1 : layer->fields().lookupField(kindField);
  if (fieldIdx < 0)
    return; // 无语义字段：不接管渲染器

  // 断层四类（normal/reverse/strike/inferred）：词面双桶 + all-other 走
  // 推测式（未标类型的线不做实线断言——不确定地质体虚线惯例）。
  QgsCategoryList cats;
  const struct
  {
    const char *id;
    const char *title;
  } kinds[] = {
    { "normal", QT_TRANSLATE_NOOP("QgisStyleService", "正断层") },
    { "reverse", QT_TRANSLATE_NOOP("QgisStyleService", "逆断层") },
    { "strike", QT_TRANSLATE_NOOP("QgisStyleService", "走滑断层") },
    { "inferred", QT_TRANSLATE_NOOP("QgisStyleService", "推测断层") },
  };
  QSet<QString> seen;
  for (const auto &k : kinds)
  {
    const QString id = QString::fromLatin1(k.id);
    const QString title = QObject::tr(k.title);
    cats.append(QgsRendererCategory(id, GeoPatterns::faultLineSymbol(id), title));
    if (!seen.contains(title))
    {
      seen.insert(title);
      cats.append(QgsRendererCategory(title, GeoPatterns::faultLineSymbol(id), title));
    }
  }
  const QString otherTitle = QObject::tr("未分类断层");
  cats.append(
      QgsRendererCategory(QVariant(), GeoPatterns::faultLineSymbol(QString()), otherTitle));
  layer->setRenderer(new QgsCategorizedSymbolRenderer(
      layer->fields().field(fieldIdx).name(), cats));
}

// ---- 方向 31 批 5：样式版本语义（符号覆盖随工程持久化）------------------------

namespace
{
  constexpr const char *kSymbolSemanticsKey = "paleo/symbolSemantics";
}

int QgisStyleService::symbolTableVersion()
{
  return 1;
}

bool QgisStyleService::applySymbolOverride(QgsVectorLayer *layer, const QString &family,
                                           const QString &patternId)
{
  if (!layer || !layer->isValid() || patternId.isEmpty())
    return false;
  // 词表命中校验（familyOf 精确族匹配，跨族 id 不收）+ 几何类型匹配
  //（面花纹不挂线层、线型不挂面层）。
  if (GeoPatterns::familyOf(patternId) != family)
    return false;
  if (family == QLatin1String("lithology"))
  {
    if (layer->geometryType() != Qgis::GeometryType::Polygon)
      return false;
    layer->setRenderer(new QgsSingleSymbolRenderer(
        GeoPatterns::lithologyFillSymbol(patternId)));
  }
  else if (family == QLatin1String("facies"))
  {
    if (layer->geometryType() != Qgis::GeometryType::Polygon)
      return false;
    layer->setRenderer(
        new QgsSingleSymbolRenderer(GeoPatterns::faciesFillSymbol(patternId)));
  }
  else if (family == QLatin1String("line"))
  {
    if (layer->geometryType() != Qgis::GeometryType::Line)
      return false;
    std::unique_ptr<QgsLineSymbol> sym(GeoPatterns::lineSymbolFor(patternId));
    if (!sym)
      return false;
    layer->setRenderer(new QgsSingleSymbolRenderer(sym.release()));
  }
  else
    return false;

  QVariantMap semantics;
  semantics.insert(QStringLiteral("family"), family);
  semantics.insert(QStringLiteral("id"), patternId);
  semantics.insert(QStringLiteral("version"), symbolTableVersion());
  layer->setCustomProperty(kSymbolSemanticsKey, semantics);
  return true;
}

bool QgisStyleService::restoreSymbolOverride(QgsVectorLayer *layer)
{
  if (!layer)
    return false;
  const QVariant prop = layer->customProperty(kSymbolSemanticsKey);
  if (prop.userType() != QMetaType::QVariantMap)
    return false; // 无覆盖/老工程异构值：不重放（保持 .qgs 原样 renderer）
  const QVariantMap semantics = prop.toMap();
  return applySymbolOverride(layer, semantics.value(QStringLiteral("family")).toString(),
                             semantics.value(QStringLiteral("id")).toString());
}

void QgisStyleService::restoreAllSymbolOverrides(QgsProject *project)
{
  if (!project)
    return;
  const auto layers = project->mapLayers();
  for (auto it = layers.cbegin(); it != layers.cend(); ++it)
    if (auto *vector = qobject_cast<QgsVectorLayer *>(it.value()))
      restoreSymbolOverride(vector);
}
