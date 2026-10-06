// 层：功能
#include "mappingworkflow.h"
#include "mappingsamples.h"

#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include "../io/timedeptool.h"
#include "../io/wellfileparsers.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../services/projectdata.h"
#include "derivedassets.h"
#include "workflows.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QVariantMap>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gdal.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

  constexpr float kNoData = -9999.0f;

  // ---- 栅格读取 -------------------------------------------------------------
  struct GridSpec
  {
    int cols = 0, rows = 0;
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    bool hasNodata = false;
    double nodata = 0.0;
    QVector<float> px; // row-major，整幅读出（641×411 约 1MB 量级）
  };

  bool readGrid( const QString &path, GridSpec *g )
  {
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
      return false;
    g->cols = GDALGetRasterXSize( ds );
    g->rows = GDALGetRasterYSize( ds );
    GDALGetGeoTransform( ds, g->gt );
    if ( GDALGetRasterCount( ds ) < 1 )
    {
      GDALClose( ds );
      return false;
    }
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    if ( !band )
    {
      GDALClose( ds );
      return false;
    }
    int flag = 0;
    g->nodata = GDALGetRasterNoDataValue( band, &flag );
    g->hasNodata = flag != 0;
    g->px.resize( g->cols * g->rows );
    const CPLErr err = GDALRasterIO( band, GF_Read, 0, 0, g->cols, g->rows,
                                     g->px.data(), g->cols, g->rows, GDT_Float32, 0, 0 );
    GDALClose( ds );
    return err == CE_None;
  }

  bool isNodata( float v, const GridSpec &g )
  {
    return std::isnan( v ) || ( g.hasNodata && static_cast<double>( v ) == g.nodata );
  }

  // 尺寸 + geotransform + nodata 三者一致才能相减（autoplan §5C）。
  bool sameGrid( const GridSpec &a, const GridSpec &b )
  {
    if ( a.cols != b.cols || a.rows != b.rows )
      return false;
    const double tol = std::max( { std::fabs( a.gt[1] ), std::fabs( a.gt[5] ), 1.0 } ) * 1e-6;
    for ( int i = 0; i < 6; ++i )
      if ( std::fabs( a.gt[i] - b.gt[i] ) > tol )
        return false;
    if ( a.hasNodata != b.hasNodata )
      return false;
    if ( a.hasNodata && a.nodata != b.nodata )
      return false;
    return true;
  }

  // ---- 凸包（Andrew monotone chain，平面坐标，无需 QgsGeometry）-------------
  struct Pt
  {
    double x = 0.0, y = 0.0;
  };

  double cross( const Pt &o, const Pt &a, const Pt &b )
  {
    return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x );
  }

  // 返回 CCW 凸包（首尾不重复）。点数 <3 或共线时包仍返回，由调用方验面积。
  QVector<Pt> convexHullOf( QVector<Pt> pts )
  {
    std::sort( pts.begin(), pts.end(), []( const Pt &a, const Pt &b ) {
      return a.x < b.x || ( a.x == b.x && a.y < b.y );
    } );
    pts.erase( std::unique( pts.begin(), pts.end(),
                            []( const Pt &a, const Pt &b ) { return a.x == b.x && a.y == b.y; } ),
               pts.end() );
    const int n = pts.size();
    if ( n < 2 )
      return pts;
    QVector<Pt> hull( 2 * n );
    int k = 0;
    for ( int i = 0; i < n; ++i )
    {
      while ( k >= 2 && cross( hull[k - 2], hull[k - 1], pts[i] ) <= 0 )
        --k;
      hull[k++] = pts[i];
    }
    const int lower = k + 1;
    for ( int i = n - 2; i >= 0; --i )
    {
      while ( k >= lower && cross( hull[k - 2], hull[k - 1], pts[i] ) <= 0 )
        --k;
      hull[k++] = pts[i];
    }
    hull.resize( k - 1 );
    return hull;
  }

  double hullArea( const QVector<Pt> &h )
  {
    double a = 0.0;
    for ( int i = 0; i < h.size(); ++i )
      a += h[i].x * h[( i + 1 ) % h.size()].y - h[( i + 1 ) % h.size()].x * h[i].y;
    return std::fabs( a ) * 0.5;
  }

  bool pointOnSegment( const Pt &p, const Pt &a, const Pt &b )
  {
    if ( std::fabs( cross( a, b, p ) ) > 1e-9 * ( std::hypot( b.x - a.x, b.y - a.y ) + 1.0 ) )
      return false;
    return std::min( a.x, b.x ) - 1e-9 <= p.x && p.x <= std::max( a.x, b.x ) + 1e-9 &&
           std::min( a.y, b.y ) - 1e-9 <= p.y && p.y <= std::max( a.y, b.y ) + 1e-9;
  }

  // 凸包包含测试（边界算在内）。等效 QGIS contains 语义：凸多边形逐边同侧。
  bool hullContains( const QVector<Pt> &hull, double x, double y )
  {
    const Pt p{ x, y };
    bool pos = false, neg = false;
    for ( int i = 0; i < hull.size(); ++i )
    {
      const Pt &a = hull[i];
      const Pt &b = hull[( i + 1 ) % hull.size()];
      const double c = cross( a, b, p );
      if ( c > 0 )
        pos = true;
      else if ( c < 0 )
        neg = true;
      else if ( pointOnSegment( p, a, b ) )
        return true; // 边界
      if ( pos && neg )
        return false;
    }
    return true;
  }

  // ---- power-2 IDW（与 paleo:paleo_constraint_idw 同权重：w=1/d²，命中即取）---
  double idwPower2( const QVector<ThicknessSample> &samples, double x, double y )
  {
    double weightSum = 0.0, valueSum = 0.0;
    for ( const ThicknessSample &s : samples )
    {
      const double dx = s.x - x, dy = s.y - y;
      const double d2 = dx * dx + dy * dy;
      if ( d2 == 0.0 )
        return s.vint;
      const double w = 1.0 / d2;
      weightSum += w;
      valueSum += w * s.vint;
    }
    return weightSum > 0.0 ? valueSum / weightSum : qQNaN();
  }

  double idwPower2Points( const QVector<ThicknessPoint> &points, double x, double y )
  {
    double weightSum = 0.0, valueSum = 0.0;
    for ( const ThicknessPoint &p : points )
    {
      const double dx = p.x - x, dy = p.y - y;
      const double d2 = dx * dx + dy * dy;
      if ( d2 == 0.0 )
        return p.thickness;
      const double w = 1.0 / d2;
      weightSum += w;
      valueSum += w * p.thickness;
    }
    return weightSum > 0.0 ? valueSum / weightSum : qQNaN();
  }

  // ---- 写 Float32 GeoTIFF（局部测网，无基准 ENGCRS，同 horizonbinner 约定）----
  bool writeFloatRaster( const QString &path, int cols, int rows, const double gt[6],
                         const QVector<float> &px, QString *error )
  {
    GDALAllRegister();
    GDALDriverH drv = GDALGetDriverByName( "GTiff" );
    if ( !drv )
    {
      setError( error, QObject::tr( "GTiff 驱动不可用" ) );
      return false;
    }
    GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), cols, rows, 1,
                                  GDT_Float32, nullptr );
    if ( !ds )
    {
      setError( error, QObject::tr( "无法创建栅格 %1" ).arg( path ) );
      return false;
    }
    GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
    GDALSetRasterNoDataValue( GDALGetRasterBand( ds, 1 ), kNoData );
    OGRSpatialReference srs;
    if ( srs.SetFromUserInput( DataCatalog::localGridCrsWkt().toUtf8().constData() ) == OGRERR_NONE )
    {
      char *wkt = nullptr;
      if ( srs.exportToWkt( &wkt ) == OGRERR_NONE && wkt )
        GDALSetProjection( ds, wkt );
      CPLFree( wkt );
    }
    const CPLErr err = GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Write, 0, 0, cols, rows,
                                     const_cast<float *>( px.constData() ), cols, rows,
                                     GDT_Float32, 0, 0 );
    GDALClose( ds );
    if ( err != CE_None )
    {
      setError( error, QObject::tr( "栅格写入失败：%1" ).arg( path ) );
      return false;
    }
    return true;
  }

  QVariantMap thicknessSampleToMap( const ThicknessSample &s )
  {
    QVariantMap m;
    m.insert( QStringLiteral( "well_id" ), s.wellId );
    m.insert( QStringLiteral( "well_name" ), s.wellName );
    if ( std::isfinite( s.tvdTop ) )
      m.insert( QStringLiteral( "tvd_top" ), s.tvdTop );
    if ( std::isfinite( s.tvdBase ) )
      m.insert( QStringLiteral( "tvd_base" ), s.tvdBase );
    if ( std::isfinite( s.vint ) )
      m.insert( QStringLiteral( "vint" ), s.vint );
    m.insert( QStringLiteral( "contributing" ), s.contributing );
    m.insert( QStringLiteral( "reason" ), s.reason );
    return m;
  }

  // 厚度样本井点层（wells.thickness.<h>）——PDF 上的井位/井名标注源。
  // GeoJSON 内嵌 "crs" 成员写工程米制 WKT（OGR 仍认 legacy crs 写法），
  // 不落到 4326 假设。只写有坐标的井；贡献井带 thickness_m 属性。
  bool writeThicknessWellsGeoJson( const QVector<ThicknessSample> &samples,
                                   const QString &path, QString *error )
  {
    QJsonArray feats;
    for ( const ThicknessSample &s : samples )
    {
      if ( !std::isfinite( s.x ) || !std::isfinite( s.y ) )
        continue;
      QJsonObject props;
      props.insert( QStringLiteral( "well_name" ), s.wellName );
      if ( s.contributing && std::isfinite( s.thickness ) )
        props.insert( QStringLiteral( "thickness_m" ), s.thickness );
      QJsonObject geom;
      geom.insert( QStringLiteral( "type" ), QStringLiteral( "Point" ) );
      geom.insert( QStringLiteral( "coordinates" ), QJsonArray{ s.x, s.y } );
      QJsonObject f;
      f.insert( QStringLiteral( "type" ), QStringLiteral( "Feature" ) );
      f.insert( QStringLiteral( "properties" ), props );
      f.insert( QStringLiteral( "geometry" ), geom );
      feats.append( f );
    }
    if ( feats.isEmpty() )
      return true; // 没有可定位的井——不写空文件，也不算失败

    QJsonObject root;
    root.insert( QStringLiteral( "type" ), QStringLiteral( "FeatureCollection" ) );
    QJsonObject crsProps;
    crsProps.insert( QStringLiteral( "name" ), DataCatalog::localGridCrsWkt() );
    QJsonObject crs;
    crs.insert( QStringLiteral( "type" ), QStringLiteral( "name" ) );
    crs.insert( QStringLiteral( "properties" ), crsProps );
    root.insert( QStringLiteral( "crs" ), crs );
    root.insert( QStringLiteral( "features" ), feats );

    QFile file( path );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
      setError( error, QObject::tr( "井点 GeoJSON 写入失败：%1" ).arg( path ) );
      return false;
    }
    file.write( QJsonDocument( root ).toJson( QJsonDocument::Compact ) );
    return true;
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

void MappingWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

// ---------------------------------------------------------------------------
// 厚度样本 — 逐井评估（约束页面板渲染源）
// ---------------------------------------------------------------------------

QVector<ThicknessSample> MappingWorkflow::computeThicknessSamples( const QString &horizon,
                                                                   const QString &baseHorizon,
                                                                   QString *error )
{
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

  QVector<ThicknessSample> out;
  for ( const ProjectWell &well : m_projectData->wells() )
  {
    ThicknessSample s;
    s.wellId = well.id;
    s.wellName = well.name;

    const QVector<WellTop> tops = m_projectData->topsFor( well.id );
    const WellTop *top = nullptr;
    const WellTop *base = nullptr;
    for ( const WellTop &t : tops )
    {
      if ( t.horizon == horizon )
        top = &t;
      else if ( t.horizon == baseHorizon )
        base = &t;
    }
    if ( top )
      s.tvdTop = top->tvd;
    if ( base )
      s.tvdBase = base->tvd;
    // 分层点坐标：层位 top 的 X/Y 优先 → 基面 top → 井口（autoplan §5C）。
    MappingSamples::pickSamplePoint( well, top, &s.x, &s.y );
    if ( ( !std::isfinite( s.x ) || !std::isfinite( s.y ) ) && base )
      MappingSamples::pickSamplePoint( well, base, &s.x, &s.y );
    if ( !std::isfinite( s.x ) || !std::isfinite( s.y ) )
    {
      s.x = well.surfaceX;
      s.y = well.surfaceY;
    }

    if ( !top )
      s.reason = tr( "无 %1 分层" ).arg( horizon );
    else if ( !base )
      s.reason = tr( "无 %1 分层" ).arg( baseHorizon );
    else if ( qIsNaN( s.tvdTop ) )
      s.reason = tr( "缺 %1 TVD" ).arg( horizon );
    else if ( qIsNaN( s.tvdBase ) )
      s.reason = tr( "缺 %1 TVD" ).arg( baseHorizon );
    else
    {
      s.thickness = s.tvdBase - s.tvdTop;
      const QVector<TdSample> td = m_projectData->tdTableFor( well.id );
      const TimeDepthTool::TdResult rt = MappingSamples::timeForTop( td, *top );
      const TimeDepthTool::TdResult rb = MappingSamples::timeForTop( td, *base );
      if ( !rt.ok() )
        s.reason = TimeDepthTool::reasonText( rt.status );
      else if ( !rb.ok() )
        s.reason = TimeDepthTool::reasonText( rb.status );
      else
      {
        s.dtMs = rb.timeMs - rt.timeMs;
        if ( !( s.dtMs > 0.0 ) )
          s.reason = tr( "两层时间差非正（dt=%1ms）" ).arg( s.dtMs, 0, 'f', 1 );
        else if ( !( s.thickness > 0.0 ) )
          s.reason = tr( "两层 TVD 差非正（%1m）" ).arg( s.thickness, 0, 'f', 1 );
        else if ( !std::isfinite( s.x ) || !std::isfinite( s.y ) )
          s.reason = tr( "分层点与井口都无坐标" );
        else
        {
          s.vint = s.thickness / ( s.dtMs / 2000.0 ); // m/s（autoplan §5C）
          s.contributing = true;
        }
      }
    }
    out.append( s );
  }
  return out;
}

