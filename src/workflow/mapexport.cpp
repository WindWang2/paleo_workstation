#include "mapexport.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../ui/layout/layoutexportactions.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>

#include <gdal.h>

#include <qgsapplication.h>
#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutpagecollection.h>
#include <qgsmaplayer.h>
#include <qgspallabeling.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsrastertransparency.h>
#include <qgsrectangle.h>
#include <qgsprintlayout.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextformat.h>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

  // Survey extent from a raster source (geotransform); empty rectangle when
  // the raster cannot be read.
  QgsRectangle rasterExtent( const QString &source )
  {
    const QString path = source.section( QLatin1Char( '|' ), 0, 0 );
    if ( path.isEmpty() || !QFile::exists( path ) )
      return QgsRectangle();
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
      return QgsRectangle();
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    GDALGetGeoTransform( ds, gt );
    const int cols = GDALGetRasterXSize( ds );
    const int rows = GDALGetRasterYSize( ds );
    const double nodataDummy = 0.0;
    Q_UNUSED( nodataDummy );
    GDALClose( ds );
    if ( cols <= 0 || rows <= 0 || gt[1] <= 0.0 )
      return QgsRectangle();
    return QgsRectangle( gt[0], gt[3] + gt[5] * rows, gt[0] + gt[1] * cols, gt[3] );
  }

  // nodata -9999 不画（§162）：单值透明表在渲染器上。
  void hideNodata( QgsMapLayer *layer )
  {
    auto *raster = qobject_cast<QgsRasterLayer *>( layer );
    if ( !raster || !raster->renderer() )
      return;
    auto *transparency = new QgsRasterTransparency; // renderer 接管所有权
    QgsRasterTransparency::TransparentSingleValuePixel px;
    px.min = -9999.0;
    px.max = -9999.0;
    px.opacity = 0.0; // 0 = 全透明
    transparency->setTransparentSingleValuePixelList( { px } );
    raster->renderer()->setRasterTransparency( transparency );
  }

  // 井名标注：分层点图层的 well_name 字段（§162「井名」）。
  void enableWellNameLabels( QgsMapLayer *layer )
  {
    auto *vl = qobject_cast<QgsVectorLayer *>( layer );
    if ( !vl || vl->fields().indexOf( QStringLiteral( "well_name" ) ) < 0 )
      return;
    QgsPalLayerSettings settings;
    settings.fieldName = QStringLiteral( "well_name" );
    settings.isExpression = false;
    vl->setLabeling( new QgsVectorLayerSimpleLabeling( settings ) );
    vl->setLabelsEnabled( true );
  }

  // 标签字体（QGIS 4.x：setFont 已弃用，走 QgsTextFormat；字号单位是 pt）。
  QgsTextFormat labelFormat( const QString &family, double sizePt )
  {
    QgsTextFormat fmt;
    QFont font( family );
    font.setPointSizeF( sizePt );
    fmt.setFont( font );
    fmt.setSizeUnit( Qgis::RenderUnit::Points );
    fmt.setSize( sizePt );
    return fmt;
  }

  // 指北针 SVG：QGIS 自带的 arrows/NorthArrow_*.svg（svgPaths 每个目录都是
  // svg 根，箭头在 arrows/ 子目录）。
  QString northArrowSvg()
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
} // namespace

