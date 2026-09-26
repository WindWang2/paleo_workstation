#include "mapversioncontroller.h"

#include "../io/timedeptool.h"
#include "../io/wellfileparsers.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../services/projectdata.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gdal.h>

#include <cmath>

#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

  // TD 表 → TIME(ms)：TimeDepthTool（文件顺序、不排序、不外推）。分层 TVD
  // 有效时查 TVD 列；TVD 空改用 MD 对 MD 列兜底——与 mappingworkflow.cpp
  // 的残差口径共用同一个插值器/同一组原因文案。
  TimeDepthTool::TdResult tdResultForTop( const QVector<TdSample> &td, const WellTop &top )
  {
    TimeDepthTool::TdResult none; // status=NoTable
    if ( td.isEmpty() )
      return none;
    TimeDepthTable table;
    table.rows.reserve( td.size() );
    for ( const TdSample &s : td )
    {
      TdRow row;
      row.timeMs = s.timeMs;
      row.tvd = s.tvd;
      row.md = s.md;
      row.hasTvd = !qIsNaN( s.tvd ); // NaN 透传 -99999/缺列 → 不进插值
      row.hasMd = !qIsNaN( s.md );
      table.rows.append( row );
    }
    if ( !qIsNaN( top.tvd ) )
      return TimeDepthTool::interpolateTimeMs( table, top.tvd, /*useMd=*/false );
    if ( !qIsNaN( top.md ) )
      return TimeDepthTool::interpolateTimeMs( table, top.md, /*useMd=*/true );
    return none;
  }

  enum class CellSample
  {
    Outside, // 井位落在测网矩形之外
    Nodata,  // 包含像元是空道（nodata）
    Value,   // 采到数值
  };

  // 包含像元采样（左闭右开：x==xmax 归最后一列）——与残差检查的采样同口径，
  // 但区分「不在测网内」与「空道」两种原因。
  CellSample sampleCellAt( GDALDatasetH ds, double x, double y, double *value )
  {
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    GDALGetGeoTransform( ds, gt );
    const int col = static_cast<int>( std::floor( ( x - gt[0] ) / gt[1] ) );
    const int row = static_cast<int>( std::floor( ( gt[3] - y ) / -gt[5] ) );
    if ( col < 0 || row < 0 || col >= GDALGetRasterXSize( ds ) || row >= GDALGetRasterYSize( ds ) )
      return CellSample::Outside;
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    float v = 0.0f;
    if ( GDALRasterIO( band, GF_Read, col, row, 1, 1, &v, 1, 1, GDT_Float32, 0, 0 ) != CE_None )
      return CellSample::Nodata;
    int hasNodata = 0;
    const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
    if ( hasNodata && qAbs( static_cast<double>( v ) - nodata ) < 1e-6 )
      return CellSample::Nodata;
    if ( value )
      *value = v;
    return CellSample::Value;
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
      if ( vl->isEditable() && !vl->commitChanges() )
      {
        if ( error )
          *error = tr( "图层 %1 提交编辑失败：%2" )
                       .arg( d.layerId, vl->commitErrors().join( QLatin1Char( ';' ) ) );
        return MapVersion();
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

QString MapVersionController::residualSummaryJson( const ProjectDataFacade *pd,
                                                   const QString &horizon )
{
  QJsonObject summary;
  summary.insert( QStringLiteral( "horizon" ), horizon );
  QJsonArray rows;
  QJsonArray missing;
  int total = 0, covered = 0;

  if ( pd )
  {
    // 结构面栅格：走到采样步的井才需要它；无栅格的井记入 missing（缺的是
    // 残差本身，不是原因——发布门按「每口井都有残差或原因」卡住）。
    const HorizonRasterInfo raster = pd->horizonRasterDecl( horizon );
    GDALAllRegister();
    GDALDatasetH ds = raster.valid
                          ? GDALOpen( raster.path.toUtf8().constData(), GA_ReadOnly )
                          : nullptr;

    for ( const ProjectWell &well : pd->wells() )
    {
      ++total;
      QJsonObject row;
      row.insert( QStringLiteral( "well_id" ), well.id );
      row.insert( QStringLiteral( "name" ), well.name );
      row.insert( QStringLiteral( "kind" ), QStringLiteral( "reason" ) );

      const QVector<WellTop> tops = pd->topsFor( well.id );
      const WellTop *pick = nullptr;
      for ( const WellTop &top : tops )
        if ( top.horizon == horizon )
          pick = &top;

      const QVector<TdSample> td =
          pick ? pd->tdTableFor( well.id ) : QVector<TdSample>();
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
        const TimeDepthTool::TdResult tdResult = tdResultForTop( td, *pick );
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
          double rasterMs = qQNaN();
          const CellSample cell = sampleCellAt( ds, well.surfaceX, well.surfaceY, &rasterMs );
          switch ( cell )
          {
            case CellSample::Outside:
              row.insert( QStringLiteral( "reason" ), tr( "井位不在测网内" ) );
              break;
            case CellSample::Nodata:
              row.insert( QStringLiteral( "reason" ), tr( "井位落在空道" ) );
              break;
            case CellSample::Value:
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