QVector<ThicknessPoint> MappingWorkflow::computeThickness( const QString &horizon,
                                                           const QString &baseHorizon,
                                                           int *skipped, QString *error )
{
  if ( skipped )
    *skipped = 0;
  const QVector<ThicknessSample> samples = computeThicknessSamples( horizon, baseHorizon, error );
  if ( error && !error->isEmpty() )
    return {};

  QVector<ThicknessPoint> points;
  for ( const ThicknessSample &s : samples )
  {
    if ( !( s.thickness > 0.0 ) ) // 缺分层/缺 TVD/TVD 差非正 — 都跳过计数
    {
      if ( skipped )
        ++( *skipped );
      continue;
    }
    ThicknessPoint p;
    p.wellId = s.wellId;
    p.wellName = s.wellName;
    p.x = s.x;
    p.y = s.y;
    p.thickness = s.thickness;
    points.append( p );
  }
  return points;
}

void MappingWorkflow::publishThicknessSamples( const QVector<ThicknessSample> &samples,
                                               const QString &message )
{
  QVariantList rows;
  rows.reserve( samples.size() );
  for ( const ThicknessSample &s : samples )
    rows.append( thicknessSampleToMap( s ) );
  m_thicknessRows = rows;
  m_thicknessMessage = message;
  // 约束页面板只持有 ConstraintWorkflow* —— 行表 typed 镜像过去，
  // 面板 showEvent 时读出渲染（不弹对话框；ARCH-06：替代动态属性暗道）。
  if ( m_constraints )
    m_constraints->setThicknessSamples( rows, message );
}

