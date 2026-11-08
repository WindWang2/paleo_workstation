// 层：QGIS 封装
#include "mapbookrender.h"

#include "mapbooklayout.h"

#include <QObject>

#include <qgsmaplayer.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>

namespace PaleoMapBookRender
{

namespace
{
  // 共同的建版 → 渲染 → 销毁骨架：版面无论如何都由本层 delete，
  // 不再把裸 QgsPrintLayout 交出封装层。
  template<typename SpecT, typename BuildFn>
  PaleoMapBookLayout::RenderOutcome renderWith( QgsProject *project, BuildFn build,
                                                const SpecT &spec, const QString &outPath,
                                                PaleoLayoutExport::Format format, double dpi )
  {
    PaleoMapBookLayout::RenderOutcome outcome;
    QString buildError;
    QgsPrintLayout *layout = build( project, spec, &buildError );
    if ( !layout )
    {
      // 保留既有失败阶段区分（tst_mapbook 断言 stage=="layout"）：建版失败
      // ≠ 渲染失败，页账本/manifest 的 stage 语义不变。
      outcome.stage = QStringLiteral( "layout" );
      outcome.error = buildError.isEmpty() ? QObject::tr( "版面构建失败" ) : buildError;
      return outcome;
    }
    outcome = PaleoMapBookLayout::renderLayout( layout, outPath, format, dpi );
    delete layout;
    return outcome;
  }
} // namespace

PaleoMapBookLayout::RenderOutcome renderTile( QgsProject *project, const TileJob &job,
                                              const QString &outPath,
                                              PaleoLayoutExport::Format format, double dpi )
{
  PaleoMapBookLayout::TileSpec spec;
  spec.title = job.title;
  spec.footer = job.footer;
  spec.extent = QgsRectangle( job.xMin, job.yMin, job.xMax, job.yMax );
  spec.layers = job.layers;
  spec.landscape = job.landscape;
  spec.legend = job.legend;
  spec.scaleBar = job.scaleBar;
  spec.northArrow = job.northArrow;
  spec.crsCaption = job.crsCaption;
  return renderWith( project, PaleoMapBookLayout::buildTileLayout, spec, outPath, format, dpi );
}

PaleoMapBookLayout::RenderOutcome renderIndex( QgsProject *project, const IndexJob &job,
                                               const QString &outPath,
                                               PaleoLayoutExport::Format format, double dpi )
{
  PaleoMapBookLayout::IndexSpec spec;
  spec.title = job.title;
  spec.entries = job.entries;
  spec.landscape = job.landscape;
  return renderWith( project, PaleoMapBookLayout::buildIndexLayout, spec, outPath, format, dpi );
}

QString layerStableId( const QgsMapLayer *layer )
{
  return layer ? layer->id() : QString();
}

QgsMapLayer *projectLayerById( QgsProject *project, const QString &id )
{
  return project ? project->mapLayer( id ) : nullptr;
}

LayerSummary summarizeLayer( const QgsMapLayer *layer )
{
  LayerSummary summary;
  if ( !layer )
    return summary;
  summary.id = layer->id();
  summary.name = layer->name();
  switch ( layer->type() )
  {
    case Qgis::LayerType::Vector: summary.type = QStringLiteral( "vector" ); break;
    case Qgis::LayerType::Raster: summary.type = QStringLiteral( "raster" ); break;
    case Qgis::LayerType::Plugin: summary.type = QStringLiteral( "plugin" ); break;
    case Qgis::LayerType::Annotation: summary.type = QStringLiteral( "annotation" ); break;
    case Qgis::LayerType::Mesh: summary.type = QStringLiteral( "mesh" ); break;
    case Qgis::LayerType::VectorTile: summary.type = QStringLiteral( "vector-tile" ); break;
    case Qgis::LayerType::PointCloud: summary.type = QStringLiteral( "point-cloud" ); break;
    case Qgis::LayerType::TiledScene: summary.type = QStringLiteral( "tiled-scene" ); break;
    case Qgis::LayerType::Group: summary.type = QStringLiteral( "group" ); break;
  }
  if ( layer->crs().isValid() )
    summary.crs = layer->crs().authid();
  const auto property = [layer]( const char *key ) {
    return layer->customProperty( key ).toString();
  };
  summary.paleoId = property( "paleoLayerId" );
  summary.assetId = property( "paleoAssetId" );
  summary.versionId = property( "paleoVersionId" );
  summary.source = layer->publicSource();
  return summary;
}

QString projectCrsAuthid( const QgsProject *project )
{
  if ( !project || !project->crs().isValid() )
    return QString();
  return project->crs().authid();
}

} // namespace PaleoMapBookRender
