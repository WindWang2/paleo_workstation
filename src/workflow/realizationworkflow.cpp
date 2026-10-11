// 层：功能
#include "realizationworkflow.h"

#include "../algorithms/ensemblestats.h"
#include "../algorithms/rasterout.h"
#include "../catalog/datacatalog.h"
#include "../catalog/realizationset.h"
#include "../qgis/qgislayerservice.h"
#include "../services/jobrunner.h"
#include "derivedassets.h"
#include "workflowerrors_internal.h"
#include "workflows_internal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <gdal.h>
#include <qgscoordinatereferencesystem.h>

namespace
{
constexpr double kRealizationNoData = -9999.0;
// 分位数行带的常驻上限：64 成员 × 1024 列也只用 ~4 行/带。
constexpr std::size_t kMaxBandValues = 8u * 1024u * 1024u;

using paleo::workflow_detail::setError;

// 成员栅格读一窗到 double：nodata（-9999）与脏值一律归一为 NaN——
// 统计核的诚实面建立在「NaN = 不在场」上。
bool readBandSlice( GDALRasterBandH band, int row0, int nRows, int cols, double *out )
{
  if ( GDALRasterIO( band, GF_Read, 0, row0, cols, nRows, out, cols, nRows,
                     GDT_Float64, 0, 0 ) != CE_None )
    return false;
  int hasNodata = 0;
  const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
  const std::size_t n = static_cast<std::size_t>( nRows ) * cols;
  for ( std::size_t i = 0; i < n; ++i )
    if ( ( hasNodata && out[i] == nodata ) || !std::isfinite( out[i] ) )
      out[i] = std::numeric_limits<double>::quiet_NaN();
  return true;
}

// 整幅 double 场 → Float32 GeoTIFF（NaN → nodata）。几何/投影沿用成员
// 栅格（集合成员同网格是契约；投影字符串直传，不经 QgsCRS 往返）。
bool writeStatRaster( const QString &path, const std::vector<double> &values,
                      int cols, int rows, const double geoTransform[6],
                      const QByteArray &projectionWkt )
{
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster(
      path, cols, rows, geoTransform, QgsCoordinateReferenceSystem(), kRealizationNoData );
  if ( !ds )
    return false;
  if ( !projectionWkt.isEmpty() )
    GDALSetProjection( ds, projectionWkt.constData() );
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  std::vector<float> row( static_cast<std::size_t>( cols ) );
  for ( int r = 0; r < rows; ++r )
  {
    for ( int c = 0; c < cols; ++c )
    {
      const double v = values[static_cast<std::size_t>( r ) * cols + c];
      row[static_cast<std::size_t>( c )] =
          std::isfinite( v ) ? static_cast<float>( v ) : static_cast<float>( kRealizationNoData );
    }
    if ( GDALRasterIO( band, GF_Write, 0, r, cols, 1, row.data(), cols, 1,
                       GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( ds );
      QFile::remove( path );
      return false;
    }
  }
  GDALClose( ds );
  return true;
}

struct MemberRaster
{
  GDALDatasetH ds = nullptr;
  GDALRasterBandH band = nullptr;
  int index = -1;
  QString versionId;
  QString path;
};

// 打开集合全部在场成员（句柄常驻——分位数带式读要随机回访各成员行带；
// 句柄本身轻，像素不驻留）。任一缺席/打不开 → 点名失败。
bool openMemberRasters( const paleo::realization::RealizationSet &set,
                        DataCatalog *catalog, const QString &projectDir,
                        QVector<MemberRaster> *out, QString *error )
{
  GDALAllRegister();
  for ( const paleo::realization::RealizationMember &m : set.members )
  {
    const CatalogVersion v = catalog->versionById( m.versionId );
    const QString abs = DataCatalog::resolvedVersionPath( projectDir, v );
    MemberRaster mr;
    mr.index = m.index;
    mr.versionId = m.versionId;
    mr.path = abs;
    if ( abs.isEmpty() || !QFile::exists( abs ) )
    {
      setError( error, QObject::tr( "成员 %1 的受管栅格缺席（%2）——集合数据不全，停止派生" )
                           .arg( m.index ).arg( v.path ) );
      return false;
    }
    mr.ds = GDALOpen( abs.toUtf8().constData(), GA_ReadOnly );
    if ( !mr.ds )
    {
      setError( error, QObject::tr( "成员 %1 栅格打不开（%2）" ).arg( m.index ).arg( abs ) );
      return false;
    }
    mr.band = GDALGetRasterBand( mr.ds, 1 );
    out->append( mr );
  }
  return true;
}

void closeMemberRasters( QVector<MemberRaster> *members )
{
  for ( MemberRaster &m : *members )
    if ( m.ds )
      GDALClose( m.ds );
  members->clear();
}

struct RealizationJob
{
  enum class Kind { DeriveAllStats, DifferenceOfMeans };
  Kind kind = Kind::DeriveAllStats;
  QString setId;
  QString setIdB;

  // Prepared data:
  QString title;
  QString horizon;
  QStringList memberParentIds;
  struct MemberData {
    int index;
    QString rasterPath;
  };
  QVector<MemberData> members;
  int cols = 0;
  int rows = 0;
  double gt[6]{ 0, 1, 0, 0, 0, -1 };
  QString projWkt;
  paleo::ensemble::StatsRequest want;

  // Difference of means prepared data:
  QString meanPathA;
  QString meanPathB;

  // Intermediate outputs computed in worker thread:
  struct OutputRaster {
    QString token;
    QString tempPath;
  };
  QVector<OutputRaster> outputs;

  QString error;
};

} // namespace

struct RealizationWorkflow::RunnerHolder
{
  paleo::jobs::JobRunner<RealizationJob> runner;
  explicit RunnerHolder( QObject *parent )
      : runner( parent )
  {
  }
};

RealizationWorkflow::RealizationWorkflow( QObject *parent )
    : QObject( parent )
    , m_runnerHolder( std::make_unique<RunnerHolder>( this ) )
{
  connect( &m_runnerHolder->runner, &paleo::jobs::JobRunnerBase::jobCompleted, this,
           [this]( quint64, bool ) {
             m_busy = false;
             emit busyChanged( false );
           } );
  connect( &m_runnerHolder->runner, &paleo::jobs::JobRunnerBase::claimDropped, this,
           [this]( quint64, int ) {
             m_busy = false;
             emit busyChanged( false );
           } );
}

RealizationWorkflow::~RealizationWorkflow() = default;

void RealizationWorkflow::setTaskService( PaleoTaskService *tasks )
{
  m_tasks = tasks;
  if ( m_runnerHolder )
    m_runnerHolder->runner.setTaskService( tasks );
}

PaleoTaskService *RealizationWorkflow::taskService() const
{
  return m_tasks;
}

bool RealizationWorkflow::isBusy() const
{
  return m_busy || ( m_runnerHolder && m_runnerHolder->runner.busy() );
}

void RealizationWorkflow::bind( DataCatalog *catalog, const QString &projectDir,
                                QgisLayerService *layers )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
  m_layers = layers;
}