QVariantList MappingWorkflow::thicknessSampleRows() const
{
  return m_thicknessRows;
}

QString MappingWorkflow::thicknessSampleMessage() const
{
  return m_thicknessMessage;
}

void MappingWorkflow::declareThicknessWellsLayer( const QString &horizon,
                                                  const QVector<ThicknessSample> &samples )
{
  if ( !m_catalog )
    return; // 无登记通道：链路主栅格已产出，井点层按旧语义只跳过（见调用序）
  DerivedAssetRegistrar registrar( m_catalog, m_projectDir );
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "thickness_wells" ), tr( "%1 厚度井位" ).arg( horizon ),
      QStringLiteral( "WELLS_THICKNESS_%1.geojson" ).arg( horizon ) );
  if ( !st.isValid() )
  {
    qWarning() << "thickness wells layer staging failed";
    return;
  }
  QString writeErr;
  if ( !writeThicknessWellsGeoJson( samples, st.absolutePath, &writeErr ) )
  {
    qWarning() << "thickness wells layer write failed:" << writeErr;
    return;
  }
  if ( !QFile::exists( st.absolutePath ) )
    return; // 没有可定位的井
  QString commitErr;
  if ( !registrar.commit( st, {}, QStringLiteral( "mappingworkflow/thickness_wells" ),
                          {}, &commitErr ) )
    qWarning() << "thickness wells version registration failed:" << commitErr;

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "wells.thickness.%1" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "vector" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "04_SingleFactor" );
  decl.title = tr( "厚度井位" );
  QString declErr;
  if ( !m_layers->declare( decl, &declErr ) )
    qWarning() << "thickness wells layer declare failed:" << declErr;
}

