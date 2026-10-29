// 层：视图
// token 例外：DESIGN 数据符号例外：测井/地层调色板、QGIS 井点/轨迹/相图符号与文字缓冲；图表 chrome 已转 token。（地震振幅色带例外随 SectionPanel 迁至 previewsectionpanel.h；tools/ui-token-exceptions.json 精确计数）。
#pragma once

// 方向20 轮4：datapreviewtabs.cpp 按资产类型拆分后的跨 TU 内部辅助。
// 原为该 .cpp 顶部的匿名命名空间（1421 行）——实测它被 buildContent 的**多个**
// 类型分支共用（caption8 5/9、qssHex 3/9、monoFont 2/9、CurvePanel 2/9…），
// 拆到独立文件后必须跨 TU 可见，故抽成内部头。
// 只放**多分支共用**的短工具与小组件；单分支专用的留在该分支文件里。

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QList>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QPushButton>
#include <QRadioButton>
#include <QRectF>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <functional> // std::function（geoJsonTableRow 取值回调）
#include <memory>     // std::shared_ptr（rasterStyleTouchedGuard 哨兵）

// ---- 项目内依赖（照 datapreviewtabs.cpp 的 include 列表，不逐个编译错误补）----
#include "datapreviewtabs.h"
#include "../paleotheme.h"
#include "../../catalog/datacatalog.h"
#include "../../domain/sectiontrace.h" // SegyTrace（方向 59：previewdoc.h 瘦身后直取）
#include "../../io/lasdoc.h"           // LasCurve/LasDoc（同上，白名单门面）
#include "../../services/previewdoc.h"
#include "../../services/welllogset.h"
#include "../seismic3d/seismic3dviewpanel.h"
#include "../seismicsection/seismicsectioncanvas.h"
#include "../wellcomposite/wellcompositepanel.h"
#include "../decorations/paleodecorations.h"
#include "previewhistogramwidget.h"
#include "previewmappage.h"
#include "previewmapstates.h"
#include "previewprofilepanel.h"
#include "previewtocpanel.h"

// 方向97：CurvePanel/SectionPanel 两个大组件类已拆至自有头（650/226 行，
// 超出本头「短工具与小组件」章程）；此处组合导出——族内 TU 经 internal.h 仍全量可见。
#include "previewcurvepanel.h"
#include "previewsectionpanel.h"
#include "../../qgis/factorcontour.h"
#include "../../qgis/previewrasteranalysis.h"

// ---- QGIS 符号/图层（与下方「类型→头」清单去重，此块只留不重复的三个）----
#include <qgsfield.h>
#include <qgsrasterlayer.h>
#include <qgsmaplayer.h>


// ---- QGIS 头（类型→头用 grep 在本机 QGIS include 里反查，不靠命名规则猜）----
#include <qgis.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsexpression.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfillsymbol.h>
#include <qgsgeometry.h>
#include <qgslinesymbol.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgspointxy.h>
#include <qgsproject.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgstextbuffersettings.h>
#include <qgstextformat.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

