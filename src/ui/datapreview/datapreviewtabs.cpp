// 层：视图
#include "datapreviewtabs.h"
#include "../paleoviewport.h"

#include "../paleotheme.h" // DESIGN.md token 出口（颜色/字阶/活体样式共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标

#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/previewdoc.h"    // 唯一数据门面——解析/解码/SHA/PDF 编排全经它（W1）
#include "../../services/welllogset.h"    // 井曲线并集（综合柱状图；只读 ~C 头）
#include "../../services/paleotaskservice.h" // PaleoTask 进度/取消（地震转码区）
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
#include <qgsmapcanvas.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrasterbandstats.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgscolorrampshader.h>
#include <qgscolorrampimpl.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgsrubberband.h>
#include <qgsexpression.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>
#include <qgsfields.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsfillsymbol.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgslinesymbol.h>
#include <qgspallabeling.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextbuffersettings.h>
#include <QButtonGroup>
#include <QTimer>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QHeaderView>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QMouseEvent>
#include <QPdfDocument>
#include <QPdfView>
#include <QPixmap>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// §4 状态文案与九类资产面板。DESIGN.md dock 面板 tokens：
// surface #FFFFFF、border #DFE5EC、text #24303E、text-muted #5D6E80、8pt captions；
// 数值列 JetBrains Mono 9pt 右对齐；语义色 #F29900(警告)/#E53935(失败)。
// ---------------------------------------------------------------------------
namespace
{
  // token 色 → QSS 大写 #RRGGBB（与 paleotheme 内部 qssHex 同口径，逐字节可比）。
  QString qssHex(const QColor &c)
  {
    return c.name().toUpper();
  }

  QLabel *caption8(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSize(PaleoTheme::kLabelPt);
    l->setFont(f);
    PaleoTheme::applyThemedStyleSheet(l, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    return l;
  }

  QLabel *stateLabel(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    l->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(l, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    l->setObjectName(QStringLiteral("stateText"));
    return l;
  }

  QLabel *warnLabel(const QString &text, QWidget *parent)
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
  QWidget *makeOpenExternalRow(const QString &absPath, QWidget *parent)
  {
    auto *row = new QWidget(parent);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(6);
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
  QFont monoFont()
  {
    return PaleoTheme::monoFont(); // wave3/ux-consistency：共用注册/vendor 路径
  }

  QLabel *valueLabel(const QString &text, QWidget *parent, bool mono = false)
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
  void stylePreviewToolBar(QWidget *bar)
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

  void setNumericItem(QTableWidgetItem *it)
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
  QgsVectorLayer *makeMemoryPointLayer( const QString &name, QWidget *owner )
  {
    const QString wkt = DataCatalog::localGridCrsWkt();
    auto *vl = new QgsVectorLayer(
        QStringLiteral( "Point?crs=WKT:%1&field=name:string(64)&field=role:string(16)&index=yes" )
            .arg( wkt ),
        name, QStringLiteral( "memory" ) );
    vl->setParent( owner ); // 所有权归预览页宿主（画布只引用裸指针）
    return vl;
  }

  void addMemoryPoint( QgsVectorLayer *vl, double x, double y, const QString &name,
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
  void applyPointNameLabels( QgsVectorLayer *vl )
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
  void stylePointLayer( QgsVectorLayer *vl, bool withLabels )
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
  void styleSurveyWellLayer( QgsVectorLayer *vl )
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
  QgsVectorLayer *makeContourLayer( const QString &gpkgPath, QWidget *owner, int density )
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
  std::unique_ptr<QgsSymbol> unifiedWellPointSymbol()
  {
    QVariantMap props;
    props[QStringLiteral( "name" )] = QStringLiteral( "circle" );
    props[QStringLiteral( "color" )] = QStringLiteral( "#24303E" );
    props[QStringLiteral( "outline_color" )] = QStringLiteral( "#FFFFFF" );
    props[QStringLiteral( "outline_width" )] = QStringLiteral( "0.4" );
    props[QStringLiteral( "size" )] = QStringLiteral( "3" );
    return QgsMarkerSymbol::createSimple( props );
  }

  QString firstExistingField( QgsVectorLayer *vlayer, std::initializer_list<QString> names )
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

  void applyFaciesRendererToLayer( QgsVectorLayer *vlayer, const QString &fieldName )
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

  QString catalogProjectDir(const DataCatalog *cat)
  {
    if (!cat)
      return {};
    const QString path = cat->catalogPath();
    const QString tail = QStringLiteral("/artifacts/metadata/catalog.json");
    if (!path.endsWith(tail))
      return {};
    return path.left(path.size() - tail.size());
  }

  bool sameFilePath(const QString &a, const QString &b)
  {
    if (a.isEmpty() || b.isEmpty())
      return false;
    return QFileInfo(a).absoluteFilePath() == QFileInfo(b).absoluteFilePath();
  }

  // 当前文件体优先；兄弟文件只认本次任务带回且解析成功的文档。
  const QList<LasCurve> *lasBodyFor(const QString &path, const QString &currentPath,
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

  WellComposite::CurveData compositeCurve(const QString &name, const QString &unit,
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

} // namespace

// T27 中文化：coordinate_status 枚举 → §4 计划文案。untransformed 用与
// 状态栏/PDF 页脚同一句「工程坐标 · 米 · 未投影」；invalid/missing 用
// 「坐标无效」「没有坐标」，仍 text-muted（#5D6E80）。
QString DataPreviewTabs::coordinateStatusText(const QString &status)
{
  if (status == QLatin1String("ok"))
    return tr("坐标有效");
  if (status == QLatin1String("untransformed"))
    return tr("工程坐标 · 米 · 未投影");
  if (status == QLatin1String("invalid"))
    return tr("坐标无效");
  return tr("没有坐标"); // missing / 空 / 未知
}

void DataPreviewTabs::setHorizonOnMap(const QString &layerId, bool on)
{
  // T29 双向同步：所有绑到该 layerId 的「在地图上显示」按钮跟随图层可见性。
  auto buttons = findChildren<QPushButton *>(QStringLiteral("showOnMapBtn"));
  if (m_detailsHost)
    buttons.append(m_detailsHost->findChildren<QPushButton *>(QStringLiteral("showOnMapBtn")));
  for (QPushButton *btn : buttons)
    if (btn->property("layerId").toString() == layerId)
    {
      btn->setProperty("onMap", on);
      btn->setText(on ? tr("已在地图上") : tr("在地图上显示"));
    }
}

DataPreviewTabs::DataPreviewTabs(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(4);

  m_tabs = new QTabWidget(this);
  m_tabs->setObjectName(QStringLiteral("dataPreviewTabs"));
  m_tabs->setTabsClosable(true);
  m_tabs->setUsesScrollButtons(true); // T32：标签超宽滚动，不挤压
  m_tabs->setAccessibleName(tr("预览"));
  // dock 面板样式（DESIGN.md）：无工作流蓝下划线，安静边框；活体跟随主题。
  PaleoTheme::applyThemedStyleSheet(m_tabs, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return QStringLiteral(
        "QTabWidget::pane { border: 1px solid %1; background: %2; top: -1px; }"
        "QTabBar::tab { padding: 4px 10px; color: %3; border: 1px solid %1;"
        " border-bottom: none; background: %2; }"
        "QTabBar::tab:selected { color: %4; font-weight: 600; }")
        .arg(qssHex(t.border), qssHex(t.surface), qssHex(t.textMuted), qssHex(t.text));
  });
  connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
    const QString assetId = assetIdAt(index);
    if (!assetId.isEmpty())
      closeAssetTab(assetId);
  });
  connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index >= 0)
      focusWellIfNeeded(assetIdAt(index), m_tabs->widget(index));
    syncDetails();
  });
  // D7 最大化 affordance：右上角 checkable 钮，切换时只发意图信号——实际
  // 分栏尺寸由 shell 决定。空态时 tabs 隐藏，按钮随之隐藏。
  auto *maxBtn = new QToolButton(m_tabs);
  maxBtn->setObjectName(QStringLiteral("previewMaxButton"));
  maxBtn->setCheckable(true);
  maxBtn->setText(tr("最大化预览"));
  maxBtn->setAccessibleName(tr("最大化预览"));
  maxBtn->setToolTip(tr("暂时收起数据列表和属性面板，让预览占满工作区"));
  // QGIS 主题没有最大化/还原语义——PaleoIcons 自绘，随勾选态切换。
  maxBtn->setIcon(PaleoIcons::maximize());
  maxBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  connect(maxBtn, &QToolButton::toggled, this, [this, maxBtn](bool on) {
    maxBtn->setText(on ? tr("还原预览") : tr("最大化预览"));
    maxBtn->setIcon(on ? PaleoIcons::restore() : PaleoIcons::maximize());
    maxBtn->setToolTip(on ? tr("恢复最大化前的面板布局") : tr("暂时收起数据列表和属性面板，让预览占满工作区"));
    emit previewMaximizeToggled(on);
  });
  m_tabs->setCornerWidget(maxBtn, Qt::TopRightCorner);
  lay->addWidget(m_tabs);

  m_emptyLabel = stateLabel(tr("还没有打开的预览 — 从顶部导入数据，再在左侧列表选择一条数据"), this);
  m_emptyLabel->setObjectName(QStringLiteral("previewEmptyLabel"));
  lay->addWidget(m_emptyLabel);
  m_tabs->setVisible(false);
}

DataPreviewTabs::~DataPreviewTabs() = default;

void DataPreviewTabs::setDetailsHost(QWidget *host)
{
  m_detailsHost = host;
  syncDetails();
}

void DataPreviewTabs::clearDetails(const QString &assetId)
{
  if (auto old = m_detailsOfAsset.take(assetId)) {
    old->hide();
    old->setParent(nullptr);
    old->deleteLater();
  }
  syncDetails();
}

void DataPreviewTabs::syncDetails()
{
  const QString active = assetIdAt(m_tabs->currentIndex());
  bool any = false;
  for (auto it = m_detailsOfAsset.cbegin(); it != m_detailsOfAsset.cend(); ++it)
    if (it.value()) {
      const bool show = it.key() == active;
      it.value()->setVisible(show);
      any |= show;
    }
  if (m_detailsHost)
    m_detailsHost->setVisible(any);
}


void DataPreviewTabs::setImportService(DataImportService *svc)
{
  // 自建门面（测试/小环境）；壳共享实例经 setDocService。
  m_docOwned.reset(svc ? new PreviewDocService(svc) : nullptr);
  attachDoc(m_docOwned.get());
}

void DataPreviewTabs::setDocService(PreviewDocService *doc)
{
  m_docOwned.reset();
  attachDoc(doc);
}

void DataPreviewTabs::attachDoc(PreviewDocService *doc)
{
  if (m_doc)
    disconnect(m_doc, nullptr, this, nullptr);
  if (m_catalogForTitles)
    disconnect(m_catalogForTitles, nullptr, this, nullptr);
  m_catalogForTitles = nullptr;
  m_doc = doc;
  if (!m_doc)
    return;
  if (m_taskSvc)
    m_doc->setTaskService(m_taskSvc); // 接线顺序无关：后到的服务补进门面
  // 文档 PDF 转换完成/失败 → 重建该资产标签（「转换中」→ 预览或降级面）。
  connect(m_doc, &PreviewDocService::documentPdfReady, this,
          [this](const QString &assetId) { rebuildAssetTab(assetId); });
  connect(m_doc, &PreviewDocService::documentPdfFailed, this,
          [this](const QString &assetId, const QString &) { rebuildAssetTab(assetId); });
  // 测线解码结果（D1/T23）：陈旧结果已在服务内按世代号丢弃。
  connect(m_doc, &PreviewDocService::seismicSectionReady, this,
          &DataPreviewTabs::onSectionReady);
  connect(m_doc, &PreviewDocService::seismicSectionFailed, this,
          &DataPreviewTabs::onSectionFailed);
  connect(m_doc, &PreviewDocService::seismicSectionCancelled, this,
          [this](const QString &assetId) {
            onSectionFailed(assetId, tr("已取消"));
          });
  // F1（goal/perf-systematize 簇2）：LAS 数据行异步填充结果（key=assetId；
  // 陈旧结果已在服务内按世代号丢弃）。
  connect(m_doc, &PreviewDocService::lasReady, this,
          &DataPreviewTabs::onLasReady);
  connect(m_doc, &PreviewDocService::lasFailed, this,
          &DataPreviewTabs::onLasFailed);
  connect(m_doc, &PreviewDocService::lasCancelled, this,
          [this](const QString &key) { onLasFailed(key, tr("已取消")); });
  // B 包 staleness-lite：stale 标记可能来自其它标签的 sha 复验或上游版本
  // 取代——catalog 任一变更后重算已开标签的「过时」徽标（GUI 线程直连，
  // 不必重建标签）。换绑服务时先断旧 catalog（上面已断）。
  m_catalogForTitles = m_doc->catalog();
  if (m_catalogForTitles)
    connect(m_catalogForTitles, &DataCatalog::changed, this, [this]() {
      for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
        updateTabTitle(it.key());
    });
}

void DataPreviewTabs::setTaskService(PaleoTaskService *svc)
{
  m_taskSvc = svc;
  if (m_doc)
    m_doc->setTaskService(svc);
}

void DataPreviewTabs::setProject(QgsProject *project)
{
  m_project = project;
}

void DataPreviewTabs::openSurveyArea()
{
  const QString key = QStringLiteral("survey_area");
  if (QWidget *existing = m_pageOfAsset.value(key))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing));
    // 井位是固定私有层——重开时重灌（会话中可能又导入了井）。
    if (auto *vl = existing->findChild<QgsVectorLayer *>(QStringLiteral("surveyWellsLayer")))
    {
      vl->dataProvider()->truncate();
      if (m_doc && m_doc->catalog())
        for (const CatalogEntity &well : m_doc->catalog()->entities(QStringLiteral("well")))
          if (well.hasSurface && std::isfinite(well.surfaceX) && std::isfinite(well.surfaceY))
            addMemoryPoint(vl, well.surfaceX, well.surfaceY, well.name, QStringLiteral("well"));
      vl->updateExtents();
    }
    if (auto *cv = existing->findChild<QgsMapCanvas *>(QStringLiteral("surveyMapCanvas")))
    {
      QgsRectangle target;
      if (auto *band = existing->findChild<QgsRubberBand *>(QStringLiteral("surveyAreaRubberBand")))
        target = band->asGeometry().boundingBox();
      if (!target.isNull() && !target.isEmpty())
      {
        target.grow(qMax(target.width(), target.height()) * 0.08);
        cv->setExtent(target);
      }
      else
      {
        cv->zoomToFullExtent();
      }
      cv->refresh();
    }
    return;
  }

  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(0, 0, 0, 0);
  pageLay->setSpacing(0);

  QWidget *content = buildSurveyAreaContent(page);
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成测区地图"), page), 1);

  const int idx = m_tabs->addTab(page, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")), tr("测区全景地图"));
  m_pageOfAsset.insert(key, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
}

QWidget *DataPreviewTabs::buildSurveyAreaContent(QWidget *page)
{
  auto *w = new QWidget(page);
  auto *lay = new QVBoxLayout(w);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  // 顶部快捷控制条（遵照 DESIGN.md 设计规范；token 活体样式见 stylePreviewToolBar）
  auto *topBar = new QWidget(w);
  auto *tbLay = new QHBoxLayout(topBar);
  tbLay->setContentsMargins(8, 4, 8, 4);
  tbLay->setSpacing(6);
  stylePreviewToolBar(topBar);

  auto *lblTitle = new QLabel(tr("测区全景地图 (QGIS 画布)"), topBar);
  PaleoTheme::applyThemedStyleSheet(lblTitle, [] {
    return QStringLiteral("font-weight: 600; color: %1; font-size: 9pt;")
        .arg(qssHex(PaleoTheme::tokens().text));
  });
  tbLay->addWidget(lblTitle);

  tbLay->addSpacing(8);

  auto *btnFull = new QToolButton(topBar);
  btnFull->setObjectName(QStringLiteral("btnSurveyFullExtent"));
  btnFull->setText(tr("全图"));
  btnFull->setToolTip(tr("缩放到测区全景范围"));
  btnFull->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));
  btnFull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnFull);

  auto *btnIn = new QToolButton(topBar);
  btnIn->setObjectName(QStringLiteral("btnSurveyZoomIn"));
  btnIn->setText(tr("放大"));
  btnIn->setToolTip(tr("放大地图 (支持鼠标滚轮缩放)"));
  btnIn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomIn.svg")));
  btnIn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnIn);

  auto *btnOut = new QToolButton(topBar);
  btnOut->setObjectName(QStringLiteral("btnSurveyZoomOut"));
  btnOut->setText(tr("缩小"));
  btnOut->setToolTip(tr("缩小地图 (支持鼠标滚轮缩放)"));
  btnOut->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomOut.svg")));
  btnOut->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnOut);

  auto *btnPan = new QToolButton(topBar);
  btnPan->setObjectName(QStringLiteral("btnSurveyPan"));
  btnPan->setText(tr("漫游"));
  btnPan->setToolTip(tr("按住鼠标左键拖拽平移地图"));
  btnPan->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionPan.svg")));
  btnPan->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnPan);

  // 1. 工区概况图应该有比例尺，指南针，工区范围等显示
  auto *btnBoundary = new QToolButton(topBar);
  btnBoundary->setObjectName(QStringLiteral("btnToggleSurveyBoundary"));
  btnBoundary->setText(tr("工区范围"));
  btnBoundary->setToolTip(tr("显示/隐藏工区范围边界多边形"));
  btnBoundary->setCheckable(true);
  btnBoundary->setChecked(true);
  btnBoundary->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  btnBoundary->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnBoundary);

  auto *btnScaleBar = new QToolButton(topBar);
  btnScaleBar->setObjectName(QStringLiteral("btnToggleScaleBar"));
  btnScaleBar->setText(tr("比例尺"));
  btnScaleBar->setToolTip(tr("开启/关闭左下角动态比例尺"));
  btnScaleBar->setCheckable(true);
  btnScaleBar->setChecked(true);
  btnScaleBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnScaleBar);

  auto *btnNorthArrow = new QToolButton(topBar);
  btnNorthArrow->setObjectName(QStringLiteral("btnToggleNorthArrow"));
  btnNorthArrow->setText(tr("指南针"));
  btnNorthArrow->setToolTip(tr("开启/关闭右上角指北针"));
  btnNorthArrow->setCheckable(true);
  btnNorthArrow->setChecked(true);
  btnNorthArrow->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnNorthArrow);

  auto *btnGrid = new QToolButton(topBar);
  btnGrid->setObjectName(QStringLiteral("btnToggleGrid"));
  btnGrid->setText(tr("网格"));
  btnGrid->setToolTip(tr("开启/关闭坐标方格网"));
  btnGrid->setCheckable(true);
  btnGrid->setChecked(false);
  btnGrid->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnGrid);

  auto *btnSwitchMain = new QToolButton(topBar);
  btnSwitchMain->setObjectName(QStringLiteral("btnSwitchToMainCanvas"));
  btnSwitchMain->setText(tr("在主画布中查看"));
  btnSwitchMain->setToolTip(tr("切换到主工作区全屏 QGIS 地图画布"));
  btnSwitchMain->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionMapSettings.svg")));
  btnSwitchMain->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnSwitchMain);

  tbLay->addStretch(1);

  // P2：测区全景走统一 PreviewMapPage（工具条/状态条/鹰眼/书签/TOC 全套）。
  auto *mapPage = new PreviewMapPage(w);
  mapPage->setObjectName(QStringLiteral("surveyPreviewPage"));
  mapPage->setProfileEnabled(false); // 全景浏览页不开剖面工具
  QgsMapCanvas *canvas = mapPage->mapCanvas()->canvas();
  canvas->setObjectName(QStringLiteral("surveyMapCanvas"));

  // 全景页不出鹰眼（固定幅面不需要总览缩略图），工具条开关一并摘掉。
  if (auto *ovAction = mapPage->findChild<QAction *>(QStringLiteral("previewOverviewAction")))
  {
    ovAction->setChecked(false); // toggled → setOverviewVisible(false)
    ovAction->setVisible(false);
  }
  else
  {
    mapPage->setOverviewVisible(false);
  }

  // 全景是固定内容画布：只挂私有井位层 + 工区范围 rubber band——不桥接
  // QgsProject 图层树（其它画布/图层服务实例化的图层不外溢进来，也不联动）。
  auto *wellsVl = makeMemoryPointLayer(tr("井位"), w);
  wellsVl->setObjectName(QStringLiteral("surveyWellsLayer"));
  QgsRectangle wellsExtent;
  if (m_doc && m_doc->catalog())
  {
    for (const CatalogEntity &well : m_doc->catalog()->entities(QStringLiteral("well")))
    {
      if (!well.hasSurface || !std::isfinite(well.surfaceX) || !std::isfinite(well.surfaceY))
        continue;
      addMemoryPoint(wellsVl, well.surfaceX, well.surfaceY, well.name, QStringLiteral("well"));
      wellsExtent.combineExtentWith(QgsRectangle(well.surfaceX, well.surfaceY,
                                                 well.surfaceX, well.surfaceY));
    }
  }
  styleSurveyWellLayer(wellsVl);
  mapPage->addMapLayer(wellsVl, tr("井位"), QString());

  // 装饰管理器（页内建，测区命名保持既有测试面）
  PaleoDecorationManager *decorMgr = mapPage->decorations();
  decorMgr->setObjectName(QStringLiteral("surveyAreaDecorManager"));
  decorMgr->setScaleBarEnabled(true);
  decorMgr->setNorthArrowEnabled(true);

  // 构建工区边界 (QgsRubberBand)
  CatalogEntity survey;
  if (m_doc && m_doc->catalog())
  {
    const auto surveys = m_doc->catalog()->entities(QStringLiteral("seismic_survey"));
    if (!surveys.isEmpty())
      survey = surveys.first();
  }

  QgsGeometry surveyGeom;
  if (survey.corners.size() >= 3)
  {
    QgsPolylineXY ring;
    for (const auto &c : survey.corners)
      ring.append(QgsPointXY(c.first, c.second));
    if (!ring.isEmpty() && ring.first() != ring.last())
      ring.append(ring.first());
    surveyGeom = QgsGeometry::fromPolygonXY(QgsPolygonXY{ring});
  }
  else if (survey.inlineMax > survey.inlineMin && survey.xlineMax > survey.xlineMin)
  {
    surveyGeom = QgsGeometry::fromRect(QgsRectangle(survey.inlineMin, survey.xlineMin,
                                                    survey.inlineMax, survey.xlineMax));
  }
  else if (!wellsExtent.isNull())
  {
    // 无 survey 几何：井位并集做兜底范围（单井退化范围扩 50 m 边）。
    if (wellsExtent.isEmpty())
      wellsExtent.grow(50.0);
    surveyGeom = QgsGeometry::fromRect(wellsExtent);
  }
  if (surveyGeom.isNull())
  {
    // 默认局部测区范围 (10 km × 10 km)
    surveyGeom = QgsGeometry::fromRect(QgsRectangle(0, 0, 10000, 10000));
  }

  auto *boundaryBand = new QgsRubberBand(canvas, Qgis::GeometryType::Polygon);
  boundaryBand->setParent(canvas);
  boundaryBand->setObjectName(QStringLiteral("surveyAreaRubberBand"));
  if (!surveyGeom.isNull() && surveyGeom.isGeosValid())
  {
    boundaryBand->setToGeometry(surveyGeom, nullptr);
  }
  boundaryBand->setColor(QColor(27, 115, 208, 16)); // #1B73D0 浅蓝半透明填充
  boundaryBand->setStrokeColor(QColor(QStringLiteral("#1B73D0"))); // 边界线
  boundaryBand->setWidth(2);
  boundaryBand->setLineStyle(Qt::DashLine);
  boundaryBand->show();

  // 工区范围与坐标系说明标签
  QString extentStr;
  if (!surveyGeom.isNull() && !surveyGeom.boundingBox().isEmpty())
  {
    const QgsRectangle box = surveyGeom.boundingBox();
    const double wKm = box.width() / 1000.0;
    const double hKm = box.height() / 1000.0;
    extentStr = tr("工区范围: %1 km × %2 km · 局部工程坐标系统 (米)")
                    .arg(QString::number(wKm, 'f', 1), QString::number(hKm, 'f', 1));
  }
  else
  {
    extentStr = tr("局部工程坐标系统 (米)");
  }
  auto *crsLabel = new QLabel(extentStr, topBar);
  crsLabel->setObjectName(QStringLiteral("surveyAreaExtentLabel"));
  QFont crsFont = PaleoTheme::monoFont();
  crsFont.setPointSize(PaleoTheme::kLabelPt);
  crsLabel->setFont(crsFont);
  PaleoTheme::applyThemedStyleSheet(crsLabel,
                                    [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  tbLay->addWidget(crsLabel);

  lay->addWidget(new PaleoToolRow(topBar, w));
  lay->addWidget(mapPage, 1);

  auto zoomFull = [canvas, surveyGeom]() {
    if (!surveyGeom.isNull() && !surveyGeom.boundingBox().isEmpty())
    {
      QgsRectangle ext = surveyGeom.boundingBox();
      ext.grow(qMax(ext.width(), ext.height()) * 0.08);
      canvas->setExtent(ext);
      canvas->refresh();
    }
    else
    {
      canvas->zoomToFullExtent();
      canvas->refresh();
    }
  };

  connect(btnFull, &QToolButton::clicked, mapPage, zoomFull);
  connect(btnIn, &QToolButton::clicked, mapPage, [mapPage]() { mapPage->mapCanvas()->canvas()->zoomIn(); });
  connect(btnOut, &QToolButton::clicked, mapPage, [mapPage]() { mapPage->mapCanvas()->canvas()->zoomOut(); });
  connect(btnPan, &QToolButton::clicked, mapPage, [mapPage]() {
    mapPage->toolManager()->activate(PreviewMapToolManager::kPan);
  });
  connect(btnBoundary, &QToolButton::toggled, canvas, [boundaryBand, canvas](bool checked) {
    boundaryBand->setVisible(checked);
    canvas->refresh();
  });
  connect(btnScaleBar, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setScaleBarEnabled(checked);
    canvas->refresh();
  });
  connect(btnNorthArrow, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setNorthArrowEnabled(checked);
    canvas->refresh();
  });
  connect(btnGrid, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setGridEnabled(checked);
    canvas->refresh();
  });
  connect(btnSwitchMain, &QToolButton::clicked, this, &DataPreviewTabs::requestShowOnMainCanvas);

  // 延迟自适应全图（等几何尺寸就绪）
  QTimer::singleShot(100, mapPage, zoomFull);

  return w;
}