// ---------------------------------------------------------------------------
// 编图链（autoplan §5C 新语义）
// ---------------------------------------------------------------------------

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
  if ( !m_catalog )
    return fail( tr( "编图链未绑定数据目录（catalog）——派生产物无法登记到工程" ) );
  DerivedAssetRegistrar registrar( m_catalog, m_projectDir );

  const QString base = baseHorizonFor( horizon );
  if ( base.isEmpty() )
    return fail( tr( "层位 %1 没有厚度基面" ).arg( horizon ) );

  // 逐井样本先算——无论链成败，行表都发布给约束页面板渲染。
  QString sampleErr;
  const QVector<ThicknessSample> samples = computeThicknessSamples( horizon, base, &sampleErr );
  if ( !sampleErr.isEmpty() )
    return fail( sampleErr );
  publishThicknessSamples( samples, QString() );

  QVector<ThicknessSample> contributing;
  for ( const ThicknessSample &s : samples )
    if ( s.contributing )
      contributing.append( s );

  // n = 提供样本的井数：0 → 「没有厚度样本」；<3 或凸包无面积 → 「不足以成面」。
  const auto insufficientSamples = [this, &samples, &fail]( int n ) {
    const QString msg = n == 0 ? tr( "没有厚度样本" ) : tr( "厚度样本不足以成面" );
    publishThicknessSamples( samples, msg );
    return fail( msg );
  };

  const HorizonRasterInfo rasterTop = m_projectData->horizonRasterDecl( horizon );
  const HorizonRasterInfo rasterBase = m_projectData->horizonRasterDecl( base );

  // ---- 退回路径：任一层位栅格缺失 → 对井点厚度本身做 power-2 IDW（无约束线）--
  if ( !rasterTop.valid || !rasterBase.valid )
  {
    QVector<ThicknessPoint> pts;
    QVector<Pt> hullPts;
    for ( const ThicknessSample &s : samples )
    {
      // TVD 差非正的井不提供样本（与贡献井同一拒绝规则，只是不需要 TD）。
      if ( !( s.thickness > 0.0 ) || !std::isfinite( s.x ) || !std::isfinite( s.y ) )
        continue;
      ThicknessPoint p;
      p.wellId = s.wellId;
      p.wellName = s.wellName;
      p.x = s.x;
      p.y = s.y;
      p.thickness = s.thickness;
      pts.append( p );
      hullPts.append( { s.x, s.y } );
    }
    if ( pts.size() < 3 )
      return insufficientSamples( static_cast<int>( pts.size() ) );
    const QVector<Pt> hull = convexHullOf( hullPts );
    if ( hullArea( hull ) <= 0.0 )
      return insufficientSamples( static_cast<int>( pts.size() ) );

    // 井点外扩网格（退回路径沿用旧式外扩；像元尺度取仍存在的层位栅格，
    // 两张都缺时按井点范围取 1/100）。
    double xmin = pts.first().x, xmax = xmin, ymin = pts.first().y, ymax = ymin;
    for ( const ThicknessPoint &p : pts )
    {
      xmin = std::min( xmin, p.x );
      xmax = std::max( xmax, p.x );
      ymin = std::min( ymin, p.y );
      ymax = std::max( ymax, p.y );
    }
    double cell = rasterTop.valid ? rasterTop.cellSize
                                  : ( rasterBase.valid ? rasterBase.cellSize : 0.0 );
    if ( !( cell > 0.0 ) )
      cell = std::max( xmax - xmin, ymax - ymin ) / 100.0;
    const double xPad = ( xmax - xmin ) > 0.0 ? ( xmax - xmin ) * 0.1 : cell;
    const double yPad = ( ymax - ymin ) > 0.0 ? ( ymax - ymin ) * 0.1 : cell;
    const double ox = xmin - xPad, oyTop = ymax + yPad;
    const int cols = std::max( 1, static_cast<int>( std::ceil( ( xmax + xPad - ox ) / cell ) ) );
    const int rows = std::max( 1, static_cast<int>( std::ceil( ( oyTop - ( ymin - yPad ) ) / cell ) ) );
    const double gt[6] = { ox, cell, 0.0, oyTop, 0.0, -cell };

    QVector<float> px( cols * rows, kNoData );
    for ( int r = 0; r < rows; ++r )
      for ( int c = 0; c < cols; ++c )
      {
        const double cx = ox + ( c + 0.5 ) * cell;
        const double cy = oyTop - ( r + 0.5 ) * cell;
        if ( !hullContains( hull, cx, cy ) )
          continue;
        px[r * cols + c] = static_cast<float>( idwPower2Points( pts, cx, cy ) );
      }

    // 退回路径产物同样登记（无栅格输入 → 无父版本；井点 tops 是间接来源）。
    QString stageErr;
    const DerivedStaging st = registrar.stage(
        QStringLiteral( "thickness_raster" ), tr( "%1 井点厚度" ).arg( horizon ),
        QStringLiteral( "THICKNESS_%1_IDW.tif" ).arg( horizon ), &stageErr );
    if ( !st.isValid() )
      return fail( stageErr );
    if ( !writeFloatRaster( st.absolutePath, cols, rows, gt, px, error ) )
      return fail( error ? *error : tr( "井点厚度栅格写入失败" ) );
    QVariantMap fallbackExtra;
    fallbackExtra.insert( QStringLiteral( "mode" ), QStringLiteral( "wellpoint_idw" ) );
    fallbackExtra.insert( QStringLiteral( "rows" ), rows );
    fallbackExtra.insert( QStringLiteral( "cols" ), cols );
    fallbackExtra.insert( QStringLiteral( "wells" ), pts.size() );
    QString commitErr;
    if ( !registrar.commit( st, {}, QStringLiteral( "mappingworkflow/thickness_fallback" ),
                            fallbackExtra, &commitErr ) )
      return fail( commitErr );

    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "factor.%1.idw" ).arg( horizon );
    decl.horizon = horizon;
    decl.type = QStringLiteral( "raster" );
    decl.source = st.absolutePath;
    decl.group = QStringLiteral( "04_SingleFactor" );
    decl.title = QStringLiteral( "井点厚度（米，无层位栅格）" );
    if ( !m_layers->declare( decl, error ) )
      return fail( error ? *error : tr( "无法声明井点厚度图层" ) );

    declareThicknessWellsLayer( horizon, samples );
    emit chainDone( horizon, decl.layerId );
    return true;
  }

  // ---- 主路径：等厚 = (D62−D61)/2000 × IDW²(Vint)，写在 D61 网格上 ---------
  GridSpec gTop, gBase;
  if ( !readGrid( rasterTop.path, &gTop ) || !readGrid( rasterBase.path, &gBase ) )
    return fail( tr( "无法读取 %1/%2 的时间栅格" ).arg( horizon, base ) );
  if ( !sameGrid( gTop, gBase ) )
    return fail( tr( "%1 与 %2 的尺寸、geotransform 或 nodata 不一致，不写等厚" )
                     .arg( base, horizon ) );

  if ( contributing.size() < 3 )
    return insufficientSamples( static_cast<int>( contributing.size() ) );
  QVector<Pt> hullPts;
  hullPts.reserve( contributing.size() );
  for ( const ThicknessSample &s : contributing )
    hullPts.append( { s.x, s.y } );
  const QVector<Pt> hull = convexHullOf( hullPts );
  if ( hullArea( hull ) <= 0.0 ) // 无面积的凸包按不足 3 口处理（autoplan §5C）
    return insufficientSamples( static_cast<int>( contributing.size() ) );

  const int cols = gTop.cols, rows = gTop.rows;
  QVector<float> px( cols * rows, kNoData );
  for ( int r = 0; r < rows; ++r )
  {
    const double cy = gTop.gt[3] + ( r + 0.5 ) * gTop.gt[5];
    for ( int c = 0; c < cols; ++c )
    {
      const int idx = r * cols + c;
      const float tTop = gTop.px[idx];
      const float tBase = gBase.px[idx];
      if ( isNodata( tTop, gTop ) || isNodata( tBase, gBase ) )
        continue;
      if ( !std::isfinite( tTop ) || !std::isfinite( tBase ) )
        continue;
      const double cx = gTop.gt[0] + ( c + 0.5 ) * gTop.gt[1];
      if ( !hullContains( hull, cx, cy ) )
        continue; // 凸包外 → -9999
      const double isochronMs = static_cast<double>( tBase ) - tTop;
      if ( !( isochronMs > 0.0 ) || !std::isfinite( isochronMs ) )
        continue; // 非正或非有限时差不是有效等厚样点 → nodata
      const double vint = idwPower2( contributing, cx, cy );
      if ( !std::isfinite( vint ) )
        continue;
      const double thicknessM = isochronMs / 2000.0 * vint;
      if ( !( thicknessM > 0.0 ) || !std::isfinite( thicknessM ) )
        continue;
      const float thicknessValue = static_cast<float>( thicknessM );
      if ( !( thicknessValue > 0.0f ) || !std::isfinite( thicknessValue ) )
        continue;
      px[idx] = thicknessValue;
    }
  }

  // 主产物：等厚栅格落 artifacts/derived + DERIVED 版本（T26），父版本 =
  // D61/D62 时间栅格版本（provenance：这份厚度由哪两版栅格算出）。
  QString stageErr;
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "thickness_raster" ), tr( "%1–%2 等厚" ).arg( horizon, base ),
      QStringLiteral( "THICKNESS_%1.tif" ).arg( horizon ), &stageErr );
  if ( !st.isValid() )
    return fail( stageErr );
  if ( !writeFloatRaster( st.absolutePath, cols, rows, gTop.gt, px, error ) )
    return fail( error ? *error : tr( "等厚栅格写入失败" ) );
  QVariantMap extra;
  extra.insert( QStringLiteral( "rows" ), rows );
  extra.insert( QStringLiteral( "cols" ), cols );
  extra.insert( QStringLiteral( "base_horizon" ), base );
  extra.insert( QStringLiteral( "contributing_wells" ), contributing.size() );
  const QStringList parents = registrar.parentVersionIdsFor(
      QStringList{ rasterTop.path, rasterBase.path } );
  QString commitErr;
  if ( !registrar.commit( st, parents, QStringLiteral( "mappingworkflow/thickness" ),
                          extra, &commitErr ) )
    return fail( commitErr );

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "factor.%1.idw" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "04_SingleFactor" );
  decl.title = QStringLiteral( "%1–%2 等厚（米）" ).arg( horizon, base );
  if ( !m_layers->declare( decl, error ) )
    return fail( error ? *error : tr( "无法声明等厚图层" ) );

  declareThicknessWellsLayer( horizon, samples );
  // 厚度栅格不是相编码——不调用 deriveFaciesPolygons（autoplan §5C）。
  emit chainDone( horizon, decl.layerId );
  return true;
}