namespace paleo::datapreview_detail {

// 统计页标签写入（B-05 失败回调的诚实状态面）：成功逐项写数值，失败/取消
// 五标签一次落「不可用」——层位/成图两个统计页共用，label 名 = 前缀+后缀。
inline void setStatLabel(QWidget *page, const char *name, const QString &text)
{
  if (auto *label = page->findChild<QLabel *>(QString::fromLatin1(name)))
    label->setText(text);
}
inline void markStatsUnavailable(QWidget *page, const char *prefix)
{
  const QString unavailable = QObject::tr("不可用");
  for (const char *suffix : {"Min", "Max", "Mean", "Std", "Valid"})
    setStatLabel(page, (QString::fromLatin1(prefix) + QLatin1String(suffix)).toLatin1(), unavailable);
}

// 大图提示与概览预热（层位/配准影像共用）。保留提示条插入时机及 host 生命周期。
inline void addRasterPyramidHint(PreviewDocService *doc, QgsRasterLayer *raster,
                                 const QString &assetId, QWidget *host, QVBoxLayout *layout)
{
  const QString bigHint = PreviewRasterAnalysis::bigRasterHint(raster);
  if (bigHint.isEmpty())
    return;
  layout->addWidget(PreviewMapStates::buildBigRasterHintBar(bigHint, host));
  if (doc)
  {
    QPointer<QgsRasterLayer> rasterGuard(raster);
    QObject::connect(doc, &PreviewDocService::rasterPyramidFinished, host,
                     [rasterGuard, assetId](const QString &doneId, bool ok) {
                       if (doneId != assetId || !ok || !rasterGuard)
                         return;
                       rasterGuard->reload();
                       rasterGuard->triggerRepaint();
                     });
    doc->ensureRasterPyramidVersion(assetId);
  }
}

// 迟到的统计回调只在「用户没换过栅格符号」时套用默认色带。QgsRasterLayer
// ::setRenderer 会 delete 旧渲染器——捕获 renderer 裸指针做地址比较，在 TOC
// 改色带/拉伸后即悬垂（UB；分配器复用地址还会误判成「没换过」）。改用 TOC 的
// 样式变更信号置位：TOC 快调是本预览里唯一替换 renderer 的用户入口。
inline std::shared_ptr<bool> rasterStyleTouchedGuard(
    PreviewMapPage *page, const QPointer<QgsRasterLayer> &layer)
{
  const auto touched = std::make_shared<bool>(false);
  QObject::connect(page->tocPanel(), &PreviewTocPanel::rasterStyleChanged, page,
                   [layer, touched](QgsRasterLayer *changed) {
                     if (changed == layer)
                       *touched = true;
                   });
  return touched;
}

// 文档/工作簿资产的 RAW 原件锚定：取**最高号** RAW（口径同层位预览的 RAW
// 选取）。relocateVersionSource 重定位成功后是「追加新 RAW、旧死路径记录留
// 表」，第一条 RAW 恒为最初那个已不存在的路径——取第一条会让预览永远停在
// 「找不到源文件」，且每次重试都白往 catalog 追加一条重复 RAW。空 id =
// 该资产没有 RAW 版本。
inline CatalogVersion latestRawVersion(const DataCatalog *cat, const QString &assetId)
{
  CatalogVersion raw;
  for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
    if (cv.stage == QLatin1String("RAW") && cv.versionNumber >= raw.versionNumber)
      raw = cv;
  return raw;
}

// GeoJSON 预览的属性键与要素计数。规模护栏：整份 QJsonDocument 解析是 GUI
// 线程上的整读 + 全量 DOM 构建，大 GeoJSON 直接冻住建标签（1000 行截断只
// 作用于表格，管不到解析与键收集）。阈值内保持原口径（DOM）；超阈值不解
// DOM——属性键走服务层流式摘要（io/streaming：无 DOM、不整读、内存平坦），
// 表格行由调用方从 OGR 图层流取（地图路径本来就要开它，不重复劳动）。
// 失败时 *error 非空（调用方据此出失败态）。
struct GeoJsonPreviewKeys
{
  QStringList propKeys;
  qint64 featureCount = 0;
  bool domPreview = false; // 真 = doc 已全量解析（表格行的 DOM 来源）
  QJsonDocument doc;
};

inline GeoJsonPreviewKeys geoJsonPreviewKeys(PreviewDocService *doc, const QString &abs,
                                            QString *error)
{
  constexpr qint64 kDomLimitBytes = 8 * 1024 * 1024;
  GeoJsonPreviewKeys out;
  out.domPreview = QFileInfo(abs).size() <= kDomLimitBytes;
  if (!out.domPreview)
  {
    PreviewDocService::GeoJsonSummary summary;
    if (!PreviewDocService::geoJsonSummaryAt(abs, &summary, error))
      return out;
    out.propKeys = summary.propKeys; // 服务层已排序去重
    out.featureCount = summary.featureCount;
    return out;
  }
  if (!doc || !doc->geoJsonDocumentAt(abs, &out.doc, error))
    return out;
  const QJsonArray features =
      out.doc.object().value(QStringLiteral("features")).toArray();
  out.featureCount = features.size();
  // 键查重走哈希：QStringList::contains 是 O(K) 线性扫，要素数×键数的
  // 字符串比较在大文件上是 GUI 线程热点。
  QSet<QString> seenKeys;
  for (const QJsonValue &fv : features)
  {
    const QJsonObject props =
        fv.toObject().value(QStringLiteral("properties")).toObject();
    for (auto it = props.begin(); it != props.end(); ++it)
      if (!seenKeys.contains(it.key()))
      {
        seenKeys.insert(it.key());
        out.propKeys.append(it.key());
      }
  }
  return out;
}

// GeoJSON 属性值序列化（DOM 路径；null 与嵌套数组如实呈现）。
inline QString geoJsonValueText(const QJsonValue &val)
{
  if (val.isDouble())
    return QString::number(val.toDouble());
  if (val.isString())
    return val.toString();
  if (val.isBool())
    return val.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  if (val.isNull())
    return QStringLiteral("null");
  return QString::fromUtf8(
      QJsonDocument(val.toArray()).toJson(QJsonDocument::Compact));
}

// GeoJSON 属性表行装配：行数据来源两条（阈值内 DOM 要素 / 超阈值 OGR 要素
// 流），单元格口径统一（序号 / 几何类型 / 属性值逐列经 valueAt 取）。
inline void geoJsonTableRow(QTableWidget *table, int row, const QString &geomType,
                            const std::function<QString(int)> &valueAt)
{
  auto *idItem = new QTableWidgetItem(QString::number(row + 1));
  idItem->setTextAlignment(Qt::AlignCenter);
  idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
  table->setItem(row, 0, idItem);

  auto *geomItem =
      new QTableWidgetItem(geomType.isEmpty() ? QStringLiteral("—") : geomType);
  geomItem->setTextAlignment(Qt::AlignCenter);
  geomItem->setFlags(geomItem->flags() & ~Qt::ItemIsEditable);
  table->setItem(row, 1, geomItem);

  for (int c = 0; c + 2 < table->columnCount(); ++c)
  {
    auto *valItem = new QTableWidgetItem(valueAt(c));
    valItem->setFlags(valItem->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, c + 2, valItem);
  }
}

// token 色 → QSS 大写 #RRGGBB（与 paleotheme 内部 qssHex 同口径，逐字节可比）。
inline QString qssHex(const QColor &c)
{
  return c.name().toUpper();
}

inline QLabel *caption8(const QString &text, QWidget *parent)
{
  auto *l = new QLabel(text, parent);
  QFont f = l->font();
  f.setPointSize(PaleoTheme::tokens().labelPt);
  l->setFont(f);
  PaleoTheme::applyThemedStyleSheet(l, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  return l;
}

inline QLabel *stateLabel(const QString &text, QWidget *parent, bool error = false)
{
  auto *l = new QLabel(text, parent);
  l->setAlignment(Qt::AlignCenter);
  l->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(l, [error] {
    return error ? QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().errorText))
                 : PaleoTheme::mutedCaptionStyleSheet();
  });
  l->setObjectName(QStringLiteral("stateText"));
  return l;
}

