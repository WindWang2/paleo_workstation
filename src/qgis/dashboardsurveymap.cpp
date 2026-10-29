// 层：QGIS 封装
#include "dashboardsurveymap.h"
#include "previewmapcanvas.h"
#include "../catalog/datacatalog.h"
#include <qgscoordinatereferencesystem.h>

#include <qgscategorizedsymbolrenderer.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmaptoolpan.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgspallabeling.h>
#include <qgsrectangle.h>
#include <qgsrubberband.h>
#include <qgssinglesymbolrenderer.h>
#include <qgstextbuffersettings.h>
#include <qgstextformat.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

#include <cmath>
#include <memory>

namespace
{
  const char *kWells = "dashboardWellsLayer";
  const char *kBand = "dashboardSurveyRubberBand";
  const char *kWellBand = "dashboardWellRangeRubberBand";
  const char *kSeismicBand = "dashboardSeismicRangeRubberBand";

  QgsRubberBand *rangeBand( QWidget *map, QgsMapCanvas *c, const char *key, const QColor &stroke,
                            Qt::PenStyle line, double width )
  {
    auto *m = qobject_cast<PreviewMapCanvas *>( map );
    if ( !m )
      return nullptr;
    auto *band = qobject_cast<QgsRubberBand *>( m->property( key ).value<QObject *>() );
    if ( !band )
    {
      band = new QgsRubberBand( c, Qgis::GeometryType::Polygon );
      band->setObjectName( QString::fromLatin1( key ) );
      QColor fill = stroke;
      fill.setAlpha( 10 );
      band->setColor( fill );
      band->setStrokeColor( stroke );
      band->setWidth( width );
      band->setLineStyle( line );
      band->show();
      m->setProperty( key, QVariant::fromValue( static_cast<QObject *>( band ) ) );
    }
    else
    {
      band->setStrokeColor( stroke );
      band->setLineStyle( line );
      band->setWidth( width );
    }
    return band;
  }

  // 与 datapreviewtabs_internal.h styleSurveyWellLayer/applyPointNameLabels 同图式
  //（Q/HS 1011—2016 表 K.1 探井靶标 + 白晕井名）。井点符号按用户要求收敛
  //（外环 4.2 / 实心 3.0，白晕井名不变）。
  void styleWells( QgsVectorLayer *vl, const DashboardSurveyMap::Style &st )
  {
    const QString hex = st.primary.name();
    QVariantMap ring;
    ring[QStringLiteral( "name" )] = QStringLiteral( "circle" );
    ring[QStringLiteral( "color" )] = QStringLiteral( "255,255,255,0" );
    ring[QStringLiteral( "outline_color" )] = hex;
    ring[QStringLiteral( "outline_width" )] = QStringLiteral( "0.35" );
    ring[QStringLiteral( "size" )] = QStringLiteral( "4.2" );
    std::unique_ptr<QgsMarkerSymbol> sym = QgsMarkerSymbol::createSimple( ring );
    QVariantMap disk;
    disk[QStringLiteral( "name" )] = QStringLiteral( "circle" );
    disk[QStringLiteral( "color" )] = hex;
    disk[QStringLiteral( "outline_style" )] = QStringLiteral( "no" );
    disk[QStringLiteral( "size" )] = QStringLiteral( "3" );
    sym->appendSymbolLayer( QgsSimpleMarkerSymbolLayer::create( disk ) );
    vl->setRenderer( new QgsSingleSymbolRenderer( sym.release() ) );

    vl->setLabelsEnabled( true );
    QgsPalLayerSettings pal;
    pal.fieldName = QStringLiteral( "name" );
    pal.isExpression = false;
    QgsTextFormat fmt;
    fmt.setFont( st.labelFont );
    fmt.setSize( st.labelPt );
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
}

namespace DashboardSurveyMap
{
  QWidget *create( QWidget *parent, const Style &style )
  {
    auto *map = new PreviewMapCanvas( parent );
    QgsMapCanvas *c = map->canvas();
    auto *pan = new QgsMapToolPan( c );
    pan->setParent( c );
    c->setMapTool( pan );

    auto *vl = new QgsVectorLayer(
        QStringLiteral( "Point?crs=EPSG:32650&field=name:string(64)&field=role:string(16)&field=entity_id:string(128)&index=yes" ),
        style.wellsLayerName, QStringLiteral( "memory" ) );
    vl->setParent( map ); // 画布只引用裸指针，层归宿主
    c->setDestinationCrs( QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:32650" ) ) );
    vl->setObjectName( QString::fromLatin1( kWells ) );
    styleWells( vl, style );
    c->setLayers( { vl } );

    auto *band = new QgsRubberBand( c, Qgis::GeometryType::Polygon );
    band->setObjectName( QString::fromLatin1( kBand ) );
    QColor fill = style.primary;
    fill.setAlpha( 16 );
    band->setColor( fill );
    band->setStrokeColor( style.primary );
    band->setWidth( 2 );
    band->setLineStyle( Qt::DashLine );
    band->show();
    // QgsRubberBand 挂在画布场景里，不是画布的 QObject 子对象——按名找不到
    //（#697 起曾因此每次 update 提前返回，看板井位从不灌层）。存裸指针属性。
    map->setProperty( kBand, QVariant::fromValue( static_cast<QObject *>( band ) ) );
    return map;
  }

  QWidget *canvasWidget( QWidget *map )
  {
    auto *m = qobject_cast<PreviewMapCanvas *>( map );
    return m ? m->canvas() : nullptr;
  }

  void update( QWidget *map, const QVector<CatalogEntity> &wells, const CatalogEntity *survey,
               const AreaRanges *ranges )
  {
    auto *m = qobject_cast<PreviewMapCanvas *>( map );
    if ( !m )
      return;
    QgsMapCanvas *c = m->canvas();
    auto *vl = m->findChild<QgsVectorLayer *>( QString::fromLatin1( kWells ) );
    auto *band = qobject_cast<QgsRubberBand *>( m->property( kBand ).value<QObject *>() );
    if ( !vl || !band )
      return;
    auto *wellBand = rangeBand( map, c, kWellBand, kWellRangeColor, Qt::DotLine, 1.4 );
    auto *seismicBand = rangeBand( map, c, kSeismicBand, kSeismicRangeColor, Qt::DashLine, 1.4 );
    // 内存层 truncate/重灌若与在飞渲染作业并发即堆损坏（0xc0000374）。
    c->stopRendering();
    vl->dataProvider()->truncate();
    QgsRectangle extent;
    for ( const CatalogEntity &well : wells )
    {
      if ( !well.hasSurface || !std::isfinite( well.surfaceX ) || !std::isfinite( well.surfaceY ) )
        continue;
      QgsFeature f( vl->fields() );
      f.setAttribute( QStringLiteral( "name" ), well.name );
      f.setAttribute( QStringLiteral( "role" ), QStringLiteral( "well" ) );
      f.setAttribute( QStringLiteral( "entity_id" ), well.id );
      f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( well.surfaceX, well.surfaceY ) ) );
      vl->dataProvider()->addFeature( f );
      extent.combineExtentWith( QgsRectangle( well.surfaceX, well.surfaceY, well.surfaceX, well.surfaceY ) );
    }
    vl->updateExtents();

