#include "mappingworkflow.h"

#include "../domain/mappinghorizons.h"
#include "../io/timedeptool.h"
#include "../io/wellfileparsers.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../services/projectdata.h"
#include "workflows.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gdal.h>

#include <cmath>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

  // TD 表 → TIME(ms)：走数据底座的 TimeDepthTool（plan §3：文件顺序、不排序、
  // 不外推）。分层 TVD 有效时查 TVD 列；TVD 空（-99999）改用 MD 对 MD 列。
  // TdResult.status 区分 无时深表/超出时深表/时深表无序，文案经
  // TimeDepthTool::reasonText() 取——剖面标层与残差共用这一个结果。
  TimeDepthTool::TdResult timeForTop( const QVector<TdSample> &td, const WellTop &top )
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
    if ( !qIsNaN( top.md ) ) // 分层 TVD 空 → MD 对 TD 的 MD 列兜底（plan §3）
      return TimeDepthTool::interpolateTimeMs( table, top.md, /*useMd=*/true );
    return none; // 分层两个深度都没有 → 按无时深表处理
  }
} // namespace

MappingWorkflow::MappingWorkflow( ConstraintWorkflow *constraints, CompositionWorkflow *compose,
                                  QgisLayerService *layers, QObject *parent )
  : QObject( parent )
  , m_constraints( constraints )
  , m_compose( compose )
  , m_layers( layers )
{
}

void MappingWorkflow::setProjectData( ProjectDataFacade *projectData )
{
  m_projectData = projectData;
}

QVector<ThicknessPoint> MappingWorkflow::computeThickness( const QString &horizon,
                                                           const QString &baseHorizon,
                                                           int *skipped, QString *error )
{
  if ( skipped )
    *skipped = 0;
  if ( !m_projectData )
  {
    setError( error, tr( "未绑定项目数据（读侧门面）" ) );
    return {};
  }
  if ( horizon.isEmpty() || baseHorizon.isEmpty() )
  {
    setError( error, tr( "厚度计算需要层位与基面（%1→%2）" ).arg( horizon, baseHorizon ) );
    return {};
  }

  QVector<ThicknessPoint> points;
  for ( const ProjectWell &well : m_projectData->wells() )
  {
    const QVector<WellTop> tops = m_projectData->topsFor( well.id );
    double tvdTop = qQNaN();
    double tvdBase = qQNaN();
    for ( const WellTop &top : tops )
    {
      if ( top.horizon == horizon )
        tvdTop = top.tvd;
      else if ( top.horizon == baseHorizon )
        tvdBase = top.tvd;
    }
    // 缺任一分层 → 跳过并计数，不造厚度（与“Time 列不填假时间”同一纪律）。
    if ( qIsNaN( tvdTop ) || qIsNaN( tvdBase ) )
    {
      if ( skipped )
        ++( *skipped );
      continue;
    }

    ThicknessPoint p;
    p.wellId = well.id;
    p.wellName = well.name;
    p.x = well.surfaceX;
    p.y = well.surfaceY;
    p.thickness = tvdBase - tvdTop; // 基面更深，正值
    points.append( p );
  }
  return points;
}