inline QLabel *warnLabel(const QString &text, QWidget *parent)
{
  auto *l = new QLabel(text, parent);
  PaleoTheme::applyThemedStyleSheet(l, [] {
    return QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().warningText));
  });
  l->setWordWrap(true);
  return l;
}

// 「用系统程序打开」兜底行：按钮 + 就地错误文本（打开失败时浮现）。
// 预览页不再常驻——只在无内嵌预览或需用系统程序打开原件时用。
inline QWidget *makeOpenExternalRow(const QString &absPath, QWidget *parent)
{
  auto *row = new QWidget(parent);
  auto *rl = new QHBoxLayout(row);
  rl->setContentsMargins(0, 0, 0, 0);
  rl->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *btn = new QPushButton(QObject::tr("用系统程序打开"), row);
  btn->setObjectName(QStringLiteral("openExternalBtn"));
  auto *openErr = warnLabel(QString(), row);
  openErr->setObjectName(QStringLiteral("openErrorText"));
  openErr->setVisible(false);
  QObject::connect(btn, &QPushButton::clicked, row, [absPath, openErr]() {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(absPath)))
    {
      openErr->setText(QObject::tr("系统未能打开该文件\n%1").arg(absPath));
      openErr->setVisible(true);
    }
  });
  rl->addWidget(btn);
  rl->addWidget(openErr, 1);
  return row;
}

// DESIGN.md mono：数值/坐标/深度一律 JetBrains Mono 9pt tnum。
inline QFont monoFont()
{
  return PaleoTheme::monoFont(); // wave3/ux-consistency：共用注册/vendor 路径
}

inline QLabel *valueLabel(const QString &text, QWidget *parent, bool mono = false)
{
  auto *v = new QLabel(text, parent);
  PaleoTheme::applyThemedStyleSheet(v, [] {
    return QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().text));
  });
  if (mono)
  {
    v->setFont(monoFont());
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter); // 数字列右对齐（§4）
  }
  return v;
}

// 预览页内工具条（测区全景 / GeoJSON 相图共用）：surface-alt 底 + 安静按钮组；
// checked = chip 语义（primary 描边 + 浮起面底，同 ribbonStyleSheet checked 范式）。
inline void stylePreviewToolBar(QWidget *bar)
{
  bar->setObjectName(QStringLiteral("previewToolBar"));
  PaleoTheme::applyThemedStyleSheet(bar, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QWidget#previewToolBar { background: %1; border-bottom: 1px solid %2; }"
        "QWidget#previewToolBar QToolButton { background: %3; border: 1px solid %2;"
        " border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.label}pt; color: %4; }"
        "QWidget#previewToolBar QToolButton:hover { background: %5; border-color: %6; }"
        "QWidget#previewToolBar QToolButton:pressed { background: %2; }"
        "QWidget#previewToolBar QToolButton:checked { background: %5;"
        " border-color: %7; color: %8; font-weight: 500; }"))
        .arg(qssHex(t.surfaceAlt), qssHex(t.border), qssHex(t.surface),
             qssHex(t.text), qssHex(t.surfaceAltRaised), qssHex(t.textDisabled),
             qssHex(t.primary), qssHex(t.primaryText));
  });
}

// 「呈现模式」切换条（xml 井道图/表格）：chip 语义
// checkable QToolButton——checked = primary 描边 + 浮起面底。
inline void styleViewSwitchBar(QWidget *bar)
{
  PaleoTheme::applyThemedStyleSheet(bar, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QToolButton { background: %1; border: 1px solid %2; border-radius: {rounded.sm}px;"
        " padding: {spacing.xs}px {spacing.md}px; font-size: {typography.label}pt; color: %3; }"
        "QToolButton:hover { background: %4; }"
        "QToolButton:checked { background: %4; border-color: %5; color: %6;"
        " font-weight: 600; }"))
        .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
             qssHex(t.surfaceAltRaised), qssHex(t.primary), qssHex(t.primaryText));
  });
}