QString RealizationWorkflow::memberLayerId( const QString &setId, int index )
{
  return QStringLiteral( "realset.%1.m%2" ).arg( setId ).arg( index );
}

QString RealizationWorkflow::statLayerId( const QString &setId, const QString &token )
{
  return QStringLiteral( "realset.%1.stat.%2" ).arg( setId, token );
}

QString RealizationWorkflow::diffLayerId( const QString &setIdA, const QString &setIdB )
{
  return QStringLiteral( "realsetdiff.%1.%2" ).arg( setIdA, setIdB );
}

QStringList RealizationWorkflow::memberFrameOrder( const QString &setId ) const
{
  QStringList frames;
  if ( !m_catalog )
    return frames;
  const paleo::realization::RealizationSet set =
      paleo::realization::setById( *m_catalog, setId );
  for ( const paleo::realization::RealizationMember &m : set.members )
    frames << memberLayerId( setId, m.index );
  return frames;
}

QString RealizationWorkflow::memberRasterPath( const QString &setId, int index ) const
{
  if ( !m_catalog )
    return QString();
  const paleo::realization::RealizationSet set =
      paleo::realization::setById( *m_catalog, setId );
  const QString vid = paleo::realization::memberVersionId( set, index );
  if ( vid.isEmpty() )
    return QString();
  return DataCatalog::resolvedVersionPath( m_projectDir, m_catalog->versionById( vid ) );
}

QStringList RealizationWorkflow::statLayerIds( const QString &setId ) const
{
  QStringList ids;
  if ( !m_catalog )
    return ids;
  for ( const paleo::realization::StatisticSurface &s :
        paleo::realization::statSurfaces( *m_catalog, setId ) )
    ids << statLayerId( setId, s.token );
  return ids;
}

QString RealizationWorkflow::publishSet( const QString &title, const QString &horizon,
                                         const QVector<MemberInput> &members,
                                         const QStringList &inputParentVersionIds,
                                         const QVariantMap &sharedExtra, QString *error )
{
  if ( !isBound() )
  {
    setError( error, tr( "realization workflow 未绑定 catalog/projectDir" ) );
    return QString();
  }
  if ( members.isEmpty() )
  {
    setError( error, tr( "拒绝登记零成员集合——集合契约要求至少一个在场成员" ) );
    return QString();
  }

  DerivedAssetRegistrar registrar( m_catalog.data(), m_projectDir );
  // 每次生成都得新集合资产：标题带运行戳，重跑不并入旧集合（成员增量补录
  // 不是本路径的语义——缺号集合走修复再生，不静默续写）。
  const QString stampedTitle = tr( "%1·集合·%2" ).arg( title, paleo::workflow_detail::stamp() );
  QString setId;

  for ( int i = 0; i < members.size(); ++i )
  {
    QString stageErr;
    const DerivedStaging st = registrar.stage(
        paleo::realization::kAssetTypeSet, stampedTitle,
        QStringLiteral( "REALIZATION_%1_r%2.tif" )
            .arg( horizon )
            .arg( i, 3, 10, QLatin1Char( '0' ) ),
        &stageErr );
    if ( !st.isValid() )
    {
      setError( error, stageErr.isEmpty() ? tr( "成员 %1 受管落位计算失败" ).arg( i )
                                        : stageErr );
      return QString();
    }
    if ( setId.isEmpty() )
      setId = st.assetId;

    QVariantMap extra = sharedExtra;
    extra.insert( paleo::realization::kKeySetId, st.assetId );
    extra.insert( paleo::realization::kKeyIndex, i );
    extra.insert( paleo::realization::kKeyMemberCount, members.size() );
    extra.insert( paleo::realization::kKeySeed, static_cast<qulonglong>( members[i].seed ) );
    extra.insert( QStringLiteral( "horizon" ), horizon );
    // 标签一律用 realizationIndex（0 起）——缺号报告/成员切换寻址同口径。
    extra.insert( QStringLiteral( "title" ),
                  tr( "%1·成员 #%2" ).arg( title ).arg( i ) );
    extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );

    QString commitErr;
    if ( !registrar.commitExternal( st, members[i].tempPath, inputParentVersionIds,
                                  QStringLiteral( "paleo:realization_member" ), extra,
                                  &commitErr ) )
    {
      // 已提交成员不回滚——catalog 成员版本不可变，缺号检测如实呈现
      // 这个不完整集合（契约允许的退化形态，见 realizationset.h）。
      setError( error, tr( "成员 %1 登记失败：%2（已提交成员 %3 个留在集合 %4 中，"
                           "按不完整集合如实呈现）" )
                           .arg( i ).arg( commitErr ).arg( i ).arg( setId ) );
      return QString();
    }

    if ( m_layers )
    {
      LayerDeclaration decl;
      decl.layerId = memberLayerId( st.assetId, i );
      decl.horizon = horizon;
      decl.type = QStringLiteral( "raster" );
      decl.source = st.absolutePath;
      decl.group = QStringLiteral( "04_SingleFactor/Realizations" );
      decl.title = tr( "%1·成员 #%2" ).arg( title ).arg( i );
      QString declErr;
      if ( !m_layers->declare( decl, &declErr ) )
        // 图层声明是呈现面——catalog 版本已锚，失败只告警不拦集合登记。
        qWarning( "realization member layer declare failed: %s",
                  qPrintable( declErr ) );
    }
  }
  emit realizationSetPublished( setId, members.size() );
  return setId;
}