int DataPreviewTabs::tabCount() const
{
  return m_tabs->count();
}

QString DataPreviewTabs::assetIdAt(int index) const
{
  QWidget *w = m_tabs->widget(index);
  if (!w)
    return QString();
  for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
    if (it.value() == w)
      return it.key();
  return QString();
}

void DataPreviewTabs::closeAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx >= 0)
    m_tabs->removeTab(idx);
  m_pageOfAsset.remove(assetId);
  clearDetails(assetId);
  m_wellEntityOfAsset.remove(assetId);
  m_titleSuffixOfAsset.remove(assetId);
  m_chosenVersionOfAsset.remove(assetId);
  // D1：标签关掉即释放该资产的索引缓存（持有文件句柄级状态）与世代号；
  // 进行中的解码任务请求取消——结果没人等了。
  if (m_doc)
  {
    m_doc->releaseSection(assetId);
    m_doc->releaseLas(assetId); // F1：同口径释放 LAS 解析世代号/取消在途
  }
  m_pendingSection.remove(assetId);
  m_pendingLas.remove(assetId);
  page->setParent(nullptr); // 摘出子树再推迟删除，关闭后 findChild 不再命中
  page->deleteLater();
  if (m_tabs->count() == 0)
  {
    m_tabs->setVisible(false);
    m_emptyLabel->setVisible(true);
  }
}

bool DataPreviewTabs::isMissingSourceState(const QString &assetId) const
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return false;
  auto *lbl = page->findChild<QLabel *>(QStringLiteral("stateText"));
  return lbl && lbl->text().contains(tr("找不到源文件"));
}

bool DataPreviewTabs::relocateMissingSourceWith(const QString &assetId,
                                                const QString &versionId,
                                                const QString &pickedPath)
{
  // wave4：把死胡同接到 relocateVersionSource——内容一致才重接（服务层拒解
  // SHA 不一致的候选文件，不静默换源）。失败保留「找不到源文件」状态与按钮，
  // 错误就地可见，可换文件再试；成功清掉本会话的 SHA 已验缓存（新路径要在
  // 重建时重新过 §3 校验门）并重建标签加载真预览。
  if (!m_doc || assetId.isEmpty())
    return false;
  QString err;
  const QString newVer = m_doc->relocateVersionSource(versionId, pickedPath, &err);
  if (newVer.isEmpty())
  {
    QWidget *page = m_pageOfAsset.value(assetId);
    if (auto *lbl = page ? page->findChild<QLabel *>(QStringLiteral("stateText")) : nullptr)
      lbl->setText(tr("找不到源文件\n重新定位失败：%1").arg(err));
    return false;
  }
  if (m_doc)
    m_doc->resetSha(assetId);
  rebuildAssetTab(assetId);
  return true;
}

QLabel *DataPreviewTabs::loadingLabel(const QString &fileName, QWidget *parent)
{
  // §4 读取中态：「正在读取」+文件名。读取仍是同步的——标签先就位并立即
  // 重绘，文件读完后隐藏（钩子存在，但不引入线程）。
  auto *l = stateLabel(tr("正在读取\n%1").arg(fileName), parent);
  l->setObjectName(QStringLiteral("loadingText"));
  return l;
}

QWidget *DataPreviewTabs::failureState(const QString &assetId, const QString &reason,
                                       QWidget *parent)
{
  // §4 失败态：「读取失败」+原因+文件名+「重试」。重试 = 重建该标签。
  const QString name =
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
  auto *box = new QWidget(parent);
  auto *l = new QVBoxLayout(box);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(4);
  l->addStretch(1);
  l->addWidget(stateLabel(tr("读取失败\n%1\n%2").arg(reason, name), box));
  auto *btn = new QPushButton(tr("重试"), box);
  btn->setObjectName(QStringLiteral("retryBtn"));
  connect(btn, &QPushButton::clicked, box,
          [this, assetId] { rebuildAssetTab(assetId); });
  l->addWidget(btn, 0, Qt::AlignHCenter);
  l->addStretch(1);
  return box;
}

void DataPreviewTabs::focusWellIfNeeded(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  if (!m_doc || assetId.isEmpty())
    return;
  // §4：well_head 标签的选中井在地图上高亮。多井标签只报该标签已选中的井
  // ——没有选中就不报，绝不拿第一条链接糊弄（m_wellEntityOfAsset 在
  // 唯一已决井/下拉框选择时写入）。
  const CatalogAsset asset = m_doc->catalog()->assetById(assetId);
  if (asset.type != QLatin1String("well_head"))
    return;
  const QString wellId = m_wellEntityOfAsset.value(assetId);
  if (!wellId.isEmpty())
    emit wellSelected(wellId);
}

void DataPreviewTabs::updateTabTitle(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx < 0)
    return;
  // §4：标题是「文件名 · 井名」/「文件名 · 测线」；无过滤时只有文件名。
  QString title =
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
  if (title.isEmpty())
    title = assetId;
  const QString suffix = m_titleSuffixOfAsset.value(assetId);
  if (!suffix.isEmpty())
    title += QStringLiteral(" · ") + suffix;
  // B 包 staleness-lite：资产当前版本被标 stale（上游 sha 失配/被取代）→
  // 标题带「过时」徽标——下游产物过期在数据页如实可见。
  if (m_doc && m_doc->catalog()->currentVersion(assetId)
                   .extra.value(QStringLiteral("stale"))
                   .toBool())
    title += QStringLiteral(" · ") + tr("过时");
  m_tabs->setTabText(idx, title);
}

void DataPreviewTabs::rebuildAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page || !m_doc)
    return;
  auto *pageLay = qobject_cast<QVBoxLayout *>(page->layout());
  if (!pageLay)
    return;
  clearDetails(assetId);
  while (QLayoutItem *it = pageLay->takeAt(0))
  {
    if (QWidget *w = it->widget())
    {
      // 信号发送者（如「重试」钮）可能就在被清的子树里——不能就地 delete，
      // 但先摘出父子树，deleteLater 后 findChild 不再碰到陈旧控件。
      w->setParent(nullptr);
      w->deleteLater();
    }
    delete it;
  }
  const QString name = m_doc->catalog()->assetById(assetId).displayName;
  QLabel *loading = loadingLabel(name.isEmpty() ? assetId : name, page);
  pageLay->addWidget(loading, 1);
  loading->repaint(); // 「正在读取」先可见，随后同步读
  QWidget *content = buildContent(assetId, page);
  loading->setVisible(false); // 保留在树里，便于测试/诊断读取中态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);
  updateTabTitle(assetId);
}

void DataPreviewTabs::openAsset(const QString &assetId)
{
  if (!m_doc || assetId.isEmpty())
    return;
  if (QWidget *existing = m_pageOfAsset.value(assetId))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing)); // 重选聚焦（§4）
    focusWellIfNeeded(assetId, existing);
    return;
  }

  const QString displayName = m_doc->catalog()->assetById(assetId).displayName;
  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(8, 8, 8, 8);

  QLabel *loading = loadingLabel(displayName.isEmpty() ? assetId : displayName, page);
  pageLay->addWidget(loading, 1);

  const int idx = m_tabs->addTab(page, displayName.isEmpty() ? assetId : displayName);
  m_pageOfAsset.insert(assetId, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
  loading->repaint(); // 「正在读取」+文件名在同步读取前先可见（§4）

  QWidget *content = buildContent(assetId, page);
  loading->setVisible(false); // 读取完成；隐藏但保留节点便于测试断言该状态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);
  updateTabTitle(assetId);
  focusWellIfNeeded(assetId, page);
}