QgsPrintLayout *buildHorizonMapLayout( QgisLayerService *layers, QgisProjectService *projectSvc,
                                     const QString &horizon, QString *error )
{
  const auto fail = [error]( const QString &msg ) -> QgsPrintLayout * {
    if ( error )
      *error = msg;
    return nullptr;
  };
  if ( !layers )
    return fail( QObject::tr( "未绑定图层服务" ) );
  QgsProject *project = projectSvc ? projectSvc->project() : nullptr;
  if ( !project )
    return fail( QObject::tr( "未绑定工程服务" ) );

  // 厚度栅格是本图主体（§162：「图内仍含井位和厚度栅格」）；相多边形只在
  // 已经跑过 paleo_facies_polygonize 之后才进图；井位点层可选（链路产物）。
  const QString thicknessId = QStringLiteral( "factor.%1.idw" ).arg( horizon );
  const QString wellsId = QStringLiteral( "wells.thickness.%1" ).arg( horizon );
  const QString faciesId = QStringLiteral( "facies.%1" ).arg( horizon );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? QObject::tr( "无法读取图层清单" ) : manifestErr );

  QString thicknessSource, horizonRasterSource;
  bool hasFacies = false;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.horizon != horizon )
      continue;
    if ( d.layerId == thicknessId )
      thicknessSource = d.source;
    else if ( d.layerId == faciesId )
      hasFacies = true;
    else if ( d.layerId.startsWith( QLatin1String( "horizon." ) ) &&
              d.type.compare( QLatin1String( "raster" ), Qt::CaseInsensitive ) == 0 )
      horizonRasterSource = d.source;
  }
  if ( thicknessSource.isEmpty() )
    return fail( QObject::tr( "层位 %1 还没有厚度栅格（先运行编图链）" ).arg( horizon ) );

  QgsMapLayer *thickness = layers->instantiate( thicknessId, error );
  if ( !thickness )
    return fail( error && !error->isEmpty()
                     ? *error
                     : QObject::tr( "无法实例化 %1" ).arg( thicknessId ) );
  hideNodata( thickness ); // nodata 不画

  // 图层表序：index 0 在最上（沿用既有 prepend 惯例）。
  QList<QgsMapLayer *> mapLayers;
  mapLayers.append( thickness );
  if ( hasFacies )
    if ( QgsMapLayer *facies = layers->instantiate( faciesId ) )
      mapLayers.prepend( facies ); // 相面压在厚度栅格之上
  if ( QgsMapLayer *wells = layers->instantiate( wellsId ) )
  {
    enableWellNameLabels( wells ); // 图上有井名
    mapLayers.prepend( wells );    // 井位压在所有图层之上
  }

  QgsRectangle extent = rasterExtent( thicknessSource );
  if ( extent.isEmpty() )
    extent = rasterExtent( horizonRasterSource );
  if ( extent.isEmpty() )
    extent = thickness->extent();

  // A4 横版 + 地图项 + 标题 + 图例 + 比例尺 + 指北针 + CRS 说明。
  auto *layout = new QgsPrintLayout( project );
  layout->initializeDefaults();
  layout->setName( QStringLiteral( "%1_map" ).arg( horizon ) );
  // initializeDefaults 的默认页是竖版 A4 —— 本图是横版，显式改。
  if ( QgsLayoutItemPage *page = layout->pageCollection()->page( 0 ) )
    page->setPageSize( QStringLiteral( "A4" ), QgsLayoutItemPage::Landscape );

  auto *map = new QgsLayoutItemMap( layout );
  map->setId( QStringLiteral( "map" ) );
  map->setLayers( mapLayers );
  map->setExtent( extent );
  layout->addLayoutItem( map );
  map->attemptResize( QgsLayoutSize( 277, 165, Qgis::LayoutUnit::Millimeters ) );
  map->attemptMove( QgsLayoutPoint( 10, 22, Qgis::LayoutUnit::Millimeters ) );

  auto *title = new QgsLayoutItemLabel( layout );
  title->setId( QStringLiteral( "title" ) );
  title->setText( QObject::tr( "%1 厚度" ).arg( horizon ) );
  title->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), 15 ) ); // DESIGN.md 图件标题
  layout->addLayoutItem( title );
  title->attemptResize( QgsLayoutSize( 200, 12, Qgis::LayoutUnit::Millimeters ) );
  title->attemptMove( QgsLayoutPoint( 10, 6, Qgis::LayoutUnit::Millimeters ) );

  // 米制图例（图例右上）：只列本图用到的图层。
  auto *legend = new QgsLayoutItemLegend( layout );
  legend->setId( QStringLiteral( "legend" ) );
  legend->setTitle( QObject::tr( "厚度（米）" ) );
  legend->setLinkedMap( map );
  legend->setSyncMode( Qgis::LegendSyncMode::VisibleLayers );
  legend->setLegendFilterByMapEnabled( true );
  layout->addLayoutItem( legend );
  legend->attemptResize( QgsLayoutSize( 55, 40, Qgis::LayoutUnit::Millimeters ) );
  legend->attemptMove( QgsLayoutPoint( 230, 26, Qgis::LayoutUnit::Millimeters ) );

  // 比例尺（左下），单位为米。
  auto *scalebar = new QgsLayoutItemScaleBar( layout );
  scalebar->setId( QStringLiteral( "scalebar" ) );
  scalebar->setLinkedMap( map );
  scalebar->setUnits( Qgis::DistanceUnit::Meters );
  scalebar->setUnitLabel( QStringLiteral( "m" ) );
  scalebar->applyDefaultSettings();
  scalebar->applyDefaultSize( Qgis::DistanceUnit::Meters );
  layout->addLayoutItem( scalebar );
  scalebar->attemptMove( QgsLayoutPoint( 14, 176, Qgis::LayoutUnit::Millimeters ) );
  scalebar->update();

  // 指北针（左上）：QGIS 自带 SVG 箭头；找不到图时退化为「N」标注。
  const QString arrowSvg = northArrowSvg();
  if ( !arrowSvg.isEmpty() )
  {
    auto *arrow = new QgsLayoutItemPicture( layout );
    arrow->setId( QStringLiteral( "northArrow" ) );
    arrow->setMode( Qgis::PictureFormat::SVG );
    arrow->setPicturePath( arrowSvg );
    arrow->setLinkedMap( map ); // 随地图旋转
    layout->addLayoutItem( arrow );
    arrow->attemptResize( QgsLayoutSize( 12, 12, Qgis::LayoutUnit::Millimeters ) );
    arrow->attemptMove( QgsLayoutPoint( 13, 25, Qgis::LayoutUnit::Millimeters ) );
  }
  else
  {
    auto *arrow = new QgsLayoutItemLabel( layout );
    arrow->setId( QStringLiteral( "northArrow" ) );
    arrow->setText( QStringLiteral( "N" ) );
    arrow->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), 12 ) );
    layout->addLayoutItem( arrow );
    arrow->attemptResize( QgsLayoutSize( 8, 8, Qgis::LayoutUnit::Millimeters ) );
    arrow->attemptMove( QgsLayoutPoint( 13, 25, Qgis::LayoutUnit::Millimeters ) );
  }

  // CRS 说明（页脚）：工程坐标 · 米 · 未投影（§227 — PDF 上印同一句话）。
  auto *crs = new QgsLayoutItemLabel( layout );
  crs->setId( QStringLiteral( "crsCaption" ) );
  crs->setText( QObject::tr( "工程坐标 · 米 · 未投影" ) );
  crs->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), 9 ) );
  layout->addLayoutItem( crs );
  crs->attemptResize( QgsLayoutSize( 140, 8, Qgis::LayoutUnit::Millimeters ) );
  crs->attemptMove( QgsLayoutPoint( 10, 192, Qgis::LayoutUnit::Millimeters ) );

  return layout;
}