    const auto surveyCrs = QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:32650" ) );
    c->setDestinationCrs( surveyCrs );

    // 范围层：数据服务三视图（工区/测井/地震）优先；无服务范围时回退
    // 角点 → 井位并集 → 10 km 默认（角点兜底与全景页同口径，见
    // datapreviewtabs_survey.cpp 取界注释——线号不是坐标）。
    const auto rectGeometry = []( const RangeRect &r ) {
      return r.valid ? QgsGeometry::fromRect( QgsRectangle( r.minX, r.minY, r.maxX, r.maxY ) )
                     : QgsGeometry();
    };
    QgsGeometry areaGeom = rectGeometry( ranges ? ranges->merged : RangeRect() );
    if ( areaGeom.isNull() && survey && survey->corners.size() >= 3 )
    {
      QgsRectangle bounds;
      for ( const auto &p : survey->corners )
        if ( std::isfinite( p.first ) && std::isfinite( p.second ) )
          bounds.combineExtentWith( QgsRectangle( p.first, p.second, p.first, p.second ) );
      if ( !bounds.isEmpty() )
        areaGeom = QgsGeometry::fromRect( bounds );
    }
    if ( areaGeom.isNull() && !extent.isNull() )
    {
      if ( extent.isEmpty() )
        extent.grow( 50.0 );
      areaGeom = QgsGeometry::fromRect( extent );
    }
    if ( areaGeom.isNull() )
      areaGeom = QgsGeometry::fromRect( QgsRectangle( 0, 0, 10000, 10000 ) );

    if ( wellBand )
    {
      const QgsGeometry g = rectGeometry( ranges ? ranges->wells : RangeRect() );
      wellBand->setVisible( !g.isNull() );
      if ( !g.isNull() && g.isGeosValid() )
        wellBand->setToGeometry( g, surveyCrs );
    }
    if ( seismicBand )
    {
      const QgsGeometry g = rectGeometry( ranges ? ranges->seismic : RangeRect() );
      seismicBand->setVisible( !g.isNull() );
      if ( !g.isNull() && g.isGeosValid() )
        seismicBand->setToGeometry( g, surveyCrs );
    }
    if ( areaGeom.isGeosValid() )
      band->setToGeometry( areaGeom, surveyCrs );
    extent.combineExtentWith( areaGeom.boundingBox() );
    if ( extent.isEmpty() )
      extent = QgsRectangle( 0, 0, 10000, 10000 );
    extent.scale( 1.08 );
    c->setExtent( extent );
    c->refresh();
  }
}