bool RealizationWorkflow::deriveStatistics( const QString &setId,
                                            const paleo::ensemble::StatsRequest &wantIn,
                                            QString *error )
{
  paleo::ensemble::StatsRequest want = wantIn;
  if ( !isBound() )
  {
    setError( error, tr( "realization workflow 未绑定 catalog/projectDir" ) );
    return false;
  }
  if ( !want.any() )
  {
    setError( error, tr( "派生请求为空——没有要计算的统计口径" ) );
    return false;
  }
  const paleo::realization::RealizationSet set =
      paleo::realization::setById( *m_catalog, setId );
  if ( set.isEmpty() )
  {
    setError( error, tr( "集合 %1 不存在或没有成员版本" ).arg( setId ) );
    return false;
  }
  if ( set.members.size() < 2 )
  {
    // N=1/0：如实拒绝，不产「零离散」假面（契约钉死）。
    setError( error, tr( "集合 %1 只有 %2 个在场成员——单成员集合无不确定性可派生" )
                         .arg( setId ).arg( set.members.size() ) );
    return false;
  }

  // #227（部分）：幂等跳过前置到读栅格之前——已派生的口径不再重读全集合
  // 再丢弃。GUI 线程上冻结期间排队的连点，恢复后逐次早退而不是逐次重跑。
  for ( const paleo::realization::StatisticSurface &s :
        paleo::realization::statSurfaces( *m_catalog, setId ) )
  {
    if ( s.token == paleo::realization::kStatMean )
      want.mean = false;
    else if ( s.token == paleo::realization::kStatStdDev )
      want.stddev = false;
    else if ( s.token == paleo::realization::kStatP10 )
      want.p10 = false;
    else if ( s.token == paleo::realization::kStatP90 )
      want.p90 = false;
  }
  if ( !want.any() )
    return true; // 全部口径已在场：与逐口径幂等跳过同语义（不发 derived 信号）

  QVector<MemberRaster> rasters;
  if ( !openMemberRasters( set, m_catalog.data(), m_projectDir, &rasters, error ) )
  {
    closeMemberRasters( &rasters );
    return false;
  }
  const auto guard = qScopeGuard( [&rasters] { closeMemberRasters( &rasters ); } );

  const int cols = GDALGetRasterXSize( rasters.first().ds );
  const int rows = GDALGetRasterYSize( rasters.first().ds );
  double gt[6] = { 0, 0, 0, 0, 0, 0 };
  GDALGetGeoTransform( rasters.first().ds, gt );
  const QByteArray projWkt = GDALGetProjectionRef( rasters.first().ds )
                                 ? QByteArray( GDALGetProjectionRef( rasters.first().ds ) )
                                 : QByteArray();
  for ( const MemberRaster &m : rasters )
  {
    if ( GDALGetRasterXSize( m.ds ) != cols || GDALGetRasterYSize( m.ds ) != rows )
    {
      setError( error, tr( "成员 %1 网格（%2×%3）与首个成员（%4×%5）不一致——"
                           "集合成员必须同网格，拒绝派生" )
                           .arg( m.index )
                           .arg( GDALGetRasterXSize( m.ds ) )
                           .arg( GDALGetRasterYSize( m.ds ) )
                           .arg( cols )
                           .arg( rows ) );
      return false;
    }
  }

  const std::size_t cells = static_cast<std::size_t>( cols ) * rows;
  // 统计/差值声明沿用成员层位（成员 extra.horizon 由 publishSet 写入）。
  const QString horizon =
      m_catalog->versionById( set.members.first().versionId )
          .extra.value( QStringLiteral( "horizon" ) )
          .toString();
  const QStringList memberParentIds = [&set] {
    QStringList ids;
    for ( const paleo::realization::RealizationMember &m : set.members )
      ids << m.versionId;
    return ids;
  }();

  // ---- 数值路径：均值/总体标准差走流式（逐成员读入即聚合）；P10/P90 走
  // 行带集齐切片（常驻 = 成员数 × 带行 × 列）。两条路都不驻留全集合。 ----
  std::vector<double> mean, stddev;
  if ( want.mean || want.stddev )
  {
    paleo::ensemble::StreamingMoments moments( cells );
    std::vector<double> field( cells );
    for ( const MemberRaster &m : rasters )
    {
      if ( !readBandSlice( m.band, 0, rows, cols, field.data() ) )
      {
        setError( error, tr( "成员 %1 栅格读失败" ).arg( m.index ) );
        return false;
      }
      moments.addField( field.data() );
    }
    if ( want.mean )
      mean = moments.mean();
    if ( want.stddev )
      stddev = moments.stddevPopulation();
  }
  std::vector<std::vector<double>> bands;
  if ( want.p10 || want.p90 )
  {
    std::vector<double> qs;
    if ( want.p10 )
      qs.push_back( 0.10 );
    if ( want.p90 )
      qs.push_back( 0.90 );
    const auto readBand = [&rasters, cols]( std::size_t member, int row0, int nRows,
                                            double *out ) {
      return readBandSlice( rasters.at( member ).band, row0, nRows, cols, out );
    };
    bands = paleo::ensemble::quantilesBanded( rasters.size(), cols, rows, qs,
                                              kMaxBandValues, readBand );
    if ( bands.size() != qs.size() )
    {
      setError( error, tr( "分位数行带计算中止（成员读失败）" ) );
      return false;
    }
  }

  // ---- 每口径独立 realization_stat 版本；parents = 全部在场成员 ----
  DerivedAssetRegistrar registrar( m_catalog.data(), m_projectDir );
  QStringList tokens;
  const auto commitStat = [&]( const QString &token, const std::vector<double> &field ) {
    // 幂等面：同口径已派生过 → 跳过（重派生 = 调用方先清理旧版本）。
    for ( const paleo::realization::StatisticSurface &s :
          paleo::realization::statSurfaces( *m_catalog, setId ) )
      if ( s.token == token )
        return true;
    QString stageErr;
    const DerivedStaging st = registrar.stage(
        paleo::realization::kAssetTypeStat,
        tr( "%1·%2" ).arg( set.title, paleo::realization::statisticDisplayLabel( token ) ),
        QStringLiteral( "REALSTAT_%1_%2.tif" ).arg( setId, token ), &stageErr );
    if ( !st.isValid() || !writeStatRaster( st.absolutePath, field, cols, rows, gt, projWkt ) )
    {
      setError( error, stageErr.isEmpty()
                           ? tr( "统计面 %1 写盘失败" ).arg( token )
                           : stageErr );
      return false;
    }
    QVariantMap extra;
    extra.insert( paleo::realization::kKeySetId, setId );
    extra.insert( paleo::realization::kKeyStatistic, token );
    extra.insert( paleo::realization::kKeyMemberCount, rasters.size() );
    extra.insert( QStringLiteral( "horizon" ), horizon );
    extra.insert( QStringLiteral( "title" ),
                  tr( "%1·%2" ).arg( set.title, paleo::realization::statisticDisplayLabel( token ) ) );
    extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
    QString commitErr;
    if ( !registrar.commit( st, memberParentIds,
                            QStringLiteral( "paleo:realization_stat" ), extra, &commitErr ) )
    {
      setError( error, commitErr );
      return false;
    }
    if ( m_layers )
    {
      LayerDeclaration decl;
      decl.layerId = statLayerId( setId, token );
      decl.horizon = horizon;
      decl.type = QStringLiteral( "raster" );
      decl.source = st.absolutePath;
      decl.group = QStringLiteral( "04_SingleFactor/Realizations" );
      decl.title = tr( "%1·%2" ).arg( set.title, paleo::realization::statisticDisplayLabel( token ) );
      QString declErr;
      if ( !m_layers->declare( decl, &declErr ) )
        qWarning( "realization stat layer declare failed: %s", qPrintable( declErr ) );
    }
    tokens << token;
    return true;
  };

  if ( want.mean && !commitStat( paleo::realization::kStatMean, mean ) )
    return false;
  if ( want.stddev && !commitStat( paleo::realization::kStatStdDev, stddev ) )
    return false;
  int bandIdx = 0;
  if ( want.p10 && !commitStat( paleo::realization::kStatP10, bands[bandIdx++] ) )
    return false;
  if ( want.p90 && !commitStat( paleo::realization::kStatP90, bands[bandIdx++] ) )
    return false;

  if ( !tokens.isEmpty() )
    emit realizationStatsDerived( setId, tokens );
  return true;
}

