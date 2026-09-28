// 层：功能
#include "mapversioncontroller.h"

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

  // 井/分层的采样点：分层 X/Y 优先，缺省退井口——与 mappingworkflow.cpp
  // pickSamplePoint 同一份逻辑（T25：发布门与验证表必须采同一个点）。
  void pickSamplePointForWell( const ProjectWell &well, const WellTop *top,
                               double *x, double *y )
  {
    if ( top && std::isfinite( top->x ) && std::isfinite( top->y ) )
    {
      *x = top->x;
      *y = top->y;
    }
    else
    {
      *x = well.surfaceX;
      *y = well.surfaceY;
    }
  }

  // 包含像元采样（左闭右开；恰在外边界归末像元，1 ULP 容差）——与
  // mappingworkflow.cpp sampleRasterAt 同一套算法，逐行对齐勿分叉：
  // 发布门残差必须与验证表同口径（T25），仅返回值分支命名不同。
  CellSample sampleCellAt( GDALDatasetH ds, double x, double y, double *value )
  {
    if ( !std::isfinite( x ) || !std::isfinite( y ) )
      return CellSample::Outside;
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    GDALGetGeoTransform( ds, gt );
    const int cols = GDALGetRasterXSize( ds );
    const int rows = GDALGetRasterYSize( ds );
    int col = static_cast<int>( std::floor( ( x - gt[0] ) / gt[1] ) );
    int row = static_cast<int>( std::floor( ( y - gt[3] ) / gt[5] ) );
    const double xmax = gt[0] + gt[1] * cols;
    const double ymin = gt[3] + gt[5] * rows;
    const double ulpX =
        std::nextafter( xmax, std::numeric_limits<double>::infinity() ) - xmax;
    const double ulpY =
        std::nextafter( ymin, std::numeric_limits<double>::infinity() ) - ymin;
    if ( col == cols && qAbs( x - xmax ) <= ulpX )
      col = cols - 1; // 恰在外边界 → 最后一列
    if ( row == rows && qAbs( y - ymin ) <= ulpY )
      row = rows - 1; // 恰在外边界 → 最后一行
    if ( col < 0 || row < 0 || col >= cols || row >= rows )
      return CellSample::Outside;
    if ( GDALGetRasterCount( ds ) < 1 )
      return CellSample::Nodata;
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    if ( !band )
      return CellSample::Nodata;
    float v = 0.0f;
    if ( GDALRasterIO( band, GF_Read, col, row, 1, 1, &v, 1, 1, GDT_Float32, 0, 0 ) != CE_None )
      return CellSample::Nodata;
    int hasNodata = 0;
    const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
    if ( std::isnan( v ) || ( hasNodata && qAbs( static_cast<double>( v ) - nodata ) < 1e-6 ) )
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
          // T25：与验证表同一点——分层 X/Y 优先，井口兜底（不是永远井口）。
          double sx = well.surfaceX, sy = well.surfaceY;
          pickSamplePointForWell( well, pick, &sx, &sy );
          double rasterMs = qQNaN();
          const CellSample cell = sampleCellAt( ds, sx, sy, &rasterMs );
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
