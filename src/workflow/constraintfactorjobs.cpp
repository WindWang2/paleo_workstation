// 层：功能
// 方向57：从 constraintfactorjobs.cpp 按函数族析出。方法定义逐字搬迁，
// 类契约仍在 workflows.h（公共 API 零改动）；族间共享辅助经
// constraintfactorjobs_internal.h。
#include "workflows.h"
#include "constraintworkflow_internal.h"
#include "workflows_internal.h"
#include "../algorithms/geostat/kriging.h"     // KrigingParams / KrigingResult（方向18）
#include "../algorithms/geostat/sgs.h"         // SgsParams / SgsResult（方向18）
#include "../algorithms/geostat/variogram.h"   // 变差函数模型（方向18）
#include "../algorithms/rasterout.h"          // PaleoRasterOut / createFloatRaster
#include "../algorithms/ensemblestats.h"     // StatsRequest（方向47 集合统计派生）
#include "../catalog/datacatalog.h"
#include "../algorithms/singlefactor/cartographicworkfile.h"
#include "../domain/arearules.h"
#include "../domain/singlefactorrequest.h"  // 制图工作场不进融合/分相
#include "../io/constraintstore.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/factorcontour.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/qgiseditingservice.h"    // 拓扑提交门（geometryCommitError）
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisstyleservice.h"      // applyFaciesBoundaryStyle
#include "../services/jobrunner.h"
#include "../services/singlefactordef.h"
#include "boundarysemantics.h"             // 相界地质语义类型词表
#include "derivedassets.h"
#include "mappingworkflow.h"
#include "realizationworkflow.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QVariantList>
#include <qgscoordinatereferencesystem.h>
#include <qgsfield.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>
#include <qgsvectorfilewriter.h> // WS-C：参与井点 samples GPKG 写出
#include <qgsvectorlayer.h>
#include <gdal.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <variant>
#include "constraintfactorjobs_internal.h"

using namespace paleo::constraint_detail;