bool RealizationWorkflow::deriveAllStatistics( const QString &setId, QString *error )
{
  paleo::ensemble::StatsRequest want;
  want.mean = want.stddev = want.p10 = want.p90 = true;
  return deriveStatistics( setId, want, error );
}

bool RealizationWorkflow::differenceOfMeans( const QString &setIdA,
                                             const QString &setIdB, QString *error )
{
  if ( !isBound() )
  {
    setError( error, tr( "realization workflow 未绑定 catalog/projectDir" ) );
    return false;
  }
  if ( setIdA.isEmpty() || setIdB.isEmpty() || setIdA == setIdB )
  {
    setError( error, tr( "差值需要两个不同的集合" ) );
    return false;
  }
  // 缺均值面的集合先补派生（派生本身是完整记档路径）。
  const auto ensureMean = [this]( const QString &sid, QString *err ) -> QString {
    QString vid = paleo::realization::meanSurfaceVersionId( *m_catalog, sid );
    if ( vid.isEmpty() )
    {
      paleo::ensemble::StatsRequest want;
      want.mean = true;
      if ( !deriveStatistics( sid, want, err ) )
        return QString();
      vid = paleo::realization::meanSurfaceVersionId( *m_catalog, sid );
    }
    return vid;
  };
  const QString vidA = ensureMean( setIdA, error );
  if ( vidA.isEmpty() )
    return false;
  const QString vidB = ensureMean( setIdB, error );
  if ( vidB.isEmpty() )
    return false;

  const QString pathA = DataCatalog::resolvedVersionPath(
      m_projectDir, m_catalog->versionById( vidA ) );
  const QString pathB = DataCatalog::resolvedVersionPath(
      m_projectDir, m_catalog->versionById( vidB ) );
  GDALAllRegister();
  GDALDatasetH dsA = GDALOpen( pathA.toUtf8().constData(), GA_ReadOnly );
  GDALDatasetH dsB = pathB.isEmpty() ? nullptr
                                    : GDALOpen( pathB.toUtf8().constData(), GA_ReadOnly );
  if ( !dsA || !dsB )
  {
    if ( dsA )
      GDALClose( dsA );
    if ( dsB )
      GDALClose( dsB );
    setError( error, tr( "集合均值面打不开：%1 / %2" ).arg( pathA, pathB ) );
    return false;
  }
  const auto guard = qScopeGuard( [dsA, dsB] {
    GDALClose( dsA );
    GDALClose( dsB );
  } );
  Q_UNUSED( guard );

  const int cols = GDALGetRasterXSize( dsA );
  const int rows = GDALGetRasterYSize( dsA );
  if ( GDALGetRasterXSize( dsB ) != cols || GDALGetRasterYSize( dsB ) != rows )
  {
    setError( error, tr( "两集合均值面网格不一致（%1×%2 vs %3×%4）——不能差" )
                         .arg( cols ).arg( rows )
                         .arg( GDALGetRasterXSize( dsB ) )
                         .arg( GDALGetRasterYSize( dsB ) ) );
    return false;
  }
  double gt[6] = { 0, 0, 0, 0, 0, 0 };
  GDALGetGeoTransform( dsA, gt );
  const QByteArray projWkt = GDALGetProjectionRef( dsA )
                                 ? QByteArray( GDALGetProjectionRef( dsA ) )
                                 : QByteArray();

  const std::size_t cells = static_cast<std::size_t>( cols ) * rows;
  std::vector<double> a( cells ), b( cells ), diff( cells );
  if ( !readBandSlice( GDALGetRasterBand( dsA, 1 ), 0, rows, cols, a.data() ) ||
       !readBandSlice( GDALGetRasterBand( dsB, 1 ), 0, rows, cols, b.data() ) )
  {
    setError( error, tr( "均值面读盘失败" ) );
    return false;
  }
  // rasteralgebra 的 Subtract 走 float——这里直接逐像元 double 减，语义同
  //（任一 NaN → NaN 诚实传播）。
  for ( std::size_t i = 0; i < cells; ++i )
    diff[i] = ( std::isfinite( a[i] ) && std::isfinite( b[i] ) )
                  ? a[i] - b[i]
                  : std::numeric_limits<double>::quiet_NaN();

  const QString titleA = m_catalog->assetById( setIdA ).displayName;
  const QString titleB = m_catalog->assetById( setIdB ).displayName;
  // 差值层的 horizon：两集合同层位才挂（不同层位 → 空，horizon-agnostic）。
  const auto setHorizon = [this]( const QString &sid ) {
    const paleo::realization::RealizationSet s =
        paleo::realization::setById( *m_catalog, sid );
    return s.members.isEmpty()
               ? QString()
               : m_catalog->versionById( s.members.first().versionId )
                     .extra.value( QStringLiteral( "horizon" ) )
                     .toString();
  };
  const QString hA = setHorizon( setIdA );
  const QString diffHorizon = ( hA == setHorizon( setIdB ) ) ? hA : QString();
  DerivedAssetRegistrar registrar( m_catalog.data(), m_projectDir );
  QString stageErr;
  const DerivedStaging st = registrar.stage(
      paleo::realization::kAssetTypeDiff,
      tr( "%1 − %2 均值差" ).arg( titleA, titleB ),
      QStringLiteral( "REALDIFF_%1_%2.tif" ).arg( setIdA, setIdB ), &stageErr );
  if ( !st.isValid() || !writeStatRaster( st.absolutePath, diff, cols, rows, gt, projWkt ) )
  {
    setError( error, stageErr.isEmpty() ? tr( "差值面写盘失败" ) : stageErr );
    return false;
  }
  QVariantMap extra;
  extra.insert( paleo::realization::kKeySetIds, QStringList{ setIdA, setIdB } );
  extra.insert( paleo::realization::kKeyStatistic, paleo::realization::kStatMeanDiff );
  extra.insert( QStringLiteral( "horizon" ), diffHorizon );
  extra.insert( QStringLiteral( "title" ), tr( "%1 − %2 均值差" ).arg( titleA, titleB ) );
  extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
  QString commitErr;
  if ( !registrar.commit( st, QStringList{ vidA, vidB },
                          QStringLiteral( "paleo:realization_diff" ), extra, &commitErr ) )
  {
    setError( error, commitErr );
    return false;
  }
  if ( m_layers )
  {
    LayerDeclaration decl;
    decl.layerId = diffLayerId( setIdA, setIdB );
    decl.horizon = diffHorizon;
    decl.type = QStringLiteral( "raster" );
    decl.source = st.absolutePath;
    decl.group = QStringLiteral( "04_SingleFactor/Realizations" );
    decl.title = tr( "%1 − %2 均值差" ).arg( titleA, titleB );
    QString declErr;
    if ( !m_layers->declare( decl, &declErr ) )
      qWarning( "realization diff layer declare failed: %s", qPrintable( declErr ) );
  }
  emit realizationDiffReady( setIdA, setIdB, diffLayerId( setIdA, setIdB ) );
  return true;
}