// ---------------------------------------------------------------------------
// 时间残差 — 阶段C验证（autoplan §5C）
// ---------------------------------------------------------------------------

QList<TimeResidualRow> computeTimeResiduals( const ProjectDataFacade *projectData,
                                             const QString &horizon, double thresholdMs )
{
  QList<TimeResidualRow> rows;
  if ( !projectData )
    return rows;

  const HorizonRasterInfo raster = projectData->horizonRasterDecl( horizon );
  if ( !raster.valid )
    return rows; // 无结构面栅格 → 本检查不适用

  GDALAllRegister();
  GDALDatasetH ds = GDALOpen( raster.path.toUtf8().constData(), GA_ReadOnly );
  if ( !ds )
    return rows;

  const bool hasInline = raster.inlineMin >= 0 && raster.inlineMax > raster.inlineMin &&
                         raster.ymax > raster.ymin;
  for ( const ProjectWell &well : projectData->wells() )
  {
    TimeResidualRow row;
    row.wellId = well.id;
    row.wellName = well.name;

    const QVector<WellTop> tops = projectData->topsFor( well.id );
    const WellTop *pick = nullptr;
    for ( const WellTop &top : tops )
      if ( top.horizon == horizon )
        pick = &top;
    // 每口井都占一行：没有该层位分层 → 「无 D61 分层」。
    if ( !pick || ( qIsNaN( pick->tvd ) && qIsNaN( pick->md ) ) )
    {
      row.reason = QObject::tr( "无 %1 分层" ).arg( horizon );
      MappingSamples::pickSamplePoint( well, pick, &row.x, &row.y );
      rows.append( row );
      continue;
    }

    MappingSamples::pickSamplePoint( well, pick, &row.x, &row.y );

    const QVector<TdSample> td = projectData->tdTableFor( well.id );
    const TimeDepthTool::TdResult tdResult = MappingSamples::timeForTop( td, *pick );
    if ( !tdResult.ok() )
    {
      // 无时深表/超出时深表/时深表无序（TimeDepthTool::reasonText）。
      row.reason = TimeDepthTool::reasonText( tdResult.status );
      rows.append( row );
      continue;
    }
    row.timeMs = tdResult.timeMs;

    double rasterMs = 0.0;
    const MappingSamples::SampleOutcome outcome = MappingSamples::sampleRasterAt( ds, row.x, row.y, &rasterMs );
    if ( outcome == MappingSamples::SampleOutcome::Outside )
    {
      row.status = TimeResidualRow::Status::Warn;
      row.reason = QObject::tr( "井位不在测网内" );
      rows.append( row );
      continue;
    }
    if ( outcome == MappingSamples::SampleOutcome::Nodata )
    {
      row.status = TimeResidualRow::Status::Warn;
      row.reason = QObject::tr( "井位落在空道" );
      rows.append( row );
      continue;
    }
    row.rasterMs = rasterMs;
    row.residualMs = row.timeMs - row.rasterMs; // 保留符号
    row.status = std::abs( row.residualMs ) > thresholdMs ? TimeResidualRow::Status::Exceeds
                                                          : TimeResidualRow::Status::Pass;
    if ( hasInline )
      row.inlineNo = static_cast<int>( std::lround(
          raster.inlineMin + ( row.y - raster.ymin ) / ( raster.ymax - raster.ymin ) *
                                 ( raster.inlineMax - raster.inlineMin ) ) );
    rows.append( row );
  }

  GDALClose( ds );
  return rows;
}