void DataPreviewTabs::openAssetForWell(const QString &assetId, const QString &wellId)
{
  if (!m_doc || assetId.isEmpty())
    return;
  if (!wellId.isEmpty())
  {
    m_wellEntityOfAsset[assetId] = wellId;
    if (m_doc->catalog())
      m_titleSuffixOfAsset[assetId] = m_doc->catalog()->entityById(wellId).name;
  }
  openAsset(assetId);
  QWidget *page = m_pageOfAsset.value(assetId);
  if (page)
  {
    if (auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo")))
    {
      const int idx = combo->findData(wellId);
      if (idx >= 0 && combo->currentIndex() != idx)
        combo->setCurrentIndex(idx);
    }
    updateTabTitle(assetId);
    focusWellIfNeeded(assetId, page);
  }
}

void DataPreviewTabs::openSeismicLine(const QString &assetId, const QString &kind,
                                      int line, double timeMs)
{
  openAsset(assetId); // §4 重选语义：已有标签聚焦，否则新开
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  auto *mode = page->findChild<QComboBox *>(QStringLiteral("lineMode"));
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  if (!mode || !no)
    return; // 非地震标签（或地震正文未建出来）——不造假测线控件
  const int want = mode->findData(
      kind == QLatin1String("crossline") ? QStringLiteral("crossline")
                                         : QStringLiteral("inline"));
  if (want >= 0 && mode->currentIndex() != want)
    mode->setCurrentIndex(want); // currentIndexChanged → 该控件链路上的 decode
  if (no->value() != line)
    no->setValue(line); // valueChanged → decode 目标测线
  if (auto *modeTabs = page->findChild<QTabWidget *>(QStringLiteral("seismicSubTabs")))
    modeTabs->setCurrentIndex(0); // 聚焦到二维测线剖面页签
  Q_UNUSED(timeMs); // 目标时间的标注由剖面自身的 D61 标定线承担（§4/阶段B）
}

QWidget *DataPreviewTabs::buildContent(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return nullptr;
  const CatalogVersion v = cat->currentVersion(assetId);
  CatalogVersion sourceVersion = v; // abs 实际对应的版本（文档标签锚回 RAW 原件）
  QString abs = m_doc->absolutePathForVersion(v);
  // 文档资产：RAW 原件是规范来源——currentVersion 可能已指向 DERIVED
  // PDF 转换件，缺失检查与「用系统程序打开」必须锚在原件上。
  if (asset.type == QLatin1String("document"))
    for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
      if (cv.stage == QLatin1String("RAW"))
      {
        sourceVersion = cv;
        abs = m_doc->absolutePathForVersion(cv);
        break;
      }

  const auto links = cat->linksForAsset(assetId);
  // 已决井链接 → 多井标签的「井」下拉框数据源（未决链接不进列表，§4）。
  QVector<QPair<QString, QString>> wells; // (entityId, 井名)
  QString linkedBoundary;
  bool hasResolvedNonAux = false;
  bool hasAuxLink = false;
  for (const EntityAssetLink &l : links)
  {
    if (l.unresolved || l.entityId.isEmpty())
      continue;
    if (l.entityType == QLatin1String("well"))
    {
      const CatalogEntity w = cat->entityById(l.entityId);
      wells.append({l.entityId, w.name.isEmpty() ? l.entityId : w.name});
      hasResolvedNonAux = true;
      continue;
    }
    if (l.entityType == QLatin1String("sequence_boundary") && linkedBoundary.isEmpty())
      linkedBoundary = l.entityId;
    if (l.entityType == QLatin1String("auxiliary"))
      hasAuxLink = true;
    else
      hasResolvedNonAux = true;
  }
  // 固定辅助参考（§4 阶段 D）：XML 被内容判成井类但按规则钉在辅助实体上
  // （如 参考资料/ 下的 HZ28-6-1）——链接全部是 auxiliary 时一律走参考面板，
  // 绝不拿 well_head/well_log 类型去解析。
  const bool auxOnly = hasAuxLink && !hasResolvedNonAux;

  QWidget *host = new QWidget(this);
  auto *lay = new QVBoxLayout(host);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(6);

  // 外链/受管缺失态（§4：「找不到源文件」+路径）。外链版本（wave4）多给一个
  // 「重新定位文件…」出口——服务层流式 SHA-256 复验，内容一致才重接，不一致
  // 如实拒绝；受管文件缺失不是这条恢复路径能解的，不给按钮、只留文案。
  if (abs.isEmpty() || !QFile::exists(abs))
  {
    lay->addWidget(stateLabel(tr("找不到源文件\n%1").arg(abs.isEmpty() ? v.path : abs), host), 1);
    if (!sourceVersion.managed && m_doc)
    {
      auto *btn = new QPushButton(tr("重新定位文件…"), host);
      btn->setObjectName(QStringLiteral("relocateBtn"));
      const QString versionId = sourceVersion.id;
      connect(btn, &QPushButton::clicked, host, [this, assetId, versionId] {
        const QString picked = QFileDialog::getOpenFileName(
            this, tr("重新定位源文件"), QString(), QString());
        if (!picked.isEmpty())
          relocateMissingSourceWith(assetId, versionId, picked);
      });
      lay->addWidget(btn, 0, Qt::AlignHCenter);
    }
    return host;
  }

  // 外链完整性（§3）：入库时留过 SHA-256 的源文件被改过就不再解码——
  // 正文如实写「源文件与入库时的 SHA-256 不一致」。
  // D1：地震资产接了任务服务时把这道哈希移交异步解码任务——体量大不该堵
  // 住建标签；其它资产类型文件小，保留同步门（会话已验过的资产直接跳过）。
  // 托管/无指纹/本会话已验的短路、失配后的下游标过时都在门面里。
  const bool deferShaToTask =
      m_doc->taskService() && asset.type == QLatin1String("seismic");
  if (!deferShaToTask)
  {
    QString verr;
    if (!m_doc->verifyExternalSha(assetId, sourceVersion, &verr))
    {
      lay->addWidget(stateLabel(verr, host), 1);
      return host;
    }
  }

  if (asset.type == QLatin1String("well_log") && !auxOnly)
  {
    // 单井曲线：按已决链接过滤（LAS 本就是单井文件），标题带井名。
    QString linkedWell;
    if (!wells.isEmpty())
      linkedWell = wells.front().first;
    if (!linkedWell.isEmpty())
    {
      m_wellEntityOfAsset[assetId] = linkedWell;
      m_titleSuffixOfAsset[assetId] = wells.front().second;
    }
    // F1 两段式（goal/perf-systematize 簇2）：lasHeaderAt 只读 ~V/~W/~C 到
    // ~A 段头（代价与头部行数成正比、与数据行数无关）——曲线名秒出，整页
    // 控件骨架同步铺完；数据行 requestLas 池内解析（大文件冷解析曾 400ms+
    // 阻塞 UI），lasReady 到达后补数据与单位。无任务服务时 requestLas 同步
    // 执行、返回前信号已发——测试环境行为与旧路径一致。
    LasHeaderInfo header;
    QString perr;
    if (!m_doc->lasHeaderAt(abs, &header, &perr))
    {
      lay->addWidget(failureState(assetId, perr, host), 1);
      return host;
    }
    const QStringList names = header.curveNames;
    auto *singlePage = new QWidget(host);
    auto *singleLay = new QVBoxLayout(singlePage);
    singleLay->setContentsMargins(0, 0, 0, 0);
    singleLay->setSpacing(6);

    auto *panel = new CurvePanel(singlePage);
    panel->setObjectName(QStringLiteral("curvePanel"));
    panel->setEmptyText(tr("这条曲线没有有效样点")); // §4：整条 -99999 → 不绘制

    // 1. 顶部控制栏（主选曲线 + 预设 + 缩放控制）
    auto *topBar = new QWidget(singlePage);
    auto *topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(6);

    auto *combo = new QComboBox(topBar);
    combo->setObjectName(QStringLiteral("curveCombo"));
    combo->setAccessibleName(tr("曲线"));
    for (int i = 1; i < names.size(); ++i) // curves[0] 是深度道
      combo->addItem(names.at(i), i); // userData = curves 下标（禁用项不受序号偏移影响）

    // §4：约定的 GR/AC/DEN 缺了就给禁用项，tooltip 写「这条曲线不在文件里」。
    static const QStringList kExpected{QStringLiteral("GR"), QStringLiteral("AC"),
                                       QStringLiteral("DEN")};
    for (const QString &cn : kExpected)
      if (combo->findText(cn) < 0)
      {
        const int j = combo->count();
        combo->addItem(cn, -1);
        combo->setItemData(j, tr("这条曲线不在文件里"), Qt::ToolTipRole);
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        if (model && model->item(j))
          model->item(j)->setEnabled(false);
      }

    const int def = combo->findText(QStringLiteral("GR"));
    if (def >= 0 && combo->itemData(def).toInt() > 0) // 禁用项不当作默认曲线
      combo->setCurrentIndex(def);

    // 深度缩放按钮组
    auto *btnZoomOut = new QToolButton(topBar);
    btnZoomOut->setText(QStringLiteral("−"));
    btnZoomOut->setToolTip(tr("缩小深度 (Ctrl+滚轮向下)"));
    btnZoomOut->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

    auto *lblZoom = new QLabel(QStringLiteral("100%"), topBar);
    lblZoom->setFont(monoFont());
    PaleoTheme::applyThemedStyleSheet(lblZoom, [] {
      return QStringLiteral("color: %1; min-width: 44px;")
          .arg(qssHex(PaleoTheme::tokens().textMuted));
    });
    lblZoom->setAlignment(Qt::AlignCenter);

    auto *btnZoomIn = new QToolButton(topBar);
    btnZoomIn->setText(QStringLiteral("+"));
    btnZoomIn->setToolTip(tr("放大深度 (Ctrl+滚轮向上)"));
    btnZoomIn->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

    auto *btnZoomReset = new QToolButton(topBar);
    btnZoomReset->setText(tr("1:1 适应"));
    btnZoomReset->setToolTip(tr("重置为全井深 (双击图道重置)"));
    btnZoomReset->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    // 曲线快速预设按钮
    auto *btnSelectDefault = new QToolButton(topBar);
    btnSelectDefault->setText(tr("常规(GR/AC/DEN)"));
    btnSelectDefault->setToolTip(tr("显示三孔隙/常规测井曲线"));
    btnSelectDefault->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    auto *btnSelectAll = new QToolButton(topBar);
    btnSelectAll->setText(tr("全选"));
    btnSelectAll->setToolTip(tr("同时显示所有曲线"));
    btnSelectAll->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    auto *btnClear = new QToolButton(topBar);
    btnClear->setText(tr("仅主选"));
    btnClear->setToolTip(tr("仅显示当前下拉框选中的单根曲线"));
    btnClear->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    topLay->addWidget(caption8(tr("主选曲线:"), topBar));
    topLay->addWidget(combo);
    topLay->addSpacing(8);
    topLay->addWidget(btnSelectDefault);
    topLay->addWidget(btnSelectAll);
    topLay->addWidget(btnClear);
    topLay->addStretch(1);
    topLay->addWidget(caption8(tr("深度缩放:"), topBar));
    topLay->addWidget(btnZoomOut);
    topLay->addWidget(lblZoom);
    topLay->addWidget(btnZoomIn);
    topLay->addWidget(btnZoomReset);

    // 初始显示曲线集合（默认优先显示 GR/AC/DEN 常规三孔隙）
    QSet<QString> defaultShown;
    if (combo->findText(QStringLiteral("GR")) >= 0 && combo->itemData(combo->findText(QStringLiteral("GR"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("GR"));
    if (combo->findText(QStringLiteral("AC")) >= 0 && combo->itemData(combo->findText(QStringLiteral("AC"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("AC"));
    if (combo->findText(QStringLiteral("DEN")) >= 0 && combo->itemData(combo->findText(QStringLiteral("DEN"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("DEN"));
    if (defaultShown.isEmpty() && names.size() > 1)
      defaultShown.insert(names.at(1));

    // 曲线数据本体（values/unit）两段式第二段到达后经 fill 回调补装——
    // 此处只建骨架（chips/下拉/缩放/呈现切换），不碰数据行。

    // 2. 曲线多选 Chips 栏（横向滚动条，支持单击自由切换各曲线可见性）
    auto *chipScroll = new QScrollArea(singlePage);
    chipScroll->setWidgetResizable(true);
    chipScroll->setFixedHeight(32);
    chipScroll->setFrameShape(QFrame::NoFrame);
    chipScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    chipScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chipScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto *chipContainer = new QWidget(chipScroll);
    chipContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *chipLay = new QHBoxLayout(chipContainer);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(6);
    chipLay->addWidget(caption8(tr("多曲线叠合:"), chipContainer));

    // 曲线 chip：描边/字色用曲线数据色（数据符号，豁免）；底/边/悬停走 chrome
    // token。切换时重算当前主题样式，活体注册保证运行中换主题跟随。
    const auto chipStyle = [](const QColor &col, bool on) {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      if (on)
        return QStringLiteral(
            "QToolButton { background: %1; border: 1.5px solid %2; border-radius: 8px; "
            "color: %2; font-weight: bold; padding: 1px 7px; font-size: 8pt; }"
            "QToolButton:hover { background: %3; }")
            .arg(qssHex(t.surface), col.name(), qssHex(t.surfaceAltRaised));
      return QStringLiteral(
          "QToolButton { background: %1; border: 1px solid %2; border-radius: 8px; "
          "color: %3; padding: 1px 7px; font-size: 8pt; }"
          "QToolButton:hover { background: %4; border-color: %5; }")
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.textMuted),
               qssHex(t.surfaceAltRaised), qssHex(t.textDisabled));
    };

    auto chipMap = std::make_shared<QHash<QString, QToolButton *>>();
    for (int i = 1; i < names.size(); ++i)
    {
      const QString &cname = names.at(i);
      const QColor col = pickCurveColor(cname, i - 1);
      auto *chip = new QToolButton(chipContainer);
      chip->setText(cname);
      chip->setCheckable(true);
      const bool isChecked = defaultShown.contains(cname);
      chip->setChecked(isChecked);
      chip->setToolTip(cname); // 单位两段式第二段（lasReady）随数据补写

      PaleoTheme::applyThemedStyleSheet(chip, [chipStyle, chip, col] {
        return chipStyle(col, chip->isChecked());
      });

      connect(chip, &QToolButton::toggled, host, [panel, chip, cname, chipStyle, col](bool on) {
        panel->setCurveVisible(cname, on);
        chip->setStyleSheet(chipStyle(col, on));
      });

      (*chipMap)[cname] = chip;
      chipLay->addWidget(chip);
    }
    chipLay->addStretch(1);
    chipScroll->setWidget(chipContainer);

    // 缩放接线
    connect(btnZoomIn, &QToolButton::clicked, host, [panel]() { panel->zoomIn(); });
    connect(btnZoomOut, &QToolButton::clicked, host, [panel]() { panel->zoomOut(); });
    connect(btnZoomReset, &QToolButton::clicked, host, [panel]() { panel->resetZoom(); });
    panel->onZoomChanged = [lblZoom](double z) {
      lblZoom->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
    };

    // 主选下拉框变更时，自动确保该曲线被勾选显示
    connect(combo, &QComboBox::currentIndexChanged, host, [panel, combo, names, chipMap]() {
      const int ci = combo->currentData().toInt();
      if (ci <= 0 || ci >= names.size())
        return;
      const QString &selName = names.at(ci);
      if (chipMap->contains(selName))
      {
        auto *btn = chipMap->value(selName);
        if (!btn->isChecked())
          btn->setChecked(true);
      }
    });

    // 预设按钮事件
    connect(btnSelectAll, &QToolButton::clicked, host, [chipMap]() {
      for (auto *b : *chipMap)
        if (!b->isChecked()) b->setChecked(true);
    });

    connect(btnSelectDefault, &QToolButton::clicked, host, [chipMap, defaultShown]() {
      for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
      {
        const bool on = defaultShown.contains(it.key());
        if (it.value()->isChecked() != on)
          it.value()->setChecked(on);
      }
    });

    connect(btnClear, &QToolButton::clicked, host, [combo, names, chipMap]() {
      const int ci = combo->currentData().toInt();
      const QString activeName = (ci > 0 && ci < names.size()) ? names.at(ci) : QString();
      for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
      {
        const bool on = (it.key() == activeName);
        if (it.value()->isChecked() != on)
          it.value()->setChecked(on);
      }
    });

    singleLay->addWidget(new PaleoToolRow(topBar, singlePage));
    singleLay->addWidget(chipScroll);
    singleLay->addWidget(panel, 1);

    // ResFormStar 多井道综合柱状图总装（骨架即建；曲线数据两段式第二段补装）
    auto *compPanel = new WellComposite::WellCompositePanel(host);
    compPanel->setObjectName(QStringLiteral("wellCompositePanel"));

    // 查询该井是否有关联分层数据 (DC.dat)
    QVector<WellComposite::FormationInterval> formationIntervals;
    if (!linkedWell.isEmpty())
    {
      static const QVector<QColor> kFormColors = {
          QColor(QStringLiteral("#FFE082")), QColor(QStringLiteral("#FFF59D")),
          QColor(QStringLiteral("#C8E6C9")), QColor(QStringLiteral("#A5D6A7")),
          QColor(QStringLiteral("#80CBC4")), QColor(QStringLiteral("#80DEEA")),
          QColor(QStringLiteral("#90CAF9")), QColor(QStringLiteral("#B39DDB"))};

      const auto wLinks = cat->linksForEntity(linkedWell);
      for (const auto &lk : wLinks)
      {
        if (lk.role == QLatin1String("tops"))
        {
          CatalogAsset topsAsset = cat->assetById(lk.assetId);
          CatalogVersion topsVer = cat->currentVersion(lk.assetId);
          QString topsPath = m_doc->absolutePathForVersion(topsVer);
          {
            QVector<WellTopRecord> tops;
            if (!topsPath.isEmpty() && m_doc->wellTopsAt(topsPath, &tops))
            {
              const QString normWell = DataCatalog::normalizeWellName(wells.isEmpty() ? QString() : wells.front().second);
              QVector<WellTopRecord> wellTops;
              for (const auto &tr : tops)
              {
                if (normWell.isEmpty() || DataCatalog::normalizeWellName(tr.wellName) == normWell)
                  wellTops.append(tr);
              }
              std::sort(wellTops.begin(), wellTops.end(), [](const WellTopRecord &a, const WellTopRecord &b) {
                return a.md < b.md;
              });
              for (int ti = 0; ti < wellTops.size(); ++ti)
              {
                WellComposite::FormationInterval fi;
                fi.name = wellTops.at(ti).topName;
                fi.topDepth = static_cast<float>(wellTops.at(ti).md);
                fi.bottomDepth = static_cast<float>((ti + 1 < wellTops.size()) ? wellTops.at(ti + 1).md : (wellTops.at(ti).md + 50.0));
                fi.color = kFormColors.at(ti % kFormColors.size());
                formationIntervals.append(fi);
              }
            }
          }
          break;
        }
      }
    }

    const QString wellTitle = wells.isEmpty() ? asset.displayName : wells.front().second;

    // 两段式期间如实占位：数据行池内解析中（DESIGN.md 诚实状态；秒级内
    // 换装真实曲线，无骨架闪空）。
    auto *lasPendingHint = new QLabel(tr("正在后台解析数据行…"), host);
    lasPendingHint->setObjectName(QStringLiteral("lasPendingHint"));
    PaleoTheme::applyThemedStyleSheet(lasPendingHint, [] {
      return PaleoTheme::mutedCaptionStyleSheet();
    });
    const QPointer<QLabel> hintFill(lasPendingHint);

    // 已决 well_log：综合图走井曲线并集。这里只读 ~C 头；兄弟文件数据体
    // 放进下面同一次 requestLas，不在 GUI 线程 parse。
    QString wellLogEntityId;
    for (const EntityAssetLink &l : links)
    {
      if (!l.unresolved && l.role == QLatin1String("well_log") && !l.entityId.isEmpty())
      {
        wellLogEntityId = l.entityId;
        break;
      }
    }
    const bool compositeFromWell = !wellLogEntityId.isEmpty();
    QVector<WellCurveRef> wellCurves;
    QStringList siblingPaths;
    if (compositeFromWell)
    {
      wellCurves = WellLogSet::wellCurveIndex(cat, catalogProjectDir(cat), wellLogEntityId);
      const QString currentAbs = QFileInfo(abs).absoluteFilePath();
      QSet<QString> seen;
      for (const WellCurveRef &ref : wellCurves)
      {
        const QString p = QFileInfo(ref.path).absoluteFilePath();
        if (p.isEmpty() || p == currentAbs || seen.contains(p))
          continue;
        seen.insert(p);
        siblingPaths.append(ref.path);
      }
    }

    // ---- F1 两段式第二段挂起：数据到达后一次装齐两个消费方 ----
    // 单道检视仍只用当前文件。无已决 well_log 时综合图与旧路径一致；
    // 有已决链接时综合图按 wellCurveIndex，每条曲线用自己文件的深度列。
    const QPointer<CurvePanel> panelFill(panel);
    const QPointer<WellComposite::WellCompositePanel> compFill(compPanel);
    const std::function<void(const QList<LasCurve> &, const QHash<QString, LasDoc> &)> fillCurves =
        [panelFill, compFill, chipMapFill = chipMap, hintFill, names, defaultShown,
         wellTitle, formationIntervals, wellCurves, compositeFromWell, abs](
            const QList<LasCurve> &curves, const QHash<QString, LasDoc> &siblings) {
          if (hintFill)
            hintFill->hide(); // 数据到齐，占位提示退场
          if (curves.size() != names.size())
            return; // 头/整份契约：lasHeaderAt 与 lasAt 曲线名逐项一致——不符不装
          if (panelFill)
          {
            for (int i = 1; i < names.size(); ++i)
            {
              const QString &cname = names.at(i);
              panelFill->addCurve(cname, curves.at(i).unit, curves.at(i).values,
                                  curves.at(0).values, pickCurveColor(cname, i - 1),
                                  defaultShown.contains(cname));
              if (auto *chip = chipMapFill->value(cname))
                chip->setToolTip(QStringLiteral("%1 (%2)").arg(cname, curves.at(i).unit));
            }
          }
          if (compFill)
          {
            QVector<WellComposite::CurveData> compCurves;
            if (!compositeFromWell)
            {
              const auto &depList = curves.at(0).values;
              QVector<float> depVec;
              depVec.reserve(depList.size());
              for (double d : depList)
                depVec.append(static_cast<float>(d));

              for (int i = 1; i < names.size(); ++i)
              {
                const auto &src = curves.at(i);
                WellComposite::CurveData cd;
                cd.name = names.at(i);
                cd.unit = src.unit;
                cd.color = pickCurveColor(cd.name, i - 1);
                cd.depths = depVec;
                cd.values.reserve(src.values.size());
                float valMin = 1e9f, valMax = -1e9f;
                for (double v : src.values)
                {
                  if (v <= -999.0 || v >= 99999.0)
                  {
                    cd.values.append(-9999.0f);
                    continue;
                  }
                  float fv = static_cast<float>(v);
                  cd.values.append(fv);
                  if (fv < valMin) valMin = fv;
                  if (fv > valMax) valMax = fv;
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
                compCurves.append(cd);
              }
            }
            else
            {
              int colorIndex = 0;
              for (const WellCurveRef &ref : wellCurves)
              {
                const QList<LasCurve> *body = lasBodyFor(ref.path, abs, curves, siblings);
                if (!body || body->isEmpty() || ref.column <= 0 || ref.column >= body->size())
                  continue; // 兄弟文件解析失败：跳过，不让整页失败
                const LasCurve &src = body->at(ref.column);
                compCurves.append(compositeCurve(ref.mnemonic, src.unit, body->at(0).values,
                                                 src.values, colorIndex));
                ++colorIndex;
              }
            }
            compFill->loadLasCurves(wellTitle, compCurves, formationIntervals);
          }
        };

    // 视图模式切换条与堆叠容器：选中 = chip 语义（primary 描边 + 浮起面底，
    // 同 ribbonStyleSheet checked 范式）；样式挂切换条一份，:checked 自动生效。
    auto *viewSwitchBar = new QWidget(host);
    auto *switchLay = new QHBoxLayout(viewSwitchBar);
    switchLay->setContentsMargins(0, 0, 0, 0);
    switchLay->setSpacing(8);
    PaleoTheme::applyThemedStyleSheet(viewSwitchBar, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return QStringLiteral(
          "QToolButton { background: %1; border: 1px solid %2; border-radius: 4px;"
          " padding: 3px 10px; font-size: 8pt; color: %3; }"
          "QToolButton:hover { background: %4; }"
          "QToolButton:checked { background: %4; border-color: %5; color: %6;"
          " font-weight: 600; }")
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
               qssHex(t.surfaceAltRaised), qssHex(t.primary), qssHex(t.primaryText));
    });

    auto *btnResForm = new QToolButton(viewSwitchBar);
    btnResForm->setObjectName(QStringLiteral("btnResFormView"));
    btnResForm->setText(tr("ResFormStar 综合多井道柱状图 (推荐)"));
    btnResForm->setCheckable(true);
    btnResForm->setChecked(true);

    auto *btnSingle = new QToolButton(viewSwitchBar);
    btnSingle->setObjectName(QStringLiteral("btnSingleView"));
    btnSingle->setText(tr("单道叠合检视"));
    btnSingle->setCheckable(true);
    btnSingle->setChecked(false);

    auto *viewStack = new QStackedWidget(host);
    viewStack->setObjectName(QStringLiteral("logViewStack"));
    viewStack->addWidget(compPanel);   // 0: ResForm 多井道柱状图（默认）
    viewStack->addWidget(singlePage);  // 1: 单道快速检视

    connect(btnResForm, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
      btnResForm->setChecked(true);
      btnSingle->setChecked(false);
      viewStack->setCurrentIndex(0);
    });

    connect(btnSingle, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
      btnSingle->setChecked(true);
      btnResForm->setChecked(false);
      viewStack->setCurrentIndex(1);
    });

    switchLay->addWidget(caption8(tr("呈现模式:"), viewSwitchBar));
    switchLay->addWidget(btnResForm);
    switchLay->addWidget(btnSingle);
    switchLay->addStretch(1);

    lay->addWidget(new PaleoToolRow(viewSwitchBar, host));
    lay->addWidget(lasPendingHint);
    lay->addWidget(viewStack, 1);

    // F1 二段挂起注册 + 数据请求（页根 = viewStack：整份解析失败时栈内容
    // 换成「读取失败」面，重试=重建标签再走一遍两段式）。无任务服务时
    // requestLas 同步执行、返回前信号已发——测试环境行为与旧路径一致。
    m_pendingLas.insert(assetId, {viewStack, fillCurves});
    m_doc->requestLas(assetId, abs, siblingPaths);
    return host;
  }

  const bool wellFilterable = asset.type == QLatin1String("well_head") ||
                              asset.type == QLatin1String("well_stratification") ||
                              asset.type == QLatin1String("time_depth");
  if (wellFilterable && !auxOnly)
  {
    if (wells.size() == 1)
    {
      // 恰好一口已决井：直接按它过滤（§4 autoplan：唯一解析时过滤即它）。
      const QString wellId = wells.front().first;
      m_wellEntityOfAsset[assetId] = wellId;
      m_titleSuffixOfAsset[assetId] = wells.front().second;
      lay->addWidget(buildWellBody(asset, abs, wellId, wells.front().second, host), 1);
      return host;
    }
    // T31 死胡同文案：多井 tab 无井可挂（下拉会是空的）时不留空白页——
    // 工程没井指向导入；资产未决指向数据页「挂到这口井」入口。
    if (wells.isEmpty())
    {
      auto *deadEnd = stateLabel(
          cat->entities(QStringLiteral("well")).isEmpty()
              ? tr("工程里还没有井 — 先导入工区文件夹（井位表会建立井）")
              : tr("这个资产还没有挂到任何井 — 在数据页资产表的「未决」行，"
                   "用「挂到这口井」把它挂上"),
          host);
      deadEnd->setObjectName(QStringLiteral("deadEndText"));
      lay->addWidget(deadEnd, 1);
      return host;
    }

    // 多井文件（井口表、DC.dat、多井 TD）或未决资产：每标签自带「井」下拉框，
    // 只列已决链接的井；默认未选 → 正文「先选择一口井」。
    auto *bar = new QWidget(host);
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    barLay->addWidget(caption8(tr("井"), bar));
    auto *combo = new QComboBox(bar);
    combo->setObjectName(QStringLiteral("wellCombo"));
    combo->setAccessibleName(tr("井"));
    for (const auto &w : wells)
      combo->addItem(w.second, w.first);
    combo->setCurrentIndex(-1); // 默认未选（§4）
    barLay->addWidget(combo);
    barLay->addStretch(1);
    lay->addWidget(bar);

    auto *bodyHost = new QWidget(host);
    auto *bodyLay = new QVBoxLayout(bodyHost);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(bodyHost, 1);

    const auto applyWell = [this, assetId, asset, abs, cat, combo, bodyLay, bodyHost,
                            host](const QString &wellId) {
      // 本标签自己的选择：不动其他标签（§4）。
      if (wellId.isEmpty())
        m_wellEntityOfAsset.remove(assetId);
      else
        m_wellEntityOfAsset[assetId] = wellId;
      const QString wname =
          wellId.isEmpty() ? QString() : cat->entityById(wellId).name;
      m_titleSuffixOfAsset[assetId] = wname;
      updateTabTitle(assetId);
      while (QLayoutItem *it = bodyLay->takeAt(0))
      {
        if (QWidget *w = it->widget())
          delete w; // 直接删：发送者（下拉框）不在正文子树里，陈旧控件立刻出树
        delete it;
      }
      if (wellId.isEmpty())
        bodyLay->addWidget(stateLabel(tr("先选择一口井"), bodyHost), 1);
      else
      {
        bodyLay->addWidget(buildWellBody(asset, abs, wellId, wname, bodyHost), 1);
        if (asset.type == QLatin1String("well_head"))
          emit wellSelected(wellId); // §4：选中时地图同时高亮该井
      }
    };
    connect(combo, &QComboBox::currentIndexChanged, host,
            [applyWell, combo](int idx) {
              applyWell(idx >= 0 ? combo->itemData(idx).toString() : QString());
            });
    // 重建时恢复本标签之前选中的井；否则保持未选。
    const QString prev = m_wellEntityOfAsset.value(assetId);
    const int prevIdx = prev.isEmpty() ? -1 : combo->findData(prev);
    if (prevIdx >= 0)
      combo->setCurrentIndex(prevIdx); // 触发 applyWell → 正文按该井渲染
    else
      applyWell(QString());
    return host;
  }

  if (asset.type == QLatin1String("horizon"))
  {
    const CatalogEntity sb =
        linkedBoundary.isEmpty() ? CatalogEntity() : cat->entityById(linkedBoundary);
    const QString pendingNote = sb.extra.value(QStringLiteral("pending")).toBool()
                                    ? tr("未决层位 — 不进入编图 chip")
                                    : QString();
    // D2.9 版本集合：DERIVED 栅格（多次生成按号降序）+ RAW 散点。
    QVector<CatalogVersion> deriveds;
    CatalogVersion raw;
    for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
    {
      if (cv.stage == QLatin1String("DERIVED"))
        deriveds.append(cv);
      else if (cv.stage == QLatin1String("RAW") && cv.versionNumber >= raw.versionNumber)
        raw = cv;
    }
    std::sort(deriveds.begin(), deriveds.end(),
              [](const CatalogVersion &a, const CatalogVersion &b) {
                return a.versionNumber > b.versionNumber;
              });
    const CatalogVersion derived = deriveds.isEmpty() ? CatalogVersion() : deriveds.first();
    const QString gridTxt =
        derived.id.isEmpty()
            ? tr("派生栅格：未生成")
            : tr("网格 %1×%2 · Z %3 %4–%5 · 拒绝 %6 · 碰撞 %7")
                  .arg(derived.extra.value(QStringLiteral("grid_rows")).toInt())
                  .arg(derived.extra.value(QStringLiteral("grid_cols")).toInt())
                  .arg(derived.extra.value(QStringLiteral("z_units")).toString(),
                       QString::number(derived.extra.value(QStringLiteral("z_min")).toDouble(), 'f', 1),
                       QString::number(derived.extra.value(QStringLiteral("z_max")).toDouble(), 'f', 1))
                  .arg(derived.extra.value(QStringLiteral("rejected")).toInt())
                  .arg(derived.extra.value(QStringLiteral("collisions")).toInt());
    // In the workbench, metadata and version controls live in Data Properties.
    // Standalone previews retain the same controls locally.
    auto *details = new QWidget(m_detailsHost ? m_detailsHost.data() : host);
    details->setObjectName(QStringLiteral("horizonPreviewDetails"));
    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(4);
    if (m_detailsHost) {
      m_detailsHost->layout()->addWidget(details);
      connect(host, &QObject::destroyed, details, &QObject::deleteLater);
      m_detailsOfAsset.insert(assetId, details);
      syncDetails();
    } else {
      lay->addWidget(details);
    }
    detailLayout->addWidget(caption8(tr("层位 %1").arg(sb.name.isEmpty() ? asset.displayName : sb.name), details));
    auto *grid = new QLabel(gridTxt, details);
    PaleoTheme::applyThemedStyleSheet(grid, [] {
      return QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().text));
    });
    grid->setWordWrap(true);
    detailLayout->addWidget(grid);
    if (!pendingNote.isEmpty())
    {
      auto *p = warnLabel(pendingNote, details);
      detailLayout->addWidget(p);
    }
    // 「在地图上显示」（§4/T29，语义原样）。
    auto *btn = new QPushButton(tr("在地图上显示"), details);
    btn->setObjectName(QStringLiteral("showOnMapBtn"));
    btn->setAccessibleName(tr("在地图上显示层位 %1").arg(sb.name.isEmpty()
                                                              ? asset.displayName
                                                              : sb.name));
    if (derived.id.isEmpty() || sb.name.isEmpty())
    {
      btn->setEnabled(false);
      btn->setToolTip(tr("还没有这个层位的栅格"));
    }
    else
    {
      const QString layerId = QStringLiteral("horizon.%1").arg(sb.name);
      btn->setProperty("layerId", layerId); // T29：双向同步按 layerId 寻址
      connect(btn, &QPushButton::clicked, this, [this, layerId]() {
        emit showHorizonOnMapRequested(layerId);
      });
    }
    detailLayout->addWidget(btn, 0, Qt::AlignLeft);

    // ---- D2.9 版本切换：≥2 个版本才给下拉；选 RAW → 散点信息卡。
    CatalogVersion chosen;
    const QString chosenId = m_chosenVersionOfAsset.value(assetId);
    for (const CatalogVersion &cv : deriveds)
      if (cv.id == chosenId)
        chosen = cv;
    if (chosen.id.isEmpty() && !raw.id.isEmpty() && raw.id == chosenId)
      chosen = raw;
    if (chosen.id.isEmpty())
      chosen = derived.id.isEmpty() ? raw : derived;
    const int totalVersions = deriveds.size() + (raw.id.isEmpty() ? 0 : 1);
    if (totalVersions > 1)
    {
      auto *verBar = new QWidget(details);
      auto *verLay = new QHBoxLayout(verBar);
      verLay->setContentsMargins(0, 0, 0, 0);
      verLay->addWidget(caption8(tr("版本"), verBar));
      auto *verCombo = new QComboBox(verBar);
      verCombo->setObjectName(QStringLiteral("previewVersionCombo"));
      for (const CatalogVersion &cv : deriveds)
        verCombo->addItem(tr("派生栅格 v%1").arg(cv.versionNumber), cv.id);
      if (!raw.id.isEmpty())
        verCombo->addItem(tr("原始散点 · %1").arg(raw.fileName), raw.id);
      const int wantIdx = verCombo->findData(chosen.id);
      if (wantIdx >= 0)
        verCombo->setCurrentIndex(wantIdx);
      connect(verCombo, &QComboBox::currentIndexChanged, host,
              [this, assetId, verCombo](int idx) {
                const QString vid = verCombo->itemData(idx).toString();
                if (!vid.isEmpty() && m_chosenVersionOfAsset.value(assetId) != vid)
                {
                  m_chosenVersionOfAsset[assetId] = vid;
                  rebuildAssetTab(assetId); // 画布即时切换（D2.9）
                }
              });
      verLay->addWidget(verCombo);
      verLay->addStretch(1);
      detailLayout->addWidget(verBar);
    }

    const bool chosenIsDerived = chosen.stage == QLatin1String("DERIVED");
    if (!chosenIsDerived)
    {
      // RAW 散点：如实给文件信息卡——散点解析属 io 层，视图不造假地图。
      lay->addWidget(caption8(tr("原始散点文件"), host));
      lay->addWidget(valueLabel(m_doc->absolutePathForVersion(chosen), host, false));
      lay->addWidget(stateLabel(tr("选中「派生栅格」版本可看地图预览"), host), 1);
      return host;
    }

    const QString tifPath = m_doc->absolutePathForVersion(chosen);
    auto raster = std::make_unique<QgsRasterLayer>(
        tifPath, sb.name.isEmpty() ? asset.displayName : sb.name, QStringLiteral("gdal"));
    if (!raster->isValid() || raster->extent().isEmpty())
    {
      // D1.7：数据源损坏给原因页，不给白画布。
      lay->addWidget(PreviewMapStates::buildErrorPage(
                         tr("无法读取层位栅格"), tifPath, host, tr("重试"),
                         [this, assetId] { rebuildAssetTab(assetId); }),
                     1);
      return host;
    }
    // D2.11 大图（>50MB 无金字塔）提示条：降级仍可用（低清先行 + 全图照渲）。
    const QString bigHint = PreviewRasterAnalysis::bigRasterHint(raster.get());
    if (!bigHint.isEmpty())
    {
      lay->addWidget(PreviewMapStates::buildBigRasterHintBar(bigHint, host));
      // B3（wave/deepen-perf）：消费侧预热——quiet 任务后台建瓦片金字塔 +
      // GDAL 外部 .ovr 概览；完成后重载层 + 刷新画布，本会话后续渲染走概览。
      if (m_doc)
      {
        QPointer<QgsRasterLayer> rasterGuard(raster.get());
        connect(m_doc, &PreviewDocService::rasterPyramidFinished, host,
                [this, rasterGuard, assetId](const QString &doneId, bool ok) {
                  if (doneId != assetId || !ok || !rasterGuard)
                    return;
                  rasterGuard->reload(); // 重开数据源——让 provider 发现 .ovr
                  rasterGuard->triggerRepaint();
                });
        m_doc->ensureRasterPyramidVersion(assetId);
      }
    }

    // ---- P2 地图正文：统一 PreviewMapPage（D1.x 框架全套） ----
    auto *page = new PreviewMapPage(host);
    page->setObjectName(QStringLiteral("horizonPreviewPage"));
    page->setAssetKey(assetId);
    page->setRenderCacheIdentity(assetId, chosen.id);
    page->mapCanvas()->canvas()->setObjectName(QStringLiteral("horizonMapCanvas"));
    page->decorations()->setObjectName(QStringLiteral("horizonDecorManager"));

    const auto sum = PreviewRasterAnalysis::summarize(raster.get());
    if (sum.valid)
      PreviewRasterAnalysis::applyPseudoColorRenderer(
          raster.get(), 1, sum.min, sum.max,
          *PreviewRasterAnalysis::rampPreset(QStringLiteral("depthBlues")), false,
          PreviewRasterAnalysis::Classification::Continuous);
    QgsRasterLayer *rasterRaw = raster.release();
    rasterRaw->setParent(host); // 私有层父子树托管（既有约定：不进 QgsProject）
    page->addMapLayer(rasterRaw, sb.name.isEmpty() ? asset.displayName : sb.name, tifPath);

    // ---- D2.1 等值线 overlay + D5.7 参数化（间距/标注密度） ----
    auto contourDir = std::make_shared<QTemporaryDir>();
    auto currentContour = std::make_shared<QgsVectorLayer *>(nullptr);
    const auto rebuildContours =
        [page, rasterRaw, tifPath, contourDir, currentContour, host](double interval, int density) {
          if (*currentContour)
          {
            page->removeMapLayer(*currentContour); // 旧层出树（父子托管，deleteLater 由父管）
            (*currentContour)->deleteLater();
            *currentContour = nullptr;
          }
          const QString gpkg =
              contourDir->filePath(QStringLiteral("contours_%1.gpkg").arg(interval));
          QString cerr;
          if (!FactorContourService::generateContours(tifPath, gpkg, interval, &cerr))
            return;
          auto *cl = makeContourLayer(gpkg, host, density);
          if (!cl->isValid())
          {
            cl->deleteLater();
            return;
          }
          *currentContour = cl;
          page->addMapLayer(cl, QObject::tr("等值线"), gpkg);
        };
    rebuildContours(10.0, 1);

    // ---- D5.5 统计面板 ----
    {
      auto *statsPage = new QWidget(page);
      auto *statsLay = new QFormLayout(statsPage);
      const auto addStat = [&statsLay, statsPage](const QString &k, const QString &v,
                                                  const QString &objectName) {
        auto *l = new QLabel(v, statsPage);
        l->setObjectName(objectName);
        l->setFont(monoFont());
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        statsLay->addRow(k, l);
      };
      addStat(tr("最小值"), QString::number(sum.min, 'f', 2), QStringLiteral("horizonStatMin"));
      addStat(tr("最大值"), QString::number(sum.max, 'f', 2), QStringLiteral("horizonStatMax"));
      addStat(tr("均值"), QString::number(sum.mean, 'f', 2), QStringLiteral("horizonStatMean"));
      addStat(tr("标准差"), QString::number(sum.stdDev, 'f', 2), QStringLiteral("horizonStatStd"));
      addStat(tr("有效像元占比"), QStringLiteral("%1%").arg(sum.validRatio() * 100.0, 0, 'f', 1),
              QStringLiteral("horizonStatValid"));
      page->addAnalysisTab(tr("统计"), statsPage);
    }

    // ---- D2.3/D5.8 直方图（分箱可调/对数纵轴；拉伸界随符号快调联动） ----
    {
      auto *histPage = new QWidget(page);
      auto *histLay = new QVBoxLayout(histPage);
      histLay->setContentsMargins(4, 4, 4, 4);
      auto *hist = new PreviewHistogramWidget(false, histPage);
      hist->setObjectName(QStringLiteral("horizonHistogram"));
      const auto refreshHist = [hist, rasterRaw](int bins) {
        hist->setHistogram(PreviewRasterAnalysis::histogram(rasterRaw, bins));
        // 当前渲染界（TOC 快调后随 renderer 读回）。
        if (auto *r = dynamic_cast<QgsSingleBandPseudoColorRenderer *>(rasterRaw->renderer()))
          if (auto *fn = r->shader()->rasterShaderFunction())
            hist->setStretchMarks(fn->minimumValue(), fn->maximumValue());
      };
      refreshHist(64);
      QObject::connect(hist, &PreviewHistogramWidget::binsChanged, histPage, refreshHist);
      QObject::connect(page->tocPanel(), &PreviewTocPanel::rasterStyleChanged, histPage,
                       [refreshHist, hist] { refreshHist(hist->bins()); });
      histLay->addWidget(hist, 1);
      page->addAnalysisTab(tr("直方图"), histPage);
    }

    // ---- D5.7 等值线参数 ----
    {
      auto *contourPage = new QWidget(page);
      auto *cform = new QFormLayout(contourPage);
      auto *intervalSpin = new QDoubleSpinBox(contourPage);
      intervalSpin->setObjectName(QStringLiteral("contourIntervalSpin"));
      intervalSpin->setRange(0.5, 1000.0);
      intervalSpin->setDecimals(1);
      intervalSpin->setValue(10.0);
      intervalSpin->setSuffix(tr(" m"));
      cform->addRow(tr("等值线间距"), intervalSpin);
      auto *densityCombo = new QComboBox(contourPage);
      densityCombo->setObjectName(QStringLiteral("contourDensityCombo"));
      densityCombo->addItem(tr("全部"), 2);
      densityCombo->addItem(tr("稀疏"), 1);
      densityCombo->addItem(tr("关"), 0);
      densityCombo->setCurrentIndex(1);
      cform->addRow(tr("标注密度"), densityCombo);
      const auto applyContours = [rebuildContours, intervalSpin, densityCombo]() {
        rebuildContours(intervalSpin->value(), densityCombo->currentData().toInt());
      };
      QObject::connect(intervalSpin, &QDoubleSpinBox::valueChanged, contourPage,
                       [applyContours](double) { applyContours(); });
      QObject::connect(densityCombo, &QComboBox::currentIndexChanged, contourPage,
                       [applyContours](int) { applyContours(); });
      page->addAnalysisTab(tr("等值线"), contourPage);
    }

    // ---- D5.6 局部极值（峰值/洼地标注开关 + 前列清单） ----
    {
      auto *extPage = new QWidget(page);
      auto *extLay = new QVBoxLayout(extPage);
      auto *extBar = new QWidget(extPage);
      auto *extBarLay = new QHBoxLayout(extBar);
      extBarLay->setContentsMargins(0, 0, 0, 0);
      auto *peakCheck = new QCheckBox(tr("峰值"), extBar);
      peakCheck->setObjectName(QStringLiteral("extremaPeakCheck"));
      auto *lowCheck = new QCheckBox(tr("洼地"), extBar);
      lowCheck->setObjectName(QStringLiteral("extremaLowCheck"));
      extBarLay->addWidget(peakCheck);
      extBarLay->addWidget(lowCheck);
      extBarLay->addStretch(1);
      extLay->addWidget(extBar);
      auto *extTable = new QTableWidget(0, 4, extPage);
      extTable->setObjectName(QStringLiteral("extremaTable"));
      extTable->setHorizontalHeaderLabels({tr("类型"), tr("X"), tr("Y"), tr("值")});
      extTable->verticalHeader()->setVisible(false);
      extTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
      extLay->addWidget(extTable, 1);

      auto *peaksBand = new QgsRubberBand(page->mapCanvas()->canvas(), Qgis::GeometryType::Point);
      peaksBand->setParent(page->mapCanvas()->canvas());
      peaksBand->setObjectName(QStringLiteral("horizonPeaksBand"));
      peaksBand->setColor(QColor(217, 38, 38));   // error 红系（语义：高凸）
      peaksBand->setIcon(QgsRubberBand::ICON_CIRCLE);
      peaksBand->setIconSize(6);
      peaksBand->hide();
      auto *lowsBand = new QgsRubberBand(page->mapCanvas()->canvas(), Qgis::GeometryType::Point);
      lowsBand->setParent(page->mapCanvas()->canvas());
      lowsBand->setObjectName(QStringLiteral("horizonLowsBand"));
      lowsBand->setColor(QColor(21, 118, 210));   // 蓝（语义：低洼）
      lowsBand->setIcon(QgsRubberBand::ICON_CIRCLE);
      lowsBand->setIconSize(6);
      lowsBand->hide();

      const auto refreshExtrema = [rasterRaw, peakCheck, lowCheck, peaksBand, lowsBand, extTable, sum]() {
        peaksBand->reset(Qgis::GeometryType::Point);
        lowsBand->reset(Qgis::GeometryType::Point);
        extTable->setRowCount(0);
        if (!peakCheck->isChecked() && !lowCheck->isChecked())
          return;
        const double prom = (sum.valid ? sum.stdDev : 0.0) * 0.5; // 显著性 = 半个标准差
        const auto extrema =
            PreviewRasterAnalysis::localExtrema(rasterRaw, 1, 5, prom, 100);
        for (const auto &e : extrema)
        {
          if (e.peak)
            peaksBand->addPoint(e.pos, false);
          else
            lowsBand->addPoint(e.pos, false);
          const int r = extTable->rowCount();
          extTable->insertRow(r);
          auto *typeItem = new QTableWidgetItem(e.peak ? tr("峰") : tr("洼"));
          auto *xItem = new QTableWidgetItem(QString::number(e.pos.x(), 'f', 1));
          auto *yItem = new QTableWidgetItem(QString::number(e.pos.y(), 'f', 1));
          auto *vItem = new QTableWidgetItem(QString::number(e.value, 'f', 2));
          for (auto *it : {xItem, yItem, vItem})
            setNumericItem(it);
          extTable->setItem(r, 0, typeItem);
          extTable->setItem(r, 1, xItem);
          extTable->setItem(r, 2, yItem);
          extTable->setItem(r, 3, vItem);
        }
        peaksBand->setVisible(peakCheck->isChecked());
        lowsBand->setVisible(lowCheck->isChecked());
      };
      QObject::connect(peakCheck, &QCheckBox::toggled, extPage,
                       [refreshExtrema](bool) { refreshExtrema(); });
      QObject::connect(lowCheck, &QCheckBox::toggled, extPage,
                       [refreshExtrema](bool) { refreshExtrema(); });
      page->addAnalysisTab(tr("极值"), extPage);
    }

    // ---- D5.1 剖面：画布拖线 → 沿线采样 → 剖面图（D5.2 悬停读数在面板内） ----
    QObject::connect(page, &PreviewMapPage::profileLineDrawn, host,
                     [page, rasterRaw](const QgsPointXY &p1, const QgsPointXY &p2, int total) {
                       const auto samples = PreviewRasterAnalysis::sampleProfile(rasterRaw, p1, p2, 200);
                       QVector<PreviewProfilePanel::Sample> pts;
                       pts.reserve(samples.size());
                       for (const auto &sm : samples)
                         pts.append({sm.distance, sm.value, sm.valid});
                       page->profilePanel()->addProfile(tr("剖面 %1").arg(total), pts, p1, p2);
                     });
    page->setProfileEnabled(true); // D1.3：栅格内容才开剖面工具

    // ---- D2.10 同目录组图：同目录可地图化资产一键叠加 ----
    {
      const auto siblings = PreviewMapStates::siblingMappableAssets(
          cat, tifPath, [this](const CatalogVersion &v) { return m_doc->absolutePathForVersion(v); });
      // 排除自身。
      QVector<QPair<QString, QString>> others;
      for (const auto &sib : siblings)
        if (sib.first != assetId)
          others.append(sib);
      if (!others.isEmpty())
      {
        auto *overlayBtn = new QToolButton(page);
        overlayBtn->setObjectName(QStringLiteral("siblingOverlayButton"));
        overlayBtn->setText(tr("同目录叠加"));
        overlayBtn->setToolTip(tr("把同目录下的相图/配准图片/层位栅格叠加到本预览"));
        overlayBtn->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(overlayBtn);
        for (const auto &sib : others)
        {
          const QString sibAssetId = sib.first;
          const QString sibName = sib.second;
          QAction *act = menu->addAction(sibName);
          QObject::connect(act, &QAction::triggered, host, [this, page, sibAssetId, sibName, host]() {
            addSiblingOverlayLayer(page, sibAssetId, sibName, host);
          });
        }
        overlayBtn->setMenu(menu);
        page->addToolBarWidget(overlayBtn);
      }
    }

    lay->addWidget(page, 1);

    // ---- D6.1/D6.2/D6.7：全图复位 → 缓存命中即上屏；未命中低清先行。 ----
    auto *openTimer = new QElapsedTimer();
    openTimer->start();
    QTimer::singleShot(0, host, [page, openTimer]() {
      page->mapCanvas()->zoomToFullExtent();
      page->primeRenderCache();
      if (!page->mapCanvas()->overlayVisible())
        page->showLowResSnapshot(); // D6.1 低清整图先上（后台精渲随后替换）
      openTimer->invalidate();
      delete openTimer;
    });
    return host;
  }

  if (asset.type == QLatin1String("seismic"))
  {
    // survey 几何（导入时冻结）驱动测线选择；只解码选中的一条（§7）。
    QString surveyId;
    for (const EntityAssetLink &l : links)
      if (l.role == QLatin1String("seismic_volume"))
        surveyId = l.entityId;
    const CatalogEntity survey =
        surveyId.isEmpty() ? CatalogEntity() : cat->entityById(surveyId);

    // ---- 标定井：catalog 序第一口有目标层位分层的井 + 主 time_depth 表插值
    // + 初始测线内插——派生量全在数据门面一次算好（§3/阶段 B 口径不变）。----
    const PreviewDocService::TieMarker tie = m_doc->seismicTieMarker(assetId);
    const bool haveTieTop = tie.haveTop;
    const QString tieWellName = tie.wellName;
    const int initialInline = tie.initialInline;

    auto *bar = new QWidget(host);
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    auto *mode = new QComboBox(bar);
    mode->setObjectName(QStringLiteral("lineMode"));
    mode->setAccessibleName(tr("测线"));
    mode->addItem(tr("纵测线"), QStringLiteral("inline"));
    mode->addItem(tr("横测线"), QStringLiteral("crossline"));
    auto *no = new QSpinBox(bar);
    no->setObjectName(QStringLiteral("lineSpin"));
    no->setAccessibleName(tr("测线号"));
    no->setRange(static_cast<int>(survey.inlineMin),
                 static_cast<int>(qMax(survey.inlineMax, survey.inlineMin)));
    if (survey.inlineMin == 0 && survey.inlineMax == 0) // 无 survey 元数据时放开范围
      no->setRange(0, 1000000);
    if (initialInline >= 0)
      no->setValue(initialInline);
    auto *panel = new SectionPanel(host);
    auto *tieCaption = caption8(QString(), host);
    tieCaption->setObjectName(QStringLiteral("tieLabel"));
    if (haveTieTop)
    {
      // 标定写「A1 D61」和时间，或「无时深表」「超出时深表」「时深表无序」之一。
      if (tie.ok)
        tieCaption->setText(
            tr("%1 %2 · %3 ms").arg(tieWellName, tie.horizon).arg(tie.timeMs, 0, 'f', 1));
      else
        tieCaption->setText(
            tr("%1 %2 · %3").arg(tieWellName, tie.horizon, tie.statusText));
    }
    const auto decode = [this, assetId, abs, v, mode, no, panel, tieCaption]() {
      panel->clearImage(); // 换测线先清掉上一张剖面（§4）
      const QString idxTip = tr("正在建立道索引");
      mode->setEnabled(false);
      no->setEnabled(false);
      mode->setToolTip(idxTip);
      no->setToolTip(idxTip);
      const bool isInline = mode->currentData().toString() == QLatin1String("inline");
      const int lineNo = no->value();

      // D1/T23：读者缓存/世代号/协作取消/SHA 复验/下游标过时全在门面——
      // 这里只挂起控件组（结果信号回来按 assetId 找回控件贴图）。
      SectionPending pend;
      pend.panel = panel;
      pend.mode = mode;
      pend.spin = no;
      pend.tieCaption = tieCaption;
      pend.tieText = tieCaption->text();
      const PreviewDocService::TieMarker tieNow = m_doc->seismicTieMarker(assetId);
      pend.hasTie = tieNow.haveTop && tieNow.ok;
      pend.tieMs = tieNow.timeMs;
      m_pendingSection[assetId] = pend;
      m_doc->requestSection(assetId, v.id, abs, v.managed, v.sha256,
                            isInline, lineNo);
    };

    connect(mode, &QComboBox::currentIndexChanged, host, [mode, no, survey, decode]() {
      const bool isInline = mode->currentData().toString() == QLatin1String("inline");
      no->setRange(isInline ? static_cast<int>(survey.inlineMin) : static_cast<int>(survey.xlineMin),
                   isInline ? static_cast<int>(qMax(survey.inlineMax, survey.inlineMin))
                            : static_cast<int>(qMax(survey.xlineMax, survey.xlineMin)));
      decode();
    });
    connect(no, &QSpinBox::valueChanged, host, decode);
    decode();
    barLay->addWidget(mode);
    barLay->addWidget(no);
    if (initialInline >= 0)
      barLay->addWidget(caption8(tr("%1 所在测线").arg(tieWellName), bar)); // §4 旁注
    barLay->addStretch(1);
    auto *modeTabs = new QTabWidget(host);
    modeTabs->setObjectName(QStringLiteral("seismicSubTabs"));
    // 普通页签选中 = 深字 + 加粗（同预览主标签栏范式），不用 primary 蓝字。
    PaleoTheme::applyThemedStyleSheet(modeTabs, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return QStringLiteral(
          "QTabWidget::pane { border: 1px solid %1; background: %2; }"
          "QTabBar::tab { background: %3; color: %4; padding: 4px 12px; border: 1px solid %1; border-bottom: none; }"
          "QTabBar::tab:selected { background: %2; color: %5; font-weight: 600; }")
          .arg(qssHex(t.border), qssHex(t.surface), qssHex(t.surfaceAlt),
               qssHex(t.textMuted), qssHex(t.text));
    });

    // 1. 二维测线 (2D)
    auto *w2d = new QWidget(modeTabs);
    w2d->setObjectName(QStringLiteral("seismic2DContainer"));
    auto *lay2d = new QVBoxLayout(w2d);
    lay2d->setContentsMargins(6, 6, 6, 6);
    lay2d->setSpacing(4);
    lay2d->addWidget(caption8(tr("选择一条测线解码"), w2d));
    lay2d->addWidget(bar);
    lay2d->addWidget(tieCaption);
    lay2d->addWidget(panel, 1);
    modeTabs->addTab(w2d, tr("二维测线 (2D)"));

    // 2. 三维立体 (3D)
    auto *panel3d = new seismic::Seismic3DViewPanel(modeTabs);
    panel3d->setObjectName(QStringLiteral("seismic3DPanel"));
    modeTabs->addTab(panel3d, tr("三维立体 (3D)"));

    // D7.3 解释层位面上图：伴生解释会话 <sgy>.seispicks.json 有拾取 →
    // 按层位名分组 IDW 网格化 → 3D 面片（显隐走面板「解释」菜单/信号）。
    {
      seismic::SeismicInterpretationSession session;
      if (seismic::SeismicTaskService::loadSession(abs, session, nullptr) &&
          !session.picks.isEmpty()) {
        QHash<QString, QList<seismic::SeismicPick>> byHorizon;
        for (const seismic::SeismicPick &p : session.picks)
          byHorizon[p.horizonName].append(p);
        QStringList names;
        std::vector<seismic::SeismicHorizonGrid> grids;
        for (auto it = byHorizon.constBegin(); it != byHorizon.constEnd(); ++it) {
          names << (it.key().isEmpty() ? tr("未命名层位") : it.key());
          grids.push_back(seismic::SeismicTaskService::gridPicks(it.value()));
        }
        if (!grids.empty())
          panel3d->setHorizons(names, grids);
      }
    }

    // D3.2：三维切片拖动/剖面条联动 2D——只拨同页 2D 测线控件（控件自己的
    // decode 链换测线）。不开新标签、不切回 2D 子页签。
    connect(panel3d, &seismic::Seismic3DViewPanel::inlineChanged,
            panel3d, [mode, no](int inlineNo) {
      const int want = mode->findData(QStringLiteral("inline"));
      if (want >= 0 && mode->currentIndex() != want)
        mode->setCurrentIndex(want);
      if (no->value() != inlineNo)
        no->setValue(inlineNo);
    });
    connect(panel3d, &seismic::Seismic3DViewPanel::crosslineChanged,
            panel3d, [mode, no](int xlineNo) {
      const int want = mode->findData(QStringLiteral("crossline"));
      if (want >= 0 && mode->currentIndex() != want)
        mode->setCurrentIndex(want);
      if (no->value() != xlineNo)
        no->setValue(xlineNo);
    });

    // ---- 引擎通道状态（先于转码区声明）：两段式体加载 + 显式 .sf3p 通道 ----
    auto sharedVol = std::make_shared<std::shared_ptr<seismic::SgyVolume>>();
    auto sharedPaged = std::make_shared<QString>();
    *sharedPaged = QFile::exists(abs + QStringLiteral(".sf3p"))
                       ? abs + QStringLiteral(".sf3p")
                       : QString(); // Auto 永不自动升级 .sf3p：存在即显式启用（引擎语义）

    // 3. 水平时间切片剖面 (Time Slice)
    auto *wTime = new QWidget(modeTabs);
    wTime->setObjectName(QStringLiteral("seismicTimeSliceContainer"));
    auto *layTime = new QVBoxLayout(wTime);
    layTime->setContentsMargins(6, 6, 6, 6);
    layTime->setSpacing(4);

    auto *timeBar = new QWidget(wTime);
    auto *timeBarLay = new QHBoxLayout(timeBar);
    timeBarLay->setContentsMargins(0, 0, 0, 0);
    timeBarLay->setSpacing(8);

    auto *lblTimeTitle = caption8(tr("水平时间切片 (TWT)"), timeBar);
    timeBarLay->addWidget(lblTimeTitle);

    auto *lblTimeIndex = new QLabel(tr("时间采样:"), timeBar);
    {
      QFont f = lblTimeIndex->font();
      f.setPointSize(PaleoTheme::kLabelPt);
      lblTimeIndex->setFont(f);
    }
    PaleoTheme::applyThemedStyleSheet(lblTimeIndex,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    timeBarLay->addWidget(lblTimeIndex);

    auto *sliderTime = new QSlider(Qt::Horizontal, timeBar);
    sliderTime->setObjectName(QStringLiteral("timeSliceSlider"));
    sliderTime->setFixedWidth(160);
    timeBarLay->addWidget(sliderTime);

    QFont mono8 = PaleoTheme::monoFont();
    mono8.setPointSize(PaleoTheme::kLabelPt);

    auto *spinTime = new QSpinBox(timeBar);
    spinTime->setObjectName(QStringLiteral("timeSliceSpin"));
    spinTime->setFont(mono8);
    spinTime->setFixedWidth(64);
    timeBarLay->addWidget(spinTime);

    // 时间读数：mono 数字面 + 正文色（数值读数不是装饰蓝的三许可用途）。
    auto *lblTimeMs = new QLabel(QStringLiteral("0.0 ms"), timeBar);
    lblTimeMs->setObjectName(QStringLiteral("timeSliceMsLabel"));
    lblTimeMs->setFont(mono8);
    PaleoTheme::applyThemedStyleSheet(lblTimeMs, [] {
      return QStringLiteral("color: %1;").arg(qssHex(PaleoTheme::tokens().text));
    });
    lblTimeMs->setFixedWidth(90);
    timeBarLay->addWidget(lblTimeMs);

    // 时间片/转码区按钮统一走一份活体样式（原 8.5pt + 浅色字面量收口）。
    PaleoTheme::applyThemedStyleSheet(timeBar, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return QStringLiteral(
          "QToolButton { background: transparent; border: 1px solid %1;"
          " border-radius: 4px; padding: 2px 8px; font-size: 8pt; color: %2; }"
          "QToolButton:hover { background: %3; border-color: %4; }"
          "QToolButton:disabled { color: %4; }")
          .arg(qssHex(t.border), qssHex(t.text), qssHex(t.surfaceAltRaised),
               qssHex(t.textDisabled));
    });

    auto *btnFitTime = new QToolButton(timeBar);
    btnFitTime->setText(tr("适应窗口"));
    timeBarLay->addWidget(btnFitTime);

    // ---- 转码区（主线6）：.sf3c（Auto 自动升级）与 .sf3p（显式 paged/LOD 通道）
    // 双通道并存；进度条非 spinner（有总量即百分比）、可取消、完成后热切换
    // 后端并把状态写在 backendLabel 上。取消后两条通道都可续跑。 ----
    auto *btnTranscode = new QToolButton(timeBar);
    btnTranscode->setText(tr("转码工作区 (.sf3c)"));
    btnTranscode->setToolTip(tr("将 SEG-Y 转码为 .sf3c 分片工作区（可续跑）；转码后切片与任意剖面走随机访问后端"));
    btnTranscode->setEnabled(!abs.isEmpty() && QFile::exists(abs)
                             && !QFile::exists(abs + QStringLiteral(".sf3c.meta")));
    // D1.2/D1.6：断点探测——半成品给「继续转码」入口，旧版/损坏给「重建」提示
    if (auto *probeSvc = (m_doc ? m_doc->seismicTaskService() : nullptr))
    {
      const seismic::SeismicWorkspaceProbe wp = probeSvc->probeWorkspace(abs);
      if (wp.exists && wp.readable && !wp.complete)
      {
        btnTranscode->setEnabled(true);
        btnTranscode->setText(tr("继续转码 (.sf3c)"));
        btnTranscode->setToolTip(tr("检测到未完成的 .sf3c 工作区（%1）。点击继续，已写分片自动跳过")
                                     .arg(wp.stateText()));
      }
      else if (wp.exists && !wp.readable)
      {
        btnTranscode->setEnabled(true);
        btnTranscode->setText(tr("重建工作区 (.sf3c)"));
        btnTranscode->setToolTip(tr("现有 .sf3c 工作区不可读（%1，格式版本 v%2）。再次转码将自动重建")
                                     .arg(wp.error.isEmpty() ? tr("格式不支持") : wp.error)
                                     .arg(wp.formatVersion));
      }
    }
    timeBarLay->addWidget(btnTranscode);

    auto *btnPagedTranscode = new QToolButton(timeBar);
    btnPagedTranscode->setObjectName(QStringLiteral("btnPagedTranscode"));
    btnPagedTranscode->setText(tr("转码分页工作区 (.sf3p)"));
    btnPagedTranscode->setToolTip(tr("转码为 .sf3p 分页工作区并构建 L1/L2 金字塔（可续跑）；"
                                     "提供瓦片渐进时间片与拖动粗/静止细的渐进 LOD。Auto 后端不自动启用，需显式选择"));
    const QString pagedPathForButtons = abs + QStringLiteral(".sf3p");
    btnPagedTranscode->setEnabled(!abs.isEmpty() && QFile::exists(abs)
                                  && !QFile::exists(pagedPathForButtons));
    if (auto *probeSvc = (m_doc ? m_doc->seismicTaskService() : nullptr))
    {
      const seismic::SeismicWorkspaceProbe pp = probeSvc->probePagedWorkspace(pagedPathForButtons);
      if (pp.exists && !pp.complete)
      {
        btnPagedTranscode->setEnabled(true);
        btnPagedTranscode->setText(tr("继续转码 (.sf3p)"));
        btnPagedTranscode->setToolTip(tr("检测到未完成的 .sf3p 分页工作区（%1）。点击继续，已完成页自动跳过")
                                          .arg(pp.stateText()));
      }
    }
    timeBarLay->addWidget(btnPagedTranscode);

    auto *transcodeProgress = new QProgressBar(timeBar);
    transcodeProgress->setObjectName(QStringLiteral("transcodeProgress"));
    transcodeProgress->setFixedWidth(140);
    transcodeProgress->setRange(0, 100);
    transcodeProgress->setVisible(false);
    timeBarLay->addWidget(transcodeProgress);

    auto *btnCancelTranscode = new QToolButton(timeBar);
    btnCancelTranscode->setObjectName(QStringLiteral("btnCancelTranscode"));
    btnCancelTranscode->setText(tr("取消"));
    btnCancelTranscode->setVisible(false);
    timeBarLay->addWidget(btnCancelTranscode);

    auto *backendLabel = new QLabel(timeBar);
    backendLabel->setObjectName(QStringLiteral("seismicBackendLabel"));
    backendLabel->setFont(mono8);
    PaleoTheme::applyThemedStyleSheet(backendLabel,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    timeBarLay->addWidget(backendLabel);

    // 后端状态探测（转码完成后的「热切换」提示；Auto 只认 .sf3c 伴生）
    const auto refreshBackendStatus = [this, abs, backendLabel](const QString &suffix = QString()) {
      auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
      if (!svc)
        return;
      const QPointer<QLabel> labelGuard(backendLabel);
      svc->startBackendProbe(abs, [labelGuard, suffix](bool ok, const seismic::SeismicBackendStatus &s, const QString &) {
        if (!labelGuard)
          return;
        if (!ok)
        {
          labelGuard->setText(QObject::tr("后端：探测失败"));
          return;
        }
        QString name;
        if (s.backendName == QLatin1String("workspace"))
          name = QObject::tr(".sf3c 工作区");
        else if (s.backendName == QLatin1String("paged-workspace"))
          name = QObject::tr(".sf3p 分页工作区");
        else
          name = QObject::tr("直读 SEG-Y");
        QString text = QObject::tr("后端：%1%2").arg(
            name, s.fellBackToDirect ? QObject::tr("（回退直读）") : QString());
        if (s.backendName == QLatin1String("paged-workspace") && !s.quality.isEmpty())
          text += QStringLiteral(" · %1").arg(s.quality);
        if (!suffix.isEmpty())
          text += suffix;
        labelGuard->setText(text);
      });
    };

    // 转码任务的进度条/取消接线（两通道共用）
    const auto bindTranscodeTask = [transcodeProgress, btnCancelTranscode](PaleoTask *task) {
      if (!task)
        return;
      transcodeProgress->setVisible(true);
      transcodeProgress->setValue(0);
      btnCancelTranscode->setVisible(true);
      const QPointer<PaleoTask> taskGuard(task);
      QObject::connect(task, &PaleoTask::changed, transcodeProgress, [taskGuard, transcodeProgress]() {
        if (taskGuard)
          transcodeProgress->setValue(taskGuard->percent());
      });
      QObject::connect(btnCancelTranscode, &QToolButton::clicked, task, &PaleoTask::requestCancel);
      QObject::connect(task, &PaleoTask::finished, transcodeProgress, [transcodeProgress, btnCancelTranscode]() {
        transcodeProgress->setVisible(false);
        btnCancelTranscode->setVisible(false);
      });
    };

    connect(btnTranscode, &QToolButton::clicked, host, [this, abs, btnTranscode, bindTranscodeTask, refreshBackendStatus]() {
      auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
      if (!svc)
        return;
      // D1.2：点击时重新探测——续跑/新建/重建三种话术
      const seismic::SeismicWorkspaceProbe wp = svc->probeWorkspace(abs);
      QString questionText;
      if (wp.exists && wp.readable && !wp.complete)
        questionText = tr("检测到未完成的 .sf3c 工作区（%1）。\n继续转码（已写分片自动跳过）？")
                           .arg(wp.stateText());
      else if (wp.exists && !wp.readable)
        questionText = tr("现有 .sf3c 工作区不可读（%1）。\n重新转码将自动重建，继续？")
                           .arg(wp.error.isEmpty() ? tr("格式版本不支持") : wp.error);
      else
        questionText = tr("将 %1 转码为 .sf3c 分片工作区（体积与源文件同量级）。\n"
                          "过程可取消并续跑；完成后切片与任意剖面走随机访问后端。")
                           .arg(QFileInfo(abs).fileName());
      const auto answer = QMessageBox::question(btnTranscode, tr("转码地震工作区"), questionText);
      if (answer != QMessageBox::Yes)
        return;
      btnTranscode->setEnabled(false);
      btnTranscode->setText(tr("转码中…（可取消）"));
      const QPointer<QToolButton> guard(btnTranscode);
      PaleoTask *task = svc->startWorkspaceTranscodeDetailed(
          abs, QString(),
          [guard, refreshBackendStatus](bool ok, const QString &, const QString &err) {
        if (!guard)
          return;
        if (ok)
        {
          guard->setText(QObject::tr("工作区已就绪"));
          guard->setEnabled(false);
          refreshBackendStatus(QObject::tr("（已热切换）"));
        }
        else
        {
          // D1.2：取消/失败后若留有半成品，入口变「继续转码」
          guard->setText(QObject::tr("继续转码 (.sf3c)"));
          guard->setEnabled(true);
          if (!err.isEmpty())
            QMessageBox::warning(guard, QObject::tr("转码未完成"), err);
        }
      },
          [guard](const seismic::SeismicTranscodeReport &report) {
        if (!guard)
          return;
        // D1.4：质量报告挂按钮 tooltip（道数/覆盖率/丢弃率/值域）
        guard->setToolTip(report.summaryLine());
        if (report.ok)
          QMessageBox::information(guard, QObject::tr("转码完成"), report.summaryLine());
        else if (report.damagedTraces > 0)
          QMessageBox::warning(guard, QObject::tr("转码包含坏道"),
                               QObject::tr("损坏源道 %1 条已跳过（NaN 填充），如 %2…")
                                   .arg(report.damagedTraces)
                                   .arg(report.damagedSample.isEmpty() ? QString() : report.damagedSample.first()));
      });
      bindTranscodeTask(task);
    });

    connect(btnPagedTranscode, &QToolButton::clicked, host,
            [this, abs, pagedPathForButtons, btnPagedTranscode, bindTranscodeTask, refreshBackendStatus,
             sharedPaged, panel3d, sharedVol]() {
      auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
      if (!svc)
        return;
      // D1.2：点击时重新探测（.partial 半成品 → 续跑话术）
      const seismic::SeismicWorkspaceProbe pp = svc->probePagedWorkspace(pagedPathForButtons);
      QString pagedQuestion;
      if (pp.exists && !pp.complete)
        pagedQuestion = tr("检测到未完成的 .sf3p 分页工作区（%1）。\n继续转码（已完成页自动跳过）？")
                            .arg(pp.stateText());
      else
        pagedQuestion = tr("将 %1 转码为 .sf3p 分页工作区并按体量自适应构建金字塔\n"
                           "（含瓦片渐进时间片与渐进 LOD；体积与源文件同量级）。\n"
                           "过程可取消并续跑。")
                            .arg(QFileInfo(abs).fileName());
      const auto answer = QMessageBox::question(btnPagedTranscode, tr("转码分页工作区"), pagedQuestion);
      if (answer != QMessageBox::Yes)
        return;
      btnPagedTranscode->setEnabled(false);
      btnPagedTranscode->setText(tr("转码中…（可取消）"));
      const QPointer<QToolButton> guard(btnPagedTranscode);
      PaleoTask *task = svc->startPagedTranscodeDetailed(
          abs, pagedPathForButtons, /*buildLod=*/true,
          [guard, refreshBackendStatus, sharedPaged, panel3d, sharedVol,
           pagedPathForButtons](bool ok, const QString &, const QString &err) {
        if (!guard)
          return;
        if (ok)
        {
          guard->setText(QObject::tr("分页工作区已就绪"));
          guard->setEnabled(false);
          // 热切换：启用显式 .sf3p 通道（瓦片时间片 + 3D 渐进 LOD）
          *sharedPaged = pagedPathForButtons;
          if (*sharedVol != nullptr)
            panel3d->setPagedWorkspace(*sharedPaged);
          refreshBackendStatus(QObject::tr("（已热切换）"));
        }
        else
        {
          guard->setText(QObject::tr("继续转码 (.sf3p)"));
          guard->setEnabled(true);
          if (!err.isEmpty())
            QMessageBox::warning(guard, QObject::tr("分页转码未完成"), err);
        }
      },
          [guard](const seismic::SeismicTranscodeReport &report) {
        if (!guard)
          return;
        guard->setToolTip(report.summaryLine());
        if (report.ok)
          QMessageBox::information(guard, QObject::tr("分页转码完成"), report.summaryLine());
      });
      bindTranscodeTask(task);
    });
    refreshBackendStatus();

    timeBarLay->addStretch(1);
    layTime->addWidget(new PaleoToolRow(timeBar, wTime));

    auto *timeCanvas = new seismic::SeismicSectionCanvas(wTime);
    timeCanvas->setObjectName(QStringLiteral("timeSliceCanvas"));
    timeCanvas->setColorMap(seismic::SectionColorMapType::RedWhiteBlue);
    timeCanvas->setGain(1.2f);
    timeCanvas->setContrast(1.3f);
    layTime->addWidget(timeCanvas, 1);

    connect(btnFitTime, &QToolButton::clicked, timeCanvas, &seismic::SeismicSectionCanvas::fitToWindow);

    modeTabs->addTab(wTime, tr("水平时间切片 (Time Slice)"));

    // ---- 两段式秒开（主线1）：QuickOpen 秒级预览 → 后台体加载换装 ----
    auto *quickInfo = new QLabel(host);
    quickInfo->setObjectName(QStringLiteral("seismicQuickInfo"));
    quickInfo->setFont(mono8);
    PaleoTheme::applyThemedStyleSheet(quickInfo,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    quickInfo->setVisible(false);

    const QPointer<QLabel> quickInfoGuard(quickInfo);
    const QPointer<QWidget> sectionPanelGuard(panel);
    const QPointer<seismic::Seismic3DViewPanel> panel3dGuard(panel3d);

    // 体就绪后的统一换装（两段共用；只装一次）
    const auto finishVolumeSetup = [panel3dGuard, sliderTime, spinTime](const std::shared_ptr<seismic::SgyVolume> &vol) {
      if (!panel3dGuard || !vol)
        return;
      if (panel3dGuard->viewport())
      {
        panel3dGuard->viewport()->setPresetView(seismic::SeismicCameraController::PresetView::Isometric);
        panel3dGuard->viewport()->fitToBounds();
      }

      sliderTime->blockSignals(true);
      spinTime->blockSignals(true);
      sliderTime->setRange(0, vol->SampleMax());
      spinTime->setRange(0, vol->SampleMax());
      const int mid = vol->SampleMax() / 2;
      sliderTime->setValue(mid);
      spinTime->setValue(mid);
      sliderTime->blockSignals(false);
      spinTime->blockSignals(false);
    };

    const auto installVolume = [sharedVol, sharedPaged, panel3dGuard, finishVolumeSetup](
                                   const std::shared_ptr<seismic::SgyVolume> &vol) {
      if (*sharedVol != nullptr || !vol)
        return;
      *sharedVol = vol;
      // paged 通道先于 volume：初始三槽切片请求即带 LOD 映射（从最粗层起步）
      if (!sharedPaged->isEmpty())
        panel3dGuard->setPagedWorkspace(*sharedPaged);
      panel3dGuard->setVolume(vol);
      finishVolumeSetup(vol);
    };

    const auto ensureVolumeLoaded = [this, abs, sharedVol, panel3dGuard, quickInfoGuard, sectionPanelGuard, installVolume]() {
      if (*sharedVol != nullptr || abs.isEmpty() || !QFile::exists(abs))
        return;
      auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
      if (panel3dGuard && svc)
        panel3dGuard->setTaskService(svc);

      if (!svc)
      {
        // 无任务服务（小夹具测试环境）：保留同步加载路径
        auto vol = std::make_shared<seismic::SgyVolume>();
        std::string volErr;
        if (vol->Load(abs.toStdString(), volErr))
          installVolume(vol);
        return;
      }

      // 第一段：QuickOpen 秒级首屏（网格/角点/中央测线真振幅缩略）
      svc->startQuickOpen(abs, 128, [quickInfoGuard, sectionPanelGuard](bool ok, const seismic::SeismicQuickPreview &p) {
        if (!quickInfoGuard)
          return;
        if (!ok)
        {
          quickInfoGuard->setText(QObject::tr("秒开失败：%1").arg(p.error));
          quickInfoGuard->setVisible(true);
          return;
        }
        QString text;
        if (p.ruleVerified)
        {
          text = QObject::tr("秒开 %1 ms · IL %2–%3 · XL %4–%5 · %6 道 × %7 样点 · 预览 IL %8")
                     .arg(p.totalMs, 0, 'f', 0)
                     .arg(p.inlineMin).arg(p.inlineMax)
                     .arg(p.xlineMin).arg(p.xlineMax)
                     .arg(p.traceCount).arg(p.sampleCount)
                     .arg(p.previewInline);
        }
        else
        {
          text = QObject::tr("秒开 %1 ms · %2").arg(p.totalMs, 0, 'f', 0).arg(p.summary);
        }
        quickInfoGuard->setText(text);
        quickInfoGuard->setVisible(true);

        // 秒级缩略：真振幅中央测线先上 2D 剖面（测线解码结果到达后自然替换）
        if (p.ruleVerified && p.preview && p.preview->width > 0 && sectionPanelGuard)
        {
          auto *sectionPanel = static_cast<SectionPanel *>(sectionPanelGuard.data());
          if (!sectionPanel->hasImage())
          {
            QVector<SegyTrace> traces;
            traces.reserve(p.preview->width);
            for (int col = 0; col < p.preview->width; ++col)
            {
              SegyTrace t;
              t.lineNo = p.previewInline;
              t.xlineNo = p.xlineMin + col;
              t.cdp = t.xlineNo;
              t.sampleIntervalUs = static_cast<float>(p.sampleIntervalUs);
              t.samples.reserve(p.preview->height);
              for (int s = 0; s < p.preview->height; ++s) // row 0 = 最深采样
                t.samples.push_back(p.preview->values[
                    static_cast<std::size_t>(p.preview->height - 1 - s) * p.preview->width + col]);
              traces.push_back(t);
            }
            sectionPanel->setTraces(traces, static_cast<float>(p.sampleIntervalUs), 0.0);
          }
        }
      });

      // 第二段：后台体加载（.sgyidx 命中时秒级），完成换装 3D/时间片面板
      svc->startVolumeLoad(abs, [quickInfoGuard, installVolume](
                                    bool ok, std::shared_ptr<seismic::SgyVolume> vol, const QString &err) {
        if (ok)
        {
          installVolume(vol);
          return;
        }
        if (quickInfoGuard && !err.isEmpty())
        {
          quickInfoGuard->setText(QObject::tr("体加载失败：%1").arg(err));
          quickInfoGuard->setVisible(true);
        }
      });
    };
    // 工区打开即启动两段式（不等页签切换）；页签切换回调只做幂等兜底与聚焦
    ensureVolumeLoaded();

    // 时间片走任务服务：sdk::Dataset 在已转码时命中工作区随机访问后端；
    // 未转码走 Direct（同一份 SgyVolume 实现）。防抖 120ms，仅贴最新请求。
    // 取舍（主线2）：显式 .sf3p 存在时走瓦片渐进（引擎焦点优先，冷缓存
    // 首见先出中心瓦片再补边角）；直读/热缓存一次性整图更省——两路并存。
    auto *sliceDebounce = new QTimer(host);
    sliceDebounce->setSingleShot(true);
    sliceDebounce->setInterval(120);
    const auto pendingIdx = std::make_shared<int>(-1);

    // 瓦片信号路由（每服务接一次）：只贴最新一次瓦片请求的目标画布，
    // 采样号世代不符（陈旧请求/其他资产标签）直接丢弃。
    if (auto *tileSvc = (m_doc ? m_doc->seismicTaskService() : nullptr);
        tileSvc && m_tiledSignalService != tileSvc)
    {
      m_tiledSignalService = tileSvc;
      connect(tileSvc, &seismic::SeismicTaskService::timeSliceTileReady, this,
              [this](const seismic::SeismicTimeTile &tile) {
                if (!m_tiledCanvas || tile.sampleIndex != m_tiledSample || !tile.image)
                  return;
                auto *canvas = qobject_cast<seismic::SeismicSectionCanvas *>(m_tiledCanvas.data());
                if (canvas)
                  canvas->appendTimeSliceTile(*tile.image, tile.x, tile.y);
              });
    }

    const auto requestTimeSlice = [this, sharedVol, sharedPaged, timeCanvas, lblTimeMs, pendingIdx](int sampleIndex) {
      if (!*sharedVol || !(*sharedVol)->IsLoaded())
        return;
      const auto &vol = *sharedVol;
      const double ms = sampleIndex * (vol->SampleIntervalUs() / 1000.0);
      lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));

      auto *svc = (m_doc ? m_doc->seismicTaskService() : nullptr);
      const QPointer<seismic::SeismicSectionCanvas> canvasGuard(timeCanvas);
      if (svc && !sharedPaged->isEmpty())
      {
        // paged 通道：瓦片渐进（焦点=网格中心；tileSize 64 与引擎页几何匹配）。
        // 焦点取轴上真值（InlineValues/XlineValues 中位）——step≠1 的轴上
        // InlineMin()+count/2 不保证存在，引擎会退回默认中心。
        const auto &inlVals = vol->InlineValues();
        const auto &xlVals = vol->XlineValues();
        const int inlCount = qMax(1, static_cast<int>(inlVals.size()));
        const int xlCount = qMax(1, static_cast<int>(xlVals.size()));
        const int focusInl = inlVals[static_cast<std::size_t>(inlCount / 2)];
        const int focusXl = xlVals[static_cast<std::size_t>(xlCount / 2)];
        timeCanvas->beginTimeSliceTiled(xlCount, inlCount, ms,
                                        vol->InlineMin(), vol->InlineMax(),
                                        vol->XlineMin(), vol->XlineMax());
        m_tiledCanvas = timeCanvas;
        m_tiledSample = sampleIndex;
        // A3（wave/deepen-perf）：失败如实显示原因态（不再留整幅 NaN 灰无解释）；
        // 被新请求顶替的取消回调经世代过滤（*pendingIdx 已是最新采样号）丢弃。
        svc->startTimeSliceTiled(
            *sharedPaged, sampleIndex, 64, focusInl, focusXl,
            [canvasGuard, pendingIdx, sampleIndex](bool ok,
                                                   std::shared_ptr<const seismic::SgySliceImage> img,
                                                   const QString &error) {
              if (!canvasGuard || *pendingIdx != sampleIndex)
                return; // 陈旧请求（已被顶替/换采样）——静默丢弃
              if (!ok || !img) {
                canvasGuard->clearData();
                canvasGuard->setNoDataReason(
                    QObject::tr("时间切片获取失败（分页通道）\n%1").arg(error));
                return;
              }
              canvasGuard->finishTimeSliceTiled(*img);
            });
        return;
      }
      if (svc)
      {
        // A3：直读/工作区通道同一空态语义（失败原因上屏，不留旧图冒充新采样）
        svc->startSliceExtraction(
            vol, seismic::SgySliceType::Time, sampleIndex,
            [canvasGuard, pendingIdx, sampleIndex, ms, vol](
                bool ok, std::shared_ptr<const seismic::SgySliceImage> img, const QString &error) {
              if (!canvasGuard || *pendingIdx != sampleIndex)
                return;
              if (!ok || !img) {
                canvasGuard->clearData();
                canvasGuard->setNoDataReason(
                    QObject::tr("时间切片获取失败\n%1").arg(error));
                return;
              }
              canvasGuard->setTimeSliceData(*img, ms, vol->InlineMin(), vol->InlineMax(),
                                            vol->XlineMin(), vol->XlineMax());
            });
        return;
      }
      seismic::SgySliceImage img;
      std::string err;
      if (vol->ExtractSlice(seismic::SgySliceType::Time, sampleIndex, img, err))
      {
        timeCanvas->setTimeSliceData(img, ms, vol->InlineMin(), vol->InlineMax(), vol->XlineMin(), vol->XlineMax());
      }
    };
    connect(sliceDebounce, &QTimer::timeout, host, [pendingIdx, requestTimeSlice]() {
      if (*pendingIdx >= 0)
        requestTimeSlice(*pendingIdx);
    });
    const auto updateTimeSlice = [pendingIdx, sliceDebounce](int sampleIndex) {
      *pendingIdx = sampleIndex;
      sliceDebounce->start();
    };

    connect(sliderTime, &QSlider::valueChanged, host, [spinTime, updateTimeSlice](int val) {
      spinTime->blockSignals(true);
      spinTime->setValue(val);
      spinTime->blockSignals(false);
      updateTimeSlice(val);
    });

    connect(spinTime, QOverload<int>::of(&QSpinBox::valueChanged), host, [sliderTime, updateTimeSlice](int val) {
      sliderTime->blockSignals(true);
      sliderTime->setValue(val);
      sliderTime->blockSignals(false);
      updateTimeSlice(val);
    });

    connect(modeTabs, &QTabWidget::currentChanged, host, [ensureVolumeLoaded, panel3d, timeCanvas, updateTimeSlice, sliderTime](int idx) {
      if (idx == 1)
      {
        ensureVolumeLoaded();
        if (panel3d->viewport())
        {
          panel3d->viewport()->fitToBounds();
          panel3d->viewport()->update();
        }
      }
      else if (idx == 2)
      {
        ensureVolumeLoaded();
        updateTimeSlice(sliderTime->value());
        QTimer::singleShot(20, timeCanvas, [timeCanvas]() {
          timeCanvas->fitToWindow();
        });
      }
    });

    lay->addWidget(quickInfo);
    lay->addWidget(modeTabs, 1);
    return host;
  }

  if (asset.type == QLatin1String("image_reference"))
  {
    // D2.7：有 world file/配准边车 → 栅格上图；未配准 → 图片查看器 + 引导。
    // 托管副本身旁没有边车、源目录有 → 成对搬进临时目录再上图（GDAL 只认
    // 数据文件旁的边车）。
    const auto georefPair =
        PreviewMapStates::stageGeorefPairIfNeeded(abs, v.sourceUri, host);
    const QString worldFile = georefPair.second;
    if (!worldFile.isEmpty())
    {
      auto raster = std::make_unique<QgsRasterLayer>(georefPair.first, asset.displayName,
                                                     QStringLiteral("gdal"));
      if (raster->isValid() && !raster->extent().isEmpty())
      {
        auto *page = new PreviewMapPage(host);
        page->setObjectName(QStringLiteral("imagePreviewPage"));
        page->setAssetKey(assetId);
        page->setRenderCacheIdentity(assetId, v.id);
        page->mapCanvas()->canvas()->setObjectName(QStringLiteral("imageMapCanvas"));
        page->decorations()->setObjectName(QStringLiteral("imageDecorManager"));
        page->setProfileEnabled(false); // 影像非连续值面——剖面采样无意义
        QgsRasterLayer *rasterRaw = raster.release();
        rasterRaw->setParent(host);
        page->addMapLayer(rasterRaw, asset.displayName, abs);
        // D2.11 大图（>50MB 无金字塔）提示：降级仍可用。
        const QString bigHint = PreviewRasterAnalysis::bigRasterHint(rasterRaw);
        if (!bigHint.isEmpty())
        {
          lay->addWidget(PreviewMapStates::buildBigRasterHintBar(bigHint, host));
          // B3：消费侧预热（同层位栅格页口径——.ovr 完成后重载层刷新）。
          if (m_doc)
          {
            QPointer<QgsRasterLayer> rasterGuard(rasterRaw);
            connect(m_doc, &PreviewDocService::rasterPyramidFinished, host,
                    [rasterGuard, assetId](const QString &doneId, bool ok) {
                      if (doneId != assetId || !ok || !rasterGuard)
                        return;
                      rasterGuard->reload();
                      rasterGuard->triggerRepaint();
                    });
            m_doc->ensureRasterPyramidVersion(assetId);
          }
        }
        lay->addWidget(page, 1);
        lay->addWidget(caption8(tr("已按配准边车 %1 上图（RGB 影像原色）")
                                    .arg(QFileInfo(worldFile).fileName()),
                                host));
        QTimer::singleShot(0, host, [page, rasterRaw]() {
          page->mapCanvas()->zoomToLayer(rasterRaw);
          page->primeRenderCache();
          if (!page->mapCanvas()->overlayVisible())
            page->showLowResSnapshot();
        });
        return host;
      }
    }
    // 未配准 → 图片查看器（原行为）+ 「去配准」引导入口（D2.7）。
    auto *scroll = new QScrollArea(host);
    scroll->setWidgetResizable(true);
    auto *imgLabel = new QLabel(scroll);
    QPixmap pm(abs);
    if (pm.isNull())
    {
      lay->addWidget(failureState(assetId, tr("无法解析图片"), host), 1);
      return host;
    }
    imgLabel->setPixmap(pm.scaledToWidth(560, Qt::SmoothTransformation)); // 按面板宽缩放
    scroll->setWidget(imgLabel);
    lay->addWidget(scroll, 1);
    auto *regGuideBtn = new QPushButton(tr("去配准…"), host);
    regGuideBtn->setObjectName(QStringLiteral("goRegisterGuideBtn"));
    regGuideBtn->setToolTip(tr("把这张平面相图配准到工程测网"));
    connect(regGuideBtn, &QPushButton::clicked, host, [this, host]() {
      QMessageBox::information(
          host, tr("去配准"),
          tr("配准两条路：\n"
             "· 在图片旁放同名 world file（.wld/.pgw/.jgw，六参数文本）——"
             "重新打开预览即按栅格上图；\n"
             "· 或把相图界线转为 GeoJSON，用 D11「临时配准（手工仿射）」"
             "登记为 DERIVED 版本。"));
    });
    lay->addWidget(regGuideBtn, 0, Qt::AlignLeft);
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
    return host;
  }

  if (asset.type == QLatin1String("document"))
  {
    // 文件名/类型等属性信息由右侧属性面板承担，预览页不再重复占空间；
    // 「用系统程序打开」只在无内嵌预览（转换失败/无门面）或需要打开
    // office 原件时作兜底出口。
    QString pdfAbs;
    if (asset.format == QLatin1String("pdf"))
      pdfAbs = abs;
    else if (m_doc)
    {
      m_doc->ensureDocumentPdf(assetId);
      switch (m_doc->documentPdfState(assetId))
      {
        case PreviewDocService::DocPdfState::Ready:
          pdfAbs = m_doc->documentPdfPath(assetId);
          break;
        case PreviewDocService::DocPdfState::Failed:
          lay->addWidget(
              stateLabel(tr("无 PDF 预览：%1").arg(m_doc->documentPdfError(assetId)),
                         host),
              1);
          lay->addWidget(makeOpenExternalRow(abs, host));
          break;
        default: // Pending（None 不可达——ensure 刚入队或已记失败）
          lay->addWidget(stateLabel(tr("正在转换为 PDF 预览…"), host), 1);
          break;
      }
    }
    else
    {
      lay->addWidget(stateLabel(tr("无法生成 PDF 预览"), host), 1);
      lay->addWidget(makeOpenExternalRow(abs, host));
    }

    if (!pdfAbs.isEmpty())
    {
      auto *doc = new QPdfDocument(host);
      if (doc->load(pdfAbs) == QPdfDocument::Error::None)
      {
        auto *view = new QPdfView(host);
        view->setObjectName(QStringLiteral("pdfView"));
        view->setDocument(doc);
        view->setPageMode(QPdfView::PageMode::MultiPage);
        lay->addWidget(view, 1);
        if (asset.format != QLatin1String("pdf"))
        {
          lay->addWidget(
              caption8(tr("预览为 PDF 转换件；原件经「用系统程序打开」"), host));
          lay->addWidget(makeOpenExternalRow(abs, host), 0, Qt::AlignLeft);
        }
      }
      else
      {
        lay->addWidget(
            stateLabel(tr("PDF 转换件无法加载\n%1").arg(pdfAbs), host), 1);
        lay->addWidget(makeOpenExternalRow(abs, host));
      }
    }
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
    return host;
  }

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") && asset.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
  {
    QJsonDocument doc;
    QString gerr;
    if (!m_doc->geoJsonDocumentAt(abs, &doc, &gerr))
    {
      lay->addWidget(failureState(assetId, gerr, host), 1);
      return host;
    }
    const QJsonArray features = doc.object().value(QStringLiteral("features")).toArray();
    QStringList propKeys;
    for (const QJsonValue &fv : features)
    {
      const QJsonObject props = fv.toObject()
                                    .value(QStringLiteral("properties"))
                                    .toObject();
      for (auto it = props.begin(); it != props.end(); ++it)
        if (!propKeys.contains(it.key()))
          propKeys.append(it.key());
    }

    // 寻找沉积相分类字段候选 (相、亚相、微相、facies 等)
    QStringList faciesCandidates;
    for (const QString &k : propKeys)
    {
      if (k == QLatin1String("相") || k == QLatin1String("微相") || k == QLatin1String("亚相") ||
          k.contains(QStringLiteral("相")) ||
          k.compare(QLatin1String("facies"), Qt::CaseInsensitive) == 0 ||
          k.compare(QLatin1String("sub_facies"), Qt::CaseInsensitive) == 0 ||
          k.compare(QLatin1String("micro_facies"), Qt::CaseInsensitive) == 0)
      {
        faciesCandidates.append(k);
      }
    }
    if (faciesCandidates.isEmpty())
    {
      for (const QString &k : propKeys)
      {
        if (k.compare(QLatin1String("name"), Qt::CaseInsensitive) == 0 ||
            k.compare(QLatin1String("type"), Qt::CaseInsensitive) == 0 ||
            k.compare(QLatin1String("zone"), Qt::CaseInsensitive) == 0)
        {
          faciesCandidates.append(k);
        }
      }
    }
    if (faciesCandidates.isEmpty() && !propKeys.isEmpty())
      faciesCandidates.append(propKeys.first());

    QString activeFaciesField;
    if (faciesCandidates.contains(QStringLiteral("相")))
      activeFaciesField = QStringLiteral("相");
    else if (!faciesCandidates.isEmpty())
      activeFaciesField = faciesCandidates.first();

    // 矢量层（私有，不进 QgsProject）
    auto *vlayer = new QgsVectorLayer(abs, asset.displayName, QStringLiteral("ogr"));
    vlayer->setParent(host);

    // 顶部操作与空间提示工具栏
    auto *topBar = new QWidget(host);
    auto *topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(8, 4, 8, 4);
    topLay->setSpacing(6);
    stylePreviewToolBar(topBar);

    // 视图切换器: 相图地图 / 属性列表
    auto *btnViewMap = new QToolButton(topBar);
    btnViewMap->setObjectName(QStringLiteral("btnViewFaciesMap"));
    btnViewMap->setText(tr("相图地图"));
    btnViewMap->setCheckable(true);
    btnViewMap->setChecked(true);
    btnViewMap->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
    btnViewMap->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnViewMap);

    auto *btnViewTable = new QToolButton(topBar);
    btnViewTable->setObjectName(QStringLiteral("btnViewFaciesTable"));
    btnViewTable->setText(tr("属性列表"));
    btnViewTable->setCheckable(true);
    btnViewTable->setChecked(false);
    btnViewTable->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
    btnViewTable->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnViewTable);

    auto *viewGroup = new QButtonGroup(topBar);
    viewGroup->addButton(btnViewMap);
    viewGroup->addButton(btnViewTable);

    topLay->addSpacing(6);

    // D2.6 名称标注开关
    auto *btnLabels = new QToolButton(topBar);
    btnLabels->setObjectName(QStringLiteral("btnToggleLabels"));
    btnLabels->setText(tr("名称标注"));
    btnLabels->setToolTip(
        vlayer && vlayer->geometryType() == Qgis::GeometryType::Point
            ? tr("显示或隐藏文字标注。点标记保持统一；有沉积相字段时标出相名")
            : tr("显示/隐藏要素名称标注"));
    btnLabels->setCheckable(true);
    btnLabels->setChecked(true);
    btnLabels->setToolButtonStyle(Qt::ToolButtonTextOnly);
    topLay->addWidget(btnLabels);

    // 地图浏览工具
    auto *btnFull = new QToolButton(topBar);
    btnFull->setObjectName(QStringLiteral("btnFaciesFullExtent"));
    btnFull->setText(tr("全图"));
    btnFull->setToolTip(tr("缩放到相图完整范围"));
    btnFull->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));
    btnFull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnFull);

    auto *btnIn = new QToolButton(topBar);
    btnIn->setObjectName(QStringLiteral("btnFaciesZoomIn"));
    btnIn->setText(tr("放大"));
    btnIn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomIn.svg")));
    btnIn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnIn);

    auto *btnOut = new QToolButton(topBar);
    btnOut->setObjectName(QStringLiteral("btnFaciesZoomOut"));
    btnOut->setText(tr("缩小"));
    btnOut->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomOut.svg")));
    btnOut->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnOut);

    auto *btnPan = new QToolButton(topBar);
    btnPan->setObjectName(QStringLiteral("btnFaciesPan"));
    btnPan->setText(tr("漫游"));
    btnPan->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionPan.svg")));
    btnPan->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnPan);

    // 字段选择下拉框（若有多个相分类字段）
    QComboBox *fieldCombo = nullptr;
    QLabel *fieldLbl = nullptr;
    if (faciesCandidates.size() > 1)
    {
      fieldLbl = caption8(tr("渲染字段:"), topBar);
      topLay->addWidget(fieldLbl);

      fieldCombo = new QComboBox(topBar);
      fieldCombo->setObjectName(QStringLiteral("faciesFieldCombo"));
      fieldCombo->addItems(faciesCandidates);
      if (!activeFaciesField.isEmpty())
        fieldCombo->setCurrentText(activeFaciesField);
      PaleoTheme::applyThemedStyleSheet(fieldCombo, [] {
        const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
        return QStringLiteral(
            "QComboBox { background: %1; border: 1px solid %2; border-radius: 4px;"
            " padding: 2px 6px; font-size: 8pt; color: %3; }"
            "QComboBox:hover { border-color: %4; }")
            .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
                 qssHex(t.textDisabled));
      });
      topLay->addWidget(fieldCombo);
    }

    topLay->addSpacing(8);

    // D11 临时配准入口
    auto *regBtn = new QPushButton(tr("临时配准（手工仿射）…"), host);
    regBtn->setObjectName(QStringLiteral("provisionalRegisterButton"));
    regBtn->setAccessibleName(tr("临时配准"));
    regBtn->setToolTip(tr("手工输入仿射参数，把 GeoJSON 变换到工程局部测网"));
    PaleoTheme::applyThemedStyleSheet(regBtn, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return QStringLiteral(
          "QPushButton { background: %1; border: 1px solid %2; border-radius: 4px;"
          " padding: 4px 8px; font-size: 8pt; color: %3; }"
          "QPushButton:hover { background: %4; border-color: %5; }")
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.text),
               qssHex(t.surfaceAltRaised), qssHex(t.textDisabled));
    });
    topLay->addWidget(regBtn);

    topLay->addStretch(1);

    // 经纬度与工程测网不同空间：阻断级提示用 error token（原 #D32F2F + 11px）。
    auto *warnLbl = new QLabel(tr("经纬度，与本测网不是同一空间"), host);
    warnLbl->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(warnLbl, [] {
      return QStringLiteral("color: %1; font-size: 8pt; font-weight: 500;")
          .arg(qssHex(PaleoTheme::tokens().error));
    });
    topLay->addWidget(warnLbl);
    lay->addWidget(new PaleoToolRow(topBar, host));

    // 「读不出坐标范围」错误就地可见（原实现创建了警告标签却没加进任何布局）。
    auto *boundsErr = warnLabel(QString(), host);
    boundsErr->setObjectName(QStringLiteral("affineBoundsError"));
    boundsErr->setVisible(false);
    lay->addWidget(boundsErr);

    connect(regBtn, &QPushButton::clicked, this, [this, assetId, abs, boundsErr]() {
      double srcB[4];
      QString berr;
      if (!m_doc->geoJsonBounds(abs, srcB, &berr))
      {
        boundsErr->setText(tr("读不出坐标范围：%1").arg(berr));
        boundsErr->setVisible(true);
        return;
      }
      boundsErr->setVisible(false);

      QDialog dlg(this);
      dlg.setWindowTitle(tr("临时配准（手工仿射）"));
      auto *form = new QFormLayout(&dlg);
      auto *srcLbl = new QLabel(
          tr("源坐标范围：X %1–%2 · Y %3–%4")
              .arg(QString::number(srcB[0], 'f', 2), QString::number(srcB[2], 'f', 2),
                   QString::number(srcB[1], 'f', 2), QString::number(srcB[3], 'f', 2)),
          &dlg);
      srcLbl->setWordWrap(true);
      form->addRow(srcLbl);
      auto *gridHint = new QLabel(
          tr("目标：工程局部测网（约 X 0–12800 · Y 0–16400，单位米）"), &dlg);
      gridHint->setWordWrap(true);
      PaleoTheme::applyThemedStyleSheet(gridHint,
                                        [] { return PaleoTheme::mutedCaptionStyleSheet(); });
      form->addRow(gridHint);

      auto *tx = new QDoubleSpinBox(&dlg);
      auto *ty = new QDoubleSpinBox(&dlg);
      auto *sx = new QDoubleSpinBox(&dlg);
      auto *sy = new QDoubleSpinBox(&dlg);
      auto *rot = new QDoubleSpinBox(&dlg);
      for (auto *s : {tx, ty})
      {
        s->setRange(-1e9, 1e9);
        s->setDecimals(2);
        s->setSingleStep(1000.0);
      }
      for (auto *s : {sx, sy})
      {
        s->setRange(1e-6, 1e6);
        s->setDecimals(6);
        s->setValue(1.0);
      }
      rot->setRange(-360.0, 360.0);
      rot->setDecimals(2);
      form->addRow(tr("平移 X（米）"), tx);
      form->addRow(tr("平移 Y（米）"), ty);
      form->addRow(tr("缩放 X"), sx);
      form->addRow(tr("缩放 Y"), sy);
      form->addRow(tr("旋转（度）"), rot);

      auto *dstLbl = new QLabel(&dlg);
      dstLbl->setObjectName(QStringLiteral("affineDstBounds"));
      dstLbl->setWordWrap(true);
      PaleoTheme::applyThemedStyleSheet(dstLbl,
                                        [] { return PaleoTheme::mutedCaptionStyleSheet(); });
      form->addRow(dstLbl);
      const auto refreshDst = [this, srcB, tx, ty, sx, sy, rot, dstLbl]() {
        const QVariantMap p{{QStringLiteral("tx"), tx->value()},
                            {QStringLiteral("ty"), ty->value()},
                            {QStringLiteral("sx"), sx->value()},
                            {QStringLiteral("sy"), sy->value()},
                            {QStringLiteral("rotDeg"), rot->value()}};
        double lo[2], hi[2];
        m_doc->affinePreviewBounds(srcB, p, lo, hi);
        dstLbl->setText(tr("变换后范围：X %1–%2 · Y %3–%4")
                            .arg(QString::number(lo[0], 'f', 1),
                                 QString::number(hi[0], 'f', 1),
                                 QString::number(lo[1], 'f', 1),
                                 QString::number(hi[1], 'f', 1)));
      };
      for (auto *s : {tx, ty, sx, sy, rot})
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), dstLbl, refreshDst);
      refreshDst();

      auto *buttons = new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
      buttons->button(QDialogButtonBox::Ok)->setText(tr("登记为临时配准"));
      form->addRow(buttons);
      connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
      if (dlg.exec() != QDialog::Accepted)
        return;

      emit provisionalRegistrationRequested(
          assetId, {{QStringLiteral("tx"), tx->value()},
                    {QStringLiteral("ty"), ty->value()},
                    {QStringLiteral("sx"), sx->value()},
                    {QStringLiteral("sy"), sy->value()},
                    {QStringLiteral("rotDeg"), rot->value()}});
    });

    auto *viewStack = new QStackedWidget(host);
    viewStack->setObjectName(QStringLiteral("faciesViewStack"));

    // 1. P2 统一预览地图页（D1.x 工具/TOC/identify/图例全套）
    auto *page = new PreviewMapPage(viewStack);
    page->setObjectName(QStringLiteral("faciesPreviewPage"));
    page->setAssetKey(assetId);
    page->mapCanvas()->canvas()->setObjectName(QStringLiteral("faciesMapCanvas"));
    page->decorations()->setObjectName(QStringLiteral("faciesDecorManager"));
    page->setProfileEnabled(false); // D1.3：矢量内容不开剖面工具

    if (vlayer && vlayer->isValid())
    {
      applyFaciesRendererToLayer(vlayer, activeFaciesField);
      // 经纬度 GeoJSON：画布跟随层 CRS（旧语义），局部网格层保持工程网格。
      page->mapCanvas()->setOverrideCrs(vlayer->crs());
      // D2.4 图例侧栏：从分类渲染器读回 category 色板。
      const auto syncLegend = [page, vlayer]() {
        QVector<PreviewTocPanel::LegendEntry> entries;
        if (auto *r = dynamic_cast<QgsCategorizedSymbolRenderer *>(vlayer->renderer()))
          for (const QgsRendererCategory &c : r->categories())
          {
            if (!c.symbol())
              continue;
            PreviewTocPanel::LegendEntry e;
            e.name = c.label();
            e.color = c.symbol()->color();
            entries.append(e);
          }
        page->tocPanel()->setLegendEntries(entries);
      };
      syncLegend();
      // D4.4 分类字段快调回调（TOC 面板收集参数，相色逻辑在视图层）。
      page->tocPanel()->vectorStyleApplier =
          [vlayer, syncLegend](QgsVectorLayer *, const QString &field, bool categorized,
                               const QColor &) {
            if (categorized && !field.isEmpty())
            {
              applyFaciesRendererToLayer(vlayer, field);
              syncLegend();
            }
          };
      page->addMapLayer(vlayer, asset.displayName, abs);

      // D2.6 名称标注开关
      connect(btnLabels, &QToolButton::toggled, page, [page, vlayer](bool on) {
        vlayer->setLabelsEnabled(on);
        page->mapCanvas()->canvas()->refresh();
      });
      connect(btnFull, &QToolButton::clicked, page, [page, vlayer]() {
        if (vlayer && !vlayer->extent().isEmpty())
          page->mapCanvas()->zoomToLayer(vlayer);
        else
          page->mapCanvas()->zoomToFullExtent();
      });
      connect(btnIn, &QToolButton::clicked, page,
              [page]() { page->mapCanvas()->canvas()->zoomIn(); });
      connect(btnOut, &QToolButton::clicked, page,
              [page]() { page->mapCanvas()->canvas()->zoomOut(); });
      connect(btnPan, &QToolButton::clicked, page, [page]() {
        page->toolManager()->activate(PreviewMapToolManager::kPan);
      });

      if (fieldCombo)
      {
        connect(fieldCombo, &QComboBox::currentTextChanged, page,
                [vlayer, syncLegend, page](const QString &fld) {
                  applyFaciesRendererToLayer(vlayer, fld);
                  syncLegend();
                  page->mapCanvas()->canvas()->refresh();
                });
      }

      // D2.10 同目录叠加
      {
        const auto siblings = PreviewMapStates::siblingMappableAssets(
            cat, abs, [this](const CatalogVersion &v) { return m_doc->absolutePathForVersion(v); });
        QVector<QPair<QString, QString>> others;
        for (const auto &sib : siblings)
          if (sib.first != assetId)
            others.append(sib);
        if (!others.isEmpty())
        {
          auto *overlayBtn = new QToolButton(page);
          overlayBtn->setObjectName(QStringLiteral("siblingOverlayButton"));
          overlayBtn->setText(tr("同目录叠加"));
          overlayBtn->setToolTip(tr("把同目录下的相图/配准图片/层位栅格叠加到本预览"));
          overlayBtn->setPopupMode(QToolButton::InstantPopup);
          auto *menu = new QMenu(overlayBtn);
          for (const auto &sib : others)
          {
            QAction *act = menu->addAction(sib.second);
            QObject::connect(act, &QAction::triggered, host, [this, page, sib, host]() {
              addSiblingOverlayLayer(page, sib.first, sib.second, host);
            });
          }
          overlayBtn->setMenu(menu);
          page->addToolBarWidget(overlayBtn);
        }
      }

      QTimer::singleShot(100, page, [page, vlayer]() {
        if (vlayer && !vlayer->extent().isEmpty())
          page->mapCanvas()->zoomToLayer(vlayer);
        else
          page->mapCanvas()->zoomToFullExtent();
      });
    }
    else
    {
      btnViewMap->setEnabled(false);
      btnViewTable->setChecked(true);
    }

    viewStack->addWidget(page);

    // 2. 要素属性表格预览
    auto *table = new QTableWidget(viewStack);
    table->setObjectName(QStringLiteral("geoJsonFeatureTable"));
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    PaleoTheme::applyThemedStyleSheet(table, [] {
      const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
      return QStringLiteral(
          "QTableWidget { background-color: %1; gridline-color: %2; border: 1px solid %2;"
          " font-size: 9pt; }"
          "QHeaderView::section { background-color: %3; color: %4; border: none;"
          " border-bottom: 1px solid %2; border-right: 1px solid %2; padding: 4px 8px;"
          " font-weight: 500; font-size: 8pt; }")
          .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.surfaceAlt),
               qssHex(t.textMuted));
    });

    QStringList headers;
    headers << tr("序号") << tr("几何类型");
    headers.append(propKeys);
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);

    const int maxRows = qMin(features.size(), 1000);
    table->setRowCount(maxRows);
    for (int r = 0; r < maxRows; ++r)
    {
      const QJsonObject feat = features.at(r).toObject();
      const QString geomType = feat.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("type")).toString();
      const QJsonObject props = feat.value(QStringLiteral("properties")).toObject();

      auto *idItem = new QTableWidgetItem(QString::number(r + 1));
      idItem->setTextAlignment(Qt::AlignCenter);
      idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
      table->setItem(r, 0, idItem);

      auto *geomItem = new QTableWidgetItem(geomType.isEmpty() ? QStringLiteral("—") : geomType);
      geomItem->setTextAlignment(Qt::AlignCenter);
      geomItem->setFlags(geomItem->flags() & ~Qt::ItemIsEditable);
      table->setItem(r, 1, geomItem);

      for (int c = 0; c < propKeys.size(); ++c)
      {
        const QString &key = propKeys.at(c);
        const QJsonValue val = props.value(key);
        QString valStr;
        if (val.isDouble())
          valStr = QString::number(val.toDouble());
        else if (val.isString())
          valStr = val.toString();
        else if (val.isBool())
          valStr = val.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (val.isNull())
          valStr = QStringLiteral("null");
        else
          valStr = QString::fromUtf8(QJsonDocument(val.toArray()).toJson(QJsonDocument::Compact));

        auto *valItem = new QTableWidgetItem(valStr);
        valItem->setFlags(valItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(r, c + 2, valItem);
      }
    }
    table->horizontalHeader()->setStretchLastSection(true);
    table->resizeColumnsToContents();

    // 表格放进容器页：超 1000 条截断时表尾如实注明（原实现静默截断）。
    auto *tablePage = new QWidget(viewStack);
    auto *tablePageLay = new QVBoxLayout(tablePage);
    tablePageLay->setContentsMargins(0, 0, 0, 0);
    tablePageLay->setSpacing(4);
    tablePageLay->addWidget(table, 1);
    if (features.size() > maxRows)
      tablePageLay->addWidget(
          caption8(tr("已截断，仅显示前 %1 条（共 %2 条）")
                       .arg(maxRows)
                       .arg(features.size()),
                   tablePage));
    viewStack->addWidget(tablePage);

    const auto updateViewMode = [viewStack, btnFull, btnIn, btnOut, btnPan, btnLabels, fieldLbl, fieldCombo](int idx) {
      viewStack->setCurrentIndex(idx);
      const bool isMap = (idx == 0);
      btnFull->setVisible(isMap);
      btnIn->setVisible(isMap);
      btnOut->setVisible(isMap);
      btnPan->setVisible(isMap);
      btnLabels->setVisible(isMap);
      if (fieldLbl) fieldLbl->setVisible(isMap);
      if (fieldCombo) fieldCombo->setVisible(isMap);
    };

    connect(btnViewMap, &QToolButton::clicked, host, [updateViewMode]() { updateViewMode(0); });
    connect(btnViewTable, &QToolButton::clicked, host, [updateViewMode]() { updateViewMode(1); });

    lay->addWidget(viewStack, 1);
    return host;
  }

  // ---- 辅助/参考与未知类型（§4 阶段 D）：预览内容为主，文件名/类型等属性
  // 信息由右侧属性面板承担（不再重复占空间）；「未配准，不加入地图」警告照旧；
  // HZ28-6-1 XML 额外写「不对应 A1–A20」；无内嵌预览时留「用系统程序打开」
  // 兜底出口。----
  {
    QString auxName;
    for (const EntityAssetLink &l : links)
      if (l.entityType == QLatin1String("auxiliary") && !l.entityId.isEmpty())
      {
        auxName = cat->entityById(l.entityId).name;
        break;
      }
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host));
    // 参考资料/ 下 HZ28-6-1 的 XML：不按内容挂井、不并进 A1–A20（§3 固定规则）。
    if (asset.displayName.contains(QStringLiteral("HZ28-6-1")) ||
        auxName.contains(QStringLiteral("HZ28-6-1")))
      lay->addWidget(warnLabel(tr("不对应 A1–A20"), host));

    if (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
    {
      // F2 两段式（goal/perf-systematize 簇2）：XML 解析进任务池（综合图可
      // 含数 MB 曲线数据，同步解析阻塞 UI 线程），面板骨架 + 「解析中」提示
      // 即时上屏；无任务服务时 loadComprehensiveXmlAsync 同步执行——失败
      // 返回 false 走原有不支持预览 fall-through（测试环境行为不变）。
      auto *compositePanel = new WellComposite::WellCompositePanel(host);
      compositePanel->setObjectName(QStringLiteral("wellCompositePanel"));
      auto *xmlPendingHint = new QLabel(tr("正在后台解析综合柱状图…"), host);
      xmlPendingHint->setObjectName(QStringLiteral("xmlPendingHint"));
      PaleoTheme::applyThemedStyleSheet(xmlPendingHint, [] {
        return PaleoTheme::mutedCaptionStyleSheet();
      });
      const QPointer<QLabel> xmlHintG(xmlPendingHint);
      const QPointer<WellComposite::WellCompositePanel> compG(compositePanel);
      connect(compositePanel, &WellComposite::WellCompositePanel::comprehensiveXmlLoaded,
              host, [this, assetId, host, lay, xmlHintG, compG](bool ok) {
                if (xmlHintG)
                  xmlHintG->hide();
                // 异步失败（截断/坏 XML）：换成「读取失败」面（重试=重建标签）。
                if (!ok && compG)
                {
                  lay->removeWidget(compG);
                  compG->deleteLater();
                  lay->addWidget(failureState(assetId, tr("综合柱状图 XML 无法解析"), host), 1);
                }
              });
      lay->addWidget(xmlPendingHint);
      if (compositePanel->loadComprehensiveXmlAsync(
              abs, m_doc ? m_doc->taskService() : nullptr))
      {
        lay->addWidget(compositePanel, 1);
        return host;
      }
      delete compositePanel;
      delete xmlPendingHint;
    }
    // P2 D2.12 未知类型：统一「不支持预览」态 + 可支持类型清单（不再留白）。
    static const QStringList kKnownTypes = {
        QStringLiteral("well_log"),      QStringLiteral("well_head"),
        QStringLiteral("well_stratification"), QStringLiteral("time_depth"),
        QStringLiteral("horizon"),       QStringLiteral("seismic"),
        QStringLiteral("image_reference"), QStringLiteral("document"),
        QStringLiteral("geojson"),       QStringLiteral("boundary")};
    if (!kKnownTypes.contains(asset.type))
      lay->addWidget(PreviewMapStates::buildUnsupportedPage(asset.type, host), 1);
    else
      lay->addStretch(1);
    lay->addWidget(makeOpenExternalRow(abs, host), 0, Qt::AlignLeft);
    return host;
  }
}