PaleoTask *RealizationWorkflow::deriveAllStatisticsAsync( const QString &setId, QString *error )
{
  if ( !isBound() )
  {
    setError( error, tr( "realization workflow 未绑定 catalog/projectDir" ) );
    return nullptr;
  }
  if ( !m_tasks || !m_runnerHolder )
  {
    setError( error, tr( "PaleoTaskService 未注入" ) );
    return nullptr;
  }
  if ( setId.isEmpty() )
  {
    setError( error, tr( "集合 id 为空" ) );
    return nullptr;
  }
  if ( isBusy() )
  {
    setError( error, tr( "正在进行计算，请稍候" ) );
    return nullptr;
  }

  const paleo::realization::RealizationSet set =
      paleo::realization::setById( *m_catalog, setId );
  if ( set.members.size() < 2 )
  {
    setError( error, tr( "集合 %1 成员不足两个（仅 %2 个）——单成员集合无不确定性可派生" )
                         .arg( setId ).arg( set.members.size() ) );
    return nullptr;
  }

  auto job = std::make_shared<RealizationJob>();
  job->kind = RealizationJob::Kind::DeriveAllStats;
  job->setId = setId;

  paleo::jobs::JobRunner<RealizationJob>::Callbacks cb;

  cb.prepare = [this]( RealizationJob &j, QString *err ) -> bool {
    const paleo::realization::RealizationSet s =
        paleo::realization::setById( *m_catalog, j.setId );
    if ( s.members.size() < 2 )
    {
      setError( err, tr( "单成员集合无不确定性可派生" ) );
      return false;
    }

    const auto existingStats = paleo::realization::statSurfaces( *m_catalog, j.setId );
    QSet<QString> existingTokens;
    for ( const auto &st : existingStats )
      existingTokens.insert( st.token );

    j.want.mean = !existingTokens.contains( paleo::realization::kStatMean );
    j.want.stddev = !existingTokens.contains( paleo::realization::kStatStdDev );
    j.want.p10 = !existingTokens.contains( paleo::realization::kStatP10 );
    j.want.p90 = !existingTokens.contains( paleo::realization::kStatP90 );

    if ( !j.want.mean && !j.want.stddev && !j.want.p10 && !j.want.p90 )
    {
      return true; // 幂等面，全部已存在
    }

    j.title = s.title;
    j.horizon = m_catalog->versionById( s.members.first().versionId )
                    .extra.value( QStringLiteral( "horizon" ) )
                    .toString();
    for ( const auto &m : s.members )
    {
      const CatalogVersion v = m_catalog->versionById( m.versionId );
      const QString abs = DataCatalog::resolvedVersionPath( m_projectDir, v );
      if ( abs.isEmpty() || !QFile::exists( abs ) )
      {
        setError( err, tr( "成员 %1 的受管栅格缺席（%2）——集合数据不全，停止派生" )
                           .arg( m.index ).arg( v.path ) );
        return false;
      }
      j.memberParentIds << m.versionId;
      j.members.push_back( { m.index, abs } );
    }

    GDALAllRegister();
    GDALDatasetH ds0 = GDALOpen( j.members.first().rasterPath.toUtf8().constData(), GA_ReadOnly );
    if ( !ds0 )
    {
      setError( err, tr( "成员 %1 栅格打不开" ).arg( j.members.first().index ) );
      return false;
    }
    j.cols = GDALGetRasterXSize( ds0 );
    j.rows = GDALGetRasterYSize( ds0 );
    GDALGetGeoTransform( ds0, j.gt );
    const char *proj = GDALGetProjectionRef( ds0 );
    j.projWkt = proj ? QString::fromUtf8( proj ) : QString();
    GDALClose( ds0 );

    for ( int i = 1; i < j.members.size(); ++i )
    {
      GDALDatasetH dsi = GDALOpen( j.members[i].rasterPath.toUtf8().constData(), GA_ReadOnly );
      if ( !dsi )
      {
        setError( err, tr( "成员 %1 栅格打不开" ).arg( j.members[i].index ) );
        return false;
      }
      const int ci = GDALGetRasterXSize( dsi );
      const int ri = GDALGetRasterYSize( dsi );
      GDALClose( dsi );
      if ( ci != j.cols || ri != j.rows )
      {
        setError( err, tr( "成员 %1 网格（%2×%3）与成员 0（%4×%5）不一致——集合成员必须同网格，拒绝派生" )
                           .arg( j.members[i].index ).arg( ci ).arg( ri ).arg( j.cols ).arg( j.rows ) );
        return false;
      }
    }
    return true;
  };

  cb.compute = []( RealizationJob &j, const paleo::jobs::CancelFn &cancel,
                   const paleo::jobs::ProgressFn &progress ) -> bool {
    if ( !j.want.mean && !j.want.stddev && !j.want.p10 && !j.want.p90 )
      return true;
    if ( cancel() )
      return false;

    GDALAllRegister();
    const int cols = j.cols;
    const int rows = j.rows;
    const std::size_t cells = static_cast<std::size_t>( cols ) * rows;

    struct OpenDataset {
      GDALDatasetH ds = nullptr;
      GDALRasterBandH band = nullptr;
    };
    QVector<OpenDataset> openDss;
    auto closeAll = qScopeGuard( [&openDss] {
      for ( auto &od : openDss )
        if ( od.ds )
          GDALClose( od.ds );
      openDss.clear();
    } );

    for ( const auto &m : j.members )
    {
      if ( cancel() )
        return false;
      GDALDatasetH ds = GDALOpen( m.rasterPath.toUtf8().constData(), GA_ReadOnly );
      if ( !ds )
      {
        j.error = QObject::tr( "成员 %1 栅格打不开" ).arg( m.index );
        return false;
      }
      openDss.push_back( { ds, GDALGetRasterBand( ds, 1 ) } );
    }

    progress( 10.0, QStringLiteral( "read" ) );

    std::vector<double> mean, stddev;
    if ( j.want.mean || j.want.stddev )
    {
      paleo::ensemble::StreamingMoments moments( cells );
      std::vector<double> field( cells );
      for ( int i = 0; i < openDss.size(); ++i )
      {
        if ( cancel() )
          return false;
        if ( !readBandSlice( openDss[i].band, 0, rows, cols, field.data() ) )
        {
          j.error = QObject::tr( "成员 %1 栅格读失败" ).arg( j.members[i].index );
          return false;
        }
        moments.addField( field.data() );
      }
      if ( j.want.mean )
        mean = moments.mean();
      if ( j.want.stddev )
        stddev = moments.stddevPopulation();
    }

    progress( 40.0, QStringLiteral( "moments" ) );

    std::vector<std::vector<double>> bands;
    if ( j.want.p10 || j.want.p90 )
    {
      if ( cancel() )
        return false;
      std::vector<double> qs;
      if ( j.want.p10 )
        qs.push_back( 0.10 );
      if ( j.want.p90 )
        qs.push_back( 0.90 );
      const auto readBand = [&openDss, cols]( std::size_t member, int row0, int nRows, double *out ) {
        return readBandSlice( openDss.at( member ).band, row0, nRows, cols, out );
      };
      bands = paleo::ensemble::quantilesBanded( openDss.size(), cols, rows, qs, kMaxBandValues, readBand );
      if ( bands.size() != qs.size() )
      {
        j.error = QObject::tr( "分位数行带计算中止（成员读失败）" );
        return false;
      }
    }

    progress( 70.0, QStringLiteral( "quantiles" ) );

    const QString tempDir = QDir::tempPath();
    const quint64 ts = QDateTime::currentMSecsSinceEpoch();
    const QByteArray projWktBytes = j.projWkt.toUtf8();

    const auto writeOut = [&]( const QString &token, const std::vector<double> &field ) -> bool {
      const QString tmpFile = QDir( tempDir ).filePath(
          QStringLiteral( "paleo_realstat_%1_%2_%3.tif" ).arg( j.setId, token ).arg( ts ) );
      if ( !writeStatRaster( tmpFile, field, cols, rows, j.gt, projWktBytes ) )
      {
        j.error = QObject::tr( "统计面 %1 临时写盘失败" ).arg( token );
        return false;
      }
      j.outputs.push_back( { token, tmpFile } );
      return true;
    };

    if ( j.want.mean && !writeOut( paleo::realization::kStatMean, mean ) )
      return false;
    if ( j.want.stddev && !writeOut( paleo::realization::kStatStdDev, stddev ) )
      return false;
    int bIdx = 0;
    if ( j.want.p10 && !writeOut( paleo::realization::kStatP10, bands[bIdx++] ) )
      return false;
    if ( j.want.p90 && !writeOut( paleo::realization::kStatP90, bands[bIdx++] ) )
      return false;

    progress( 100.0, QStringLiteral( "complete" ) );
    return true;
  };

  cb.commit = [this]( RealizationJob &j, QString *err ) -> bool {
    DerivedAssetRegistrar registrar( m_catalog.data(), m_projectDir );
    QStringList tokens;
    for ( const auto &out : j.outputs )
    {
      QString stageErr;
      const DerivedStaging st = registrar.stage(
          paleo::realization::kAssetTypeStat,
          tr( "%1·%2" ).arg( j.title, paleo::realization::statisticDisplayLabel( out.token ) ),
          QStringLiteral( "REALSTAT_%1_%2.tif" ).arg( j.setId, out.token ), &stageErr );
      if ( !st.isValid() )
      {
        setError( err, stageErr );
        emit errorOccurred( stageErr );
        return false;
      }
      if ( QFile::exists( st.absolutePath ) )
        QFile::remove( st.absolutePath );
      if ( !QFile::copy( out.tempPath, st.absolutePath ) )
      {
        const QString msg = tr( "统计面临时文件复制到受管区失败" );
        setError( err, msg );
        emit errorOccurred( msg );
        return false;
      }
      QFile::remove( out.tempPath );

      QVariantMap extra;
      extra.insert( paleo::realization::kKeySetId, j.setId );
      extra.insert( paleo::realization::kKeyStatistic, out.token );
      extra.insert( paleo::realization::kKeyMemberCount, j.members.size() );
      extra.insert( QStringLiteral( "horizon" ), j.horizon );
      extra.insert( QStringLiteral( "title" ),
                    tr( "%1·%2" ).arg( j.title, paleo::realization::statisticDisplayLabel( out.token ) ) );
      extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
      QString commitErr;
      if ( !registrar.commit( st, j.memberParentIds,
                              QStringLiteral( "paleo:realization_stat" ), extra, &commitErr ) )
      {
        setError( err, commitErr );
        emit errorOccurred( commitErr );
        return false;
      }
      if ( m_layers )
      {
        LayerDeclaration decl;
        decl.layerId = statLayerId( j.setId, out.token );
        decl.horizon = j.horizon;
        decl.type = QStringLiteral( "raster" );
        decl.source = st.absolutePath;
        decl.group = QStringLiteral( "04_SingleFactor/Realizations" );
        decl.title = tr( "%1·%2" ).arg( j.title, paleo::realization::statisticDisplayLabel( out.token ) );
        QString declErr;
        if ( !m_layers->declare( decl, &declErr ) )
          qWarning( "realization stat layer declare failed: %s", qPrintable( declErr ) );
      }
      tokens << out.token;
    }
    if ( !tokens.isEmpty() )
      emit realizationStatsDerived( j.setId, tokens );
    return true;
  };

  cb.cleanup = []( RealizationJob &j ) {
    for ( const auto &out : j.outputs )
      if ( QFile::exists( out.tempPath ) )
        QFile::remove( out.tempPath );
    j.outputs.clear();
  };

  m_busy = true;
  emit busyChanged( true );

  PaleoTask *task = m_runnerHolder->runner.start(
      tr( "派生集合统计：%1" ).arg( set.title.isEmpty() ? setId : set.title ),
      job, cb, QString(), true /* quiet */ );

  if ( !task )
  {
    m_busy = false;
    emit busyChanged( false );
    setError( error, tr( "任务启动失败" ) );
    return nullptr;
  }
  return task;
}

