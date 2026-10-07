// 层：功能
#include "horizonbatchexport.h"

#include "mapexport.h" // registerMapPdfAsset：图件 → catalog OUTPUT 登记

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutundostack.h>
#include <qgsmaplayer.h>
#include <qgsprintlayout.h>
#include <qgsrectangle.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>

namespace
{
  // 文件名段消毒：路径非法字符与空白替换为 '_'，空段回 "untitled"。
  QString safeSegment( const QString &raw )
  {
    QString s = raw;
    for ( const QChar ch : { QLatin1Char( '/' ), QLatin1Char( '\\' ), QLatin1Char( ':' ),
                             QLatin1Char( '*' ), QLatin1Char( '?' ), QLatin1Char( '"' ),
                             QLatin1Char( '<' ), QLatin1Char( '>' ), QLatin1Char( '|' ),
                             QLatin1Char( ' ' ), QLatin1Char( '\t' ) } )
      s.replace( ch, QLatin1Char( '_' ) );
    s.remove( QLatin1Char( '\n' ) ).remove( QLatin1Char( '\r' ) ).simplified();
    return s.isEmpty() ? QStringLiteral( "untitled" ) : s;
  }

  QString extensionFor( PaleoHorizonBatchExport::Format format )
  {
    switch ( format )
    {
      case PaleoHorizonBatchExport::Format::Png: return QStringLiteral( "png" );
      case PaleoHorizonBatchExport::Format::Pdf: return QStringLiteral( "pdf" );
      case PaleoHorizonBatchExport::Format::Svg: return QStringLiteral( "svg" );
    }
    return QStringLiteral( "png" );
  }

  QgsRectangle combinedExtent( const QList<QgsMapLayer *> &layers, QgsLayoutItemMap *map )
  {
    QgsRectangle extent;
    for ( QgsMapLayer *layer : layers )
    {
      if ( !layer )
        continue;
      if (layer->customProperty("paleoBasemap").toBool()) continue;
      const auto bounds = QgsCoordinateTransform(layer->crs(), map->crs(), map->layout()->project()).transformBoundingBox(layer->extent());
      if ( extent.isNull() )
        extent = bounds;
      else
        extent.combineExtentWith( bounds );
    }
    return extent;
  }
} // namespace

