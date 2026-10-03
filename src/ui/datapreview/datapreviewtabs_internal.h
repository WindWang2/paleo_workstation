// 层：视图
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

// ---- 项目内依赖（照 datapreviewtabs.cpp 的 include 列表，不逐个编译错误补）----
#include "datapreviewtabs.h"
#include "../paleotheme.h"
#include "../../catalog/datacatalog.h"
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
#include "../../qgis/factorcontour.h"
#include "../../qgis/previewrasteranalysis.h"

// ---- QGIS 符号/图层（匿名命名空间用到 QgsMarkerSymbol/QgsLineSymbol 等）----
#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgsmarkersymbol.h>
#include <qgssymbol.h>
#include <qgsgeometry.h>
#include <qgsfield.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsmaplayer.h>


// ---- QGIS 头（类型→头用 grep 在本机 QGIS include 里反查，不靠命名规则猜）----
#include <qgis.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsexpression.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsmarkersymbollayer.h>
#include <qgspointxy.h>
#include <qgsproject.h>
#include <qgssinglesymbolrenderer.h>
#include <qgstextbuffersettings.h>
#include <qgstextformat.h>
#include <qgsvectorlayerlabeling.h>

namespace paleo::datapreview_detail {

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

// token 色 → QSS 大写 #RRGGBB（与 paleotheme 内部 qssHex 同口径，逐字节可比）。
inline QString qssHex(const QColor &c)
{
  return c.name().toUpper();
}

inline QLabel *caption8(const QString &text, QWidget *parent)
{
  auto *l = new QLabel(text, parent);
  QFont f = l->font();
  f.setPointSize(PaleoTheme::kLabelPt);
  l->setFont(f);
  PaleoTheme::applyThemedStyleSheet(l, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  return l;
}

inline QLabel *stateLabel(const QString &text, QWidget *parent)
{
  auto *l = new QLabel(text, parent);
  l->setAlignment(Qt::AlignCenter);
  l->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(l, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  l->setObjectName(QStringLiteral("stateText"));
  return l;
}

inline QLabel *warnLabel(const QString &text, QWidget *parent)
{
  auto *l = new QLabel(text, parent);
  PaleoTheme::applyThemedStyleSheet(l, [] {
    return QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().warning));
  });
  l->setWordWrap(true);
  return l;
}

// 「用系统程序打开」兜底行：按钮 + 就地错误文本（打开失败时浮现）。
// 预览页不再常驻——只在无内嵌预览或需开原件（office 转换件）时用。
inline QWidget *makeOpenExternalRow(const QString &absPath, QWidget *parent)
{
  auto *row = new QWidget(parent);
  auto *rl = new QHBoxLayout(row);
  rl->setContentsMargins(0, 0, 0, 0);
  rl->setSpacing(8);
  auto *btn = new QPushButton(QObject::tr("用系统程序打开"), row);
  btn->setObjectName(QStringLiteral("openExternalBtn"));
  auto *openErr = warnLabel(QString(), row);
  openErr->setObjectName(QStringLiteral("openErrorText"));
  openErr->setVisible(false);
  QObject::connect(btn, &QPushButton::clicked, row, [absPath, openErr]() {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(absPath)))
    {
      openErr->setText(QObject::tr("系统没有打开这个文件\n%1").arg(absPath));
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
    return QStringLiteral(
        "QWidget#previewToolBar { background: %1; border-bottom: 1px solid %2; }"
        "QWidget#previewToolBar QToolButton { background: %3; border: 1px solid %2;"
        " border-radius: 4px; padding: 4px 8px; font-size: 8pt; color: %4; }"
        "QWidget#previewToolBar QToolButton:hover { background: %5; border-color: %6; }"
        "QWidget#previewToolBar QToolButton:pressed { background: %2; }"
        "QWidget#previewToolBar QToolButton:checked { background: %5;"
        " border-color: %7; color: %8; font-weight: 500; }")
        .arg(qssHex(t.surfaceAlt), qssHex(t.border), qssHex(t.surface),
             qssHex(t.text), qssHex(t.surfaceAltRaised), qssHex(t.textDisabled),
             qssHex(t.primary), qssHex(t.primaryText));
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
  if (lower.contains(QStringLiteral("深湖")) || lower.contains(QStringLiteral("深盆")) || lower.contains(QStringLiteral("deep basin")))
    return QColor(QStringLiteral("#4DD0E1"));
  if (lower.contains(QStringLiteral("半深湖")) || lower.contains(QStringLiteral("semi-deep")))
    return QColor(QStringLiteral("#80DEEA"));
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
      QStringLiteral( "Point?crs=WKT:%1&field=name:string(64)&field=role:string(16)&index=yes" )
          .arg( wkt ),
      name, QStringLiteral( "memory" ) );
  vl->setParent( owner ); // 所有权归预览页宿主（画布只引用裸指针）
  return vl;
}

inline void addMemoryPoint( QgsVectorLayer *vl, double x, double y, const QString &name,
                     const QString &role )
{
  if ( !vl )
    return;
  QgsFeature f( vl->fields() );
  f.setAttribute( QStringLiteral( "name" ), name );
  f.setAttribute( QStringLiteral( "role" ), role );
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
  QFont font( QStringLiteral( "Noto Sans SC" ), 9, QFont::Medium );
  fmt.setFont( font );
  fmt.setSize( 9.0 );
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
inline void styleSurveyWellLayer( QgsVectorLayer *vl )
{
  if ( !vl )
    return;
  QVariantMap ring;
  ring[QStringLiteral( "name" )] = QStringLiteral( "circle" );
  ring[QStringLiteral( "color" )] = QStringLiteral( "255,255,255,0" );
  ring[QStringLiteral( "outline_color" )] = QStringLiteral( "#1B73D0" );
  ring[QStringLiteral( "outline_width" )] = QStringLiteral( "0.5" );
  ring[QStringLiteral( "size" )] = QStringLiteral( "6" );
  std::unique_ptr<QgsMarkerSymbol> sym = QgsMarkerSymbol::createSimple( ring );
  QVariantMap disk;
  disk[QStringLiteral( "name" )] = QStringLiteral( "circle" );
  disk[QStringLiteral( "color" )] = QStringLiteral( "#1B73D0" );
  disk[QStringLiteral( "outline_style" )] = QStringLiteral( "no" );
  disk[QStringLiteral( "size" )] = QStringLiteral( "4.4" );
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
    QFont font( QStringLiteral( "Noto Sans SC" ), 8 );
    fmt.setFont( font );
    fmt.setSize( 8.0 );
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

inline void applyFaciesRendererToLayer( QgsVectorLayer *vlayer, const QString &fieldName )
{
  if ( !vlayer || !vlayer->isValid() || fieldName.isEmpty() )
    return;
  const int fieldIdx = vlayer->fields().lookupField( fieldName );
  if ( fieldIdx < 0 )
    return;
  const bool point = vlayer->geometryType() == Qgis::GeometryType::Point;
  QSet<QString> uniqueVals;
  QgsFeatureIterator it = vlayer->getFeatures();
  QgsFeature feat;
  while ( it.nextFeature( feat ) )
  {
    const QString v = feat.attribute( fieldIdx ).toString().trimmed();
    if ( !v.isEmpty() )
      uniqueVals.insert( v );
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
  categories.append( QgsRendererCategory( QVariant(), defSym.release(), QObject::tr( "其他" ) ) );
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
  QFont font( QStringLiteral( "Noto Sans SC" ), 9, QFont::Medium );
  txtFmt.setFont( font );
  txtFmt.setSize( 9.0 );
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

struct CurveData
{
  QString name;
  QString unit;
  QColor color;
  QVector<QPointF> pts; // (x = value, y = depth)
  QPair<double, double> vRange{0, 1};
  bool visible = true;
  double hoverValue = std::numeric_limits<double>::quiet_NaN();
};

// 测井道曲线面板：支持多曲线叠合、深度缩放、拖拽平移、标尺与光标读数
class CurvePanel : public QWidget
{
public:
  explicit CurvePanel(QWidget *parent = nullptr) : QWidget(parent)
  {
    setMinimumSize(280, 320);
    setMouseTracking(true);
    m_vScroll = new QScrollBar(Qt::Vertical, this);
    m_vScroll->setVisible(false);
    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int val) {
      if (m_updatingScroll || m_zoomFactor <= 1.0)
        return;
      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = totalSpan / m_zoomFactor;
      const double maxScroll = m_dRange.second - visibleSpan;
      if (maxScroll <= m_dRange.first)
        return;
      const int maxVal = m_vScroll->maximum();
      const double frac = maxVal > 0 ? static_cast<double>(val) / maxVal : 0.0;
      m_scrollDepth = m_dRange.first + frac * (maxScroll - m_dRange.first);
      update();
    });
  }

  void setEmptyText(const QString &text)
  {
    m_emptyText = text;
    update();
  }

  void clearCurves()
  {
    m_curves.clear();
    m_dRange = {0, 1};
    m_scrollDepth = 0.0;
    m_zoomFactor = 1.0;
    updateScrollBar();
    update();
  }

  void addCurve(const QString &name, const QString &unit,
                const QVector<double> &values, const QVector<double> &depths,
                const QColor &color, bool visible = true)
  {
    CurveData cd;
    cd.name = name;
    cd.unit = unit;
    cd.color = color;
    cd.visible = visible;

    double vMin = std::numeric_limits<double>::max(), vMax = std::numeric_limits<double>::lowest();
    double dMin = m_curves.isEmpty() ? std::numeric_limits<double>::max() : m_dRange.first;
    double dMax = m_curves.isEmpty() ? std::numeric_limits<double>::lowest() : m_dRange.second;

    const int n = qMin(values.size(), depths.size());
    for (int i = 0; i < n; ++i)
    {
      const double v = values.at(i);
      const double d = depths.at(i);
      if (std::isnan(v) || std::isnan(d) || v <= -9999.0)
        continue;
      cd.pts.append(QPointF(v, d));
      vMin = qMin(vMin, v);
      vMax = qMax(vMax, v);
      dMin = qMin(dMin, d);
      dMax = qMax(dMax, d);
    }

    if (!cd.pts.isEmpty())
    {
      cd.vRange = {vMin == vMax ? vMin - 1.0 : vMin, vMax == vMin ? vMax + 1.0 : vMax};
      m_dRange = {dMin, dMax};
      m_scrollDepth = m_dRange.first;
    }
    m_curves.append(cd);
    updateScrollBar();
    update();
  }

  // 单道兼容接口（time_depth 或旧代码调用）
  void setCurve(const QString &name, const QString &unit,
                const QVector<double> &values, const QVector<double> &depths)
  {
    clearCurves();
    addCurve(name, unit, values, depths, QColor(QStringLiteral("#1B73D0")), true);
  }

  void setCurveVisible(const QString &name, bool visible)
  {
    for (CurveData &c : m_curves)
    {
      if (c.name.compare(name, Qt::CaseInsensitive) == 0)
      {
        c.visible = visible;
        break;
      }
    }
    update();
  }

  bool isCurveVisible(const QString &name) const
  {
    for (const CurveData &c : m_curves)
      if (c.name.compare(name, Qt::CaseInsensitive) == 0)
        return c.visible;
    return false;
  }

  int pointCount() const
  {
    int count = 0;
    for (const CurveData &c : m_curves)
      count += c.pts.size();
    return count;
  }

  double zoomFactor() const { return m_zoomFactor; }

  void setZoom(double z, double anchorDepth = -1.0)
  {
    const double clampedZ = qBound(1.0, z, 50.0);
    if (qFuzzyCompare(clampedZ, m_zoomFactor) && anchorDepth < 0)
      return;

    const double totalSpan = m_dRange.second - m_dRange.first;
    if (totalSpan <= 0)
      return;

    const double oldSpan = totalSpan / m_zoomFactor;
    const double newSpan = totalSpan / clampedZ;

    if (anchorDepth < 0)
      anchorDepth = m_scrollDepth + oldSpan * 0.5;

    const double anchorFrac = oldSpan > 0 ? (anchorDepth - m_scrollDepth) / oldSpan : 0.5;
    m_scrollDepth = anchorDepth - anchorFrac * newSpan;
    m_zoomFactor = clampedZ;

    const double maxScroll = m_dRange.second - newSpan;
    m_scrollDepth = qBound(m_dRange.first, m_scrollDepth, qMax(m_dRange.first, maxScroll));

    updateScrollBar();
    update();
    if (onZoomChanged)
      onZoomChanged(m_zoomFactor);
  }

  void zoomIn() { setZoom(m_zoomFactor * 1.5); }
  void zoomOut() { setZoom(m_zoomFactor / 1.5); }
  void resetZoom() { setZoom(1.0); }

  std::function<void(double)> onZoomChanged;
  std::function<void(double depth, const QString &info)> onHoverChanged;

  double depthAtY(int y) const
  {
    const QRect pRect = plotRect();
    if (pRect.height() <= 0)
      return m_dRange.first;
    const double visibleSpan = (m_dRange.second - m_dRange.first) / m_zoomFactor;
    const double frac = static_cast<double>(y - pRect.top()) / pRect.height();
    return m_scrollDepth + frac * visibleSpan;
  }

  int yAtDepth(double d) const
  {
    const QRect pRect = plotRect();
    if (pRect.height() <= 0)
      return 0;
    const double visibleSpan = (m_dRange.second - m_dRange.first) / m_zoomFactor;
    if (visibleSpan <= 0)
      return pRect.top();
    const double frac = (d - m_scrollDepth) / visibleSpan;
    return pRect.top() + qRound(frac * pRect.height());
  }

  int calculateHeaderHeight() const
  {
    int visibleCount = 0;
    for (const CurveData &c : m_curves)
      if (c.visible) visibleCount++;
    if (visibleCount <= 2)
      return 28;
    if (visibleCount <= 4)
      return 46;
    return 64;
  }

  QRect plotRect() const
  {
    const int kRulerW = 50;
    const int headerH = calculateHeaderHeight();
    const int scrollW = (m_zoomFactor > 1.0) ? 14 : 0;
    return QRect(kRulerW, headerH, qMax(20, width() - kRulerW - scrollW - 6),
                 qMax(20, height() - headerH - 8));
  }

protected:
  void updateScrollBar()
  {
    if (!m_vScroll)
      return;
    const QRect pRect = plotRect();
    m_vScroll->setGeometry(width() - 14, pRect.top(), 14, pRect.height());

    if (m_zoomFactor <= 1.0)
    {
      m_vScroll->setVisible(false);
      return;
    }
    m_vScroll->setVisible(true);

    const double totalSpan = m_dRange.second - m_dRange.first;
    const double visibleSpan = totalSpan / m_zoomFactor;
    const double maxScroll = m_dRange.second - visibleSpan;

    m_updatingScroll = true;
    const int kRange = 10000;
    const int pageStep = qMax(1, qRound(kRange / m_zoomFactor));
    m_vScroll->setRange(0, kRange - pageStep);
    m_vScroll->setPageStep(pageStep);
    const double frac = (maxScroll > m_dRange.first)
                            ? (m_scrollDepth - m_dRange.first) / (maxScroll - m_dRange.first)
                            : 0.0;
    m_vScroll->setValue(qRound(frac * (kRange - pageStep)));
    m_updatingScroll = false;
  }

  void updateHoverValues()
  {
    for (CurveData &c : m_curves)
    {
      if (!c.visible || c.pts.isEmpty() || std::isnan(m_hoverDepth))
      {
        c.hoverValue = std::numeric_limits<double>::quiet_NaN();
        continue;
      }

      auto it = std::lower_bound(c.pts.begin(), c.pts.end(), m_hoverDepth,
                                 [](const QPointF &pt, double d) { return pt.y() < d; });
      if (it == c.pts.end())
      {
        c.hoverValue = c.pts.last().x();
      }
      else if (it == c.pts.begin())
      {
        c.hoverValue = c.pts.first().x();
      }
      else
      {
        const QPointF &p0 = *(it - 1);
        const QPointF &p1 = *it;
        if (qAbs(p1.y() - p0.y()) > 1e-4)
        {
          const double t = (m_hoverDepth - p0.y()) / (p1.y() - p0.y());
          c.hoverValue = p0.x() + t * (p1.x() - p0.x());
        }
        else
        {
          c.hoverValue = p1.x();
        }
      }
    }
    if (onHoverChanged)
    {
      if (std::isnan(m_hoverDepth))
      {
        onHoverChanged(-1.0, QString());
      }
      else
      {
        QString info = QString::asprintf("MD: %.1f m", m_hoverDepth);
        for (const CurveData &c : m_curves)
        {
          if (c.visible && !std::isnan(c.hoverValue))
          {
            info += QStringLiteral(" | ") + c.name + QString::asprintf(": %.2f", c.hoverValue);
            if (!c.unit.isEmpty())
              info += QStringLiteral(" ") + c.unit;
          }
        }
        onHoverChanged(m_hoverDepth, info);
      }
    }
  }

  void mousePressEvent(QMouseEvent *e) override
  {
    const QRect pRect = plotRect();
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
    {
      if (m_zoomFactor > 1.0 && pRect.contains(e->pos()))
      {
        m_dragging = true;
        m_dragStartY = e->pos().y();
        m_dragStartScrollDepth = m_scrollDepth;
        setCursor(Qt::ClosedHandCursor);
      }
    }
  }

  void mouseMoveEvent(QMouseEvent *e) override
  {
    const QRect pRect = plotRect();
    if (pRect.contains(e->pos()))
    {
      m_hoverDepth = depthAtY(e->pos().y());
      updateHoverValues();
    }
    else
    {
      m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
    }

    if (m_dragging)
    {
      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = totalSpan / m_zoomFactor;
      const double dy = e->pos().y() - m_dragStartY;
      const double dDepth = (dy / static_cast<double>(pRect.height())) * visibleSpan;
      const double maxScroll = m_dRange.second - visibleSpan;
      m_scrollDepth = qBound(m_dRange.first, m_dragStartScrollDepth - dDepth, qMax(m_dRange.first, maxScroll));
      updateScrollBar();
      update();
    }
    else
    {
      if (m_zoomFactor > 1.0 && pRect.contains(e->pos()))
        setCursor(Qt::OpenHandCursor);
      else
        unsetCursor();
      update();
    }
  }

  void mouseReleaseEvent(QMouseEvent *) override
  {
    if (m_dragging)
    {
      m_dragging = false;
      if (m_zoomFactor > 1.0 && plotRect().contains(mapFromGlobal(QCursor::pos())))
        setCursor(Qt::OpenHandCursor);
      else
        unsetCursor();
    }
  }

  void mouseDoubleClickEvent(QMouseEvent *e) override
  {
    if (plotRect().contains(e->pos()))
    {
      resetZoom();
    }
  }

  void wheelEvent(QWheelEvent *e) override
  {
    const QRect pRect = plotRect();
    if (!pRect.contains(e->position().toPoint()))
    {
      e->ignore();
      return;
    }

    if (e->modifiers() & Qt::ControlModifier)
    {
      const double f = e->angleDelta().y() > 0 ? 1.25 : 1.0 / 1.25;
      setZoom(m_zoomFactor * f, depthAtY(e->position().y()));
      e->accept();
      return;
    }

    if (m_zoomFactor > 1.0)
    {
      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = totalSpan / m_zoomFactor;
      const double step = visibleSpan * 0.12 * (e->angleDelta().y() > 0 ? -1.0 : 1.0);
      const double maxScroll = m_dRange.second - visibleSpan;
      m_scrollDepth = qBound(m_dRange.first, m_scrollDepth + step, qMax(m_dRange.first, maxScroll));
      updateScrollBar();
      update();
      e->accept();
      return;
    }

    e->ignore();
  }

  void leaveEvent(QEvent *) override
  {
    m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
    for (CurveData &c : m_curves)
      c.hoverValue = std::numeric_limits<double>::quiet_NaN();
    unsetCursor();
    update();
    if (onHoverChanged)
      onHoverChanged(-1.0, QString());
  }

  void resizeEvent(QResizeEvent *) override
  {
    updateScrollBar();
  }

  void drawHeader(QPainter &p, const QRect &pRect)
  {
    const int headerTop = 4;
    int curX = pRect.left() + 4;
    int curY = headerTop;
    const int rowHeight = 18;

    QFont fName = font();
    fName.setPointSize(8);
    fName.setBold(true);

    QFont fMono = PaleoTheme::monoFont();
    fMono.setPointSize(8);

    for (const CurveData &c : m_curves)
    {
      if (!c.visible)
        continue;

      // Sample line
      p.setPen(QPen(c.color, 2.5, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(curX, curY + rowHeight / 2, curX + 12, curY + rowHeight / 2);
      curX += 16;

      // Curve Name
      p.setFont(fName);
      p.setPen(c.color);
      const QString nameStr = c.name;
      p.drawText(curX, curY + rowHeight - 4, nameStr);
      curX += fontMetrics().horizontalAdvance(nameStr) + 4;

      // Scale range & unit: e.g. "0–150 API"
      p.setFont(fMono);
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      QString scaleStr;
      if (!std::isnan(c.hoverValue))
      {
        scaleStr = QString::asprintf(": %.1f", c.hoverValue);
        if (!c.unit.isEmpty())
          scaleStr += QStringLiteral(" ") + c.unit;
        scaleStr += QString::asprintf(" (%.0f–%.0f)", c.vRange.first, c.vRange.second);
      }
      else
      {
        scaleStr = QString::asprintf("[%.0f–%.0f", c.vRange.first, c.vRange.second);
        if (!c.unit.isEmpty())
          scaleStr += QStringLiteral(" ") + c.unit;
        scaleStr += QStringLiteral("]");
      }

      p.drawText(curX, curY + rowHeight - 4, scaleStr);
      curX += QFontMetrics(fMono).horizontalAdvance(scaleStr) + 12;

      if (curX > pRect.right() - 80)
      {
        curX = pRect.left() + 4;
        curY += rowHeight;
      }
    }
  }

  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.fillRect(rect(), Qt::white);

    const QRect pRect = plotRect();
    const int kRulerW = pRect.left();

    // Draw ruler background
    const QRect rulerRect(0, pRect.top(), kRulerW, pRect.height());
    p.fillRect(rulerRect, QColor(QStringLiteral("#F8FAFC")));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawLine(kRulerW, pRect.top(), kRulerW, pRect.bottom());

    // Ruler title "MD (m)"
    QFont fCaption = font();
    fCaption.setPointSize(8);
    p.setFont(fCaption);
    p.setPen(QColor(QStringLiteral("#5D6E80")));
    p.drawText(QRect(2, 4, kRulerW - 4, pRect.top() - 4), Qt::AlignCenter | Qt::AlignVCenter,
               QStringLiteral("MD (m)"));

    p.drawRect(pRect);

    int totalPoints = 0;
    int visibleCurves = 0;
    for (const CurveData &c : m_curves)
    {
      totalPoints += c.pts.size();
      if (c.visible)
        visibleCurves++;
    }

    if (totalPoints == 0)
    {
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      p.drawText(pRect, Qt::AlignCenter, m_emptyText);
      return;
    }

    if (visibleCurves == 0)
    {
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      p.drawText(pRect, Qt::AlignCenter, tr("未勾选任何曲线 — 在上方选择要显示的曲线"));
      return;
    }

    const double totalSpan = m_dRange.second - m_dRange.first;
    const double visibleSpan = (totalSpan > 0 && m_zoomFactor >= 1.0)
                                   ? totalSpan / m_zoomFactor
                                   : 1.0;
    const double dTop = m_scrollDepth;
    const double dBottom = m_scrollDepth + visibleSpan;

    const int targetTicks = qBound(4, pRect.height() / 45, 12);
    const double rawInterval = visibleSpan / targetTicks;
    double niceInterval = 100.0;
    const double intervals[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
    for (double iv : intervals)
    {
      niceInterval = iv;
      if (iv >= rawInterval)
        break;
    }

    const double firstTick = std::ceil(dTop / niceInterval) * niceInterval;
    QFont fMono = PaleoTheme::monoFont();
    fMono.setPointSize(8);

    p.setFont(fMono);
    for (double d = firstTick; d <= dBottom; d += niceInterval)
    {
      const int y = yAtDepth(d);
      if (y < pRect.top() || y > pRect.bottom())
        continue;

      p.setPen(QColor(QStringLiteral("#9AA7B4")));
      p.drawLine(kRulerW - 5, y, kRulerW, y);

      p.setPen(QPen(QColor(QStringLiteral("#F0F4F8")), 1, Qt::DashLine));
      p.drawLine(pRect.left(), y, pRect.right(), y);

      p.setPen(QColor(QStringLiteral("#5D6E80")));
      const QString dText = QString::number(d, 'f', (niceInterval < 1.0 ? 1 : 0));
      p.drawText(QRect(2, y - 8, kRulerW - 9, 16), Qt::AlignRight | Qt::AlignVCenter, dText);
    }

    p.setPen(QPen(QColor(QStringLiteral("#F0F4F8")), 1, Qt::DotLine));
    for (int i = 1; i <= 3; ++i)
    {
      const int vx = pRect.left() + (pRect.width() * i) / 4;
      p.drawLine(vx, pRect.top(), vx, pRect.bottom());
    }

    p.setClipRect(pRect);
    for (const CurveData &c : m_curves)
    {
      if (!c.visible || c.pts.isEmpty())
        continue;

      const double vSpan = c.vRange.second - c.vRange.first;
      if (vSpan <= 0)
        continue;

      const auto mapX = [&](double v) {
        const double f = (v - c.vRange.first) / vSpan;
        return pRect.left() + f * pRect.width();
      };

      p.setPen(QPen(c.color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

      bool first = true;
      QPointF prev;
      for (const QPointF &pt : c.pts)
      {
        const double v = pt.x();
        const double d = pt.y();

        if (d < dTop - niceInterval || d > dBottom + niceInterval)
        {
          first = true;
          continue;
        }

        const QPointF mapped(mapX(v), yAtDepth(d));
        if (!first)
          p.drawLine(prev, mapped);
        prev = mapped;
        first = false;
      }
    }
    p.setClipping(false);

    drawHeader(p, pRect);

    if (!std::isnan(m_hoverDepth) && pRect.contains(mapFromGlobal(QCursor::pos())))
    {
      const int hy = yAtDepth(m_hoverDepth);
      if (hy >= pRect.top() && hy <= pRect.bottom())
      {
        p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1, Qt::DashLine));
        p.drawLine(pRect.left(), hy, pRect.right(), hy);

        const QString hText = QString::number(m_hoverDepth, 'f', 1);
        p.setFont(fMono);
        const QRect badgeRect(2, hy - 8, kRulerW - 4, 16);
        p.fillRect(badgeRect, QColor(QStringLiteral("#24303E")));
        p.setPen(Qt::white);
        p.drawText(badgeRect, Qt::AlignCenter, hText);
      }
    }
  }

private:
  QVector<CurveData> m_curves;
  QPair<double, double> m_dRange{0, 1};
  double m_zoomFactor = 1.0;
  double m_scrollDepth = 0.0;
  QScrollBar *m_vScroll = nullptr;
  bool m_updatingScroll = false;
  double m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
  bool m_dragging = false;
  int m_dragStartY = 0;
  double m_dragStartScrollDepth = 0.0;
  QString m_emptyText = QObject::tr("无有效采样");
};

// 地震剖面：一条 inline/crossline 的红白蓝双极振幅渲染（§4/§7：只解码这一条）。
// 时间轴与色标：左侧显示 TWT(ms) 时间刻度轴，右侧显示红白蓝振幅色标与极性标注。
// D61 标定：井的 D61 分层经时深表换算成 ms 后，在剖面上画一条水平标记线。
class SectionPanel : public QWidget
{
public:
  SectionPanel(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(320, 260); }
  void setTraces(const QVector<SegyTrace> &traces, float dtUs, double t0Ms)
  {
    if (traces.isEmpty())
    {
      clearImage();
      return;
    }
    const int w = qMax(1, traces.size());
    const SegySectionGrid grid = SegySectionGrid::forTraces(traces, dtUs, t0Ms);
    const int h = grid.rows;
    m_img = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    m_img.fill(qRgb(255, 255, 255));
    float amp = 1e-6f;
    for (const SegyTrace &t : traces)
      for (float s : t.samples)
        amp = qMax(amp, qAbs(s));
    for (int x = 0; x < w; ++x)
    {
      const SegyTrace &t = traces.at(x);
      for (int y = 0; y < h; ++y)
      {
        float sample = 0.0f;
        if (!grid.sampleAt(t, y, dtUs, t0Ms, &sample)) continue;
        const float v = std::clamp((sample / amp) * 1.35f, -1.0f, 1.0f);
        const float mag = std::pow(std::abs(v), 0.85f);
        const float k = 1.0f - mag;
        QRgb color;
        if (v < 0.0f) {
            // Deep blue to white (Trough)
            color = qRgb(static_cast<int>(217 * k), static_cast<int>(230 * k), 255);
        } else {
            // White to deep red (Peak)
            color = qRgb(255, static_cast<int>(224 * k), static_cast<int>(214 * k));
        }
        m_img.setPixel(x, y, color);
      }
    }
    m_maxAmp = amp;
    m_t0Ms = grid.startMs;
    m_dtMs = grid.stepMs;
    m_caption = QObject::tr("%1 道 · %2 样点 · %3 ms 采样 · t0 = %4 ms")
                    .arg(traces.size())
                    .arg(grid.rows)
                    .arg(grid.stepMs, 0, 'f', 1)
                    .arg(grid.startMs, 0, 'f', 1);
    update();
  }
  bool hasImage() const { return !m_img.isNull(); }
  void clearImage()
  {
    m_img = QImage();
    m_caption.clear();
    m_error.clear();
    clearTieMarker();
    update();
  }
  void setError(const QString &text)
  {
    m_img = QImage();
    m_error = text;
    clearTieMarker();
    update();
  }
  void setTieMarker(const QString &label, double ms)
  {
    m_tieLabel = label;
    m_tieMs = ms;
    update();
  }
  void clearTieMarker()
  {
    m_tieMs = qQNaN();
    m_tieLabel.clear();
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Qt::white);
    if (m_img.isNull())
    {
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      p.drawText(rect(), Qt::AlignCenter,
                 m_error.isEmpty() ? QObject::tr("尚未解码剖面") : m_error);
      return;
    }

    const int leftMargin = 58;
    const int rightMargin = 54;
    const int topMargin = 26;
    const int bottomMargin = 16;
    const QRect dst(leftMargin, topMargin,
                    std::max(10, width() - leftMargin - rightMargin),
                    std::max(10, height() - topMargin - bottomMargin));

    // 1. 左侧时间刻度轴 (TWT ms 时间剖面)
    const QRect leftRuler(0, topMargin, leftMargin, dst.height());
    p.fillRect(leftRuler, QColor(QStringLiteral("#F5F7FA")));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawLine(leftMargin, topMargin, leftMargin, dst.bottom());

    QFont monoFont(QStringLiteral("JetBrains Mono"), 7);
    QFont bodyFont(QStringLiteral("Noto Sans SC"), 7);
    p.setFont(bodyFont);
    p.setPen(QColor(QStringLiteral("#5D6E80")));
    p.drawText(QRect(2, 4, leftMargin - 4, 18), Qt::AlignCenter, QStringLiteral("TWT (ms)"));

    if (m_dtMs > 0.0 && m_img.height() > 0)
    {
      const double endTimeMs = m_t0Ms + m_img.height() * m_dtMs;
      const auto ticks = seismic::NiceStep::GenerateTicks(m_t0Ms, endTimeMs, topMargin, dst.bottom(), 6, QStringLiteral("%.0f"));
      p.setFont(monoFont);
      for (const auto &tk : ticks)
      {
        if (tk.pixelPos < topMargin || tk.pixelPos > dst.bottom()) continue;
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.drawLine(QPointF(leftMargin - 6.0, tk.pixelPos), QPointF(leftMargin, tk.pixelPos));
        p.setPen(QColor(QStringLiteral("#24303E")));
        p.drawText(QRectF(2, tk.pixelPos - 7.0, leftMargin - 10, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(qRound(tk.value)));
      }
    }

    // 2. 剖面核心地震图像
    p.drawImage(dst, m_img.scaled(dst.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));

    // 3. D61 标定线（时间 ms → 剖面内水平线 + 井名标注）
    if (std::isfinite(m_tieMs) && m_dtMs > 0.0 && m_img.height() > 1)
    {
      const double row = (m_tieMs - m_t0Ms) / m_dtMs;
      const double yFrac = (row + 0.5) / m_img.height();
      if (yFrac >= 0.0 && yFrac <= 1.0)
      {
        const int y = dst.top() + qRound(yFrac * dst.height());
        p.setPen(QPen(QColor(QStringLiteral("#24303E")), 1.5));
        p.drawLine(dst.left(), y, dst.right(), y);
        p.setFont(bodyFont);
        p.drawText(QRect(dst.left() + 4, y - 16, dst.width() - 8, 14), Qt::AlignLeft,
                   m_tieLabel);
      }
    }

    // 4. 右侧振幅色标 (Color Bar)
    const QRect rightBarRect(dst.right(), 0, rightMargin, height());
    p.fillRect(rightBarRect, QColor(QStringLiteral("#F5F7FA")));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawLine(dst.right(), 0, dst.right(), height());

    p.setFont(QFont(QStringLiteral("Noto Sans SC"), 7, QFont::Bold));
    p.setPen(QColor(QStringLiteral("#24303E")));
    p.drawText(QRect(dst.right(), 4, rightMargin, 16), Qt::AlignCenter, tr("色标"));

    const int barW = 10;
    const int barX = dst.right() + 6;
    const int barTop = topMargin + 8;
    const int barH = std::max(20, dst.height() - 24);

    QLinearGradient grad(barX, barTop, barX, barTop + barH);
    grad.setColorAt(0.0, QColor(220, 38, 38));   // Red Peak
    grad.setColorAt(0.5, QColor(255, 255, 255)); // White Zero
    grad.setColorAt(1.0, QColor(25, 118, 210));  // Blue Trough

    p.setBrush(grad);
    p.setPen(QPen(QColor(QStringLiteral("#DFE5EC")), 1.0));
    p.drawRoundedRect(QRectF(barX, barTop, barW, barH), 2.0, 2.0);

    // 刻度值
    p.setFont(monoFont);
    p.setPen(QColor(QStringLiteral("#24303E")));
    const QString maxStr = m_maxAmp >= 1000.0f
        ? QStringLiteral("+%1k").arg(m_maxAmp / 1000.0f, 0, 'f', 0)
        : QStringLiteral("+%1").arg(qRound(m_maxAmp));
    const QString minStr = m_maxAmp >= 1000.0f
        ? QStringLiteral("-%1k").arg(m_maxAmp / 1000.0f, 0, 'f', 0)
        : QStringLiteral("-%1").arg(qRound(m_maxAmp));

    p.drawLine(QPointF(barX + barW, barTop), QPointF(barX + barW + 3, barTop));
    p.drawText(QRectF(barX + barW + 4, barTop - 6, rightMargin - barW - 10, 12), Qt::AlignLeft | Qt::AlignVCenter, maxStr);

    const double midY = barTop + barH * 0.5;
    p.drawLine(QPointF(barX + barW, midY), QPointF(barX + barW + 3, midY));
    p.drawText(QRectF(barX + barW + 4, midY - 6, rightMargin - barW - 10, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));

    p.drawLine(QPointF(barX + barW, barTop + barH), QPointF(barX + barW + 3, barTop + barH));
    p.drawText(QRectF(barX + barW + 4, barTop + barH - 6, rightMargin - barW - 10, 12), Qt::AlignLeft | Qt::AlignVCenter, minStr);

    p.setFont(QFont(QStringLiteral("Noto Sans SC"), 7));
    p.setPen(QColor(220, 38, 38));
    p.drawText(QRectF(dst.right(), barTop - 12, rightMargin - 4, 10), Qt::AlignRight, tr("波峰+"));
    p.setPen(QColor(25, 118, 210));
    p.drawText(QRectF(dst.right(), barTop + barH + 2, rightMargin - 4, 10), Qt::AlignRight, tr("波谷-"));

    // 5. 顶部说明条
    p.setPen(QColor(QStringLiteral("#5D6E80")));
    p.setFont(bodyFont);
    p.drawText(QRect(leftMargin + 4, 4, dst.width() - 8, 18), Qt::AlignLeft | Qt::AlignVCenter, m_caption);
  }

private:
  QImage m_img;
  QString m_caption;
  QString m_error;
  float m_maxAmp = 1.0f;
  double m_t0Ms = 0.0, m_dtMs = 0.0;
  double m_tieMs = qQNaN();
  QString m_tieLabel;
};

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


} // namespace paleo::datapreview_detail