inline void setNumericItem(QTableWidgetItem *it)
{
  it->setFont(monoFont());
  it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

static QColor pickCurveColor(const QString &name, int index)
{
  const QString upper = name.toUpper();
  if (upper == QLatin1String("GR") || upper.startsWith(QLatin1String("GR_")))
    return QColor(QStringLiteral("#2E7D32")); // Forest Green (Standard Gamma Ray)
  if (upper == QLatin1String("AC") || upper.startsWith(QLatin1String("AC_")) || upper == QLatin1String("DT"))
    return QColor(QStringLiteral("#0288D1")); // Cyan/Light Blue (Acoustic Sonic)
  if (upper == QLatin1String("DEN") || upper.startsWith(QLatin1String("DEN_")) || upper == QLatin1String("RHOB"))
    return QColor(QStringLiteral("#D32F2F")); // Red (Bulk Density)
  if (upper == QLatin1String("CNL") || upper == QLatin1String("NPHI"))
    return QColor(QStringLiteral("#E65100")); // Orange (Neutron Porosity)
  if (upper.startsWith(QLatin1String("RT")) || upper.startsWith(QLatin1String("RD")) || upper.startsWith(QLatin1String("LLD")))
    return QColor(QStringLiteral("#7B1FA2")); // Purple (Deep Resistivity)
  if (upper == QLatin1String("SP"))
    return QColor(QStringLiteral("#00796B")); // Teal (Spontaneous Potential)
  if (upper.startsWith(QLatin1String("CAL")))
    return QColor(QStringLiteral("#455A64")); // Slate (Caliper)

  static const QStringList kPalette{
    QStringLiteral("#1B73D0"),
    QStringLiteral("#E65100"),
    QStringLiteral("#7B1FA2"),
    QStringLiteral("#00838F"),
    QStringLiteral("#C2185B"),
    QStringLiteral("#5D6E80"),
    QStringLiteral("#F57C00"),
    QStringLiteral("#388E3C")
  };
  return QColor(kPalette.at(index % kPalette.size()));
}

static QColor faciesColor(const QString &name)
{
  const QString lower = name.toLower();
  // 长关键词先判：「半深湖」包含「深湖」，先判深湖会把半深湖染成深湖色。
  if (lower.contains(QStringLiteral("半深湖")) || lower.contains(QStringLiteral("semi-deep")))
    return QColor(QStringLiteral("#80DEEA"));
  if (lower.contains(QStringLiteral("深湖")) || lower.contains(QStringLiteral("深盆")) || lower.contains(QStringLiteral("deep basin")))
    return QColor(QStringLiteral("#4DD0E1"));
  if (lower.contains(QStringLiteral("滨浅湖")) || lower.contains(QStringLiteral("浅湖")) || lower.contains(QStringLiteral("shallow lake")) || lower.contains(QStringLiteral("lake")))
    return QColor(QStringLiteral("#81D4FA"));
  if (lower.contains(QStringLiteral("滩坝")) || lower.contains(QStringLiteral("滩砂")) || lower.contains(QStringLiteral("beach bar")))
    return QColor(QStringLiteral("#FFF59D"));
  if (lower.contains(QStringLiteral("水下分流河道")) || lower.contains(QStringLiteral("distributary channel")))
    return QColor(QStringLiteral("#FFD54F"));
  if (lower.contains(QStringLiteral("河口坝")) || lower.contains(QStringLiteral("mouth bar")))
    return QColor(QStringLiteral("#FFE082"));
  if (lower.contains(QStringLiteral("远砂坝")) || lower.contains(QStringLiteral("distal bar")))
    return QColor(QStringLiteral("#FFE57F"));
  if (lower.contains(QStringLiteral("分流间湾")) || lower.contains(QStringLiteral("interdistributary")))
    return QColor(QStringLiteral("#DCEDC8"));
  if (lower.contains(QStringLiteral("席状砂")) || lower.contains(QStringLiteral("sheet sand")))
    return QColor(QStringLiteral("#FFF176"));
  if (lower.contains(QStringLiteral("三角洲前缘")) || lower.contains(QStringLiteral("delta front")))
    return QColor(QStringLiteral("#FFE082"));
  if (lower.contains(QStringLiteral("三角洲平原")) || lower.contains(QStringLiteral("delta plain")))
    return QColor(QStringLiteral("#E6EE9C"));
  if (lower.contains(QStringLiteral("前三角洲")) || lower.contains(QStringLiteral("prodelta")))
    return QColor(QStringLiteral("#B2DFDB"));
  if (lower.contains(QStringLiteral("三角洲")) || lower.contains(QStringLiteral("delta")))
    return QColor(QStringLiteral("#FFE082"));
  if (lower.contains(QStringLiteral("冲积扇")) || lower.contains(QStringLiteral("alluvial")))
    return QColor(QStringLiteral("#FFAB91"));
  if (lower.contains(QStringLiteral("河流")) || lower.contains(QStringLiteral("fluvial")) || lower.contains(QStringLiteral("channel")))
    return QColor(QStringLiteral("#FFB74D"));
  if (lower.contains(QStringLiteral("碳酸盐")) || lower.contains(QStringLiteral("台地")) || lower.contains(QStringLiteral("carbonate")) || lower.contains(QStringLiteral("platform")))
    return QColor(QStringLiteral("#A5D6A7"));
  if (lower.contains(QStringLiteral("生物礁")) || lower.contains(QStringLiteral("礁滩")) || lower.contains(QStringLiteral("reef")))
    return QColor(QStringLiteral("#80CBC4"));
  if (lower.contains(QStringLiteral("陆棚")) || lower.contains(QStringLiteral("浅海")) || lower.contains(QStringLiteral("shelf")) || lower.contains(QStringLiteral("marine")))
    return QColor(QStringLiteral("#90CAF9"));
  if (lower.contains(QStringLiteral("潮坪")) || lower.contains(QStringLiteral("tidal")))
    return QColor(QStringLiteral("#D7CCC8"));
  if (lower.contains(QStringLiteral("浊积")) || lower.contains(QStringLiteral("重力流")) || lower.contains(QStringLiteral("turbidite")))
    return QColor(QStringLiteral("#FFCC80"));

  static const QVector<QColor> fallbackPalette = {
    QColor(QStringLiteral("#81D4FA")),
    QColor(QStringLiteral("#FFE082")),
    QColor(QStringLiteral("#A5D6A7")),
    QColor(QStringLiteral("#FFAB91")),
    QColor(QStringLiteral("#CE93D8")),
    QColor(QStringLiteral("#FFF59D")),
    QColor(QStringLiteral("#80CBC4")),
    QColor(QStringLiteral("#B0BEC5")),
    QColor(QStringLiteral("#FFCC80")),
    QColor(QStringLiteral("#B39DDB"))
  };
  const uint h = qHash(name);
  return fallbackPalette.at(h % fallbackPalette.size());
}

static std::unique_ptr<QgsSymbol> createFaciesSymbol(Qgis::GeometryType geomType, const QColor &color)
{
  const QColor strokeColor = color.darker(150);
  if (geomType == Qgis::GeometryType::Point)
  {
    QVariantMap props;
    props[QStringLiteral("name")] = QStringLiteral("circle");
    props[QStringLiteral("color")] = color.name(QColor::HexArgb);
    props[QStringLiteral("outline_color")] = strokeColor.name();
    props[QStringLiteral("outline_width")] = QStringLiteral("0.8");
    props[QStringLiteral("size")] = QStringLiteral("5.5");
    return QgsMarkerSymbol::createSimple(props);
  }
  else if (geomType == Qgis::GeometryType::Line)
  {
    QVariantMap props;
    props[QStringLiteral("line_color")] = color.name();
    props[QStringLiteral("line_width")] = QStringLiteral("1.5");
    return QgsLineSymbol::createSimple(props);
  }
  else // Polygon
  {
    QVariantMap props;
    QColor fill = color;
    fill.setAlpha(200);
    props[QStringLiteral("color")] = QStringLiteral("%1,%2,%3,%4")
                                        .arg(fill.red()).arg(fill.green()).arg(fill.blue()).arg(fill.alpha());
    props[QStringLiteral("outline_color")] = strokeColor.name();
    props[QStringLiteral("outline_width")] = QStringLiteral("0.8");
    props[QStringLiteral("outline_style")] = QStringLiteral("solid");
    return QgsFillSymbol::createSimple(props);
  }
}

// ---- P2 地图化辅助（井位落图 D2.6/D2.8、等值线 D2.1）----

// 工程局部网格内存点层（井位/分层顶点；私有层不进 QgsProject）。
inline QgsVectorLayer *makeMemoryPointLayer( const QString &name, QWidget *owner )
{
  const QString wkt = DataCatalog::localGridCrsWkt();
  auto *vl = new QgsVectorLayer(
      QStringLiteral( "Point?crs=WKT:%1&field=name:string(64)&field=role:string(16)&field=entity_id:string(128)&index=yes" )
          .arg( wkt ),
      name, QStringLiteral( "memory" ) );
  vl->setParent( owner ); // 所有权归预览页宿主（画布只引用裸指针）
  return vl;
}

inline QString uniqueWellPointId(DataCatalog *catalog, const QString &name)
{
  const auto ids = catalog->wellsMatchingName(name);
  return ids.size() == 1 ? ids.front() : QString();
}

inline void addMemoryPoint( QgsVectorLayer *vl, double x, double y, const QString &name,
                     const QString &role, const QString &entityId = QString() )
{
  if ( !vl )
    return;
  QgsFeature f( vl->fields() );
  f.setAttribute( QStringLiteral( "name" ), name );
  f.setAttribute( QStringLiteral( "role" ), role );
  f.setAttribute( QStringLiteral( "entity_id" ), entityId );
  f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( x, y ) ) );
  vl->dataProvider()->addFeature( f );
  vl->updateExtents();
}