QWidget *DataPreviewTabs::buildWellBody(const CatalogAsset &asset, const QString &absPath,
                                        const QString &wellEntityId,
                                        const QString &wellName, QWidget *parent)
{
  const QString normWell = DataCatalog::normalizeWellName(wellName);
  const auto matchWell = [&normWell](const QString &rowName) {
    return normWell.isEmpty() ||
           DataCatalog::normalizeWellName(rowName) == normWell;
  };

  if (asset.type == QLatin1String("well_stratification"))
  {
    QVector<WellTopRecord> tops;
    QString werr;
    if (!m_doc->wellTopsAt(absPath, &tops, &werr))
      return failureState(asset.id, werr, parent);
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(6);
    // §4：层名、MD、TVD、X、Y；Time 列为空就显示空，不填 -99999，也不填假时间。
    auto *table = new QTableWidget(0, 6, holder);
    table->setObjectName(QStringLiteral("topsTable"));
    table->setHorizontalHeaderLabels(
        {tr("层名"), tr("MD"), tr("TVD"), tr("X"), tr("Y"), tr("Time(ms)")});
    table->verticalHeader()->setVisible(false);
    for (const WellTopRecord &t : tops)
    {
      if (!matchWell(t.wellName))
        continue; // 多井文件按当前井过滤，不拆文件（§3）
      const int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0, new QTableWidgetItem(t.topName));
      auto *md = new QTableWidgetItem(t.hasMd ? QString::number(t.md, 'f', 1) : QString());
      auto *tvd = new QTableWidgetItem(t.hasTvd ? QString::number(t.tvd, 'f', 1) : QString());
      auto *x = new QTableWidgetItem(t.hasX ? QString::number(t.x, 'f', 2) : QString());
      auto *y = new QTableWidgetItem(t.hasY ? QString::number(t.y, 'f', 2) : QString());
      auto *tm = new QTableWidgetItem(t.hasTime ? QString::number(t.timeMs, 'f', 1) : QString());
      for (QTableWidgetItem *it : {md, tvd, x, y, tm})
        setNumericItem(it); // JetBrains Mono 9pt 右对齐（§4/DESIGN.md）
      table->setItem(r, 1, md);
      table->setItem(r, 2, tvd);
      table->setItem(r, 3, x);
      table->setItem(r, 4, y);
      table->setItem(r, 5, tm); // Time 空（-99999）就显示空，不填假时间
    }
    table->horizontalHeader()->setStretchLastSection(true);
    hl->addWidget(caption8(wellName.isEmpty() ? tr("分层表")
                                              : tr("%1 的分层表").arg(wellName),
                           holder));
    hl->addWidget(table, 1);

    // P2 D2.8 井位落图：分层行带坐标（X/Y）时把层位顶点打上图。
    bool anyCoords = false;
    for (const WellTopRecord &t : tops)
      if (matchWell(t.wellName) && t.hasX && t.hasY)
      {
        anyCoords = true;
        break;
      }
    if (anyCoords)
    {
      auto *topsVl = makeMemoryPointLayer(
          wellName.isEmpty() ? tr("分层顶点") : tr("%1 分层顶点").arg(wellName), holder);
      for (const WellTopRecord &t : tops)
        if (matchWell(t.wellName) && t.hasX && t.hasY)
          addMemoryPoint(topsVl, t.x, t.y, t.topName, QStringLiteral("well"));
      stylePointLayer(topsVl, true);
      auto *mapPage = new PreviewMapPage(holder);
      mapPage->setObjectName(QStringLiteral("topsPreviewPage"));
      mapPage->mapCanvas()->canvas()->setObjectName(QStringLiteral("topsMapCanvas"));
      mapPage->setProfileEnabled(false);
      mapPage->addMapLayer(topsVl, tr("分层顶点"), absPath);
      auto *labelRow = new QWidget(holder);
      auto *labelLay = new QHBoxLayout(labelRow);
      labelLay->setContentsMargins(0, 0, 0, 0);
      auto *labelToggle = new QCheckBox(tr("名称标注"), labelRow);
      labelToggle->setObjectName(QStringLiteral("topsLabelToggle"));
      labelToggle->setChecked(true);
      labelLay->addWidget(labelToggle);
      labelLay->addStretch(1);
      QObject::connect(labelToggle, &QCheckBox::toggled, mapPage,
                       [topsVl, mapPage](bool on) {
                         topsVl->setLabelsEnabled(on);
                         mapPage->mapCanvas()->canvas()->refresh();
                       });
      hl->addWidget(labelRow);
      hl->addWidget(mapPage, 1);
      QTimer::singleShot(0, holder, [mapPage, topsVl]() {
        mapPage->mapCanvas()->zoomToLayer(topsVl);
      });
    }
    return holder;
  }

  if (asset.type == QLatin1String("time_depth"))
  {
    TimeDepthTable td;
    QString terr;
    if (!m_doc->timeDepthAt(absPath, &td, &terr))
      return failureState(asset.id, terr, parent);
    QVector<double> tvds, times;
    for (const TdRow &r : td.rows)
    {
      if (!r.hasTvd)
        continue;
      tvds.append(r.tvd);
      times.append(r.timeMs);
    }
    // §4：time_depth 没有可用样点时写「无时深表」，不画假线。
    if (tvds.isEmpty())
      return stateLabel(tr("无时深表"), parent);
    auto *panel = new CurvePanel(parent);
    panel->setEmptyText(tr("无时深表")); // 双保险：NaN 过滤后仍空的兜底文案
    panel->setCurve(tr("TIME–TVD"), QStringLiteral("ms"), times, tvds);
    return panel;
  }

  if (asset.type == QLatin1String("well_head"))
  {
    QVector<WellHeadRecord> rows;
    QString herr;
    if (!m_doc->wellHeadsAt(absPath, &rows, &herr))
      return failureState(asset.id, herr, parent);
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(6);
    auto *info = new QWidget(holder);
    auto *grid = new QVBoxLayout(info);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    const auto addRow = [&](const QString &k, const QString &val, bool mono = false,
                            bool muted = false) {
      auto *row = new QWidget(info);
      auto *rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->addWidget(caption8(k, row));
      auto *v = valueLabel(val, row, mono);
      if (muted)
        PaleoTheme::applyThemedStyleSheet(
            v, [] { return PaleoTheme::mutedCaptionStyleSheet(); }); // text-muted
      rl->addWidget(v, 1);
      grid->addWidget(row);
    };
    // 预览按当前井过滤（多井井位文件；井名规范化后比较）
    const WellHeadRecord *rec = nullptr;
    for (const WellHeadRecord &r : rows)
      if (matchWell(r.name))
        rec = &r;
    if (!rec && !wellName.isEmpty())
    {
      hl->addWidget(stateLabel(tr("井 %1 不在该井位文件中").arg(wellName), holder), 1);
      return holder;
    }
    if (rec)
    {
      // §4：井名、X、Y、KB、TD、BottomX、BottomY、WellType、coordinate_status；
      // 数字 JetBrains Mono 9pt 右对齐。
      addRow(tr("井名"), rec->name);
      addRow(tr("X"), QString::number(rec->x, 'f', 2), true);
      addRow(tr("Y"), QString::number(rec->y, 'f', 2), true);
      addRow(tr("KB"), QString::number(rec->kb, 'f', 2), true);
      addRow(tr("TD"), QString::number(rec->td, 'f', 2), true);
      addRow(tr("BottomX"),
             rec->hasBottomX ? QString::number(rec->bottomX, 'f', 2) : QString(), true);
      addRow(tr("BottomY"),
             rec->hasBottomY ? QString::number(rec->bottomY, 'f', 2) : QString(), true);
      addRow(tr("WellType"), rec->wellType);
    }
    const QString status =
        wellEntityId.isEmpty()
            ? QString()
            : m_doc->catalog()->entityById(wellEntityId).coordinateStatus;
    // T27：坐标状态行中文化 + text-muted（计划 §4：这些状态仍用 #5D6E80）。
    addRow(tr("坐标状态"), coordinateStatusText(status), false, true);
    hl->addWidget(info);
    hl->addWidget(caption8(tr("选中时地图同时高亮该井"), holder));

    // P2 D2.6 井位地图预览：全部井位打点 + 当前井高亮 + 名称标注开关。
    auto *wellsVl = makeMemoryPointLayer(tr("井位"), holder);
    for (const WellHeadRecord &r : rows)
      if (r.x > -99990.0 && r.y > -99990.0) // 坐标哨兵过滤
        addMemoryPoint(wellsVl, r.x, r.y, r.name,
                       (rec && matchWell(r.name)) ? QStringLiteral("highlight")
                                                   : QStringLiteral("well"));
    stylePointLayer(wellsVl, true);
    auto *mapPage = new PreviewMapPage(holder);
    mapPage->setObjectName(QStringLiteral("wellHeadPreviewPage"));
    mapPage->mapCanvas()->canvas()->setObjectName(QStringLiteral("wellHeadMapCanvas"));
    mapPage->setProfileEnabled(false);
    mapPage->addMapLayer(wellsVl, tr("井位"), absPath);
    auto *labelRow = new QWidget(holder);
    auto *labelLay = new QHBoxLayout(labelRow);
    labelLay->setContentsMargins(0, 0, 0, 0);
    auto *labelToggle = new QCheckBox(tr("名称标注"), labelRow);
    labelToggle->setObjectName(QStringLiteral("wellLabelToggle"));
    labelToggle->setChecked(true);
    labelLay->addWidget(labelToggle);
    labelLay->addStretch(1);
    QObject::connect(labelToggle, &QCheckBox::toggled, mapPage,
                     [wellsVl, mapPage](bool on) {
                       wellsVl->setLabelsEnabled(on);
                       mapPage->mapCanvas()->canvas()->refresh();
                     });
    hl->addWidget(labelRow);
    hl->addWidget(mapPage, 1);
    QTimer::singleShot(0, holder, [mapPage, wellsVl]() {
      mapPage->mapCanvas()->zoomToLayer(wellsVl);
    });
    return holder;
  }

  return stateLabel(tr("先选择一口井"), parent); // 兜底（不可达）
}