bool MappingWorkflow::runThicknessChain( const QString &horizon, QString *error )
{
  const auto fail = [this, &horizon, error]( const QString &msg ) {
    setError( error, msg );
    emit chainFailed( horizon, msg );
    return false;
  };

  if ( !m_projectData )
    return fail( tr( "未绑定项目数据（读侧门面）" ) );
  if ( !m_constraints || !m_compose || !m_layers )
    return fail( tr( "编图工作流未绑定服务" ) );

  // 结构面：D61 派生时间栅格（登记进 LayerManifest）。像元尺度取测网像元。
  const HorizonRasterInfo raster = m_projectData->horizonRasterDecl( horizon );
  if ( !raster.valid )
    return fail( tr( "层位 %1 没有已登记的时间栅格" ).arg( horizon ) );

  const QString base = baseHorizonFor( horizon );
  if ( base.isEmpty() )
    return fail( tr( "层位 %1 没有厚度基面" ).arg( horizon ) );

  int skipped = 0;
  QString thicknessError;
  const QVector<ThicknessPoint> points =
      computeThickness( horizon, base, &skipped, &thicknessError );
  if ( !thicknessError.isEmpty() )
    return fail( thicknessError );
  if ( points.isEmpty() )
    return fail( tr( "层位 %1 没有可用的厚度控制点（%2 口井缺 %3 分层）" )
                     .arg( horizon )
                     .arg( skipped )
                     .arg( base ) );

  // 厚度点层：GeoJSON（ogr 可直接实例化），落在系统临时目录，保存工程时
  // 由 PaleoProjectStore 写队列按 temp-then-merge 纪律提升（§41.2）。
  const QString stamp = QDateTime::currentDateTimeUtc().toString( QStringLiteral( "yyyyMMdd-hhmmss-zzz" ) );
  const QString pointsPath = QDir::temp().filePath(
      QStringLiteral( "paleo_thickness_%1_%2.geojson" ).arg( horizon, stamp ) );
  {
    QJsonArray features;
    for ( const ThicknessPoint &p : points )
    {
      QJsonObject props;
      props.insert( QStringLiteral( "well_id" ), p.wellId );
      props.insert( QStringLiteral( "well_name" ), p.wellName );
      props.insert( QStringLiteral( "thickness" ), p.thickness );
      QJsonArray coord = { p.x, p.y };
      QJsonObject geom;
      geom.insert( QStringLiteral( "type" ), QStringLiteral( "Point" ) );
      geom.insert( QStringLiteral( "coordinates" ), coord );
      QJsonObject feature;
      feature.insert( QStringLiteral( "type" ), QStringLiteral( "Feature" ) );
      feature.insert( QStringLiteral( "properties" ), props );
      feature.insert( QStringLiteral( "geometry" ), geom );
      features.append( feature );
    }
    QJsonObject fc;
    fc.insert( QStringLiteral( "type" ), QStringLiteral( "FeatureCollection" ) );
    fc.insert( QStringLiteral( "features" ), features );

    QFile f( pointsPath );
    if ( !f.open( QIODevice::WriteOnly ) )
      return fail( tr( "无法写厚度点临时文件 %1" ).arg( pointsPath ) );
    f.write( QJsonDocument( fc ).toJson( QJsonDocument::Compact ) );
    f.close();
  }

  const QString pointsLayerId = QStringLiteral( "wells.thickness.%1" ).arg( horizon );
  LayerDeclaration decl;
  decl.layerId = pointsLayerId;
  decl.horizon = horizon;
  decl.type = QStringLiteral( "vector" );
  decl.source = pointsPath;
  decl.group = QStringLiteral( "00_Wells" );
  if ( !m_layers->declare( decl, error ) )
    return fail( error ? *error : tr( "无法声明厚度点图层 %1" ).arg( pointsLayerId ) );

  // 约束 IDW：CELL_SIZE = 测网像元；constraints.<h> 图层存在时算法沿用凸包裁剪。
  if ( !m_constraints->runConstraintIDW( horizon, pointsLayerId,
                                         QStringLiteral( "thickness" ), raster.cellSize, error ) )
  {
    return fail( error ? *error : tr( "约束 IDW 失败" ) );
  }

  // 相多边形：厚度栅格 → paleo:paleo_facies_polygonize → facies.<h>。
  if ( !m_compose->deriveFaciesPolygons(
           horizon, QStringLiteral( "factor.%1.idw" ).arg( horizon ), QVariantMap(), error ) )
  {
    return fail( error ? *error : tr( "相多边形转换失败" ) );
  }

  emit chainDone( horizon, QStringLiteral( "facies.%1" ).arg( horizon ) );
  return true;
}

// ---------------------------------------------------------------------------
// 时间残差 — 阶段C验证（§5C）
// ---------------------------------------------------------------------------

namespace
{
  // 最近像元采样 — NaN 表示井位在栅格外或落在 nodata 上。
  double sampleRasterAt( GDALDatasetH ds, double x, double y )
  {
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    GDALGetGeoTransform( ds, gt );
    const int col = static_cast<int>( std::floor( ( x - gt[0] ) / gt[1] ) );
    const int row = static_cast<int>( std::floor( ( gt[3] - y ) / -gt[5] ) );
    if ( col < 0 || row < 0 || col >= GDALGetRasterXSize( ds ) || row >= GDALGetRasterYSize( ds ) )
      return qQNaN();
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    float v = 0.0f;
    if ( GDALRasterIO( band, GF_Read, col, row, 1, 1, &v, 1, 1, GDT_Float32, 0, 0 ) != CE_None )
      return qQNaN();
    int hasNodata = 0;
    const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
    if ( hasNodata && qAbs( static_cast<double>( v ) - nodata ) < 1e-6 )
      return qQNaN();
    return v;
  }
} // namespace