// 井名标注（name 字段直排 + 白晕缓冲）。井位/分层顶点/测区全景共用。
inline void applyPointNameLabels( QgsVectorLayer *vl )
{
  if ( !vl )
    return;
  vl->setLabelsEnabled( true );
  QgsPalLayerSettings pal;
  pal.fieldName = QStringLiteral( "name" );
  pal.isExpression = false;
  QgsTextFormat fmt;
  QFont font = PaleoTheme::bodyFont(PaleoTheme::tokens().bodyPt); font.setWeight(QFont::Medium);
  fmt.setFont( font );
  fmt.setSize(PaleoTheme::tokens().bodyPt);
  fmt.setSizeUnit( Qgis::RenderUnit::Points );
  fmt.setColor( QColor( QStringLiteral( "#24303E" ) ) );
  QgsTextBufferSettings buf;
  buf.setEnabled( true );
  buf.setSize( 1.5 );
  buf.setColor( Qt::white );
  fmt.setBuffer( buf );
  pal.setFormat( fmt );
  vl->setLabeling( new QgsVectorLayerSimpleLabeling( pal ) );
}

// 点层符号：普通井 #1B73D0 空心圆 + 高亮井加粗描边 + 名称标注开关。
inline void stylePointLayer( QgsVectorLayer *vl, bool withLabels )
{
  if ( !vl )
    return;
  QgsCategoryList cats;
  QVariantMap props;
  props[QStringLiteral( "name" )] = QStringLiteral( "circle" );
  props[QStringLiteral( "color" )] = QStringLiteral( "255,255,255,220" );
  props[QStringLiteral( "outline_color" )] = QStringLiteral( "#1B73D0" );
  props[QStringLiteral( "outline_width" )] = QStringLiteral( "1.2" );
  props[QStringLiteral( "size" )] = QStringLiteral( "5" );
  cats.append( QgsRendererCategory( QStringLiteral( "well" ),
                                    QgsMarkerSymbol::createSimple( props ).release(),
                                    QObject::tr( "井位" ) ) );
  QVariantMap hprops = props;
  hprops[QStringLiteral( "color" )] = QStringLiteral( "27,115,208,90" );
  hprops[QStringLiteral( "outline_width" )] = QStringLiteral( "2.2" );
  hprops[QStringLiteral( "size" )] = QStringLiteral( "7.5" );
  cats.append( QgsRendererCategory( QStringLiteral( "highlight" ),
                                    QgsMarkerSymbol::createSimple( hprops ).release(),
                                    QObject::tr( "当前井" ) ) );
  vl->setRenderer( new QgsCategorizedSymbolRenderer( QStringLiteral( "role" ), cats ) );
  if ( withLabels )
    applyPointNameLabels( vl );
}