// ---- D2.10 同目录组图：叠一层同目录可地图化资产（geojson/带配准图片/
// 层位栅格）。不支持的类型如实跳过，不造假层。----
void DataPreviewTabs::addSiblingOverlayLayer(PreviewMapPage *page, const QString &sibAssetId,
                                             const QString &sibName, QWidget *owner)
{
  if (!page || !m_doc || sibAssetId.isEmpty())
    return;
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(sibAssetId);
  if (asset.id.isEmpty())
    return;
  const CatalogVersion v = cat->currentVersion(sibAssetId);
  const QString abs = m_doc->absolutePathForVersion(v);
  if (abs.isEmpty() || !QFile::exists(abs))
    return;

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") &&
       abs.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
  {
    auto *vl = new QgsVectorLayer(abs, sibName, QStringLiteral("ogr"));
    vl->setParent(owner);
    if (!vl->isValid())
    {
      vl->deleteLater();
      return;
    }
    QString field;
    for (const QgsField &f : vl->fields())
    {
      const QString n = f.name();
      if (n == QLatin1String("相") || n.contains(QLatin1String("相")) ||
          n.compare(QLatin1String("facies"), Qt::CaseInsensitive) == 0)
      {
        field = n;
        break;
      }
    }
    if (!field.isEmpty())
      applyFaciesRendererToLayer(vl, field);
    page->addMapLayer(vl, sibName, abs);
    return;
  }
  if (asset.type == QLatin1String("image_reference"))
  {
    if (PreviewMapStates::detectWorldFile(abs).isEmpty())
      return; // 未配准图片不进地图（D2.7 语义）
    auto *rl = new QgsRasterLayer(abs, sibName, QStringLiteral("gdal"));
    rl->setParent(owner);
    if (!rl->isValid() || rl->extent().isEmpty())
    {
      rl->deleteLater();
      return;
    }
    page->addMapLayer(rl, sibName, abs);
    return;
  }
  if (asset.type == QLatin1String("horizon"))
  {
    CatalogVersion best;
    for (const CatalogVersion &cv : cat->versionsForAsset(sibAssetId))
      if (cv.stage == QLatin1String("DERIVED") && cv.versionNumber >= best.versionNumber)
        best = cv;
    if (best.id.isEmpty())
      return;
    const QString tif = m_doc->absolutePathForVersion(best);
    auto *rl = new QgsRasterLayer(tif, sibName, QStringLiteral("gdal"));
    rl->setParent(owner);
    if (!rl->isValid() || rl->extent().isEmpty())
    {
      rl->deleteLater();
      return;
    }
    const auto sum = PreviewRasterAnalysis::summarize(rl);
    if (sum.valid)
      PreviewRasterAnalysis::applyPseudoColorRenderer(
          rl, 1, sum.min, sum.max,
          *PreviewRasterAnalysis::rampPreset(QStringLiteral("terrain")), false,
          PreviewRasterAnalysis::Classification::Continuous);
    page->addMapLayer(rl, sibName, tif);
    return;
  }
}

