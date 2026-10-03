// 层：QGIS 封装
#include "standardelements.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>

#include <qgsapplication.h>
#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemmapgrid.h>
#include <qgslayoutitemmapoverview.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutmultiframe.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutpoint.h>
#include <qgslayoutsize.h>
#include <qgslayoutundostack.h>
#include <qgsmaplayer.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgstextformat.h>

#include <cmath>

namespace
{
  // 图件字体族：与 layoutexport.buildHorizonMapLayout 同一选择
  //（DESIGN.md：Noto Sans SC，Linux 退化 Noto Sans CJK SC——图件走系统族名，
  // 由渲染环境解析）。
  constexpr auto kFigureFontFamily = "Noto Sans CJK SC";

  // 整刻度间隔（1/2/5 × 10^n）：坐标网格与比例尺共用。
  double niceInterval( double raw )
  {
    if ( raw <= 0 || std::isnan( raw ) || std::isinf( raw ) )
      return 1000.0;
    const double exponent = std::floor( std::log10( raw ) );
    const double fraction = raw / std::pow( 10.0, exponent );
    double mantissa = 10.0;
    if ( fraction <= 1.0 )
      mantissa = 1.0;
    else if ( fraction <= 2.0 )
      mantissa = 2.0;
    else if ( fraction <= 5.0 )
      mantissa = 5.0;
    return mantissa * std::pow( 10.0, exponent );
  }

  // 图层全幅（插图的「全图」范围）：空集回空矩形。
  QgsRectangle combinedExtent( const QList<QgsMapLayer *> &layers )
  {
    QgsRectangle extent;
    for ( QgsMapLayer *layer : layers )
    {
      if ( !layer )
        continue;
      if ( extent.isNull() )
        extent = layer->extent();
      else
        extent.combineExtentWith( layer->extent() );
    }
    return extent;
  }

  // 地图项在 scene 里的毫米矩形（装饰定位基准）。
  QRectF mapSceneRectMm( QgsLayoutItemMap *map )
  {
    if ( !map )
      return QRectF( 10, 24, 277, 165 ); // A4 横版缺省，防呆
    return map->mapRectToScene( map->rect() );
  }
} // namespace