PaleoTask *RealizationWorkflow::differenceOfMeansAsync( const QString &setIdA, const QString &setIdB,
                                                        QString *error )
{
  if ( !isBound() )
  {
    setError( error, tr( "realization workflow 未绑定 catalog/projectDir" ) );
    return nullptr;
  }
  if ( !m_tasks || !m_runnerHolder )
  {
    setError( error, tr( "PaleoTaskService 未注入" ) );
    return nullptr;
  }
  if ( setIdA.isEmpty() || setIdB.isEmpty() || setIdA == setIdB )
  {
    setError( error, tr( "差值需要两个不同的集合" ) );
    return nullptr;
  }
  if ( isBusy() )
  {
    setError( error, tr( "正在进行计算，请稍候" ) );
    return nullptr;
  }

  const auto ensureMean = [this]( const QString &sid, QString *err ) -> QString {
    QString vid = paleo::realization::meanSurfaceVersionId( *m_catalog, sid );
    if ( vid.isEmpty() )
    {
      paleo::ensemble::StatsRequest want;
      want.mean = true;
      if ( !deriveStatistics( sid, want, err ) )
        return QString();
      vid = paleo::realization::meanSurfaceVersionId( *m_catalog, sid );
    }
    return vid;
  };
  const QString vidA = ensureMean( setIdA, error );
  if ( vidA.isEmpty() )
    return nullptr;
  const QString vidB = ensureMean( setIdB, error );
  if ( vidB.isEmpty() )
    return nullptr;

  auto job = std::make_shared<RealizationJob>();
  job->kind = RealizationJob::Kind::DifferenceOfMeans;
  job->setId = setIdA;
  job->setIdB = setIdB;
  job->memberParentIds = { vidA, vidB };

  paleo::jobs::JobRunner<RealizationJob>::Callbacks cb;

  cb.prepare = [this, vidA, vidB]( RealizationJob &j, QString *err ) -> bool {
    j.meanPathA = DataCatalog::resolvedVersionPath( m_projectDir, m_catalog->versionById( vidA ) );
    j.meanPathB = DataCatalog::resolvedVersionPath( m_projectDir, m_catalog->versionById( vidB ) );
    GDALAllRegister();
    GDALDatasetH dsA = GDALOpen( j.meanPathA.toUtf8().constData(), GA_ReadOnly );
    GDALDatasetH dsB = GDALOpen( j.meanPathB.toUtf8().constData(), GA_ReadOnly );
    if ( !dsA || !dsB )
    {
      if ( dsA ) GDALClose( dsA );
      if ( dsB ) GDALClose( dsB );
      setError( err, tr( "集合均值面打不开：%1 / %2" ).arg( j.meanPathA, j.meanPathB ) );
      return false;
    }
    j.cols = GDALGetRasterXSize( dsA );
    j.rows = GDALGetRasterYSize( dsA );
    if ( GDALGetRasterXSize( dsB ) != j.cols || GDALGetRasterYSize( dsB ) != j.rows )
    {
      GDALClose( dsA );
      GDALClose( dsB );
      setError( err, tr( "两集合均值面网格不一致（%1×%2 vs %3×%4）——不能差" )
                           .arg( j.cols ).arg( j.rows )
                           .arg( GDALGetRasterXSize( dsB ) )
                           .arg( GDALGetRasterYSize( dsB ) ) );
      return false;
    }
    GDALGetGeoTransform( dsA, j.gt );
    const char *proj = GDALGetProjectionRef( dsA );
    j.projWkt = proj ? QString::fromUtf8( proj ) : QString();
    GDALClose( dsA );
    GDALClose( dsB );

    j.title = m_catalog->assetById( j.setId ).displayName;
    const QString titleB = m_catalog->assetById( j.setIdB ).displayName;

    const auto setHorizon = [this]( const QString &sid ) {
      const paleo::realization::RealizationSet s =
          paleo::realization::setById( *m_catalog, sid );
      return s.members.isEmpty()
                 ? QString()
                 : m_catalog->versionById( s.members.first().versionId )
                       .extra.value( QStringLiteral( "horizon" ) )
                       .toString();
    };
    const QString hA = setHorizon( j.setId );
    j.horizon = ( hA == setHorizon( j.setIdB ) ) ? hA : QString();
    return true;
  };

  cb.compute = []( RealizationJob &j, const paleo::jobs::CancelFn &cancel,
                   const paleo::jobs::ProgressFn &progress ) -> bool {
    if ( cancel() )
      return false;
    GDALAllRegister();
    GDALDatasetH dsA = GDALOpen( j.meanPathA.toUtf8().constData(), GA_ReadOnly );
    GDALDatasetH dsB = GDALOpen( j.meanPathB.toUtf8().constData(), GA_ReadOnly );
    if ( !dsA || !dsB )
    {
      if ( dsA ) GDALClose( dsA );
      if ( dsB ) GDALClose( dsB );
      j.error = QObject::tr( "集合均值面打不开" );
      return false;
    }
    const auto guard = qScopeGuard( [dsA, dsB] {
      GDALClose( dsA );
      GDALClose( dsB );
    } );
    const int cols = j.cols;
    const int rows = j.rows;
    const std::size_t cells = static_cast<std::size_t>( cols ) * rows;
    std::vector<double> a( cells ), b( cells ), diff( cells );
    if ( !readBandSlice( GDALGetRasterBand( dsA, 1 ), 0, rows, cols, a.data() ) ||
         !readBandSlice( GDALGetRasterBand( dsB, 1 ), 0, rows, cols, b.data() ) )
    {
      j.error = QObject::tr( "均值面读盘失败" );
      return false;
    }
    for ( std::size_t i = 0; i < cells; ++i )
      diff[i] = ( std::isfinite( a[i] ) && std::isfinite( b[i] ) )
                    ? a[i] - b[i]
                    : std::numeric_limits<double>::quiet_NaN();

    const QString tmpFile = QDir( QDir::tempPath() ).filePath(
        QStringLiteral( "paleo_realdiff_%1_%2_%3.tif" )
            .arg( j.setId, j.setIdB ).arg( QDateTime::currentMSecsSinceEpoch() ) );
    if ( !writeStatRaster( tmpFile, diff, cols, rows, j.gt, j.projWkt.toUtf8() ) )
    {
      j.error = QObject::tr( "差值面写盘失败" );
      return false;
    }
    j.outputs.push_back( { paleo::realization::kStatMeanDiff, tmpFile } );
    progress( 100.0, QStringLiteral( "complete" ) );
    return true;
  };

  cb.commit = [this]( RealizationJob &j, QString *err ) -> bool {
    DerivedAssetRegistrar registrar( m_catalog.data(), m_projectDir );
    const QString titleB = m_catalog->assetById( j.setIdB ).displayName;
    QString stageErr;
    const DerivedStaging st = registrar.stage(
        paleo::realization::kAssetTypeDiff,
        tr( "%1 − %2 均值差" ).arg( j.title, titleB ),
        QStringLiteral( "REALDIFF_%1_%2.tif" ).arg( j.setId, j.setIdB ), &stageErr );
    if ( !st.isValid() )
    {
      setError( err, stageErr );
      emit errorOccurred( stageErr );
      return false;
    }
    if ( QFile::exists( st.absolutePath ) )
      QFile::remove( st.absolutePath );
    if ( !QFile::copy( j.outputs.first().tempPath, st.absolutePath ) )
    {
      const QString msg = tr( "差值面临时文件复制到受管区失败" );
      setError( err, msg );
      emit errorOccurred( msg );
      return false;
    }
    QFile::remove( j.outputs.first().tempPath );

    QVariantMap extra;
    extra.insert( paleo::realization::kKeySetIds, QStringList{ j.setId, j.setIdB } );
    extra.insert( paleo::realization::kKeyStatistic, paleo::realization::kStatMeanDiff );
    extra.insert( QStringLiteral( "horizon" ), j.horizon );
    extra.insert( QStringLiteral( "title" ), tr( "%1 − %2 均值差" ).arg( j.title, titleB ) );
    extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
    QString commitErr;
    if ( !registrar.commit( st, j.memberParentIds,
                            QStringLiteral( "paleo:realization_diff" ), extra, &commitErr ) )
    {
      setError( err, commitErr );
      emit errorOccurred( commitErr );
      return false;
    }
    if ( m_layers )
    {
      LayerDeclaration decl;
      decl.layerId = diffLayerId( j.setId, j.setIdB );
      decl.horizon = j.horizon;
      decl.type = QStringLiteral( "raster" );
      decl.source = st.absolutePath;
      decl.group = QStringLiteral( "04_SingleFactor/Realizations" );
      decl.title = tr( "%1 − %2 均值差" ).arg( j.title, titleB );
      QString declErr;
      if ( !m_layers->declare( decl, &declErr ) )
        qWarning( "realization diff layer declare failed: %s", qPrintable( declErr ) );
    }
    emit realizationDiffReady( j.setId, j.setIdB, diffLayerId( j.setId, j.setIdB ) );
    return true;
  };

  cb.cleanup = []( RealizationJob &j ) {
    for ( const auto &out : j.outputs )
      if ( QFile::exists( out.tempPath ) )
        QFile::remove( out.tempPath );
    j.outputs.clear();
  };

  m_busy = true;
  emit busyChanged( true );

  PaleoTask *task = m_runnerHolder->runner.start(
      tr( "两集合均值差：%1 vs %2" ).arg( setIdA, setIdB ),
      job, cb, QString(), true /* quiet */ );

  if ( !task )
  {
    m_busy = false;
    emit busyChanged( false );
    setError( error, tr( "任务启动失败" ) );
    return nullptr;
  }
  return task;
}