QList<ValidationIssue> computeTimeResiduals( const ProjectDataFacade *projectData,
                                             const QString &horizon, double thresholdMs )
{
  QList<ValidationIssue> issues;
  if ( !projectData )
    return issues;

  const HorizonRasterInfo raster = projectData->horizonRasterDecl( horizon );
  if ( !raster.valid )
    return issues; // 无结构面栅格 → 本检查静默不适用（validate 的其他检查照跑）

  GDALAllRegister();
  GDALDatasetH ds = GDALOpen( raster.path.toUtf8().constData(), GA_ReadOnly );
  if ( !ds )
    return issues;

  const bool hasInline = raster.inlineMin >= 0 && raster.inlineMax > raster.inlineMin;
  for ( const ProjectWell &well : projectData->wells() )
  {
    const QVector<WellTop> tops = projectData->topsFor( well.id );
    const WellTop *pick = nullptr;
    for ( const WellTop &top : tops )
      if ( top.horizon == horizon )
        pick = &top;
    // 该井没有此层位分层，或分层 TVD/MD 皆空 → 无残差可评。
    if ( !pick || ( qIsNaN( pick->tvd ) && qIsNaN( pick->md ) ) )
      continue;

    const QVector<TdSample> td = projectData->tdTableFor( well.id );
    if ( td.isEmpty() )
    {
      ValidationIssue v;
      v.severity = ValidationIssue::Info;
      v.code = QStringLiteral( "NO_TD_TABLE" );
      v.message = QObject::tr( "井 %1 无时深表，%2 时间残差无法计算" ).arg( well.name, horizon );
      v.horizon = horizon;
      v.wellId = well.id;
      v.wktLocation = QStringLiteral( "POINT(%1 %2)" ).arg( well.surfaceX ).arg( well.surfaceY );
      issues.append( v );
      continue;
    }

    const TimeDepthTool::TdResult tdResult = timeForTop( td, *pick );
    const double rasterMs = sampleRasterAt( ds, well.surfaceX, well.surfaceY );
    // TD 不给出值（无时深表/超出时深表/时深表无序，见 tdResult.status 与
    // TimeDepthTool::reasonText()）或井位无栅格采样（如凸包外）→ 不评，不造数。
    if ( !tdResult.ok() || qIsNaN( rasterMs ) )
      continue;

    const double wellTimeMs = tdResult.timeMs;

    const double residual = wellTimeMs - rasterMs;
    if ( std::abs( residual ) <= thresholdMs )
      continue;

    ValidationIssue v;
    v.severity = ValidationIssue::Warning;
    v.code = QStringLiteral( "TIME_RESIDUAL" );
    int inlineNo = -1;
    QString inlineText;
    if ( hasInline )
    {
      inlineNo = static_cast<int>( std::lround(
          raster.inlineMin + ( well.surfaceX - raster.xmin ) / ( raster.xmax - raster.xmin )
                                  * ( raster.inlineMax - raster.inlineMin ) ) );
      inlineText = QObject::tr( "，目标测线 %1" ).arg( inlineNo );
    }
    v.message = QObject::tr( "井 %1 %2 时间残差 %3ms（井 %4ms vs 栅格 %5ms%6）" )
                    .arg( well.name, horizon )
                    .arg( residual, 0, 'f', 1 )
                    .arg( wellTimeMs, 0, 'f', 1 )
                    .arg( rasterMs, 0, 'f', 1 )
                    .arg( inlineText );
    v.layerId = raster.layerId;
    v.horizon = horizon;
    v.wktLocation = QStringLiteral( "POINT(%1 %2)" ).arg( well.surfaceX ).arg( well.surfaceY );
    v.wellId = well.id;
    v.details.insert( QStringLiteral( "well_name" ), well.name );
    v.details.insert( QStringLiteral( "well_x" ), well.surfaceX );
    v.details.insert( QStringLiteral( "well_y" ), well.surfaceY );
    v.details.insert( QStringLiteral( "inline" ), inlineNo );
    v.details.insert( QStringLiteral( "time_ms" ), wellTimeMs );
    v.details.insert( QStringLiteral( "raster_ms" ), rasterMs );
    v.details.insert( QStringLiteral( "residual_ms" ), residual );
    issues.append( v );
  }

  GDALClose( ds );
  return issues;
}