// 方向57：本文件只留「统一异步面」（JobRunner 三组分派 + 临时产物清理面）。
// 约束与单因素工作流（②）的**单因素作业面**部分。
// 方向20 轮4 从 constraintworkflow.cpp 二次拆出：约束 CRUD（构造 / setter /
// 增删改查约束）留在 constraintworkflow.cpp；三组单因素作业的生成链与三段式
//（局部方向 / 克里金-SGS / 等值线 / 解释性等值线 + JobRunner 统一面）在此。
// 实测两段**零跨段调用**（各自只经 m_proc / m_layers / m_catalog 等成员取数据），
// 故按语义边界切分安全。头文件契约不动。
// ---------------------------------------------------------------------------
// 方向20：三组作业的统一异步面（JobRunner 迁移）
//
// 上面 9 个 prepare/compute/publish 一行未改；这里只做「同一个协议的统一
// 入口」：把三组分派到各自的既有实现上，并把忙则互斥、取消接线、进度回包、
// 临时产物清理交给框架。行为等价点见 docs/progress/job-framework.md 的迁移
// 映射表。
// ---------------------------------------------------------------------------
bool ConstraintWorkflow::prepareConstraintJob( ConstraintJob &job, const QVariantMap &params,
                                               QString *error )
{
  // params 约定（与各调用点现状一致）：
  //   local_direction : {horizon, factorId, ...} + params
  //   analysis_contour: {horizon, factorLayerId, interval, levels, fixedLevels}
  //   interpretive   : {horizon, factorLayerId, levels, strict}
  const QString kind = params.value( QStringLiteral( "kind" ) ).toString();
  const QString horizon = params.value( QStringLiteral( "horizon" ) ).toString();
  const QString factorId = params.value( QStringLiteral( "factorId" ) ).toString();
  const QString factorLayerId = params.value( QStringLiteral( "factorLayerId" ) ).toString();
  // QVariant 没有 toVector（那是 QVariantList→QVector<double> 的事）：逐个转，
  // 元素非数值的按 0 计入——与各调用点现状「参数面由 UI 构造、类型已定」一致。
  QVector<double> levels;
  const QVariantList levelList = params.value( QStringLiteral( "levels" ) ).toList();
  levels.reserve( levelList.size() );
  for ( const QVariant &v : levelList )
    levels.append( v.toDouble() );

  if ( kind == QLatin1String( "local_direction" ) )
  {
    LocalDirectionJob payload;
    if ( !prepareLocalDirectionJob( horizon, factorId, params, &payload, error ) )
      return false;
    job.kind = ConstraintJobKind::LocalDirection;
    job.payload = payload;
    // 现状局部方向的 stage 词表：<10 prepare / <25 geometry / >=80 encode，
    // 其余 interpolate。原样搬过来，不改阈值。
    job.stageOf = []( double percent ) -> QString {
      const int pct = qBound( 0, qRound( percent ), 100 );
      if ( pct < 10 )
        return QStringLiteral( "prepare" );
      if ( pct < 25 )
        return QStringLiteral( "geometry" );
      if ( pct >= 80 )
        return QStringLiteral( "encode" );
      return QStringLiteral( "interpolate" );
    };
    return true;
  }
  if ( kind == QLatin1String( "analysis_contour" ) )
  {
    AnalysisContourJob payload;
    const double interval = params.value( QStringLiteral( "interval" ) ).toDouble();
    const bool fixedLevels = params.value( QStringLiteral( "fixedLevels" ) ).toBool();
    if ( !prepareAnalysisContourJob( horizon, factorLayerId, interval, levels, fixedLevels,
                                     &payload, error ) )
      return false;
    job.kind = ConstraintJobKind::AnalysisContour;
    job.payload = payload;
    job.stageOf = []( double ) -> QString { return QStringLiteral( "contour" ); };
    return true;
  }
  if ( kind == QLatin1String( "interpretive_contour" ) )
  {
    InterpretiveContourJob payload;
    const bool strict = params.value( QStringLiteral( "strict" ), true ).toBool();
    if ( !prepareInterpretiveContourJob( horizon, factorLayerId, levels, strict, &payload, error ) )
      return false;
    job.kind = ConstraintJobKind::InterpretiveContour;
    job.payload = payload;
    job.stageOf = []( double ) -> QString { return QStringLiteral( "contour" ); };
    return true;
  }
  paleo::workflow_detail::setError( error, tr( "未知的单因素作业类型：%1" ).arg( kind ) );
  return false;
}

bool ConstraintWorkflow::computeConstraintJob( ConstraintJob &job, const std::function<bool()> &cancelled,
                                               const std::function<void(double)> &progress )
{
  // 进度口径对齐现状：各 compute 的 progress 回调收的是百分数（0..100）。
  const std::function<void(double)> pct = [progress]( double p ) {
    if ( progress )
      progress( p );
  };
  switch ( job.kind )
  {
    case ConstraintJobKind::LocalDirection:
      return computeLocalDirectionJob( &std::get<LocalDirectionJob>( job.payload ), cancelled, pct );
    case ConstraintJobKind::AnalysisContour:
      return computeAnalysisContourJob( &std::get<AnalysisContourJob>( job.payload ), cancelled );
    case ConstraintJobKind::InterpretiveContour:
      return computeInterpretiveContourJob( &std::get<InterpretiveContourJob>( job.payload ), cancelled );
  }
  return false;
}

bool ConstraintWorkflow::publishConstraintJob( const ConstraintJob &job, QString *error )
{
  switch ( job.kind )
  {
    case ConstraintJobKind::LocalDirection:
      return publishLocalDirectionJob( std::get<LocalDirectionJob>( job.payload ), error );
    case ConstraintJobKind::AnalysisContour:
      return publishAnalysisContourJob( std::get<AnalysisContourJob>( job.payload ), error );
    case ConstraintJobKind::InterpretiveContour:
      return publishInterpretiveContourJob( std::get<InterpretiveContourJob>( job.payload ), error );
  }
  paleo::workflow_detail::setError( error, tr( "未知的单因素作业类型" ) );
  return false;
}