// ---- 测线解码结果应用（PreviewDocService 信号 → 挂起控件组）----
// 陈旧结果与 SHA 标过时都在服务内做完；这里只把最新一代贴上控件。
void DataPreviewTabs::onSectionReady(const QString &assetId,
                                     const PreviewDocService::SectionDoc &doc)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (!pend.panel)
    return;
  // SectionPanel 是本 cpp 内聚的预览控件——挂起时存的是它。
  auto *sp = static_cast<SectionPanel *>(pend.panel.data());
  sp->setTraces(doc.traces, doc.sampleIntervalUs, doc.startTimeMs);
  if (pend.hasTie)
    sp->setTieMarker(pend.tieText, pend.tieMs);
  // 标题后缀：「文件名 · IL1315」/「文件名 · XL4165」（§4）。
  m_titleSuffixOfAsset[assetId] =
      (doc.isInline ? QStringLiteral("IL") : QStringLiteral("XL")) +
      QString::number(doc.lineNo);
  updateTabTitle(assetId);
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(QString());
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(QString());
  }
}

void DataPreviewTabs::onSectionFailed(const QString &assetId,
                                      const QString &reason)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (auto *sp = static_cast<SectionPanel *>(
          pend.panel ? pend.panel.data() : nullptr))
    sp->setError(reason.isEmpty() ? tr("无法解码测线") : reason); // §4：如实写，不装灰图
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(QString());
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(QString());
  }
}

