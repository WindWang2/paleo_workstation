// 层：功能
#include "mapversioncontroller.h"
#include "mappingsamples.h"

#include "../io/timedeptool.h"
#include "../io/wellfileparsers.h"
#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../services/projectdata.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gdal.h>

#include <cmath>
#include <limits>

#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

} // namespace

MapVersionController::MapVersionController( MapVersionStore *store, QgisLayerService *layers,
                                            QObject *parent )
  : QObject( parent )
  , m_store( store )
  , m_layers( layers )
{
}

MapVersion MapVersionController::saveVersion( const QString &horizon, const QVariantMap &provenance,
                                              QString *error )
{
  const QString json = QJsonDocument( QJsonObject::fromVariantMap( provenance ) )
                           .toJson( QJsonDocument::Compact );

  // 编辑会话 commit（§1223 版本提交边界）：该层位全部矢量图层逐一提交；
  // commitChanges 原生清 undo 栈，undoStack()->clear() 再兜底一次，保证
  // undo 不跨版本边界。栅格图层无编辑缓冲，跳过。
  if ( m_layers )
  {
    QVector<LayerDeclaration> declared;
    if ( !m_layers->tryDeclared( &declared, error ) )
      return MapVersion(); // 清单读失败时不得在未提交编辑的情况下出版本
    for ( const LayerDeclaration &d : declared )
    {
      if ( d.horizon != horizon )
        continue;
      if ( d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) != 0 )
        continue;
      QString instantiateErr;
      QgsMapLayer *layer = m_layers->instantiate( d.layerId, &instantiateErr );
      auto *vl = qobject_cast<QgsVectorLayer *>( layer );
      if ( !vl )
        continue; // 声明暂不可实例化（如源未落盘）不影响其他图层的提交
      if ( vl->isEditable() )
      {
        if ( m_editSvc )
        {
          if ( !m_editSvc->commitEdit( vl, error ) )
            return MapVersion();
        }
        else if ( m_projectStore )
        {
          const auto res = m_projectStore->enqueueWrite( [vl]() -> PaleoProjectStore::WriteResult {
            if ( !vl->commitChanges() )
              return { false, QObject::tr( "commitChanges failed for layer '%1'" ).arg( vl->id() ) };
            return { true, QString() };
          } );
          m_projectStore->markLayerFree( d.layerId );
          if ( !res.ok )
          {
            if ( error )
              *error = tr( "图层 %1 提交编辑失败：%2" ).arg( d.layerId, res.error );
            return MapVersion();
          }
        }
        else
        {
          if ( !vl->commitChanges() )
          {
            if ( error )
              *error = tr( "图层 %1 提交编辑失败：%2" )
                           .arg( d.layerId, vl->commitErrors().join( QLatin1Char( ';' ) ) );
            return MapVersion();
          }
        }
      }
      else if ( m_projectStore )
      {
        m_projectStore->markLayerFree( d.layerId );
      }
      vl->undoStack()->clear();
    }
  }

  MapVersion v = m_store ? m_store->saveVersion( horizon, json, error ) : MapVersion();
  if ( v.version > 0 )
    emit versionSaved( horizon, v.version );
  return v;
}

QString MapVersionController::publish( const QString &horizon, const QString &residualSummary,
                                       QString *error )
{
  if ( !m_store )
  {
    if ( error )
      *error = tr( "未绑定版本存储" );
    return QString();
  }
  QVector<LayerDeclaration> declared;
  if ( m_layers && !m_layers->tryDeclared( &declared, error ) )
    return QString(); // 清单读失败时不得发布空快照
  const QString dir = m_store->publish( horizon, declared, residualSummary, error );
  if ( !dir.isEmpty() )
    emit published( horizon, m_store->latest( horizon ).version, dir );
  return dir;
}

