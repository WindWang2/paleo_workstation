// 层：QGIS 封装
#include "dashboardsurveymap.h"
#include "previewmapcanvas.h"
#include "../catalog/datacatalog.h"

#include <qgscategorizedsymbolrenderer.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmaptoolpan.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgspallabeling.h>
#include <qgsrectangle.h>
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

  // 与 datapreviewtabs_internal.h styleSurveyWellLayer/applyPointNameLabels 同图式
  //（Q/HS 1011—2016 表 K.1 探井靶标 + 白晕井名）。
  void styleWells( QgsVectorLayer *vl, const DashboardSurveyMap::Style &st )
  {
    const QString hex = st.primary.name();
    QVariantMap ring;
    ring[QStringLiteral( "name" )] = QStringLiteral( "circle" );
    ring[QStringLiteral( "color" )] = QStringLiteral( "255,255,255,0" );
    ring[QStringLiteral( "outline_color" )] = hex;
    ring[QStringLiteral( "outline_width" )] = QStringLiteral( "0.5" );
    ring[QStringLiteral( "size" )] = QStringLiteral( "4.5" );
    std::unique_ptr<QgsMarkerSymbol> sym = QgsMarkerSymbol::createSimple( ring );
    QVariantMap disk;
    disk[QStringLiteral( "name" )] = QStringLiteral( "circle" );
    disk[QStringLiteral( "color" )] = hex;
    disk[QStringLiteral( "outline_style" )] = QStringLiteral( "no" );
    disk[QStringLiteral( "size" )] = QStringLiteral( "3.3" );
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
        QStringLiteral( "Point?crs=WKT:%1&field=name:string(64)&field=role:string(16)&field=entity_id:string(128)&index=yes" )
            .arg( DataCatalog::localGridCrsWkt() ),
        style.wellsLayerName, QStringLiteral( "memory" ) );
    vl->setParent( map ); // 画布只引用裸指针，层归宿主
    vl->setObjectName( QString::fromLatin1( kWells ) );
    styleWells( vl, style );
    c->setLayers( { vl } );
    return map;
  }

  QWidget *canvasWidget( QWidget *map )
  {
    auto *m = qobject_cast<PreviewMapCanvas *>( map );
    return m ? m->canvas() : nullptr;
  }

  void update( QWidget *map, const QVector<CatalogEntity> &wells )
  {
    auto *m = qobject_cast<PreviewMapCanvas *>( map );
    if ( !m )
      return;
    QgsMapCanvas *c = m->canvas();
    auto *vl = m->findChild<QgsVectorLayer *>( QString::fromLatin1( kWells ) );
    if ( !vl )
      return;
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
    // 工区范围带（原由地震测区角点/井位并集拟合）按用户要求撤下，看板小地图
    // 只留井位；取景随之只按井位并集，无井时 10 km 默认幅面。
    if ( extent.isNull() || extent.isEmpty() )
    {
      if ( extent.isNull() )
        extent = QgsRectangle( 0, 0, 10000, 10000 );
      else
        extent.grow( 50.0 );
    }
    extent.scale( 1.08 );
    c->setExtent( extent );
    c->refresh();
  }
}