// 测区全景井位符号：Q/HS 1011—2016《勘探管理图件图册编制规范》表 K.1
// 通用「探井」图式（TJLBD1-5）——外细圆环 + 内实心圆盘靶标（圆盘≈0.73
// 外径，与环间留细缝）。规范默认黑墨；彩色工作图用应用蓝 #1B73D0。
// 全景是固定内容画布，井位层只有「井位」一种符号。
// 尺寸按用户要求收敛（外环 4.2 / 实心 3.0，与看板 styleWells 同图式）。
inline void styleSurveyWellLayer( QgsVectorLayer *vl )
{
  if ( !vl )
    return;
  QVariantMap ring;
  ring[QStringLiteral( "name" )] = QStringLiteral( "circle" );
  ring[QStringLiteral( "color" )] = QStringLiteral( "255,255,255,0" );
  ring[QStringLiteral( "outline_color" )] = QStringLiteral( "#1B73D0" );
  ring[QStringLiteral( "outline_width" )] = QStringLiteral( "0.35" );
  ring[QStringLiteral( "size" )] = QStringLiteral( "4.2" );
  std::unique_ptr<QgsMarkerSymbol> sym = QgsMarkerSymbol::createSimple( ring );
  QVariantMap disk;
  disk[QStringLiteral( "name" )] = QStringLiteral( "circle" );
  disk[QStringLiteral( "color" )] = QStringLiteral( "#1B73D0" );
  disk[QStringLiteral( "outline_style" )] = QStringLiteral( "no" );
  disk[QStringLiteral( "size" )] = QStringLiteral( "3" );
  sym->appendSymbolLayer( QgsSimpleMarkerSymbolLayer::create( disk ) );
  vl->setRenderer( new QgsSingleSymbolRenderer( sym.release() ) );
  applyPointNameLabels( vl );
}

// 等值线层（D2.1/D5.7）：FactorContourService 产出的 GPKG → 线符号 +
// ELEV 标注（density：0=关 1=稀疏 2=全部）。
inline QgsVectorLayer *makeContourLayer( const QString &gpkgPath, QWidget *owner, int density )
{
  auto *vl = new QgsVectorLayer( gpkgPath + QStringLiteral( "|layername=contours" ),
                                 QObject::tr( "等值线" ), QStringLiteral( "ogr" ) );
  vl->setParent( owner );
  if ( !vl->isValid() )
    return vl;
  QVariantMap props;
  props[QStringLiteral( "line_color" )] = QStringLiteral( "#5D6E80" );
  props[QStringLiteral( "line_width" )] = QStringLiteral( "0.6" );
  vl->setRenderer( new QgsSingleSymbolRenderer( QgsLineSymbol::createSimple( props ).release() ) );
  if ( density > 0 )
  {
    QgsPalLayerSettings pal;
    pal.fieldName = QStringLiteral( "ELEV" );
    pal.isExpression = false;
    QgsTextFormat fmt;
    QFont font = PaleoTheme::bodyFont(PaleoTheme::tokens().labelPt);
    fmt.setFont( font );
    fmt.setSize(PaleoTheme::tokens().labelPt);
    fmt.setSizeUnit( Qgis::RenderUnit::Points );
    fmt.setColor( QColor( QStringLiteral( "#24303E" ) ) );
    QgsTextBufferSettings buf;
    buf.setEnabled( true );
    buf.setSize( 1.2 );
    buf.setColor( Qt::white );
    fmt.setBuffer( buf );
    pal.setFormat( fmt );
    if ( density == 1 )
      pal.dist = 60.0; // 稀疏：等值线间最小标注距离
    vl->setLabeling( new QgsVectorLayerSimpleLabeling( pal ) );
    vl->setLabelsEnabled( true );
  }
  else
    vl->setLabelsEnabled( false );
  return vl;
}