QString MapVersionController::residualSummaryJson( const ProjectDataFacade *projectData,
                                                   const QString &horizon )
{
  QJsonObject summary;
  summary.insert( QStringLiteral( "horizon" ), horizon );
  QJsonArray rows;
  QJsonArray missing;
  int total = 0, covered = 0;

  if ( projectData )
  {
    // 结构面栅格：走到采样步的井才需要它；无栅格的井记入 missing（缺的是
    // 残差本身，不是原因——发布门按「每口井都有残差或原因」卡住）。
    const HorizonRasterInfo raster = projectData->horizonRasterDecl( horizon );
    GDALAllRegister();
    GDALDatasetH ds = raster.valid
                          ? GDALOpen( raster.path.toUtf8().constData(), GA_ReadOnly )
                          : nullptr;

    for ( const ProjectWell &well : projectData->wells() )
    {
      ++total;
      QJsonObject row;
      row.insert( QStringLiteral( "well_id" ), well.id );
      row.insert( QStringLiteral( "name" ), well.name );
      row.insert( QStringLiteral( "kind" ), QStringLiteral( "reason" ) );

      const QVector<WellTop> tops = projectData->topsFor( well.id );
      const WellTop *pick = nullptr;
      for ( const WellTop &top : tops )
        if ( top.horizon == horizon )
          pick = &top;

      const QVector<TdSample> td =
          pick ? projectData->tdTableFor( well.id ) : QVector<TdSample>();
      if ( !pick || ( qIsNaN( pick->tvd ) && qIsNaN( pick->md ) ) )
      {
        row.insert( QStringLiteral( "reason" ),
                    tr( "无 %1 分层" ).arg( horizon ) );
      }
      else if ( td.isEmpty() )
      {
        row.insert( QStringLiteral( "reason" ), tr( "无时深表" ) );
      }
      else
      {
        const TimeDepthTool::TdResult tdResult = MappingSamples::timeForTop( td, *pick );
        if ( !tdResult.ok() )
        {
          row.insert( QStringLiteral( "reason" ),
                      TimeDepthTool::reasonText( tdResult.status ) );
        }
        else if ( !ds )
        {
          // 层位还没有时间栅格 — 残差无从评起，这口井不算覆盖。
          missing.append( well.name );
          row.insert( QStringLiteral( "reason" ),
                      tr( "层位 %1 还没有时间栅格" ).arg( horizon ) );
          rows.append( row );
          continue;
        }
        else
        {
          // T25：与验证表同一点——分层 X/Y 优先，井口兜底（不是永远井口）。
          double sx = well.surfaceX, sy = well.surfaceY;
          MappingSamples::pickSamplePoint( well, pick, &sx, &sy );
          double rasterMs = qQNaN();
          const MappingSamples::SampleOutcome cell = MappingSamples::sampleRasterAt( ds, sx, sy, &rasterMs );
          switch ( cell )
          {
            case MappingSamples::SampleOutcome::Outside:
              row.insert( QStringLiteral( "reason" ), tr( "井位不在测网内" ) );
              break;
            case MappingSamples::SampleOutcome::Nodata:
              row.insert( QStringLiteral( "reason" ), tr( "井位落在空道" ) );
              break;
            case MappingSamples::SampleOutcome::Ok:
              row.insert( QStringLiteral( "kind" ), QStringLiteral( "residual" ) );
              row.insert( QStringLiteral( "residual_ms" ), tdResult.timeMs - rasterMs );
              break;
          }
        }
      }
      ++covered;
      rows.append( row );
    }
    if ( ds )
      GDALClose( ds );
  }

  summary.insert( QStringLiteral( "wells_total" ), total );
  summary.insert( QStringLiteral( "covered" ), covered );
  summary.insert( QStringLiteral( "missing" ), missing );
  summary.insert( QStringLiteral( "rows" ), rows );
  return QString::fromUtf8( QJsonDocument( summary ).toJson( QJsonDocument::Compact ) );
}

// B 包 staleness-lite：发布门 advisory 的数据口径——遍历资产的全部版本，
// 只数 stage==DERIVED 且 extra["stale"] 为真的（RAW/INTERMEDIATE 不计；
// OUTPUT 是发布产物本身，不在「下游待重算」语义里）。只读，绝不改 catalog。
int MapVersionController::staleDerivedCount( const DataCatalog *catalog )
{
  if ( !catalog || !catalog->isOpen() )
    return 0;
  int stale = 0;
  for ( const CatalogAsset &a : catalog->assets() )
    for ( const CatalogVersion &v : catalog->versionsForAsset( a.id ) )
      if ( v.stage == QLatin1String( "DERIVED" ) &&
           v.extra.value( QStringLiteral( "stale" ) ).toBool() )
        ++stale;
  return stale;
}

QString MapVersionController::stalePublishAdvisory( const DataCatalog *catalog )
{
  const int stale = staleDerivedCount( catalog );
  if ( stale <= 0 )
    return QString();
  // advisory 文案（规格定稿）：如实计数 + 明确不阻断——可见但不拦发布。
  return QObject::tr( "存在过时下游产物（%1 个）——不阻断本次发布，请确认后继续" )
      .arg( stale );
}