QString exportHorizonMapPdf( QgisLayerService *layers, QgisProjectService *projectSvc,
                             const QString &horizon, const QString &outPath, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QString();
  };
  if ( outPath.isEmpty() )
    return fail( QObject::tr( "导出路径为空" ) );

  QgsPrintLayout *layout = buildHorizonMapLayout( layers, projectSvc, horizon, error );
  if ( !layout )
    return QString();

  PaleoLayoutExportActions exports;
  const auto outcome = exports.exportLayout( layout, outPath,
                                             PaleoLayoutExportActions::Format::Pdf, 300.0,
                                             PaleoLayoutExportActions::PageRange() );
  delete layout;
  if ( !outcome.ok )
    return fail( outcome.error );
  return outcome.files.value( 0 );
}

QString registerMapPdfAsset( DataCatalog *catalog, const QString &projectDir,
                             const QString &pdfPath, QString *sha256Out,
                             QString *managedPathOut, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QString();
  };
  if ( !catalog )
    return fail( QObject::tr( "未绑定数据目录" ) );
  if ( projectDir.isEmpty() )
    return fail( QObject::tr( "工程目录未设置" ) );
  if ( pdfPath.isEmpty() || !QFile::exists( pdfPath ) )
    return fail( QObject::tr( "找不到 PDF 文件: %1" ).arg( pdfPath ) );

  // 文件摘要先算 —— dedup 与登记共用同一份 SHA-256。
  QString shaErr;
  const QString sha = DataCatalog::sha256FileHex( pdfPath, &shaErr );
  if ( sha.isEmpty() )
    return fail( shaErr.isEmpty() ? QObject::tr( "无法计算 SHA-256: %1" ).arg( pdfPath )
                                  : shaErr );

  // §3 dedup：同 SHA-256 已在库 → 复用既有版本的资产，不新增。
  const CatalogVersion existing = catalog->versionBySha256( sha );
  if ( !existing.id.isEmpty() )
  {
    if ( sha256Out )
      *sha256Out = sha;
    if ( managedPathOut )
      *managedPathOut = QDir( projectDir ).absoluteFilePath( existing.path );
    return existing.assetId;
  }

  const QString fileName = QFileInfo( pdfPath ).fileName();
  if ( !DataCatalog::isSafePathSegment( fileName ) )
    return fail( QObject::tr( "文件名不是合法路径段: %1" ).arg( fileName ) );

  const QString assetId = catalog->nextAssetId();
  const QString versionId = catalog->nextVersionId();
  const QString relDir =
      DataCatalog::managedPath( QStringLiteral( "output" ), assetId, versionId, fileName );
  if ( relDir.isEmpty() )
    return fail( QObject::tr( "受管路径段不合法（output/%1/%2）" ).arg( assetId, versionId ) );
  const QString relPath = QStringLiteral( "artifacts/" ) + relDir;
  const QString dst = QDir( projectDir ).absoluteFilePath( relPath );
  const QDir dir = QFileInfo( dst ).absoluteDir();
  if ( !dir.exists() && !dir.mkpath( QStringLiteral( "." ) ) )
    return fail( QObject::tr( "cannot create directory %1" ).arg( dir.absolutePath() ) );

  // 受管副本：partial + rename 原子落位，成功后置只读（与 RAW 入库同纪律）。
  const QString partial = dst + QStringLiteral( ".partial" );
  if ( !QFile::copy( pdfPath, partial ) )
    return fail( QObject::tr( "cannot copy %1 → %2" ).arg( pdfPath, partial ) );
  if ( ::rename( QFile::encodeName( partial ).constData(), QFile::encodeName( dst ).constData() ) != 0 )
  {
    QFile::remove( partial );
    return fail( QObject::tr( "cannot place %1" ).arg( dst ) );
  }
  QFile::setPermissions( dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther );

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = QStringLiteral( "document" ); // 图件 PDF 归 document 一类
  asset.format = QStringLiteral( "pdf" );
  asset.displayName = fileName;
  if ( !catalog->addAsset( asset, error ) )
    return fail( error ? *error : QStringLiteral( "catalog addAsset failed" ) );

  CatalogVersion ver;
  ver.id = versionId;
  ver.assetId = assetId;
  ver.stage = QStringLiteral( "OUTPUT" );
  ver.versionNumber = 1;
  ver.managed = true;
  ver.path = relPath;
  ver.sourceUri = QFileInfo( pdfPath ).absoluteFilePath();
  ver.sha256 = sha;
  ver.fileName = fileName;
  if ( !catalog->addVersion( ver, error ) )
    return fail( error ? *error : QStringLiteral( "catalog addVersion failed" ) );

  if ( sha256Out )
    *sha256Out = sha;
  if ( managedPathOut )
    *managedPathOut = dst;
  return assetId;
}