void DataPreviewTabs::onLasReady(const QString &key, const QStringList &, const QList<LasCurve> &curves)
{
  // F1（goal/perf-systematize 簇2）：数据行到达——fill 闭包内自带 QPointer
  // 护栏（页没了就不装）；服务侧世代号已保证这是最新一代。兄弟文件文档
  // 在 lasReady 之前已写入门面，失败的不在表里。
  if (!m_pendingLas.contains(key))
    return;
  const LasPending pend = m_pendingLas.take(key);
  const QHash<QString, LasDoc> siblings =
      m_doc ? m_doc->lasSiblingDocs(key) : QHash<QString, LasDoc>();
  if (pend.fill)
    pend.fill(curves, siblings);
}

void DataPreviewTabs::onLasFailed(const QString &key, const QString &reason)
{
  // 头部能解但整份解析失败（截断/坏行）：页面骨架已建好——把视图栈内容
  // 换成「读取失败」面（带重试=重建标签重走两段式），与其它失败态同一
  // 形态（§4）。呈现切换条保留（重试成功后仍有用）。
  if (!m_pendingLas.contains(key))
    return;
  const LasPending pend = m_pendingLas.take(key);
  if (auto *stack = qobject_cast<QStackedWidget *>(pend.page.data()))
  {
    while (stack->count() > 0)
    {
      QWidget *w = stack->widget(0);
      stack->removeWidget(w);
      w->deleteLater();
    }
    stack->addWidget(failureState(key, reason.isEmpty() ? tr("无法解析 LAS 文件") : reason, stack));
  }
}