QStringList ConstraintWorkflow::constraintJobTempPaths( const ConstraintJob &job )
{
  // 各组的临时产物路径：取消/失败时框架 cleanup 钩子据此清理。路径集合与
  // 现状三处 dropTemp 的调用点一一对应。
  QStringList paths;
  switch ( job.kind )
  {
    case ConstraintJobKind::LocalDirection:
    {
      const auto &p = std::get<LocalDirectionJob>( job.payload );
      paths << p.outputPath;
      break;
    }
    case ConstraintJobKind::AnalysisContour:
    {
      const auto &p = std::get<AnalysisContourJob>( job.payload );
      paths << p.outputPath;
      break;
    }
    case ConstraintJobKind::InterpretiveContour:
    {
      const auto &p = std::get<InterpretiveContourJob>( job.payload );
      paths << p.workPath << p.contourPath;
      break;
    }
  }
  return paths;
}

PaleoTask *ConstraintWorkflow::startConstraintJob( paleo::jobs::JobRunner<ConstraintJob> &runner,
                                                  ConstraintJob job, QObject *progressSink )
{
  using paleo::jobs::JobRunner;

  auto shared = std::make_shared<ConstraintJob>( std::move( job ) );
  const QString title = shared->title.isEmpty() ? tr( "单因素作业" ) : shared->title;

  JobRunner<ConstraintJob>::Callbacks cb;

  // prepare 已在调用方完成（各调用点原本就把 prepare 放在 GUI 线程做，且需要
  // 把失败直接呈到页面 statusLabel——这属于 UI 语义，留在 UI 侧）。故框架的
  // prepare 段不重复抓快照，只做一次空转确认。
  cb.prepare = []( ConstraintJob &, QString * ) { return true; };

  // compute 跑在 worker 线程，但它要调用的 computeConstraintJob 是非静态成员
  // （要访问 catalog / layers）。捕获 this 是安全的：这三组的 compute 段本来
  // 就是「读层位文件 + GDAL 栅格运算」，不写活 catalog（#106 owner-thread 写
  // 守卫只覆盖 commit 段的 registrar 写面），与现状 worker 直调
  // computeXxxJob 的线程模型完全一致。
  cb.compute = [this, shared]( ConstraintJob &j, const paleo::jobs::CancelFn &cancelled,
                              const paleo::jobs::ProgressFn &progress ) {
    // 现状三处都是「把百分数映射成 stage 再 reportStage」。这里把 stageOf 的
    // 映射结果交给框架的 ProgressFn，由框架统一节流（50ms）后经
    // PaleoTask::reportStage 上任务面板——不再自行节流。
    return computeConstraintJob(
        j, cancelled,
        [shared, progress]( double percent ) {
          if ( !progress )
            return;
          const int pct = qBound( 0, qRound( percent ), 100 );
          const QString stage = shared->stageOf ? shared->stageOf( percent )
                                                : QStringLiteral( "interpolate" );
          progress( static_cast<double>( pct ), stage );
        } );
  };

  // commit：owner 线程，publish 段——「发布是临界区」，框架此时已不接受取消。
  cb.commit = [this]( ConstraintJob &j, QString * ) {
    QString err;
    return publishConstraintJob( j, &err );
  };

  // cleanup：未成功发布时清临时产物。现状是三处内联 dropTemp，这里换成框架
  // 钩子，判据（只清 paleo-sf- 前缀）保持一致。
  cb.cleanup = []( ConstraintJob &j ) {
    const QStringList paths = constraintJobTempPaths( j );
    for ( const QString &path : paths )
      discardTempTree( path );
  };

  Q_UNUSED( progressSink );
  return runner.start( title, shared, cb, QString(), /*quiet=*/true );
}
