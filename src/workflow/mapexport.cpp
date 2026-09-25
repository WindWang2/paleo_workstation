#include "mapexport.h"

#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../ui/layout/layoutexportactions.h"

#include <QFile>

#include <gdal.h>

#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsprintlayout.h>

namespace
{
  // Survey extent from the horizon's DERIVED raster (geotransform); empty
  // rectangle when the raster cannot be read.
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
    GDALClose( ds );
    if ( cols <= 0 || rows <= 0 || gt[1] <= 0.0 )
      return QgsRectangle();
    return QgsRectangle( gt[0], gt[3] + gt[5] * rows, gt[0] + gt[1] * cols, gt[3] );
  }
} // namespace

QString exportHorizonMapPdf( QgisLayerService *layers, const QString &horizon,
                             const QString &outPath, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QString();
  };
  if ( !layers )
    return fail( QObject::tr( "未绑定图层服务" ) );
  if ( outPath.isEmpty() )
    return fail( QObject::tr( "导出路径为空" ) );

  // 相多边形（本图的主体）必须已声明；井位点层可选（链路产物）。
  const QString faciesId = QStringLiteral( "facies.%1" ).arg( horizon );
  const QString wellsId = QStringLiteral( "wells.thickness.%1" ).arg( horizon );
  const QString rasterId = QStringLiteral( "horizon.%1.derived" ).arg( horizon );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? QObject::tr( "无法读取图层清单" ) : manifestErr );

  QString faciesSource, rasterSource;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == faciesId )
      faciesSource = d.source;
    else if ( d.layerId == rasterId )
      rasterSource = d.source;
  }
  if ( faciesSource.isEmpty() )
    return fail( QObject::tr( "层位 %1 还没有相多边形图层（先运行编图链）" ).arg( horizon ) );

  QList<QgsMapLayer *> mapLayers;
  if ( QgsMapLayer *facies = layers->instantiate( faciesId, error ) )
    mapLayers.append( facies );
  else
    return fail( error && !error->isEmpty() ? *error
                                             : QObject::tr( "无法实例化 %1" ).arg( faciesId ) );
  if ( QgsMapLayer *wells = layers->instantiate( wellsId ) )
    mapLayers.prepend( wells ); // 井位压在相面之上

  QgsRectangle extent = rasterExtent( rasterSource );
  if ( extent.isEmpty() )
    extent = mapLayers.constFirst()->extent();

  // A4 横版 + 地图项 + 标题；导出走 PaleoLayoutExportActions（原生管线）。
  QgsProject *project = QgsProject::instance();
  QgsPrintLayout layout( project );
  layout.initializeDefaults();
  layout.setName( QStringLiteral( "%1_map" ).arg( horizon ) );

  auto *map = new QgsLayoutItemMap( &layout );
  map->setLayers( mapLayers );
  map->setExtent( extent );
  layout.addLayoutItem( map );
  map->attemptResize( QgsLayoutSize( 277, 175, Qgis::LayoutUnit::Millimeters ) );
  map->attemptMove( QgsLayoutPoint( 10, 22, Qgis::LayoutUnit::Millimeters ) );

  auto *title = new QgsLayoutItemLabel( &layout );
  title->setText( QObject::tr( "%1 沉积相图" ).arg( horizon ) );
  title->setFont( QFont( QObject::tr( "Noto Sans CJK SC" ), 14 ) );
  layout.addLayoutItem( title );
  title->attemptResize( QgsLayoutSize( 200, 12, Qgis::LayoutUnit::Millimeters ) );
  title->attemptMove( QgsLayoutPoint( 10, 6, Qgis::LayoutUnit::Millimeters ) );

  PaleoLayoutExportActions exports;
  const auto outcome = exports.exportLayout( &layout, outPath,
                                             PaleoLayoutExportActions::Format::Pdf, 300.0,
                                             PaleoLayoutExportActions::PageRange() );
  if ( !outcome.ok )
    return fail( outcome.error );
  return outcome.files.value( 0 );
}