// 相字段分类渲染（geojson 预览与 D2.10 同目录叠加共用；D2.4 图例的
// category 数据也从渲染器读回）。
// 测井点只用一套圆点。相的差别放在文字上，不放在标记颜色上。
inline std::unique_ptr<QgsSymbol> unifiedWellPointSymbol()
{
  QVariantMap props;
  props[QStringLiteral( "name" )] = QStringLiteral( "circle" );
  props[QStringLiteral( "color" )] = QStringLiteral( "#24303E" );
  props[QStringLiteral( "outline_color" )] = QStringLiteral( "#FFFFFF" );
  props[QStringLiteral( "outline_width" )] = QStringLiteral( "0.4" );
  props[QStringLiteral( "size" )] = QStringLiteral( "3" );
  return QgsMarkerSymbol::createSimple( props );
}

inline QString firstExistingField( QgsVectorLayer *vlayer, std::initializer_list<QString> names )
{
  if ( !vlayer )
    return {};
  for ( const QString &name : names )
  {
    const int idx = vlayer->fields().lookupField( name );
    if ( idx >= 0 )
      return vlayer->fields().at( idx ).name();
  }
  return {};
}

// 相字段唯一值收集的有界口径（与同文件 8MB DOM 预算同族）：GUI 线程不再
// 无预算全要素遍历——扫过 kFaciesScanFeatures 个要素、或去重计数已达
// kFaciesMaxCategories 即停；未列全的相值落默认类别（图例标「未列全」，
// 不谎报完备）。阈值内保持原口径（全量去重）。
constexpr int kFaciesScanFeatures = 100000; // 扫描要素上限
constexpr int kFaciesMaxCategories = 64;    // 唯一相值上限（默认类别另算）

inline void applyFaciesRendererToLayer( QgsVectorLayer *vlayer, const QString &fieldName )
{
  if ( !vlayer || !vlayer->isValid() || fieldName.isEmpty() )
    return;
  const int fieldIdx = vlayer->fields().lookupField( fieldName );
  if ( fieldIdx < 0 )
    return;
  const bool point = vlayer->geometryType() == Qgis::GeometryType::Point;
  QSet<QString> uniqueVals;
  bool valuesTruncated = false;
  {
    QgsFeatureIterator it = vlayer->getFeatures();
    QgsFeature feat;
    for ( int scanned = 0; scanned < kFaciesScanFeatures; ++scanned )
    {
      if ( !it.nextFeature( feat ) )
        break;
      const QString v = feat.attribute( fieldIdx ).toString().trimmed();
      if ( v.isEmpty() || uniqueVals.contains( v ) )
        continue;
      if ( uniqueVals.size() >= kFaciesMaxCategories )
      {
        valuesTruncated = true; // 相的种类超出配色预算——其余归默认类别
        break;
      }
      uniqueVals.insert( v );
    }
  }
  QgsCategoryList categories;
  for ( const QString &val : uniqueVals )
  {
    std::unique_ptr<QgsSymbol> sym =
        point ? unifiedWellPointSymbol()
              : createFaciesSymbol( vlayer->geometryType(), faciesColor( val ) );
    categories.append( QgsRendererCategory( val, sym.release(), val ) );
  }
  std::unique_ptr<QgsSymbol> defSym =
      point ? unifiedWellPointSymbol()
            : createFaciesSymbol( vlayer->geometryType(), QColor( QStringLiteral( "#CFD8DC" ) ) );
  categories.append( QgsRendererCategory(
      QVariant(), defSym.release(),
      valuesTruncated ? QObject::tr( "其他（未列全）" ) : QObject::tr( "其他" ) ) );
  vlayer->setRenderer( new QgsCategorizedSymbolRenderer( fieldName, categories ) );

  const QString labelField = vlayer->fields().at( fieldIdx ).name();
  const QString nameField = firstExistingField(
      vlayer, { QStringLiteral( "name" ), QStringLiteral( "well_name" ),
                QStringLiteral( "井名" ) } );
  QgsPalLayerSettings palSettings;
  if ( point && !nameField.isEmpty() &&
       nameField.compare( labelField, Qt::CaseInsensitive ) != 0 )
  {
    const auto nullif = []( const QString &field ) {
      return QStringLiteral( "nullif(trim(to_string(%1)), '')" )
          .arg( QgsExpression::quotedColumnRef( field ) );
    };
    palSettings.isExpression = true;
    palSettings.fieldName = QStringLiteral(
        "with_variable('nm', %1, with_variable('fc', %2, "
        "CASE WHEN @nm IS NULL AND @fc IS NULL THEN '' "
        "WHEN @nm IS NULL THEN @fc "
        "WHEN @fc IS NULL THEN @nm "
        "ELSE @nm || '\\n' || @fc END))" )
                                .arg( nullif( nameField ), nullif( labelField ) );
    palSettings.placement = Qgis::LabelPlacement::OrderedPositionsAroundPoint;
  }
  else
  {
    palSettings.fieldName = labelField;
    palSettings.isExpression = false;
  }
  QgsTextFormat txtFmt;
  QFont font = PaleoTheme::bodyFont(PaleoTheme::tokens().bodyPt); font.setWeight(QFont::Medium);
  txtFmt.setFont( font );
  txtFmt.setSize(PaleoTheme::tokens().bodyPt);
  txtFmt.setSizeUnit( Qgis::RenderUnit::Points );
  txtFmt.setColor( QColor( QStringLiteral( "#24303E" ) ) );
  QgsTextBufferSettings buf;
  buf.setEnabled( true );
  buf.setSize( 1.5 );
  buf.setColor( Qt::white );
  txtFmt.setBuffer( buf );
  palSettings.setFormat( txtFmt );
  vlayer->setLabeling( new QgsVectorLayerSimpleLabeling( palSettings ) );
  vlayer->setLabelsEnabled( true );
  vlayer->triggerRepaint();
}

