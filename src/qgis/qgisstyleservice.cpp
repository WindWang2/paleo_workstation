// 层：QGIS 封装
#include "qgisstyleservice.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>

#include <qgscategorizedsymbolrenderer.h>
#include <qgsexpression.h>
#include <qgsfillsymbol.h>
#include <qgsmaplayer.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h> // QgsSimpleMarkerSymbolLayer（QGIS 4 无独立头）
#include <qgspallabeling.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbollayer.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

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

  // 12 类探井符号（存储 id → 符号）。序 = 表 K.1 类别序；词面与色源见
  // wellCategoryDefinitions()。未知 id → 通用「探井」。
  QgsMarkerSymbol *wellCategorySymbol(const QString &id)
  {
    const QString ink = QStringLiteral("#333333");
    if (id == QLatin1String("wildcat")) // 预探井：双环
      return wellSymbolLayers({ ringProps(ink, 0.5, 6.0), ringProps(ink, 0.4, 3.4) });
    if (id == QLatin1String("appraisal")) // 评价井：单环 + 中点
      return wellSymbolLayers({ ringProps(ink, 0.5, 6.0), diskProps(ink, 1.8) });
    if (id == QLatin1String("drilling")) // 正钻井：环 + 半填盘
      return wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("semi_circle"), ink, 4.4) });
    if (id == QLatin1String("planned")) // 待钻井：虚线环
      return wellSymbolLayers({ ringProps(ink, 0.5, 6.0, QStringLiteral("dash")) });
    if (id == QLatin1String("discovery")) // 发现井：环 + 星
      return wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("star"), QStringLiteral("#B8860B"), 4.6) });
    if (id == QLatin1String("oil_flow")) // 工业油流井：绿盘 + 细环
      return wellSymbolLayers(
          { ringProps(ink, 0.4, 6.0), diskProps(QStringLiteral("#00AA00"), 4.4) });
    if (id == QLatin1String("gas_flow")) // 工业气流井：红环 + 红斜线
      return wellSymbolLayers({ ringProps(QStringLiteral("#FF3300"), 0.6, 6.0),
                                glyphProps(QStringLiteral("line"), QStringLiteral("#FF3300"), 6.4, 45.0) });
    if (id == QLatin1String("parameter")) // 参数井：环内十字
      return wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("cross"), ink, 3.6) });
    if (id == QLatin1String("scientific")) // 科学探索井：环内斜十字
      return wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("cross2"), ink, 3.6, 45.0) });
    if (id == QLatin1String("oil_test")) // 试油井：环内上箭头
      return wellSymbolLayers(
          { ringProps(ink, 0.5, 6.0), glyphProps(QStringLiteral("arrow"), ink, 3.4) });
    if (id == QLatin1String("dry")) // 未见显示井：灰空心环
      return wellSymbolLayers({ ringProps(QStringLiteral("#999999"), 0.5, 6.0) });
    if (id == QLatin1String("abandoned")) // 报废井：灰环 + 灰叉
      return wellSymbolLayers(
          { ringProps(QStringLiteral("#999999"), 0.5, 6.0),
            glyphProps(QStringLiteral("cross2"), QStringLiteral("#999999"), 3.6, 45.0) });
    // 通用「探井」：外细环 + 实心盘（0.73 外径比）
    return wellSymbolLayers({ ringProps(QStringLiteral("#1B73D0"), 0.5, 6.0),
                              diskProps(QStringLiteral("#1B73D0"), 4.4) });
  }

  struct WellCategoryDef
  {
    const char *id;
    const char *title;
    const char *glyph;
  };

  // 表 K.1 十二类（词面/图式描述；色源与resources/geology 井型目录一致）。
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
  auto boundaryFill = [](const QString &outlineColor, double outlineWidthMm) {
    QVariantMap props;
    props.insert(QStringLiteral("color"), QStringLiteral("188,199,209,60"));
    props.insert(QStringLiteral("outline_color"), outlineColor);
    props.insert(QStringLiteral("outline_width"), QString::number(outlineWidthMm));
    return QgsFillSymbol::createSimple(props).release();
  };

  QgsCategoryList cats;
  // 断层切割（首发单类型）：断层红粗描边（#D71414，resources/geology/faults
  // 调性；resources/geology/boundaries/bnd_fault_line.svg 同族图式）。
  cats.append(QgsRendererCategory(
      QStringLiteral("fault_cut"), boundaryFill(QStringLiteral("#D71414"), 1.0),
      QObject::tr("断层切割")));
  // 其余/未分类：常规细灰边（含空串——未标类型的相界落这一类）。
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