namespace PaleoStandardElements
{

QString figureKindKey( FigureKind kind )
{
  switch ( kind )
  {
    case FigureKind::WellPosition: return QStringLiteral( "well_position" );
    case FigureKind::SingleFactor: return QStringLiteral( "single_factor" );
    case FigureKind::Facies: return QStringLiteral( "facies" );
  }
  return QString();
}

bool figureKindFromKey( const QString &key, FigureKind *out )
{
  if ( key == QLatin1String( "well_position" ) )
  {
    if ( out ) *out = FigureKind::WellPosition;
    return true;
  }
  if ( key == QLatin1String( "single_factor" ) )
  {
    if ( out ) *out = FigureKind::SingleFactor;
    return true;
  }
  if ( key == QLatin1String( "facies" ) )
  {
    if ( out ) *out = FigureKind::Facies;
    return true;
  }
  return false;
}

QString figureKindTitle( FigureKind kind )
{
  switch ( kind )
  {
    case FigureKind::WellPosition: return QObject::tr( "井位图" );
    case FigureKind::SingleFactor: return QObject::tr( "单因素图" );
    case FigureKind::Facies: return QObject::tr( "沉积相图" );
  }
  return QString();
}

bool applyPageSetup( QgsPrintLayout *layout, const PageSetup &setup )
{
  if ( !layout )
    return false;
  QgsLayoutPageCollection *pages = layout->pageCollection();
  if ( !pages || pages->pageCount() < 1 )
    return false;
  QgsLayoutItemPage *page = pages->page( 0 );
  if ( !page )
    return false;
  page->setPageSize( setup.sizeName,
                     setup.landscape ? QgsLayoutItemPage::Landscape : QgsLayoutItemPage::Portrait );
  return true;
}

QgsTextFormat figureTextFormat( double sizePt )
{
  QgsTextFormat fmt;
  QFont font( QString::fromLatin1( kFigureFontFamily ) );
  font.setPointSizeF( sizePt );
  fmt.setFont( font );
  fmt.setSizeUnit( Qgis::RenderUnit::Points );
  fmt.setSize( sizePt );
  return fmt;
}

QString northArrowSvgPath()
{
  for ( const QString &root : QgsApplication::svgPaths() )
    for ( const QString &name :
          { QStringLiteral( "NorthArrow_04.svg" ), QStringLiteral( "NorthArrow_02.svg" ),
            QStringLiteral( "NorthArrow_01.svg" ), QStringLiteral( "NorthArrow_03.svg" ),
            QStringLiteral( "NorthArrow_05.svg" ), QStringLiteral( "NorthArrow_06.svg" ) } )
    {
      const QString path = QDir( root ).filePath( QStringLiteral( "arrows/" ) + name );
      if ( QFile::exists( path ) )
        return path;
    }
  return QString();
}

QgsLayoutItemMap *addMainMap( QgsPrintLayout *layout, const QList<QgsMapLayer *> &layers,
                              const QgsRectangle &extent )
{
  if ( !layout )
    return nullptr;
  auto *map = new QgsLayoutItemMap( layout );
  map->setId( QStringLiteral( "map" ) );
  // 顺序纪律（mapbooklayout 同款）：先入场景、定图层；范围等调用方把框尺寸
  // 定了再 setExtent——attemptResize 保比例尺会重扩范围，setExtent 只按宽度
  // 调高度，先后不可倒。这里的 extent 只在框已有真实尺寸时直接生效。
  layout->addLayoutItem( map );
  if ( !layers.isEmpty() )
    map->setLayers( layers );
  if ( !extent.isNull() && !extent.isEmpty() && map->rect().width() > 0.0 )
    map->setExtent( extent );
  return map;
}

QgsLayoutItemMap *addInsetMap( QgsPrintLayout *layout, QgsLayoutItemMap *mainMap )
{
  if ( !layout || !mainMap )
    return nullptr;

  // 插图范围 = 主图图层的全幅（主图范围是里面被 overview 框出的那块）。
  QgsRectangle full = combinedExtent( mainMap->layers() );
  if ( full.isNull() || full.isEmpty() )
  {
    full = mainMap->extent();
    full.scale( 1.5 ); // 无图层时的防呆：主图范围外扩一圈当「全图」
  }

  auto *inset = new QgsLayoutItemMap( layout );
  inset->setId( QStringLiteral( "inset" ) );
  layout->addLayoutItem( inset );
  inset->attemptResize( QgsLayoutSize( 42, 30, Qgis::LayoutUnit::Millimeters ) );
  inset->setLayers( mainMap->layers() );
  inset->zoomToExtent( full ); // 定尺寸后套范围（setExtent 会改尺寸，不用）

  // overview：框住主图当前范围（主图一动，重绘时自动跟随——联动在渲染层）。
  auto *overview = new QgsLayoutItemMapOverview( QStringLiteral( "overview" ), inset );
  overview->setLinkedMap( mainMap );
  overview->setEnabled( true );
  inset->overviews()->addOverview( overview );

  // 缺省贴主图右下角（外贴 2mm），不压主图内容。
  const QRectF mapRect = mapSceneRectMm( mainMap );
  inset->attemptMove( QgsLayoutPoint( mapRect.right() - 42.0, mapRect.bottom() + 2.0,
                                      Qgis::LayoutUnit::Millimeters ) );
  return inset;
}

QgsLayoutItemScaleBar *addScaleBar( QgsPrintLayout *layout, QgsLayoutItemMap *map )
{
  if ( !layout || !map )
    return nullptr;
  auto *scalebar = new QgsLayoutItemScaleBar( layout );
  scalebar->setId( QStringLiteral( "scalebar" ) );
  scalebar->setLinkedMap( map ); // 比例随主图联动（M3）
  scalebar->setUnits( Qgis::DistanceUnit::Meters );
  scalebar->setUnitLabel( QObject::tr( "m" ) );
  scalebar->setStyle( QStringLiteral( "Single Box" ) ); // 条式 + 段落数字
  scalebar->applyDefaultSettings();
  scalebar->applyDefaultSize( Qgis::DistanceUnit::Meters );
  scalebar->setTextFormat( figureTextFormat( 8 ) ); // DESIGN.md 次级标签 8pt
  layout->addLayoutItem( scalebar );

  // 主图左下、内缩 4mm（DESIGN.md 画布内装饰位）。
  const QRectF mapRect = mapSceneRectMm( map );
  scalebar->attemptMove( QgsLayoutPoint( mapRect.left() + 4.0, mapRect.bottom() - 10.0,
                                          Qgis::LayoutUnit::Millimeters ) );
  scalebar->update();
  return scalebar;
}

QgsLayoutItemLegend *addLegend( QgsPrintLayout *layout, QgsLayoutItemMap *map )
{
  if ( !layout || !map )
    return nullptr;
  auto *legend = new QgsLayoutItemLegend( layout );
  legend->setId( QStringLiteral( "legend" ) );
  legend->setLinkedMap( map );
  legend->setSyncMode( Qgis::LegendSyncMode::AllProjectLayers ); // 随图层树自动更新
  legend->setLegendFilterByMapEnabled( true );                    // 只列本图图层
  // DESIGN.md「白底半透明卡片承载」：不透明白底 + 细边框，浮在图上可读。
  legend->setBackgroundEnabled( true );
  legend->setBackgroundColor( QColor( 255, 255, 255, 235 ) );
  legend->setFrameEnabled( true );
  layout->addLayoutItem( legend );
  legend->attemptResize( QgsLayoutSize( 50, 36, Qgis::LayoutUnit::Millimeters ) );

  // 主图右上、内缩 2mm（DESIGN.md 画布内装饰位）。
  const QRectF mapRect = mapSceneRectMm( map );
  legend->attemptMove( QgsLayoutPoint( mapRect.right() - 52.0, mapRect.top() + 2.0,
                                       Qgis::LayoutUnit::Millimeters ) );
  return legend;
}

QgsLayoutItem *addNorthArrow( QgsPrintLayout *layout, QgsLayoutItemMap *map )
{
  if ( !layout || !map )
    return nullptr;

  const QRectF mapRect = mapSceneRectMm( map );
  const QString arrowSvg = northArrowSvgPath();
  if ( !arrowSvg.isEmpty() )
  {
    auto *arrow = new QgsLayoutItemPicture( layout );
    arrow->setId( QStringLiteral( "northArrow" ) );
    arrow->setMode( Qgis::PictureFormat::SVG );
    arrow->setPicturePath( arrowSvg );
    arrow->setLinkedMap( map ); // 随地图旋转
    layout->addLayoutItem( arrow );
    arrow->attemptResize( QgsLayoutSize( 12, 12, Qgis::LayoutUnit::Millimeters ) );
    arrow->attemptMove( QgsLayoutPoint( mapRect.left() + 3.0, mapRect.top() + 3.0,
                                        Qgis::LayoutUnit::Millimeters ) );
    return arrow;
  }

  // 无 SVG 资源（精简前缀）：退化为「N」标注，指北语义不丢。
  auto *arrow = new QgsLayoutItemLabel( layout );
  arrow->setId( QStringLiteral( "northArrow" ) );
  arrow->setText( QStringLiteral( "N" ) );
  arrow->setTextFormat( figureTextFormat( 12 ) );
  layout->addLayoutItem( arrow );
  arrow->attemptResize( QgsLayoutSize( 8, 8, Qgis::LayoutUnit::Millimeters ) );
  arrow->attemptMove( QgsLayoutPoint( mapRect.left() + 3.0, mapRect.top() + 3.0,
                                      Qgis::LayoutUnit::Millimeters ) );
  return arrow;
}

void addCoordinateGrid( QgsLayoutItemMap *map )
{
  if ( !map )
    return;
  auto *grid = new QgsLayoutItemMapGrid( QStringLiteral( "grid" ), map );
  grid->setEnabled( true );
  const QgsRectangle extent = map->extent();
  double interval = niceInterval( extent.width() / 4.0 );
  if ( !( interval > 0 ) )
    interval = 1000.0; // 无范围（空骨架地图）：米制缺省
  grid->setIntervalX( interval );
  grid->setIntervalY( niceInterval( extent.height() / 4.0 ) > 0 ? niceInterval( extent.height() / 4.0 )
                                                                : interval );
  grid->setAnnotationEnabled( true );
  grid->setAnnotationTextFormat( figureTextFormat( 6 ) ); // 网格注记比次级标签再小一档
  grid->setFrameStyle( Qgis::MapGridFrameStyle::LineBorder );
  map->grids()->addGrid( grid );
}

TitleBlock addTitleBlock( QgsPrintLayout *layout, const QString &title, const QString &subtitle )
{
  TitleBlock block;
  if ( !layout )
    return block;

  QgsLayoutPageCollection *pages = layout->pageCollection();
  QgsLayoutItemPage *page = pages && pages->pageCount() > 0 ? pages->page( 0 ) : nullptr;
  if ( !page )
    return block;
  const QgsLayoutSize paper = page->pageSize(); // mm
  const double pageW = paper.width();
  const double pageH = paper.height();

  block.title = new QgsLayoutItemLabel( layout );
  block.title->setId( QStringLiteral( "title" ) );
  block.title->setText( title );
  block.title->setTextFormat( figureTextFormat( 15 ) ); // DESIGN.md 图件标题 15pt
  layout->addLayoutItem( block.title );
  block.title->attemptResize( QgsLayoutSize( pageW - 20, 12, Qgis::LayoutUnit::Millimeters ) );
  block.title->attemptMove( QgsLayoutPoint( 10, 6, Qgis::LayoutUnit::Millimeters ) );

  block.subtitle = new QgsLayoutItemLabel( layout );
  block.subtitle->setId( QStringLiteral( "subtitle" ) );
  block.subtitle->setText( subtitle );
  block.subtitle->setTextFormat( figureTextFormat( 9 ) ); // 正文 9pt
  layout->addLayoutItem( block.subtitle );
  block.subtitle->attemptResize( QgsLayoutSize( pageW - 20, 8, Qgis::LayoutUnit::Millimeters ) );
  block.subtitle->attemptMove( QgsLayoutPoint( 10, 18, Qgis::LayoutUnit::Millimeters ) );

  block.signature = new QgsLayoutItemLabel( layout );
  block.signature->setId( QStringLiteral( "signature" ) );
  block.signature->setText( QDateTime::currentDateTime().toString( QStringLiteral( "yyyy-MM-dd" ) ) );
  block.signature->setTextFormat( figureTextFormat( 8 ) ); // 次级标签 8pt
  block.signature->setHAlign( Qt::AlignRight );
  layout->addLayoutItem( block.signature );
  block.signature->attemptResize( QgsLayoutSize( 60, 6, Qgis::LayoutUnit::Millimeters ) );
  block.signature->attemptMove( QgsLayoutPoint( pageW - 70, pageH - 9, Qgis::LayoutUnit::Millimeters ) );

  return block;
}

int addStandardSet( QgsPrintLayout *layout, QgsLayoutItemMap *map, const QString &title,
                    const QString &subtitle )
{
  if ( !layout || !map )
    return 0;
  int added = 0;
  if ( addScaleBar( layout, map ) )
    ++added;
  if ( addLegend( layout, map ) )
    ++added;
  if ( addNorthArrow( layout, map ) )
    ++added;
  addCoordinateGrid( map );
  ++added; // 网格无独立项（挂在地图项上），计入装配数
  const TitleBlock block = addTitleBlock( layout, title, subtitle );
  if ( block.title )
    ++added;
  if ( block.subtitle )
    ++added;
  if ( block.signature )
    ++added;
  return added;
}

namespace
{
  // 清掉版面内容（页与页面集合保留）：内容模板重套/批量导出换骨架用。
  // loadFromTemplate(clearExisting) 的 XML 路径之外的编程路径——多帧也一起清。
  void clearContentItems( QgsPrintLayout *layout )
  {
    const QList<QgsLayoutMultiFrame *> frames = layout->multiFrames();
    for ( QgsLayoutMultiFrame *mf : frames )
      layout->removeMultiFrame( mf );
    QList<QgsLayoutItem *> items;
    layout->layoutItems( items );
    for ( QgsLayoutItem *item : items )
    {
      if ( qobject_cast<QgsLayoutItemPage *>( item ) )
        continue;
      layout->removeLayoutItem( item );
    }
  }
} // namespace

QgsPrintLayout *buildFigureLayout( FigureKind kind, const PageSetup &setup, QgsProject *project,
                                   const QList<QgsMapLayer *> &layers, const QgsRectangle &extent,
                                   QString *error )
{
  if ( !project )
  {
    if ( error )
      *error = QObject::tr( "未提供 QgsProject" );
    return nullptr;
  }
  auto *layout = new QgsPrintLayout( project );
  layout->initializeDefaults(); // 一页缺省纸，随后按规格改
  if ( !populateFigureLayout( layout, kind, setup, layers, extent, error ) )
  {
    delete layout;
    return nullptr;
  }
  return layout;
}

bool populateFigureLayout( QgsPrintLayout *layout, FigureKind kind, const PageSetup &setup,
                           const QList<QgsMapLayer *> &layers, const QgsRectangle &extent,
                           QString *error )
{
  if ( !layout )
  {
    if ( error )
      *error = QObject::tr( "未提供版面" );
    return false;
  }

  // 模板装配是原子操作，不进 undo 历史（同测试 makeLayout 的 blockCommands 纪律）。
  if ( layout->undoStack() )
    layout->undoStack()->blockCommands( true );

  clearContentItems( layout );
  if ( !applyPageSetup( layout, setup ) )
  {
    if ( layout->undoStack() )
      layout->undoStack()->blockCommands( false );
    if ( error )
      *error = QObject::tr( "版面没有可设置的页面" );
    return false;
  }

  // 页面毫米数：骨架排版的坐标基准。
  QgsLayoutItemPage *page = layout->pageCollection()->page( 0 );
  const double W = page->pageSize().width();
  const double H = page->pageSize().height();

  addTitleBlock( layout, figureKindTitle( kind ), QString() );

  // 地图区：y 从标题块下开始，下边留署名带。
  const double topY = 28.0;
  const double bottomY = H - 14.0;
  double mapLeft = 10.0;
  double mapRight = W - 10.0;

  QgsLayoutItemMap *map = addMainMap( layout, layers, extent );

  // 图例列宽随种类：井位图无列（图例浮在图内），单因素 58mm，沉积相 66mm。
  double legendColumnW = 0.0;
  if ( kind == FigureKind::SingleFactor )
    legendColumnW = 58.0;
  else if ( kind == FigureKind::Facies )
    legendColumnW = 66.0;
  if ( legendColumnW > 0.0 )
    mapRight = W - 10.0 - legendColumnW - 4.0;

  map->attemptResize( QgsLayoutSize( mapRight - mapLeft, bottomY - topY,
                                     Qgis::LayoutUnit::Millimeters ) );
  map->attemptMove( QgsLayoutPoint( mapLeft, topY, Qgis::LayoutUnit::Millimeters ) );
  if ( !extent.isNull() && !extent.isEmpty() )
    map->setExtent( extent ); // 定框后设范围（顺序纪律见 addMainMap 注）

  addScaleBar( layout, map );
  QgsLayoutItemLegend *legend = addLegend( layout, map );
  addNorthArrow( layout, map );
  addCoordinateGrid( map );

  if ( kind == FigureKind::Facies )
  {
    // 沉积相图：插图（全图位置指示）落在图例列下方。
    QgsLayoutItemMap *inset = addInsetMap( layout, map );
    if ( inset )
      inset->attemptMove( QgsLayoutPoint( W - 10.0 - 42.0, bottomY - 32.0,
                                          Qgis::LayoutUnit::Millimeters ) );
  }

  if ( legend && legendColumnW > 0.0 )
  {
    // 有图例列的品种：图例整列落位（浮图样式改为贴列顶）。
    legend->attemptResize( QgsLayoutSize( legendColumnW, 80.0, Qgis::LayoutUnit::Millimeters ) );
    legend->attemptMove( QgsLayoutPoint( W - 10.0 - legendColumnW, topY,
                                         Qgis::LayoutUnit::Millimeters ) );
  }

  if ( layout->undoStack() )
    layout->undoStack()->blockCommands( false );
  return true;
}

} // namespace PaleoStandardElements