inline QString catalogProjectDir(const DataCatalog *cat)
{
  if (!cat)
    return {};
  const QString path = cat->catalogPath();
  const QString tail = QStringLiteral("/artifacts/metadata/catalog.json");
  if (!path.endsWith(tail))
    return {};
  return path.left(path.size() - tail.size());
}

inline bool sameFilePath(const QString &a, const QString &b)
{
  if (a.isEmpty() || b.isEmpty())
    return false;
  return QFileInfo(a).absoluteFilePath() == QFileInfo(b).absoluteFilePath();
}

// 当前文件体优先；兄弟文件只认本次任务带回且解析成功的文档。
inline const QList<LasCurve> *lasBodyFor(const QString &path, const QString &currentPath,
                                  const QList<LasCurve> &current,
                                  const QHash<QString, LasDoc> &siblings)
{
  if (sameFilePath(path, currentPath))
    return &current;
  const auto direct = siblings.constFind(path);
  if (direct != siblings.cend() && direct->ok && !direct->curves.isEmpty())
    return &direct->curves;
  const QString abs = QFileInfo(path).absoluteFilePath();
  const auto byAbs = siblings.constFind(abs);
  if (byAbs != siblings.cend() && byAbs->ok && !byAbs->curves.isEmpty())
    return &byAbs->curves;
  for (auto it = siblings.cbegin(); it != siblings.cend(); ++it)
    if (it->ok && !it->curves.isEmpty() && sameFilePath(it.key(), path))
      return &it->curves;
  return nullptr;
}

inline WellComposite::CurveData compositeCurve(const QString &name, const QString &unit,
                                       const QVector<double> &depths,
                                       const QVector<double> &values, int colorIndex)
{
  WellComposite::CurveData cd;
  cd.name = name;
  cd.unit = unit;
  cd.color = pickCurveColor(name, colorIndex);
  cd.depths.reserve(depths.size());
  for (double d : depths)
    cd.depths.append(static_cast<float>(d));
  cd.values.reserve(values.size());
  float valMin = 1e9f;
  float valMax = -1e9f;
  for (double v : values)
  {
    if (v <= -999.0 || v >= 99999.0)
    {
      cd.values.append(-9999.0f);
      continue;
    }
    const float fv = static_cast<float>(v);
    cd.values.append(fv);
    if (fv < valMin)
      valMin = fv;
    if (fv > valMax)
      valMax = fv;
  }
  if (valMin < valMax)
  {
    cd.minScale = valMin;
    cd.maxScale = valMax;
  }
  else
  {
    cd.minScale = 0.0f;
    cd.maxScale = 100.0f;
  }
  return cd;
}


inline bool isMapProductAsset(const CatalogAsset &asset, const CatalogVersion &v)
{
  if (v.extra.value(QStringLiteral("mapping_product")).toBool())
    return true;
  const QString &t = asset.type;
  if (t == QLatin1String("seismic_prediction") ||
      t == QLatin1String("wells_prediction") ||
      t == QLatin1String("composed_facies") ||
      t == QLatin1String("facies_polygons") ||
      t == QLatin1String("edited_facies") ||
      t == QLatin1String("single_factor_raster") ||
      t == QLatin1String("contour_lines") ||
      t == QLatin1String("single_factor_cartographic_work") ||
      t == QLatin1String("single_factor_cartographic_contour") ||
      t == QLatin1String("facies_fusion_raster"))
    return true;
  const QString kind = v.extra.value(QStringLiteral("kind")).toString();
  if (kind == QLatin1String("seismic_prediction") ||
      kind == QLatin1String("wells_prediction") ||
      kind == QLatin1String("composed_facies") ||
      kind == QLatin1String("facies_polygons") ||
      kind == QLatin1String("edited_facies") ||
      kind == QLatin1String("single_factor_raster") ||
      kind == QLatin1String("contour_lines"))
    return true;
  return false;
}

} // namespace paleo::datapreview_detail