namespace PaleoHorizonBatchExport
{

QMap<QString, QList<QgsMapLayer *>> resolveHorizonLayers( QgisLayerService *layers,
                                                          QStringList *horizonsOut )
{
  QMap<QString, QList<QgsMapLayer *>> out;
  if ( horizonsOut )
    horizonsOut->clear();
  if ( !layers )
    return out;

  QVector<LayerDeclaration> declared;
  QString err;
  if ( !layers->tryDeclared( &declared, &err ) )
    return out;

  for ( const LayerDeclaration &d : declared )
  {
    if ( d.horizon.isEmpty() )
      continue; // 与层位无关的图层不进批量出图
    QgsMapLayer *layer = layers->instantiate( d.layerId );
    if ( !layer )
      continue; // 实例化失败如实少一层；整层位空集由 run 入账
    out[d.horizon].append( layer );
    if ( horizonsOut && !horizonsOut->contains( d.horizon ) )
      horizonsOut->append( d.horizon );
  }
  return out;
}

QString Result::summary() const
{
  return QObject::tr( "批量出图：%1 层位，成功 %2、失败 %3" ).arg( total ).arg( succeeded ).arg( failed );
}

Result run( const Request &request )
{
  Result result;
  result.total = request.horizonLayers.size();

  const auto bailAll = [ &result, &request ]( const QString &why )
  {
    HorizonOutcome outcome;
    outcome.ok = false;
    outcome.error = why;
    for ( const QString &horizon : request.horizonLayers.keys() )
    {
      outcome.horizon = horizon;
      result.horizons.append( outcome );
      ++result.failed;
    }
    return result;
  };

  if ( !request.layout )
    return bailAll( QObject::tr( "未提供版面" ) );
  if ( !request.catalog || request.projectDir.isEmpty() )
    return bailAll( QObject::tr( "未绑定 catalog 受管区（图件纪律：不产游离文件）" ) );
  if ( request.horizonLayers.isEmpty() )
    return result; // total=0：没有层位组，不是错误

  // 批处理是版面骨架的原子操作：不进 undo 历史。
  if ( request.layout->undoStack() )
    request.layout->undoStack()->blockCommands( true );

  // ---- 版面原状快照（地图项图层/范围/跟随态 + 标题文本）------------------
  QList<QgsLayoutItemMap *> maps;
  request.layout->layoutItems( maps );
  struct MapState
  {
    QgsLayoutItemMap *map = nullptr;
    QList<QgsMapLayer *> layers;
    QgsRectangle extent;
    bool followPreset = false;
  };
  QVector<MapState> states;
  for ( QgsLayoutItemMap *map : maps )
    states.append( { map, map->layers(), map->extent(), map->followVisibilityPreset() } );

  auto *title = qobject_cast<QgsLayoutItemLabel *>(
      request.layout->itemById( QStringLiteral( "title" ) ) );
  const QString titleBackup = title ? title->text() : QString();

  const QString project = safeSegment( request.projectName.isEmpty()
                                           ? QStringLiteral( "project" )
                                           : request.projectName );
  const QString date =
      request.date.isEmpty() ? QDate::currentDate().toString( QStringLiteral( "yyyyMMdd" ) )
                             : request.date;
  const QString ext = extensionFor( request.format );

  const QStringList horizons = request.horizonLayers.keys();

  QTemporaryDir scratch;
  for ( const QString &horizon : horizons )
  {
    HorizonOutcome outcome;
    outcome.horizon = horizon;

    const QList<QgsMapLayer *> layers = request.horizonLayers.value( horizon );
    if ( layers.isEmpty() )
    {
      outcome.error = QObject::tr( "层位 %1 没有可出图的图层" ).arg( horizon );
      result.horizons.append( outcome );
      ++result.failed;
      continue;
    }
    if ( maps.isEmpty() )
    {
      outcome.error = QObject::tr( "版面中没有地图项" );
      result.horizons.append( outcome );
      ++result.failed;
      continue;
    }

    // ---- 换内容：层集快照 + 全幅范围 + 标题 -------------------------------
    bool extentFailed = false;
    for ( QgsLayoutItemMap *map : maps )
    {
      map->setFollowVisibilityPreset( false ); // 批量出图即快照语义
      map->setLayers( layers );
      try {
        const auto extent = combinedExtent(layers, map);
        if (!extent.isNull() && !extent.isEmpty()) map->setExtent(extent);
      } catch (const QgsCsException &ex) { outcome.error = ex.what(); extentFailed = true; break; }
    }
    if (extentFailed) { result.horizons.append(outcome); ++result.failed; continue; }
    if ( title )
      title->setText( QObject::tr( "%1 · %2" ).arg( request.projectName.isEmpty()
                                                         ? QStringLiteral( "project" )
                                                         : request.projectName,
                                                     horizon ) );

    // ---- 导出到临时文件（文件名即最终受管文件名）--------------------------
    const QString baseName = QStringLiteral( "%1_%2_%3" ).arg( project, safeSegment( horizon ), date );
    const QString tmpPath = scratch.isValid()
                                ? scratch.filePath( baseName + QLatin1Char( '.' ) + ext )
                                : QDir::temp().filePath( baseName + QLatin1Char( '.' ) + ext );

    const auto exported = PaleoLayoutExport::exportLayout( request.layout, tmpPath, request.format,
                                                           request.dpi, PaleoLayoutExport::PageRange() );
    if ( !exported.ok )
    {
      outcome.error = exported.error;
      result.horizons.append( outcome );
      ++result.failed;
      continue;
    }

    // ---- 登记 catalog（OUTPUT；受管副本即成品，不留游离文件）----------------
    QString sha, managed, err;
    const QString assetId = registerMapPdfAsset( request.catalog, request.projectDir,
                                                 exported.files.value( 0 ), &sha, &managed,
                                                 &err, ext );
    if ( assetId.isEmpty() )
    {
      outcome.error = err.isEmpty() ? QObject::tr( "catalog 登记失败：%1" ).arg( baseName ) : err;
      result.horizons.append( outcome );
      ++result.failed;
      continue;
    }

    outcome.ok = true;
    outcome.file = managed;
    outcome.assetId = assetId;
    outcome.sha256 = sha;
    result.horizons.append( outcome );
    ++result.succeeded;
  }

  // ---- 版面复原 -----------------------------------------------------------
  for ( const MapState &state : states )
  {
    if ( !state.map )
      continue;
    state.map->setFollowVisibilityPreset( state.followPreset );
    state.map->setLayers( state.layers );
    state.map->setExtent( state.extent );
  }
  if ( title )
    title->setText( titleBackup );
  if ( request.layout->undoStack() )
    request.layout->undoStack()->blockCommands( false );

  return result;
}

} // namespace PaleoHorizonBatchExport
