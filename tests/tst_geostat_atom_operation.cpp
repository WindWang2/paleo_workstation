// 层：测试壳
// tests/tst_geostat_atom_operation.cpp — I1 地统计原子生产链端到端（W3–W9）。
//
// 链路：真实工具 JSON 解码（runCapability 同款）→ CapabilityHost::prepare
//（capture 受信解析 + 后台物化）→ Recovery 持久 operation → TaskService →
// NativeCppAdapter 数值核 → source_operation_id 发布/回放；另有真实
// AiChatToolRunner 入口用例与子进程真实重启用例（子进程在 checkpoint 处
// _Exit，父进程重开工程 resume/继承/发布）。结论对照解析可检验值（样本格心
// 处估值=样本值、离开样本处方差为正、同 seed 逐字节复现）与内核/适配器的
// 真实行为（computeCount、端口集、全 nodata 不是成功）。
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <paleo_data/service.h>

#include "ai/chat/chatmessage.h"
#include "ai/chat/domaintools.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "metadata/projectlock.h"
#include "qgis/geostatsamplereader.h"
#include "services/geostaticapabilityspec.h"
#include "services/operationretention.h"
#include "services/paleotaskservice.h"
#include "services/stratigraphicsubintervals.h"
#include "workflow/aichattoolrunner.h"
#include "workflow/capabilitygateway.h"
#include "workflow/capabilityhost.h"
#include "workflow/derivedassets.h"
#include "workflow/geostatatomcontract.h"
#include "workflow/geostatatominputs.h"
#include "workflow/geostatatomoperation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

using namespace paleo;
using namespace paleo::capability;
using namespace paleo::geostat_atom;

namespace {

// 25 个井点：0..40 米、10 米间距；sand_thick = sin(.35c) + .25r + 12。
constexpr int kWellsSide = 5;
// 数值 oracle 的落点：格心 (10,10) 是样本点（行 4 列 1）；行 0（y=50）无样本。
constexpr int kSampleCell = 4 * 6 + 1;
constexpr int kOffSampleCell = 0;

enum class WellsContent
{
  Normal,
  MissingField,
  NonNumericField,
  SomeSkipped,
  WithDuplicates,  // 正常 25 点 + 一个与 (0,0) 重合的样本
  AllNullValues,   // 全 null 值：OGR 无法推断数值类型 → 该列按字符串报告
  NoUsablePoints,  // 有 CRS 与几何列，但点全为空几何、仅一条线要素（非点）
  AllInvalid = AllNullValues
};

QString wellsJson( int side, bool withCrs, WellsContent content )
{
  const auto feature = []( double x, double y, const QString &property, bool nullGeometry ) {
    const QString geometry =
        nullGeometry ? QStringLiteral( "null" )
                     : QStringLiteral( "{\"type\":\"Point\",\"coordinates\":[%1,%2]}" ).arg( x ).arg( y );
    return QStringLiteral( "{\"type\":\"Feature\",\"properties\":{%1},\"geometry\":%2}" )
        .arg( property, geometry );
  };
  const auto value = [content]( double z ) {
    switch( content )
    {
      case WellsContent::NonNumericField:
        return QStringLiteral( "\"sand_thick\":\"abc\"" );
      case WellsContent::AllInvalid:
        return QStringLiteral( "\"sand_thick\":null" );
      case WellsContent::MissingField:
        return QStringLiteral( "\"z\":%1" ).arg( z );
      default:
        return QStringLiteral( "\"sand_thick\":%1" ).arg( z );
    }
  };
  QStringList features;
  for( int row = 0; row < side; ++row )
    for( int column = 0; column < side; ++column )
      features.append( feature( 10.0 * column, 10.0 * row,
                                value( std::sin( 0.35 * column ) + 0.25 * row + 12.0 ),
                                content == WellsContent::NoUsablePoints ) );
  // 一条线要素：图层有几何列与 CRS，但线不是点样本（读取器拒绝，不取首点）。
  if( content == WellsContent::NoUsablePoints )
    features.append( QStringLiteral( "{\"type\":\"Feature\",\"properties\":{\"sand_thick\":1.0},"
                                     "\"geometry\":{\"type\":\"LineString\","
                                     "\"coordinates\":[[0,0],[10,0]]}}" ) );
  if( content == WellsContent::SomeSkipped )
  {
    features.append( feature( 0, 0, QStringLiteral( "\"sand_thick\":null" ), false ) );
    features.append( feature( 0, 0, QStringLiteral( "\"sand_thick\":1.0" ), true ) );
  }
  if( content == WellsContent::WithDuplicates )
    features.append( feature( 0, 0, QStringLiteral( "\"sand_thick\":12.5" ), false ) );
  const QString crs = withCrs
                          ? QStringLiteral( "\"crs\":{\"type\":\"name\",\"properties\":{\"name\":\"EPSG:3857\"}}," )
                          : QString();
  return QStringLiteral( "{\"type\":\"FeatureCollection\",%1\"features\":[%2]}" )
      .arg( crs, features.join( QLatin1Char( ',' ) ) );
}

bool writeWells( const QString &path, int side, bool withCrs = true,
                 WellsContent content = WellsContent::Normal )
{
  QFile file( path );
  if( !file.open( QIODevice::WriteOnly ) )
    return false;
  file.write( wellsJson( side, withCrs, content ).toUtf8() );
  return true;
}

bool readRasterAll( const QString &path, QVector<double> *values, int *cols, int *rows )
{
  GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
  if( !ds )
    return false;
  *cols = GDALGetRasterXSize( ds );
  *rows = GDALGetRasterYSize( ds );
  int hasNoData = 0;
  const double noData = GDALGetRasterNoDataValue( GDALGetRasterBand( ds, 1 ), &hasNoData );
  values->resize( static_cast<std::size_t>( *cols ) * *rows );
  QVector<float> raw( static_cast<std::size_t>( *cols ) * *rows );
  const CPLErr io = GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Read, 0, 0, *cols, *rows, raw.data(),
                                  *cols, *rows, GDT_Float32, 0, 0 );
  GDALClose( ds );
  if( io != CE_None )
    return false;
  for( int i = 0; i < *cols * *rows; ++i )
    ( *values )[i] = ( hasNoData && raw[i] == static_cast<float>( noData ) )
                         ? std::numeric_limits<double>::quiet_NaN()
                         : static_cast<double>( raw[i] );
  return true;
}

QString fileSha256( const QString &path )
{
  QFile file( path );
  if( !file.open( QIODevice::ReadOnly ) )
    return {};
  return QString::fromLatin1(
      QCryptographicHash::hash( file.readAll(), QCryptographicHash::Sha256 ).toHex() );
}

QString compact( const QJsonObject &object )
{
  return QString::fromUtf8( QJsonDocument( object ).toJson( QJsonDocument::Compact ) );
}

// 宿主夹具。projectDir 可指向跨进程持久的目录（子进程重启用例）。
struct Host
{
  std::unique_ptr<QTemporaryDir> owned; // 自有工程目录（销毁顺序：锁 → 目录）
  QString projectDir;
  DataCatalog catalog;
  ProjectDirLock lock;
  std::unique_ptr<PaleoTaskService> tasks;
  std::unique_ptr<CapabilityHost> service;
  quint64 epoch = 1;
  QString documentId = QStringLiteral( "doc-i1" );
  QObject *receiver = nullptr;

  // 先建临时目录再取其真实路径（模板串不是目录）。
  static QString createOwned( std::unique_ptr<QTemporaryDir> &directory )
  {
    directory = std::make_unique<QTemporaryDir>( QDir::temp().filePath( QStringLiteral( "i1-XXXXXX" ) ) );
    return directory->path();
  }
  explicit Host( const QString &existing = QString() )
    : projectDir( existing.isEmpty() ? createOwned( owned ) : existing )
    , lock( projectDir )
  {
  }
  QString operationDir() const { return projectDir + QStringLiteral( "/.paleo/capability-ops" ); }
  QString snapshotRoot() const
  {
    return projectDir + QStringLiteral( "/.paleo/capability-ops/geostat-snapshots" );
  }
  bool open( QObject *owner )
  {
    receiver = owner;
    if( !lock.tryLock() || !catalog.open( projectDir ) )
      return false;
    restart();
    return true;
  }
  // 同进程重建 Host/Recovery/TaskService（生产装配 geostaticwiring 同款注册）：
  // 内存票据与运行时钩子全部丢弃，只有磁盘上的操作日志与快照留存。
  void restart()
  {
    service.reset();
    tasks = std::make_unique<PaleoTaskService>();
    service = std::make_unique<CapabilityHost>();
    service->setTaskService( tasks.get() );
    service->setCallerScope( host::Caller::Agent, QStringLiteral( "agent" ) );
    const QPointer<DataCatalog> catalogGuard( &catalog );
    GeostatAtomProviders providers;
    providers.alive = [catalogGuard] { return catalogGuard && catalogGuard->isOpen(); };
    providers.catalog = [catalogGuard] { return catalogGuard.data(); };
    service->setBindingProvider( [this, catalogGuard] {
      HostBinding b;
      b.projectDir = projectDir;
      b.operationDir = operationDir();
      b.catalog = catalogGuard.data();
      b.writeClaim = lock.claim();
      b.context.documentId = documentId;
      b.context.epoch = epoch;
      b.context.open = true;
      b.context.persistentIdentity = true;
      b.context.readOnly = false; // CallContext 默认失败关闭（readOnly=true）
      b.context.ownsWrites = true;
      b.context.allowedEffects =
          host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      return b;
    } );
    service->registerExecutionAdapter(
        kKrigingCapabilityId(), geostatAtomExecutionAdapter( kKrigingCapabilityId(), providers ) );
    service->registerExecutionAdapter(
        kSgsCapabilityId(), geostatAtomExecutionAdapter( kSgsCapabilityId(), providers ) );
  }
  // 等价 appcontext 换工程后的重绑：新文档、新代次，旧准备全部失效。
  void switchProject( const QString &document )
  {
    documentId = document;
    ++epoch;
    service->invalidate();
  }
  ExecuteResult run( const ExecuteRequest &request, const QString &title )
  {
    const auto prepared = service->prepare( request, host::Caller::Agent );
    if( !prepared.ok )
      return ExecuteResult{ false, prepared.code };
    return start( prepared.handle, request, title );
  }
  // start + 有界等待：回调在 owner 线程投递，QTest::qWaitFor 驱动事件循环
  //（非 void 辅助函数不能用 QTRY 宏）。start 被拒时返回 {false, 拒绝码}。
  ExecuteResult start( const QString &handle, const ExecuteRequest &request, const QString &title )
  {
    ExecuteResult result;
    QString error;
    bool done = false;
    PaleoTask *task = service->start( handle, request, receiver, title,
                                      [&]( ExecuteResult out ) { result = out; done = true; },
                                      &error );
    if( !task )
      return ExecuteResult{ false, error };
    if( !QTest::qWaitFor( [&] { return done; }, 120000 ) )
      return ExecuteResult{ false, QStringLiteral( "test_timeout" ) };
    return result;
  }
};

OperationLog journal( const Host &h )
{
  return OperationLog( h.operationDir(), h.projectDir, {} );
}

QJsonObject planOf( const OperationLog::Record &record )
{
  return record.frozenRequest.value( "plan" ).toObject();
}

QStringList portIds( const OperationLog::Record &record )
{
  QStringList ids;
  for( const auto &port : record.artifactManifest.value( "ports" ).toArray() )
    ids.append( port.toObject().value( "portId" ).toString() );
  ids.sort();
  return ids;
}

// 成员绝对路径（清单路径是工程相对）。
QString artifactPath( const Host &h, const OperationLog::Record &record, const QString &portId )
{
  for( const auto &port : record.artifactManifest.value( "ports" ).toArray() )
    if( port.toObject().value( "portId" ).toString() == portId )
      return h.projectDir + '/' + port.toObject().value( "path" ).toString();
  return {};
}

QVariantMap unitIdentity( const paleo::strat::UnitSelection &unit )
{
  return QVariantMap{ { "schemeId", unit.schemeId },
                      { "revision", unit.revision },
                      { "intervalId", unit.intervalId },
                      { "subIntervalId", unit.subIntervalId } };
}

// -5..55、cell 10 → 6×6：行 0（y=50）没有样本，其余格心落在井点上。
QVariantMap gridOf()
{
  return QVariantMap{ { "min_x", -5.0 }, { "max_x", 55.0 }, { "min_y", -5.0 }, { "max_y", 55.0 },
                      { "cell_size", 10.0 } };
}

QVariantMap krigingParameters()
{
  return QVariantMap{ { "VARIOGRAM_MODEL", QStringLiteral( "spherical" ) },
                      { "NUGGET", 0.0 }, { "SILL", 1.0 }, { "RANGE", 40.0 } };
}

struct ProjectFixture
{
  paleo::strat::UnitSelection s1;
  paleo::data::StratigraphicScheme committed;
  QString wellsVersion;

  // 登记一个受管井点版本（manifest_layer_id = layerKey；可空）。
  QString addWells( Host &h, const QString &name, const QString &layerKey, const QVariantMap &extra,
                    bool withCrs = true, WellsContent content = WellsContent::Normal )
  {
    DerivedAssetRegistrar registrar( &h.catalog, h.projectDir );
    QString error;
    const auto staged = registrar.stage( QStringLiteral( "well_factor_points" ), name,
                                         name + QStringLiteral( ".geojson" ), &error );
    if( !staged.isValid() || !writeWells( staged.absolutePath, kWellsSide, withCrs, content ) )
      return {};
    DerivedFileFingerprint fingerprint =
        DerivedFileFingerprint::read( staged.absolutePath, {}, &error );
    if( fingerprint.sha256.isEmpty() )
      return {};
    QVariantMap versionExtra = extra;
    if( !layerKey.isEmpty() )
      versionExtra.insert( QStringLiteral( "manifest_layer_id" ), layerKey );
    DerivedOutputSetReceipt receipt;
    if( !registrar.commit( staged, {}, QStringLiteral( "" ), versionExtra, &error, &fingerprint ) )
      return {};
    return staged.versionId;
  }

  // DS 方案（T70/T71 边界 → 区间 [T70,T71]，S1 子层段）+ 受管井点版本
  // wells.T70（字段 sand_thick：单因素词表 sandthick 的登记字段）。
  bool build( Host &h )
  {
    std::string detail;
    paleo::data::DataService ds;
    if( !ds.createProject( paleo::area_model::directory( h.projectDir ).toStdString(), "i1", detail ) )
      return false;
    const QString raw = QDir( h.projectDir ).filePath( QStringLiteral( "i1-source.txt" ) );
    {
      QFile file( raw );
      if( !file.open( QIODevice::WriteOnly ) )
        return false;
      file.write( "i1" );
    }
    paleo::data::ParsedFile parsed;
    parsed.path = raw.toStdString();
    parsed.kind = "test";
    parsed.status = "parsed";
    paleo::data::WellDataset well;
    well.wellInfo.wellName = "W-1";
    well.wellInfo.wellheadX = 0;
    well.wellInfo.wellheadY = 0;
    well.defaultDepthInfo.depthType = "MD";
    well.defaultDepthInfo.depthUnit = "m";
    parsed.wells.push_back( well );
    long long sourceId = 0;
    if( !ds.importPreparedFile( raw.toStdString(), raw.toStdString(), raw.toStdString(),
                                QCryptographicHash::hash( QByteArray( "i1" ),
                                                          QCryptographicHash::Sha256 )
                                    .toHex()
                                    .toStdString(),
                                parsed, sourceId, detail ) )
      return false;
    std::vector<paleo::data::WellSummary> wells;
    if( !ds.listWells( wells, detail ) || wells.empty() )
      return false;
    paleo::data::StratigraphicScheme scheme;
    std::vector<paleo::data::FrameworkIssue> issues;
    if( !ds.commitStratigraphicScheme(
             paleo::strat::draftScheme( "scheme", "scheme", { "T70", "T71" },
                                        { { wells.front().id, { { "T70", 100. }, { "T71", 200. } } } } ),
             {}, scheme, issues, detail ) )
      return false;
    committed = scheme;
    s1 = paleo::strat::makeSelection(
        scheme, QString::fromStdString( scheme.intervals.front().id.value ), 1 );
    ds.closeProject();
    if( !s1.isValid() )
      return false;
    wellsVersion = addWells( h, QStringLiteral( "wells" ), QStringLiteral( "wells.T70" ), {} );
    return !wellsVersion.isEmpty();
  }
};

QVariantMap sampleInputs( const ProjectFixture &fixture, const QString &sampleRef = QString() )
{
  return QVariantMap{ { "sample_ref", sampleRef.isEmpty()
                                          ? QStringLiteral( "catalog:" ) + fixture.wellsVersion
                                          : sampleRef },
                      { "factor_id", QStringLiteral( "sandthick" ) },
                      { "unit_selection", unitIdentity( fixture.s1 ) },
                      { "grid", gridOf() } };
}

// 由持久计划重建请求所需的夹具视图（跨进程重启用例：方案是子进程建的）。
ProjectFixture fixtureFrom( const QJsonObject &plan )
{
  ProjectFixture fixture;
  fixture.s1 = paleo::strat::fromMap( plan.value( "unit" ).toObject().toVariantMap() );
  fixture.wellsVersion = plan.value( "sample" ).toObject().value( "version_id" ).toString();
  return fixture;
}

QJsonObject toolArguments( const QString &requestId, const QString &intent,
                           const QVariantMap &parameters, const QVariantMap &inputs )
{
  return QJsonObject{ { "client_request_id", requestId },
                      { "binding_mode", QStringLiteral( "pinned-value" ) },
                      { "publish_intent", intent },
                      { "parameters", QJsonObject::fromVariantMap( parameters ) },
                      { "inputs", QJsonObject::fromVariantMap( inputs ) } };
}

struct Decoded
{
  bool ok = false;
  ExecuteRequest request;
  QJsonObject rejected;
};

// 生产工具入口解码（AiChatToolRunner::runCapability 同款调用）。
Decoded decodeTool( const QString &tool, const QString &requestId, const QString &intent,
                    const QVariantMap &parameters, const QVariantMap &inputs )
{
  Decoded out;
  out.rejected = prepareHostedChatToolRequest( out.request, tool,
                                               toolArguments( requestId, intent, parameters, inputs ) );
  out.ok = out.rejected.isEmpty();
  return out;
}

Decoded prepareRequest( const QString &requestId, const ProjectFixture &fixture )
{
  return decodeTool( kKrigingToolAlias(), requestId, QStringLiteral( "prepare" ), krigingParameters(),
                     sampleInputs( fixture ) );
}

Decoded computeRequest( const QString &requestId, const QString &prepareOperationId,
                        const QString &tool = QString() )
{
  return decodeTool( tool.isEmpty() ? kKrigingToolAlias() : tool, requestId,
                     QStringLiteral( "compute-only" ), QVariantMap{},
                     QVariantMap{ { "source_operation_id", prepareOperationId } } );
}

Decoded publishRequest( const QString &requestId, const QString &sourceOperationId,
                        const QString &tool = QString() )
{
  return decodeTool( tool.isEmpty() ? kKrigingToolAlias() : tool, requestId, QStringLiteral( "publish" ),
                     QVariantMap{}, QVariantMap{ { "source_operation_id", sourceOperationId } } );
}

// 工具回包投影（runCapability 完成回调里的同款调用）。
QJsonObject responseFor( const Host &h, const ExecuteResult &result, const QString &documentId,
                         const QString &scope )
{
  return geostatAtomToolResponse( chatToolResponse( result ), result, h.projectDir, h.operationDir(),
                                  documentId, scope );
}

// 可选录制（环境变量 PALEO_I1_RECORD_DIR 指向已存在目录）：把经真实 runner 发出的
// 请求与收到的回包原样落盘，供实施记录引用真实产物；不设时不产生任何文件。
void record( int step, const QString &tool, const QString &requestId, const QJsonObject &arguments,
             const QJsonObject &response )
{
  const QString directory = qEnvironmentVariable( "PALEO_I1_RECORD_DIR" );
  if( directory.isEmpty() || !QFileInfo::exists( directory ) )
    return;
  const QJsonObject entry{ { "step", step },
                           { "tool", tool },
                           { "client_request_id", requestId },
                           { "request", arguments },
                           { "response", response } };
  QFile file( QDir( directory ).filePath( QStringLiteral( "%1-%2.json" )
                                             .arg( step, 2, 10, QLatin1Char( '0' ) )
                                             .arg( tool.section( QLatin1Char( '.' ), -1 ) ) ) );
  if( !file.open( QIODevice::WriteOnly ) )
    return;
  file.write( QJsonDocument( entry ).toJson( QJsonDocument::Indented ) );
}

CatalogVersion publishedVersion( const DataCatalog &catalog, const QString &kind )
{
  for( const auto &version : catalog.versions() )
    if( version.extra.value( QStringLiteral( "kind" ) ).toString() == kind )
      return version;
  return {};
}

int geostatVersions( const DataCatalog &catalog, const QString &sampleVersion )
{
  int count = 0;
  for( const auto &version : catalog.versions() )
    if( version.id != sampleVersion )
      ++count;
  return count;
}

int computeCount( const Host &h, const QString &operationId )
{
  return int( h.service->recovery()->get( QStringLiteral( "agent" ), operationId )
                  .value( "computeCount" )
                  .toDouble() );
}

// 子进程：在同一工程目录里跑到指定阶段后退出（crash-running 在 "running"
// checkpoint 处 _Exit(90)——进程在数值 worker 完成前真实消失）。
constexpr int kCrashExit = 90;

int crashChild( const QString &root, const QString &phase )
{
  Host h( root );
  QObject receiver;
  if( !h.open( &receiver ) )
    return 70;
  ProjectFixture fixture;
  if( !fixture.build( h ) )
    return 71;
  const Decoded prepare = prepareRequest( QStringLiteral( "i1-prepare" ), fixture );
  if( !prepare.ok )
    return 72;
  const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "prepare" ) );
  if( !prepared.ok || prepared.execution != QLatin1String( "prepared" ) )
    return 73;
  if( phase == QLatin1String( "prepare" ) )
    return 0;
  if( phase == QLatin1String( "crash-running" ) )
    h.service->recovery()->setCheckpointHook( []( const QString &at ) {
      if( at == QLatin1String( "running" ) )
        std::_Exit( kCrashExit );
    } );
  const Decoded compute = computeRequest( QStringLiteral( "i1-compute" ), prepared.operationId );
  if( !compute.ok )
    return 74;
  const ExecuteResult computed = h.run( compute.request, QStringLiteral( "compute" ) );
  return phase == QLatin1String( "computed" ) && computed.ok ? 0 : 75;
}

} // namespace

class GeostatAtomOperationTests : public QObject
{
  Q_OBJECT
private slots:
  // 同一受管样本：kriging prepare →（重建宿主）→ compute-only 继承 →（重建宿主）
  // → 发布（不重算）→ 回放（不重算）→ 提交后取消保留事实 → 同键异参冲突。
  void krigingPrepareComputePublishReplayChain()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );

    // ① prepare：真实工具 JSON 解码 → capture 受信解析 → 后台物化 → 持久
    // phase=prepare 记录（返回持久 operationId，不是进程内票据）。
    const Decoded prepare = prepareRequest( QStringLiteral( "i1-prepare" ), fixture );
    QVERIFY2( prepare.ok, qPrintable( compact( prepare.rejected ) ) );
    const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "prepare" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    QCOMPARE( prepared.execution, QStringLiteral( "prepared" ) );
    QCOMPARE( prepared.commitState, QStringLiteral( "not_requested" ) );
    const QString prepareOperation = prepared.operationId;
    QVERIFY( !prepareOperation.isEmpty() );
    const OperationLog::Record prepareRecord = journal( h ).lookupRecordById( prepareOperation ).record;
    QCOMPARE( prepareRecord.schemaVersion, 2 );
    QCOMPARE( prepareRecord.frozenRequest.value( "phase" ).toString(), QStringLiteral( "prepare" ) );
    QCOMPARE( prepareRecord.computeCount, 0 ); // prepare 不运行数值核
    // 冻结计划：物化事实、来源/单位/关联、资源估算（按实际样本数）。
    const QJsonObject plan = planOf( prepareRecord );
    QCOMPARE( plan.value( "method" ).toString(), QStringLiteral( "kriging" ) );
    const QJsonObject sample = plan.value( "sample" ).toObject();
    QCOMPARE( sample.value( "sample_count" ).toInt(), kWellsSide * kWellsSide );
    QCOMPARE( sample.value( "skipped_rows" ).toInt(), 0 );
    QCOMPARE( sample.value( "value_unit" ).toString(), QStringLiteral( "m" ) );
    QCOMPARE( sample.value( "factor_id" ).toString(), QStringLiteral( "sandthick" ) );
    QCOMPARE( sample.value( "field" ).toString(), QStringLiteral( "sand_thick" ) );
    QCOMPARE( sample.value( "association" ).toString(), QStringLiteral( "layer_key" ) );
    QCOMPARE( sample.value( "version_id" ).toString(), fixture.wellsVersion );
    QCOMPARE( sample.value( "crs_authid" ).toString(), QStringLiteral( "EPSG:3857" ) );
    QCOMPARE( plan.value( "unit" ).toObject().value( "subIntervalId" ).toString(),
              fixture.s1.subIntervalId );
    QCOMPARE( plan.value( "unit" ).toObject().value( "revision" ).toString(), fixture.s1.revision );
    const QJsonObject estimate = plan.value( "resource_estimate" ).toObject();
    QCOMPARE( estimate.value( "sample_bytes" ).toDouble(), double( 25 * ( 24 + 48 ) ) );
    QCOMPARE( estimate.value( "grid_bytes" ).toDouble(), double( 36 * 16 ) );
    QVERIFY( estimate.value( "peak_bytes" ).toDouble() > 0 );
    QVERIFY( estimate.value( "peak_bytes" ).toDouble() <= 1024 * 1024 );
    // 快照：工程内、内容寻址；operation retention 认领其目录（记录引用随操作
    // 保留），且保护范围精确到该快照目录。
    const QString snapshotRelative = sample.value( "snapshot_path" ).toString();
    QCOMPARE( snapshotRelative, geostatSnapshotRelativePath( sample.value( "snapshot_sha256" ).toString() ) );
    const QString snapshot = h.projectDir + '/' + snapshotRelative;
    QVERIFY( QFileInfo( snapshot ).isFile() );
    QCOMPARE( fileSha256( snapshot ), sample.value( "snapshot_sha256" ).toString() );
    const paleo::storage::OperationRetention retention = paleo::storage::operationRetention( h.projectDir );
    QVERIFY2( retention.complete, qPrintable( retention.code ) );
    QVERIFY( retention.protectsPath( snapshot ) );
    QVERIFY( !retention.protectsPath( h.snapshotRoot() + QStringLiteral( "/other/samples.json" ) ) );
    QVERIFY( retention.versionIds.contains( fixture.wellsVersion ) );
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 ) ); // prepare 不发布任何版本
    // 模型可见回包：物化成本/来源/单位/参数（无路径）；他方作用域/文档读不到。
    const QJsonObject payload =
        responseFor( h, prepared, QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ) );
    QCOMPARE( payload.value( "code" ).toString(), QStringLiteral( "prepared" ) );
    QCOMPARE( payload.value( "operationId" ).toString(), prepareOperation );
    const QJsonObject facts = payload.value( "frozenPlan" ).toObject();
    QCOMPARE( facts.value( "sample" ).toObject().value( "count" ).toInt(), 25 );
    QCOMPARE( facts.value( "sample" ).toObject().value( "valueUnit" ).toString(), QStringLiteral( "m" ) );
    QCOMPARE( facts.value( "sample" ).toObject().value( "association" ).toString(),
              QStringLiteral( "layer_key" ) );
    QVERIFY( facts.value( "resourceEstimate" ).toObject().contains( QStringLiteral( "peak_bytes" ) ) );
    QCOMPARE( facts.value( "effectiveParameters" ).toObject().value( "RANGE" ).toDouble(), 40.0 );
    QCOMPARE( facts.value( "effectiveParameters" ).toObject().value( "MAX_POINTS" ).toInt(), 16 );
    QCOMPARE( facts.value( "grid" ).toObject().value( "cols" ).toInt(), 6 );
    QCOMPARE( facts.value( "constraintsApplied" ).toString(), QStringLiteral( "none" ) );
    QVERIFY( !facts.contains( QStringLiteral( "seed" ) ) );
    QVERIFY( !compact( facts ).contains( QStringLiteral( ".paleo/" ) ) );
    QVERIFY( !responseFor( h, prepared, QStringLiteral( "doc-i1" ), QStringLiteral( "someone-else" ) )
                 .contains( QStringLiteral( "frozenPlan" ) ) );
    QVERIFY( !responseFor( h, prepared, QStringLiteral( "doc-other" ), QStringLiteral( "agent" ) )
                 .contains( QStringLiteral( "frozenPlan" ) ) );

    // ② 重建宿主后按原 operation compute-only：继承持久冻结计划，不重新取样本。
    h.restart();
    const Decoded compute = computeRequest( QStringLiteral( "i1-compute" ), prepareOperation );
    QVERIFY2( compute.ok, qPrintable( compact( compute.rejected ) ) );
    const ExecuteResult computed = h.run( compute.request, QStringLiteral( "compute" ) );
    QVERIFY2( computed.ok, qPrintable( computed.code ) );
    QCOMPARE( computed.commitState, QStringLiteral( "not_requested" ) );
    QCOMPARE( computed.execution, QStringLiteral( "succeeded" ) );
    const OperationLog::Record computeRecord =
        journal( h ).lookupRecordById( computed.operationId ).record;
    QCOMPARE( computeRecord.computeCount, 1 ); // 真实数值核恰好一次
    QCOMPARE( computeRecord.frozenRequest.value( "sourceOperationId" ).toString(), prepareOperation );
    QCOMPARE( planOf( computeRecord ), plan ); // 冻结计划逐字节继承
    const QStringList krigingPorts{ QStringLiteral( "OUTPUT" ), QStringLiteral( "VARIANCE" ) };
    QCOMPARE( portIds( computeRecord ), krigingPorts );
    QCOMPARE( computed.summary.value( "finite_cells" ).toInt(), 36 );
    QCOMPARE( computed.summary.value( "nodata_cells" ).toInt(), 0 );
    QCOMPARE( computed.summary.value( "sample_count" ).toInt(), 25 );
    QCOMPARE( computed.summary.value( "value_unit" ).toString(), QStringLiteral( "m" ) );
    QCOMPARE( computed.summary.value( "constraints_applied" ).toString(), QStringLiteral( "none" ) );
    QVERIFY( computed.summary.contains( QStringLiteral( "solver_failures" ) ) );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 0 ); // compute-only 不发布
    // 数值 oracle：样本格心处估值 = 样本值、方差 = 0；离开样本处方差 > 0。
    QVector<double> values;
    int cols = 0, rows = 0;
    const QString outputPath = artifactPath( h, computeRecord, QStringLiteral( "OUTPUT" ) );
    QVERIFY2( readRasterAll( outputPath, &values, &cols, &rows ), qPrintable( outputPath ) );
    QCOMPARE( cols, 6 );
    QCOMPARE( rows, 6 );
    const double sampleValue = std::sin( 0.35 * 1 ) + 0.25 * 1 + 12.0;
    QVERIFY2( std::abs( values[kSampleCell] - sampleValue ) < 1e-3,
              qPrintable( QString::number( values[kSampleCell] ) ) );
    QVector<double> variance;
    QVERIFY( readRasterAll( artifactPath( h, computeRecord, QStringLiteral( "VARIANCE" ) ), &variance,
                            &cols, &rows ) );
    QVERIFY2( variance[kSampleCell] < 1e-6, qPrintable( QString::number( variance[kSampleCell] ) ) );
    QVERIFY2( variance[kOffSampleCell] > 0, qPrintable( QString::number( variance[kOffSampleCell] ) ) );
    // 估值落在样本值域内（样本最值：12.0 与 sin(1.4)+1+12）；全程有限，无 nodata。
    const auto [low, high] = std::minmax_element( values.cbegin(), values.cend() );
    QVERIFY2( *low >= 12.0 - 1e-2, qPrintable( QString::number( *low ) ) );
    QVERIFY2( *high <= 13.0 + std::sin( 1.4 ) + 1e-2, qPrintable( QString::number( *high ) ) );
    QVERIFY( std::all_of( values.cbegin(), values.cend(), []( double value ) { return std::isfinite( value ); } ) );

    // ③ 重建宿主后发布已计算结果：走 freezeSourcePublication，不重算、不重取样本。
    h.restart();
    const Decoded publish = publishRequest( QStringLiteral( "i1-publish" ), computed.operationId );
    QVERIFY2( publish.ok, qPrintable( compact( publish.rejected ) ) );
    const ExecuteResult committed = h.run( publish.request, QStringLiteral( "publish" ) );
    QVERIFY2( committed.ok, qPrintable( committed.code ) );
    QCOMPARE( committed.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( committed.presentation, QStringLiteral( "not_requested" ) ); // 不自动显示
    QCOMPARE( computeCount( h, computed.operationId ), 1 ); // 发布不再调用数值核
    QVERIFY( !committed.summary.value( "receipt" ).toMap().value( "outputSetId" ).toString().isEmpty() );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 ); // OUTPUT + VARIANCE
    const CatalogVersion estimateVersion =
        publishedVersion( h.catalog, QStringLiteral( "geostat_estimate_raster" ) );
    const CatalogVersion varianceVersion =
        publishedVersion( h.catalog, QStringLiteral( "geostat_variance_raster" ) );
    QVERIFY( !estimateVersion.id.isEmpty() && !varianceVersion.id.isEmpty() );
    // 结果单位/CRS/分析单元/来源摘要来自冻结值。
    const QStringList expectedParents{ fixture.wellsVersion };
    QCOMPARE( estimateVersion.parentVersionIds, expectedParents );
    QCOMPARE( estimateVersion.extra.value( "algorithm_id" ).toString(),
              QStringLiteral( "geostat:ordinary_kriging" ) );
    QCOMPARE( estimateVersion.extra.value( "value_unit" ).toString(), QStringLiteral( "m" ) );
    QCOMPARE( estimateVersion.extra.value( "factor_id" ).toString(), QStringLiteral( "sandthick" ) );
    QCOMPARE( estimateVersion.extra.value( "subIntervalId" ).toString(), fixture.s1.subIntervalId );
    QCOMPARE( estimateVersion.extra.value( "schemeRevision" ).toString(), fixture.s1.revision );
    QCOMPARE( estimateVersion.extra.value( "crs_authid" ).toString(), QStringLiteral( "EPSG:3857" ) );
    QCOMPARE( estimateVersion.extra.value( "source_sha256" ).toString(),
              sample.value( "source_sha256" ).toString() );
    QCOMPARE( estimateVersion.extra.value( "finite_cells" ).toInt(), 36 );
    QCOMPARE( estimateVersion.extra.value( "constraints_applied" ).toString(), QStringLiteral( "none" ) );
    QVERIFY( !estimateVersion.extra.contains( QStringLiteral( "subinterval_verified" ) ) );
    QCOMPARE( varianceVersion.parentVersionIds, expectedParents );
    // 发布后的 retention 仍完整（发布记录 + 派生版本 + 快照都在册）。
    const paleo::storage::OperationRetention afterPublish = paleo::storage::operationRetention( h.projectDir );
    QVERIFY2( afterPublish.complete, qPrintable( afterPublish.code ) );
    QVERIFY( afterPublish.protectsPath( snapshot ) );

    // ④ 同键重传：receipt 复用，无新版本、无重算。
    const ExecuteResult again = h.run( publish.request, QStringLiteral( "publish-again" ) );
    QVERIFY2( again.ok, qPrintable( again.code ) );
    QCOMPARE( again.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( again.operationId, committed.operationId );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
    const ExecuteResult replayed = h.run( compute.request, QStringLiteral( "compute-replay" ) );
    QVERIFY2( replayed.ok, qPrintable( replayed.code ) );
    QCOMPARE( replayed.operationId, computed.operationId );
    QCOMPARE( computeCount( h, computed.operationId ), 1 );
    // ⑤ 提交后取消：保留已提交事实。
    QCOMPARE( h.service->recovery()->cancel( QStringLiteral( "agent" ), committed.operationId )
                  .value( "code" )
                  .toString(),
              QStringLiteral( "cancel_after_commit" ) );
    QCOMPARE( h.service->recovery()->get( QStringLiteral( "agent" ), committed.operationId )
                  .value( "commitState" )
                  .toString(),
              QStringLiteral( "committed" ) );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
    // ⑥ 同键异参：冲突而不是静默复用。
    const Decoded conflict = computeRequest( QStringLiteral( "i1-compute" ), computed.operationId );
    QVERIFY( conflict.ok );
    QCOMPARE( h.service->prepare( conflict.request, host::Caller::Agent ).code,
              QStringLiteral( "idempotency_conflict" ) );
    // 受管样本原件未被改动（内容指纹仍与 catalog 记录一致）。
    const CatalogVersion wells = h.catalog.versionById( fixture.wellsVersion );
    QCOMPARE( fileSha256( DataCatalog::resolvedVersionPath( h.projectDir, wells ) ), wells.sha256 );
  }

  // SGS：同 seed 同输入 → OUTPUT/STD 逐字节一致；端口仅 OUTPUT/STD；无 MEMBER 文件；
  // 种子在摘要里保持精确十进制字符串（>2^53）。
  void sgsSameSeedReproducesWithoutMembers()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const QString bigSeed = QStringLiteral( "18446744073709551615" );
    const auto sgsRequest = [&]( const QString &requestId, const QString &seed ) {
      QVariantMap parameters{ { "VARIOGRAM_MODEL", QStringLiteral( "gaussian" ) },
                              { "NUGGET", 0.01 }, { "SILL", 1.0 }, { "RANGE", 25.0 },
                              { "REALIZATIONS", 4 }, { "SEED", seed } };
      return decodeTool( kSgsToolAlias(), requestId, QStringLiteral( "compute-only" ), parameters,
                         sampleInputs( fixture, QStringLiteral( "wells.T70" ) ) ); // 层键解析路径
    };
    ExecuteResult results[3];
    const QString seeds[3]{ bigSeed, bigSeed, QStringLiteral( "7" ) };
    OperationLog::Record records[3];
    for( int i = 0; i < 3; ++i )
    {
      const Decoded decoded = sgsRequest( QStringLiteral( "sgs-%1" ).arg( i ), seeds[i] );
      QVERIFY2( decoded.ok, qPrintable( compact( decoded.rejected ) ) );
      results[i] = h.run( decoded.request, QStringLiteral( "sgs-%1" ).arg( i ) );
      QVERIFY2( results[i].ok, qPrintable( results[i].code ) );
      QCOMPARE( results[i].summary.value( "seed" ).toString(), seeds[i] );
      QCOMPARE( results[i].summary.value( "realizations" ).toInt(), 4 );
      QVERIFY( results[i].summary.contains( QStringLiteral( "sample_mean" ) ) );
      records[i] = journal( h ).lookupRecordById( results[i].operationId ).record;
      QCOMPARE( records[i].computeCount, 1 );
    }
    const QStringList sgsPorts{ QStringLiteral( "OUTPUT" ), QStringLiteral( "STD" ) };
    for( const auto &record : records )
      QCOMPARE( portIds( record ), sgsPorts );
    const QString firstOutput = artifactPath( h, records[0], QStringLiteral( "OUTPUT" ) );
    const QString firstStd = artifactPath( h, records[0], QStringLiteral( "STD" ) );
    QCOMPARE( fileSha256( firstOutput ), fileSha256( artifactPath( h, records[1], QStringLiteral( "OUTPUT" ) ) ) );
    QCOMPARE( fileSha256( firstStd ), fileSha256( artifactPath( h, records[1], QStringLiteral( "STD" ) ) ) );
    QVERIFY( fileSha256( firstOutput ) != fileSha256( artifactPath( h, records[2], QStringLiteral( "OUTPUT" ) ) ) );
    // 私有 attempt 目录里只有两个端口文件：没有 MEMBER_n 个体实现。
    QStringList expectedFiles{ QStringLiteral( "OUTPUT.tif" ), QStringLiteral( "STD.tif" ) };
    expectedFiles.sort();
    QCOMPARE( QDir( QFileInfo( firstOutput ).absolutePath() ).entryList( QDir::Files | QDir::NoDotAndDotDot ),
              expectedFiles );
    // STD 是实现间离散度：样本格点处为 0，离开样本处为正；均值落在样本值域附近。
    QVector<double> spread, mean;
    int cols = 0, rows = 0;
    QVERIFY( readRasterAll( firstStd, &spread, &cols, &rows ) );
    QVERIFY( readRasterAll( firstOutput, &mean, &cols, &rows ) );
    QVERIFY2( spread[kSampleCell] < 1e-6, qPrintable( QString::number( spread[kSampleCell] ) ) );
    QVERIFY2( spread[kOffSampleCell] > 0, qPrintable( QString::number( spread[kOffSampleCell] ) ) );
    QVERIFY2( std::abs( mean[kSampleCell] - ( std::sin( 0.35 ) + 12.25 ) ) < 1e-3,
              qPrintable( QString::number( mean[kSampleCell] ) ) );
    // 回包里的种子同样精确（不经 JSON double）。
    const QJsonObject payload =
        responseFor( h, results[0], QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ) );
    QCOMPARE( payload.value( "seed" ).toString(), bigSeed );
    QCOMPARE( payload.value( "frozenPlan" ).toObject().value( "seed" ).toString(), bigSeed );
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 ) );
    // 发布已有 SGS 结果：不再调用数值核；OUTPUT/STD 的 parent/unit/CRS/seed
    // 元数据都来自冻结计划。
    const Decoded publish = publishRequest( QStringLiteral( "sgs-publish" ), results[0].operationId,
                                            kSgsToolAlias() );
    QVERIFY( publish.ok );
    const ExecuteResult committed = h.run( publish.request, QStringLiteral( "sgs-publish" ) );
    QVERIFY2( committed.ok, qPrintable( committed.code ) );
    QCOMPARE( committed.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( committed.presentation, QStringLiteral( "not_requested" ) );
    QCOMPARE( computeCount( h, results[0].operationId ), 1 );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
    const CatalogVersion meanVersion =
        publishedVersion( h.catalog, QStringLiteral( "geostat_mean_raster" ) );
    const CatalogVersion stdVersion = publishedVersion( h.catalog, QStringLiteral( "geostat_std_raster" ) );
    QVERIFY( !meanVersion.id.isEmpty() && !stdVersion.id.isEmpty() );
    const QStringList expectedParents{ fixture.wellsVersion };
    for( const auto &version : { meanVersion, stdVersion } )
    {
      QCOMPARE( version.parentVersionIds, expectedParents );
      QCOMPARE( version.extra.value( "factor_id" ).toString(), QStringLiteral( "sandthick" ) );
      QCOMPARE( version.extra.value( "field" ).toString(), QStringLiteral( "sand_thick" ) );
      QCOMPARE( version.extra.value( "value_unit" ).toString(), QStringLiteral( "m" ) );
      QCOMPARE( version.extra.value( "crs_authid" ).toString(), QStringLiteral( "EPSG:3857" ) );
      QCOMPARE( version.extra.value( "subIntervalId" ).toString(), fixture.s1.subIntervalId );
      QCOMPARE( version.extra.value( "schemeRevision" ).toString(), fixture.s1.revision );
      QCOMPARE( version.extra.value( "source_version_id" ).toString(), fixture.wellsVersion );
      QCOMPARE( version.extra.value( "seed" ).toString(), bigSeed );
      QCOMPARE( version.extra.value( "realizations" ).toInt(), 4 );
      QCOMPARE( version.extra.value( "algorithm_id" ).toString(), QStringLiteral( "geostat:sgs" ) );
    }
    QCOMPARE( h.catalog.versions().size(), qsizetype( 3 ) ); // 样本 + 均值 + 离散度
  }

  // 身份伪造/单元不匹配/未知版本/未知因子：在数值之前拒绝，且不立项。
  void untrustedIdentitiesAreRejectedBeforeComputation()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const auto prepareCode = [&]( const QString &requestId, const QVariantMap &inputs ) {
      const Decoded decoded = decodeTool( kKrigingToolAlias(), requestId, QStringLiteral( "prepare" ),
                                          krigingParameters(), inputs );
      if( !decoded.ok )
        return QStringLiteral( "decode:" ) + compact( decoded.rejected );
      return h.service->prepare( decoded.request, host::Caller::Agent ).code;
    };
    struct Case
    {
      QString id;
      QVariantMap inputs;
      QString expected;
    };
    // 伪造 S9：DS 实体核验失败。
    QVariantMap fake = unitIdentity( fixture.s1 );
    fake.insert( "subIntervalId", QStringLiteral( "S9" ) );
    // 旧方案修订：按修订读取方案失败（不静默换当前头）。
    QVariantMap stale = unitIdentity( fixture.s1 );
    stale.insert( "revision", QStringLiteral( "999" ) );
    // 不存在的区间。
    QVariantMap otherInterval = unitIdentity( fixture.s1 );
    otherInterval.insert( "intervalId", QStringLiteral( "interval-elsewhere" ) );
    QVariantMap noSample = sampleInputs( fixture );
    noSample.insert( "sample_ref", QStringLiteral( "catalog:v-elsewhere" ) );
    QVariantMap unknownFactor = sampleInputs( fixture );
    unknownFactor.insert( "factor_id", QStringLiteral( "poro" ) );
    QVariantMap authority = sampleInputs( fixture );
    authority.insert( "approved", true ); // inputs 内的权限自称键：封闭键集合拒绝
    QVariantMap constrained = sampleInputs( fixture );
    constrained.insert( "constraints_ref", QStringLiteral( "catalog:" ) + fixture.wellsVersion );
    QVariantMap wrongField = sampleInputs( fixture );
    wrongField.insert( "field", QStringLiteral( "z" ) );
    const QList<Case> cases{
      { QStringLiteral( "reject-fake" ), fake, QStringLiteral( "geostat_unit_invalid" ) },
      { QStringLiteral( "reject-stale" ), stale, QStringLiteral( "geostat_unit_invalid" ) },
      { QStringLiteral( "reject-interval" ), otherInterval, QStringLiteral( "geostat_unit_invalid" ) },
      { QStringLiteral( "reject-version" ), noSample, QStringLiteral( "geostat_sample_version_missing" ) },
      { QStringLiteral( "reject-factor" ), unknownFactor, QStringLiteral( "geostat_factor_unsupported" ) },
      { QStringLiteral( "reject-authority" ), authority, QStringLiteral( "geostat_input_unknown" ) },
      { QStringLiteral( "reject-constraints" ), constrained, QStringLiteral( "geostat_input_unsupported" ) },
      { QStringLiteral( "reject-field" ), wrongField, QStringLiteral( "geostat_field_mismatch" ) } };
    for( const Case &entry : cases )
    {
      QVariantMap inputs = entry.inputs;
      // unit_selection 三例需要包成 unit_selection 键。
      if( entry.id == QStringLiteral( "reject-fake" ) || entry.id == QStringLiteral( "reject-stale" ) ||
          entry.id == QStringLiteral( "reject-interval" ) )
      {
        inputs = sampleInputs( fixture );
        inputs.insert( "unit_selection", entry.inputs );
      }
      const QString code = prepareCode( entry.id, inputs );
      QVERIFY2( code.contains( entry.expected ), qPrintable( entry.id + QStringLiteral( " -> " ) + code ) );
      QCOMPARE( journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ), entry.id ).code,
                QStringLiteral( "not_found" ) );
    }
    // 顶层权限自称键：由通用聊天解码器以 untrusted_authority 拒绝。
    QJsonObject arguments = toolArguments( QStringLiteral( "reject-top-authority" ),
                                           QStringLiteral( "prepare" ), krigingParameters(),
                                           sampleInputs( fixture ) );
    arguments.insert( "approved", true );
    ExecuteRequest request;
    const QJsonObject rejected = prepareHostedChatToolRequest( request, kKrigingToolAlias(), arguments );
    QCOMPARE( rejected.value( "code" ).toString(), QStringLiteral( "untrusted_authority" ) );
    QVERIFY( request.capabilityId.isEmpty() );
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 ) );
    QVERIFY( !QFileInfo::exists( h.snapshotRoot() ) ); // 数值/物化之前全部拒绝
  }

  // 样本↔单元关联核验：层键挂别的上界面、或版本单元身份只有部分字段相同，都不
  // 获得 verified 关联；三元组逐字一致才接受（association=version_unit）。
  void sampleUnitAssociationIsVerified()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const auto prepareCode = [&]( const QString &requestId, const QString &sampleRef ) {
      const Decoded decoded = decodeTool( kKrigingToolAlias(), requestId, QStringLiteral( "prepare" ),
                                          krigingParameters(), sampleInputs( fixture, sampleRef ) );
      return decoded.ok ? h.service->prepare( decoded.request, host::Caller::Agent ).code
                        : QStringLiteral( "decode:" ) + compact( decoded.rejected );
    };
    // 层键 wells.T80 ≠ 冻结单元父上界面 T70。
    const QString otherHorizon =
        fixture.addWells( h, QStringLiteral( "wells80" ), QStringLiteral( "wells.T80" ), {} );
    QVERIFY( !otherHorizon.isEmpty() );
    QCOMPARE( prepareCode( QStringLiteral( "assoc-horizon" ), QStringLiteral( "catalog:" ) + otherHorizon ),
              QStringLiteral( "geostat_sample_unit_unverified" ) );
    // 版本单元身份：仅 subIntervalId 相同、schemeId 不同 / intervalId 不同。
    QVariantMap versionUnit{ { "schemeId", QStringLiteral( "scheme-other" ) },
                             { "intervalId", fixture.s1.intervalId },
                             { "subIntervalId", fixture.s1.subIntervalId } };
    const QString wrongScheme = fixture.addWells( h, QStringLiteral( "wrongScheme" ), QString(),
                                                  QVariantMap{ { "unitSelection", versionUnit } } );
    QVERIFY( !wrongScheme.isEmpty() );
    QCOMPARE( prepareCode( QStringLiteral( "assoc-scheme" ), QStringLiteral( "catalog:" ) + wrongScheme ),
              QStringLiteral( "geostat_sample_unit_unverified" ) );
    versionUnit.insert( "schemeId", fixture.s1.schemeId );
    versionUnit.insert( "intervalId", QStringLiteral( "interval-other" ) );
    const QString wrongInterval =
        fixture.addWells( h, QStringLiteral( "wrongInterval" ), QString(),
                          QVariantMap{ { "unitSelection", versionUnit } } );
    QVERIFY( !wrongInterval.isEmpty() );
    QCOMPARE( prepareCode( QStringLiteral( "assoc-interval" ), QStringLiteral( "catalog:" ) + wrongInterval ),
              QStringLiteral( "geostat_sample_unit_unverified" ) );
    // 三元组一致（无层键）：接受，关联依据记为 version_unit。
    versionUnit.insert( "intervalId", fixture.s1.intervalId );
    const QString matching = fixture.addWells( h, QStringLiteral( "matching" ), QString(),
                                               QVariantMap{ { "unitSelection", versionUnit } } );
    QVERIFY( !matching.isEmpty() );
    const Decoded accepted = decodeTool( kKrigingToolAlias(), QStringLiteral( "assoc-ok" ),
                                         QStringLiteral( "prepare" ), krigingParameters(),
                                         sampleInputs( fixture, QStringLiteral( "catalog:" ) + matching ) );
    QVERIFY( accepted.ok );
    const ExecuteResult prepared = h.run( accepted.request, QStringLiteral( "assoc-ok" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    const QJsonObject sample =
        planOf( journal( h ).lookupRecordById( prepared.operationId ).record ).value( "sample" ).toObject();
    QCOMPARE( sample.value( "association" ).toString(), QStringLiteral( "version_unit" ) );
    // 层键字符串解析（非 catalog: 前缀）同样走受管版本解析与关联核验。
    // HostPrepared.code 成功时为 "ok"。
    const QString byLayerKey = prepareCode( QStringLiteral( "assoc-layerkey" ), QStringLiteral( "wells.T70" ) );
    QCOMPARE( byLayerKey, QStringLiteral( "ok" ) );
  }

  // 地理 CRS、字段缺失/非数值、无有效样本：物化在数值之前如实失败并给出原因，
  // 不立项；空几何/缺测行计入 skipped 而不是静默丢弃。
  void unusableSamplesFailHonestlyBeforeNumerics()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    struct Case
    {
      QString name;
      bool withCrs;
      WellsContent content;
      QString expected;
      bool succeeds;
    };
    const QList<Case> cases{
      { QStringLiteral( "geo" ), false, WellsContent::Normal, QStringLiteral( "geostat_reader_crs_geographic" ), false },
      { QStringLiteral( "nofield" ), true, WellsContent::MissingField, QStringLiteral( "geostat_reader_field_missing" ), false },
      { QStringLiteral( "nonnumeric" ), true, WellsContent::NonNumericField, QStringLiteral( "geostat_reader_field_not_numeric" ), false },
      { QStringLiteral( "allnull" ), true, WellsContent::AllNullValues, QStringLiteral( "geostat_reader_field_not_numeric" ), false },
      { QStringLiteral( "nopoints" ), true, WellsContent::NoUsablePoints, QStringLiteral( "geostat_reader_no_valid_samples" ), false },
      { QStringLiteral( "partial" ), true, WellsContent::SomeSkipped, QString(), true } };
    for( const Case &entry : cases )
    {
      const QString version =
          fixture.addWells( h, entry.name, QStringLiteral( "wells.T70" ), {}, entry.withCrs, entry.content );
      QVERIFY2( !version.isEmpty(), qPrintable( entry.name ) );
      const Decoded decoded = decodeTool( kKrigingToolAlias(), entry.name, QStringLiteral( "prepare" ),
                                          krigingParameters(),
                                          sampleInputs( fixture, QStringLiteral( "catalog:" ) + version ) );
      QVERIFY2( decoded.ok, qPrintable( compact( decoded.rejected ) ) );
      const auto prepared = h.service->prepare( decoded.request, host::Caller::Agent );
      QVERIFY2( prepared.ok, qPrintable( entry.name + QStringLiteral( ": " ) + prepared.code ) );
      const ExecuteResult result = h.start( prepared.handle, decoded.request, entry.name );
      QCOMPARE( result.ok, entry.succeeds );
      if( entry.succeeds )
      {
        const QJsonObject sample =
            planOf( journal( h ).lookupRecordById( result.operationId ).record ).value( "sample" ).toObject();
        QCOMPARE( sample.value( "sample_count" ).toInt(), kWellsSide * kWellsSide );
        QCOMPARE( sample.value( "skipped_rows" ).toInt(), 2 );
        continue;
      }
      QCOMPARE( result.code, entry.expected );
      QCOMPARE( result.execution, QStringLiteral( "rejected" ) );
      QVERIFY( result.operationId.isEmpty() ); // 物化失败：不立持久操作
      QCOMPARE( journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ), entry.name ).code,
                QStringLiteral( "not_found" ) );
    }
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 + 6 ) ); // 仅受管井点版本
  }

  // 重复坐标：去重策略沿用内核（重合样本合并为均值），合并数在诊断里外显。
  void duplicateCoordinatesAreMergedByKernelAndReported()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const QString version = fixture.addWells( h, QStringLiteral( "duplicates" ),
                                              QStringLiteral( "wells.T70" ), {}, true,
                                              WellsContent::WithDuplicates );
    QVERIFY( !version.isEmpty() );
    const Decoded decoded = decodeTool( kKrigingToolAlias(), QStringLiteral( "duplicates" ),
                                        QStringLiteral( "compute-only" ), krigingParameters(),
                                        sampleInputs( fixture, QStringLiteral( "catalog:" ) + version ) );
    QVERIFY( decoded.ok );
    const ExecuteResult result = h.run( decoded.request, QStringLiteral( "duplicates" ) );
    QVERIFY2( result.ok, qPrintable( result.code ) );
    QCOMPARE( result.summary.value( "sample_count" ).toInt(), kWellsSide * kWellsSide + 1 );
    QCOMPARE( result.summary.value( "merged_duplicates" ).toInt(), 1 );
    QCOMPARE( result.summary.value( "finite_cells" ).toInt(), 36 );
    // (0,0) 处两个样本 12.0 与 12.5 合并为均值 12.25（格心 (0,0) = 行 5 列 0）。
    QVector<double> values;
    int cols = 0, rows = 0;
    const OperationLog::Record record = journal( h ).lookupRecordById( result.operationId ).record;
    QVERIFY( readRasterAll( artifactPath( h, record, QStringLiteral( "OUTPUT" ) ), &values, &cols, &rows ) );
    QVERIFY2( std::abs( values[5 * 6] - 12.25 ) < 1e-3, qPrintable( QString::number( values[5 * 6] ) ) );
  }

  // 全 nodata（半径模式下邻点不足）不是成功：不交付空栅格。
  void allNodataGridIsNotReportedAsSuccess()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    QVariantMap parameters = krigingParameters();
    parameters.insert( "SEARCH_RADIUS", 1.0 ); // 井距 10 米：半径 1 米内邻点 < MIN_POINTS
    parameters.insert( "MIN_POINTS", 4 );
    const Decoded decoded = decodeTool( kKrigingToolAlias(), QStringLiteral( "nodata" ),
                                        QStringLiteral( "compute-only" ), parameters, sampleInputs( fixture ) );
    QVERIFY( decoded.ok );
    const ExecuteResult result = h.run( decoded.request, QStringLiteral( "nodata" ) );
    QVERIFY( !result.ok );
    QCOMPARE( result.code, QStringLiteral( "geostat_no_finite_cells" ) );
    QCOMPARE( result.execution, QStringLiteral( "failed" ) );
    QCOMPARE( result.commitState, QStringLiteral( "not_requested" ) );
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 ) );
    QCOMPARE( h.service->recovery()->results( QStringLiteral( "agent" ), result.operationId )
                  .value( "code" )
                  .toString(),
              QStringLiteral( "result_unavailable" ) );
  }

  // 快照缺失/被改：compute-only 继承在立项前（工作线程核对快照）明确不可恢复，
  // 不静默换当前样本重算。
  void missingOrTamperedSnapshotIsHonestlyUnrecoverable()
  {
    const auto snapshotOf = []( const Host &h, const QString &operationId ) {
      const QJsonObject sample =
          planOf( journal( h ).lookupRecordById( operationId ).record ).value( "sample" ).toObject();
      return h.projectDir + '/' + sample.value( "snapshot_path" ).toString();
    };
    for( const bool tamper : { false, true } )
    {
      Host h;
      QVERIFY( h.open( this ) );
      ProjectFixture fixture;
      QVERIFY2( fixture.build( h ), "fixture build failed" );
      const QString tag = tamper ? QStringLiteral( "tamper" ) : QStringLiteral( "missing" );
      const Decoded prepare = prepareRequest( tag + QStringLiteral( "-prepare" ), fixture );
      QVERIFY( prepare.ok );
      const ExecuteResult prepared = h.run( prepare.request, tag + QStringLiteral( "-prepare" ) );
      QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
      const QString snapshot = snapshotOf( h, prepared.operationId );
      if( !tamper )
      {
        QVERIFY( QFile::remove( snapshot ) );
      }
      else
      {
        QFile file( snapshot );
        QVERIFY( file.open( QIODevice::ReadWrite ) );
        const QByteArray bytes = file.readAll();
        QVERIFY( !bytes.isEmpty() );
        QByteArray altered = bytes;
        const int middle = altered.size() / 2;
        altered[middle] = altered[middle] == '1' ? '2' : '1'; // 同长度内容替换 → 摘要不符
        QVERIFY( file.seek( 0 ) );
        QCOMPARE( file.write( altered ), qint64( bytes.size() ) );
      }
      const Decoded compute = computeRequest( tag + QStringLiteral( "-compute" ), prepared.operationId );
      QVERIFY( compute.ok );
      const ExecuteResult result = h.run( compute.request, tag + QStringLiteral( "-compute" ) );
      QVERIFY( !result.ok );
      QCOMPARE( result.code, QStringLiteral( "geostat_snapshot_unavailable" ) );
      QCOMPARE( result.execution, QStringLiteral( "rejected" ) );
      QVERIFY( result.operationId.isEmpty() );
      QCOMPARE( journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ),
                                    tag + QStringLiteral( "-compute" ) )
                    .code,
                QStringLiteral( "not_found" ) );
    }
  }

  // compute-only 继承的参数只能与冻结计划一致：相同 → 通过，不同/多余 → 冲突。
  void inheritRejectsConflictingParameters()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const Decoded prepare = prepareRequest( QStringLiteral( "inherit-prepare" ), fixture );
    QVERIFY( prepare.ok );
    const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "inherit-prepare" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    const QString conflictCode = QStringLiteral( "geostat_source_parameters_conflict" );
    QVariantMap differing = krigingParameters();
    differing.insert( "RANGE", 80.0 );
    const Decoded conflict = decodeTool( kKrigingToolAlias(), QStringLiteral( "inherit-conflict" ),
                                         QStringLiteral( "compute-only" ), differing,
                                         QVariantMap{ { "source_operation_id", prepared.operationId } } );
    QVERIFY( conflict.ok );
    QCOMPARE( h.service->prepare( conflict.request, host::Caller::Agent ).code, conflictCode );
    QVariantMap extra = krigingParameters();
    extra.insert( "MAX_POINTS", 8 );
    const Decoded extraConflict = decodeTool( kKrigingToolAlias(), QStringLiteral( "inherit-extra" ),
                                              QStringLiteral( "compute-only" ), extra,
                                              QVariantMap{ { "source_operation_id", prepared.operationId } } );
    QVERIFY( extraConflict.ok );
    QCOMPARE( h.service->prepare( extraConflict.request, host::Caller::Agent ).code, conflictCode );
    // 与冻结计划相同的参数：接受；冻结计划与省略参数的继承逐字节一致。
    const Decoded same = decodeTool( kKrigingToolAlias(), QStringLiteral( "inherit-same" ),
                                     QStringLiteral( "compute-only" ), krigingParameters(),
                                     QVariantMap{ { "source_operation_id", prepared.operationId } } );
    QVERIFY( same.ok );
    const ExecuteResult computed = h.run( same.request, QStringLiteral( "inherit-same" ) );
    QVERIFY2( computed.ok, qPrintable( computed.code ) );
    QCOMPARE( planOf( journal( h ).lookupRecordById( computed.operationId ).record ),
              planOf( journal( h ).lookupRecordById( prepared.operationId ).record ) );
  }

  // publish 按契约文本可以省略 parameters，也可以给 {} 或重复冻结计划的参数。
  // 与冻结计划不一致的 parameters 仍拒绝，不把新的 sill/range 发布出去。
  void publishAcceptsOmittedEmptyOrRepeatedFrozenParameters()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const Decoded prepare = prepareRequest( QStringLiteral( "pub-prepare" ), fixture );
    QVERIFY2( prepare.ok, qPrintable( compact( prepare.rejected ) ) );
    const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "pub-prepare" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    const Decoded compute = computeRequest( QStringLiteral( "pub-compute" ), prepared.operationId );
    QVERIFY( compute.ok );
    const ExecuteResult computed = h.run( compute.request, QStringLiteral( "pub-compute" ) );
    QVERIFY2( computed.ok, qPrintable( computed.code ) );
    const QJsonObject plan = planOf( journal( h ).lookupRecordById( computed.operationId ).record );
    const QJsonObject effective = plan.value( QStringLiteral( "effective_parameters" ) ).toObject();
    QCOMPARE( effective.value( QStringLiteral( "RANGE" ) ).toDouble(), 40.0 );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 0 );

    const auto publishArgs = [&]( const QString &requestId, const QJsonValue *parameters ) {
      QJsonObject arguments{ { "client_request_id", requestId },
                             { "binding_mode", QStringLiteral( "pinned-value" ) },
                             { "publish_intent", QStringLiteral( "publish" ) },
                             { "inputs", QJsonObject{ { "source_operation_id", computed.operationId } } } };
      if ( parameters )
        arguments.insert( QStringLiteral( "parameters" ), *parameters );
      return arguments;
    };
    const auto hostRequest = [&]( const QJsonObject &arguments ) {
      Decoded out;
      out.rejected = prepareHostedChatToolRequest( out.request, kKrigingToolAlias(), arguments );
      out.ok = out.rejected.isEmpty();
      return out;
    };

    QVariantMap changed = krigingParameters();
    changed.insert( QStringLiteral( "RANGE" ), 80.0 );
    changed.insert( QStringLiteral( "SILL" ), 2.0 );
    const QJsonValue changedParameters = QJsonObject::fromVariantMap( changed );
    const QJsonObject changedArgs = publishArgs( QStringLiteral( "pub-changed" ), &changedParameters );
    QVERIFY( aiToolSpec( kKrigingToolAlias() ).validateParameters( changedArgs ).isEmpty() );
    const Decoded changedRequest = hostRequest( changedArgs );
    QVERIFY2( changedRequest.ok, qPrintable( compact( changedRequest.rejected ) ) );
    QCOMPARE( h.service->prepare( changedRequest.request, host::Caller::Agent ).code,
              QStringLiteral( "mixed_source_operation_inputs" ) );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 0 );

    const QJsonObject omittedArgs = publishArgs( QStringLiteral( "pub-omit" ), nullptr );
    QVERIFY2( aiToolSpec( kKrigingToolAlias() ).validateParameters( omittedArgs ).isEmpty(),
              qPrintable( aiToolSpec( kKrigingToolAlias() ).validateParameters( omittedArgs ) ) );
    const Decoded omitted = hostRequest( omittedArgs );
    QVERIFY2( omitted.ok, qPrintable( compact( omitted.rejected ) ) );
    QVERIFY( omitted.request.parameters.isEmpty() );
    const ExecuteResult committed = h.run( omitted.request, QStringLiteral( "pub-omit" ) );
    QVERIFY2( committed.ok, qPrintable( committed.code ) );
    QCOMPARE( committed.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( computeCount( h, computed.operationId ), 1 );
    const CatalogVersion estimate =
        publishedVersion( h.catalog, QStringLiteral( "geostat_estimate_raster" ) );
    QCOMPARE( estimate.extra.value( QStringLiteral( "variogram" ) ).toMap().value( QStringLiteral( "range" ) )
                  .toDouble(),
              40.0 );
    QCOMPARE( estimate.extra.value( QStringLiteral( "variogram" ) ).toMap().value( QStringLiteral( "sill" ) )
                  .toDouble(),
              1.0 );

    const QJsonValue emptyParameters = QJsonObject{};
    const QJsonObject emptyArgs = publishArgs( QStringLiteral( "pub-empty" ), &emptyParameters );
    QVERIFY( aiToolSpec( kKrigingToolAlias() ).validateParameters( emptyArgs ).isEmpty() );
    const Decoded empty = hostRequest( emptyArgs );
    QVERIFY( empty.ok );
    const auto emptyPrepared = h.service->prepare( empty.request, host::Caller::Agent );
    QVERIFY2( emptyPrepared.ok, qPrintable( emptyPrepared.code ) );

    const QJsonValue repeatedParameters = QJsonObject::fromVariantMap( krigingParameters() );
    const QJsonObject repeatedArgs = publishArgs( QStringLiteral( "pub-repeat" ), &repeatedParameters );
    QVERIFY( aiToolSpec( kKrigingToolAlias() ).validateParameters( repeatedArgs ).isEmpty() );
    const Decoded repeated = hostRequest( repeatedArgs );
    QVERIFY( repeated.ok );
    const auto repeatedPrepared = h.service->prepare( repeated.request, host::Caller::Agent );
    QVERIFY2( repeatedPrepared.ok, qPrintable( repeatedPrepared.code ) );

    const QJsonValue echoedParameters = effective;
    const QJsonObject echoedArgs = publishArgs( QStringLiteral( "pub-echo" ), &echoedParameters );
    QVERIFY( aiToolSpec( kKrigingToolAlias() ).validateParameters( echoedArgs ).isEmpty() );
    const Decoded echoed = hostRequest( echoedArgs );
    QVERIFY( echoed.ok );
    const auto echoedPrepared = h.service->prepare( echoed.request, host::Caller::Agent );
    QVERIFY2( echoedPrepared.ok, qPrintable( echoedPrepared.code ) );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
  }

  // 单元修订在 capture 后变化：继承 compute 仍用冻结数据（pinned-value）；携带
  // 旧修订的新请求冻结到该历史修订，而不是静默换成当前头。
  void computeInheritsFrozenUnitAcrossSchemeRevision()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const Decoded prepare = prepareRequest( QStringLiteral( "rev-prepare" ), fixture );
    QVERIFY( prepare.ok );
    const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "rev-prepare" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    // CAS 重提交同 scheme（顶底面上移）→ 旧修订不再是头。
    {
      std::string detail;
      paleo::data::DataService ds;
      QVERIFY( ds.openProject( paleo::area_model::directory( h.projectDir ).toStdString(), detail ) );
      auto edited = fixture.committed;
      for( auto &occurrence : edited.wellOccurrences )
      {
        occurrence.top += 20;
        occurrence.bottom += 20;
      }
      paleo::data::StratigraphicScheme shifted;
      std::vector<paleo::data::FrameworkIssue> issues;
      QVERIFY2( ds.commitStratigraphicScheme( edited, fixture.committed.revision.value, shifted,
                                              issues, detail ),
                detail.c_str() );
      ds.closeProject();
      QVERIFY( shifted.revision.value != fixture.committed.revision.value );
    }
    const Decoded compute = computeRequest( QStringLiteral( "rev-compute" ), prepared.operationId );
    QVERIFY( compute.ok );
    const ExecuteResult result = h.run( compute.request, QStringLiteral( "rev-compute" ) );
    QVERIFY2( result.ok, qPrintable( result.code ) );
    QCOMPARE( result.summary.value( "subinterval_id" ).toString(), fixture.s1.subIntervalId );
    QCOMPARE( result.summary.value( "source_version_id" ).toString(), fixture.wellsVersion );
    const Decoded stale = prepareRequest( QStringLiteral( "rev-stale" ), fixture );
    QVERIFY( stale.ok );
    const ExecuteResult stalePrepared = h.run( stale.request, QStringLiteral( "rev-stale" ) );
    QVERIFY2( stalePrepared.ok, qPrintable( stalePrepared.code ) );
    const QJsonObject staleUnit =
        planOf( journal( h ).lookupRecordById( stalePrepared.operationId ).record ).value( "unit" ).toObject();
    QCOMPARE( staleUnit.value( "revision" ).toString(), fixture.s1.revision );
  }

  // 取消请求收到后：回放被拒（cancelled）、已取消的 prepare 不再作为 compute 源，
  // 都不产生数值。
  void cancelBeforeCommitBlocksReplayAndInheritance()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const Decoded prepare = prepareRequest( QStringLiteral( "cancel-prepare" ), fixture );
    QVERIFY( prepare.ok );
    const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "cancel-prepare" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    QCOMPARE( h.service->recovery()->cancel( QStringLiteral( "agent" ), prepared.operationId )
                  .value( "code" )
                  .toString(),
              QStringLiteral( "cancel_requested" ) );
    // 同键重发（首次响应丢失的等价场景）：回放被拒。
    const auto replay = h.service->prepare( prepare.request, host::Caller::Agent );
    QVERIFY2( replay.ok, qPrintable( replay.code ) );
    QCOMPARE( replay.code, QStringLiteral( "replay" ) );
    QString error;
    QVERIFY( h.service->start( replay.handle, prepare.request, h.receiver,
                               QStringLiteral( "cancel-replay" ), []( ExecuteResult ) {}, &error ) == nullptr );
    QCOMPARE( error, QStringLiteral( "cancelled" ) );
    // 已取消的 prepare 不能作为 compute 的源。
    const Decoded compute = computeRequest( QStringLiteral( "cancel-compute" ), prepared.operationId );
    QVERIFY( compute.ok );
    QCOMPARE( h.service->prepare( compute.request, host::Caller::Agent ).code,
              QStringLiteral( "source_result_unavailable" ) );
    QCOMPARE( journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ),
                                  QStringLiteral( "cancel-compute" ) )
                  .code,
              QStringLiteral( "not_found" ) );
  }

  // 准备在途时切换工程：晚完成的物化不向新工程投递、不在任何工程立项、不产生版本。
  void projectSwitchDuringPreparationIsDropped()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    // 单工作线程 + 门闩任务：物化 worker 确定性地停在队首之后。
    h.tasks->setMaxWorkerThreads( 1 );
    const std::shared_ptr<QSemaphore> gate = std::make_shared<QSemaphore>();
    h.tasks->start( QStringLiteral( "blocker" ), [gate]( PaleoTask * ) {
      gate->acquire();
      return QString();
    } );
    // 断言失败提前返回时也放行工作线程（不留孤儿线程池）。
    const auto releaseGate = qScopeGuard( [gate] { gate->release(); } );
    Q_UNUSED( releaseGate )
    const Decoded prepare = prepareRequest( QStringLiteral( "i1-switch" ), fixture );
    QVERIFY( prepare.ok );
    const auto prepared = h.service->prepare( prepare.request, host::Caller::Agent );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    ExecuteResult result;
    bool done = false;
    QString error;
    QVERIFY2( h.service->start( prepared.handle, prepare.request, h.receiver, QStringLiteral( "i1-switch" ),
                                [&]( ExecuteResult out ) { result = out; done = true; }, &error ),
              qPrintable( error ) );
    QVERIFY( !done );
    h.switchProject( QStringLiteral( "doc-other" ) );
    gate->release(); // 物化 worker 此刻才真正开始（已失效的准备）
    QVERIFY( QTest::qWaitFor( [&] { return done; }, 120000 ) );
    QVERIFY( !result.ok );
    QCOMPARE( result.code, QStringLiteral( "stale_session" ) );
    QVERIFY( result.operationId.isEmpty() );
    for( const auto &document : { QStringLiteral( "doc-i1" ), QStringLiteral( "doc-other" ) } )
      QCOMPARE( journal( h ).lookup( document, QStringLiteral( "agent" ), QStringLiteral( "i1-switch" ) ).code,
                QStringLiteral( "not_found" ) );
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 ) );
    QVERIFY( !QFileInfo::exists( h.snapshotRoot() ) );
  }

  // 计算中取消：operation 进入 running 后发出取消，数值核协作终止，无正式成果。
  void cancellationDuringComputeStopsKernelWithoutPublishing()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    // 4M 格（准入上界内）使取消必然落在真实核执行窗口内，而不是 prepare 阶段。
    QVariantMap inputs = sampleInputs( fixture );
    inputs.insert( "grid", QVariantMap{ { "min_x", 0.0 }, { "max_x", 20000.0 },
                                       { "min_y", 0.0 }, { "max_y", 20000.0 },
                                       { "cell_size", 10.0 } } );
    const Decoded decoded = decodeTool( kKrigingToolAlias(), QStringLiteral( "mid-cancel" ),
                                        QStringLiteral( "compute-only" ), krigingParameters(), inputs );
    QVERIFY( decoded.ok );
    const auto prepared = h.service->prepare( decoded.request, host::Caller::Agent );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    ExecuteResult result;
    bool done = false;
    QString error;
    PaleoTask *task = h.service->start( prepared.handle, decoded.request, h.receiver,
                                        QStringLiteral( "mid-cancel" ),
                                        [&]( ExecuteResult out ) { result = out; done = true; },
                                        &error );
    QVERIFY2( task, qPrintable( error ) );
    // 等持久 operation 真正进入 running，而不是在 capture/物化阶段提前取消。
    QVERIFY2( QTest::qWaitFor( [&] {
                return journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ),
                                            QStringLiteral( "mid-cancel" ) ).record.execution ==
                       QStringLiteral( "running" );
              }, 30000 ),
              "compute never reached running" );
    const QString operationId =
        journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ),
                             QStringLiteral( "mid-cancel" ) ).record.operationId;
    QVERIFY( !operationId.isEmpty() );
    QCOMPARE( h.service->recovery()->cancel( QStringLiteral( "agent" ), operationId )
                  .value( "code" )
                  .toString(),
              QStringLiteral( "cancel_requested" ) );
    QVERIFY2( QTest::qWaitFor( [&] { return done; }, 120000 ), "cancelled compute did not finish" );
    QVERIFY( !result.ok );
    QCOMPARE( result.code, QStringLiteral( "cancelled" ) );
    QCOMPARE( result.execution, QStringLiteral( "cancelled" ) );
    QCOMPARE( result.commitState, QStringLiteral( "not_requested" ) );
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 ) );
    QCOMPARE( computeCount( h, operationId ), 1 );
    QCOMPARE( h.service->recovery()->results( QStringLiteral( "agent" ), operationId )
                  .value( "code" )
                  .toString(),
              QStringLiteral( "result_unavailable" ) );
  }

  // 提交阶段的 journal 写盘故障：不虚报 committed；故障清除后同一请求恢复发布，
  // 不重复计算、不重复版本。
  void journalWriteFaultDuringCommitRecoversWithoutRecompute()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const Decoded prepare = prepareRequest( QStringLiteral( "fault-prepare" ), fixture );
    QVERIFY( prepare.ok );
    const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "fault-prepare" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    const Decoded compute = computeRequest( QStringLiteral( "fault-compute" ), prepared.operationId );
    QVERIFY( compute.ok );
    const ExecuteResult computed = h.run( compute.request, QStringLiteral( "fault-compute" ) );
    QVERIFY2( computed.ok, qPrintable( computed.code ) );
    const Decoded publish = publishRequest( QStringLiteral( "fault-publish" ), computed.operationId );
    QVERIFY( publish.ok );
    const auto clearFault = qScopeGuard( [] { OperationLog::armOperationFileWriteFault( -1 ); } );
    Q_UNUSED( clearFault )
    // worker 已把发布集备好，下一次 journal 写入（进入 publishing）失败。
    h.service->recovery()->setCheckpointHook( []( const QString &at ) {
      if( at == QLatin1String( "prepared" ) )
        OperationLog::armOperationFileWriteFault( 0 );
    } );
    const ExecuteResult failed = h.run( publish.request, QStringLiteral( "fault-publish" ) );
    // ExecuteResult.ok 这里只表示计算与 prepared set 已就绪；提交是否发生必须看
    // commitState。store 失败回写持久记录后，状态为 prepared/not_requested。
    QCOMPARE( failed.execution, QStringLiteral( "prepared" ) );
    QCOMPARE( failed.commitState, QStringLiteral( "not_requested" ) );
    QVERIFY( !failed.operationId.isEmpty() );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 0 );
    QCOMPARE( computeCount( h, computed.operationId ), 1 );
    OperationLog::armOperationFileWriteFault( -1 );
    h.service->recovery()->setCheckpointHook( {} );
    // 对账如实暴露未提交事实；重试沿用原 source_operation_id，不重算数值。
    const QJsonObject state = h.service->recovery()->get( QStringLiteral( "agent" ), failed.operationId );
    QVERIFY( state.value( "commitState" ).toString() != QLatin1String( "committed" ) );
    const ExecuteResult recovered = h.run( publish.request, QStringLiteral( "fault-publish-retry" ) );
    QVERIFY2( recovered.ok, qPrintable( recovered.code ) );
    QCOMPARE( recovered.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( computeCount( h, computed.operationId ), 1 );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
    const ExecuteResult replayed = h.run( publish.request, QStringLiteral( "fault-publish-replay" ) );
    QVERIFY2( replayed.ok, qPrintable( replayed.code ) );
    QCOMPARE( replayed.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
  }

  // 必需成员缺失：发布不交付部分成果；已计算事实保留、失败的发布操作可读。
  void missingRequiredMemberBlocksPublish()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    const Decoded prepare = prepareRequest( QStringLiteral( "member-prepare" ), fixture );
    QVERIFY( prepare.ok );
    const ExecuteResult prepared = h.run( prepare.request, QStringLiteral( "member-prepare" ) );
    QVERIFY2( prepared.ok, qPrintable( prepared.code ) );
    const Decoded compute = computeRequest( QStringLiteral( "member-compute" ), prepared.operationId );
    QVERIFY( compute.ok );
    const ExecuteResult computed = h.run( compute.request, QStringLiteral( "member-compute" ) );
    QVERIFY2( computed.ok, qPrintable( computed.code ) );
    const OperationLog::Record computeRecord =
        journal( h ).lookupRecordById( computed.operationId ).record;
    QVERIFY( QFile::remove( artifactPath( h, computeRecord, QStringLiteral( "VARIANCE" ) ) ) );
    const Decoded publish = publishRequest( QStringLiteral( "member-publish" ), computed.operationId );
    QVERIFY( publish.ok );
    const ExecuteResult result = h.run( publish.request, QStringLiteral( "member-publish" ) );
    QVERIFY( !result.ok );
    QVERIFY( result.commitState != QLatin1String( "committed" ) );
    QVERIFY( !result.operationId.isEmpty() ); // 失败的发布操作本身可读
    QCOMPARE( h.service->recovery()->get( QStringLiteral( "agent" ), result.operationId )
                  .value( "commitState" )
                  .toString(),
              result.commitState );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 0 );
    QCOMPARE( h.catalog.versions().size(), qsizetype( 1 ) );
    QCOMPARE( computeCount( h, computed.operationId ), 1 ); // 源计算事实未被改写
  }

  // 读取口的扫描取消 / 样本上限 / 缺 CRS 语义（直接调用 qgis 读取封装）。
  void readerIsBoundedCancellableAndHonest()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString path = dir.filePath( QStringLiteral( "wells.geojson" ) );
    QVERIFY( writeWells( path, kWellsSide ) );
    using Reader = paleo::geostat::GeostatSampleReadResult;
    const auto read = [&]( const QString &file, int maximum, const std::function<bool()> &cancelled ) {
      return paleo::geostat::readGeostatPointSamples( file, QStringLiteral( "sand_thick" ), maximum,
                                                      cancelled );
    };
    // 扫描中取消。
    const Reader cancelled = read( path, 1000, [] { return true; } );
    QVERIFY( !cancelled.ok );
    QCOMPARE( cancelled.code, QStringLiteral( "cancelled" ) );
    QVERIFY( cancelled.samples.empty() );
    // 样本上限：超限即失败（不返回截断样本）。
    const Reader over = read( path, 10, {} );
    QVERIFY( !over.ok );
    QCOMPARE( over.code, QStringLiteral( "geostat_sample_budget_exceeded" ) );
    QVERIFY( over.samples.size() <= std::size_t( 10 ) );
    // 正常读取：计数、CRS 句柄由样本 CRS 重建。
    const Reader ok = read( path, 1000, {} );
    QVERIFY2( ok.ok, qPrintable( ok.code ) );
    QCOMPARE( int( ok.samples.size() ), kWellsSide * kWellsSide );
    QVERIFY( ok.hasBounds );
    QVERIFY( ok.crsAuthId.startsWith( QStringLiteral( "EPSG:" ) ) );
    QVERIFY( paleo::geostat::geostatRasterCrsFromWkt( ok.crsWkt ).get().isValid() );
    QVERIFY( !paleo::geostat::geostatRasterCrsFromWkt( QStringLiteral( "not a wkt" ) ).get().isValid() );
    // NULL 值是缺测，不是 0 值样本（QGIS 把 OGR NULL 数值表示为值 0 且 isNull）。
    const QString partial = dir.filePath( QStringLiteral( "partial.geojson" ) );
    QVERIFY( writeWells( partial, kWellsSide, true, WellsContent::SomeSkipped ) );
    const Reader skipped = read( partial, 1000, {} );
    QVERIFY2( skipped.ok, qPrintable( skipped.code ) );
    QCOMPARE( int( skipped.samples.size() ), kWellsSide * kWellsSide );
    QCOMPARE( skipped.skippedRows, 2 );
    for( const auto &sample : skipped.samples )
      QVERIFY2( !( sample.x == 0 && sample.y == 0 && sample.value == 0 ),
                "a NULL measurement was materialized as a 0-value sample" );
    // 缺 CRS（CSV + WKT 几何列，无坐标参考）：明确拒绝，不套默认 CRS。
    const QString csv = dir.filePath( QStringLiteral( "wells.csv" ) );
    {
      QFile file( csv );
      QVERIFY( file.open( QIODevice::WriteOnly ) );
      file.write( "WKT,sand_thick\n\"POINT (0 0)\",12.5\n\"POINT (10 0)\",13.5\n" );
    }
    const Reader noCrs = read( csv, 1000, {} );
    QVERIFY( !noCrs.ok );
    QCOMPARE( noCrs.code, QStringLiteral( "geostat_reader_crs_missing" ) );
  }

  // 真实 AiChatToolRunner 入口（Builtin 与 Pi 共用的唯一执行链）：工具枚举、
  // prepare/compute 回包带冻结计划事实、非法约束在入口被拒、作用域为会话级。
  void realRunnerDeliversPrepareAndComputeWithFrozenPlan()
  {
    Host h;
    QVERIFY( h.open( this ) );
    ProjectFixture fixture;
    QVERIFY2( fixture.build( h ), "fixture build failed" );
    AiChatToolRunner runner;
    runner.setTaskService( h.tasks.get() );
    AiChatToolContext context;
    context.projectDir = h.projectDir;
    context.documentId = QStringLiteral( "doc-i1" );
    context.documentEpoch = h.epoch;
    context.catalog = &h.catalog;
    context.capabilityHost = h.service.get();
    context.requireCapabilityHost = true;
    runner.setContext( context );
    runner.setResultSession( QStringLiteral( "chat-1" ) );
    const QString scope = h.service->callerScope( host::Caller::Agent, QStringLiteral( "chat-1" ) );
    QVERIFY( scope.startsWith( QStringLiteral( "agent:chat:" ) ) );
    // 枚举：两只工具对模型可见；枚举不扫描样本、不落快照、不跑核。
    QStringList advertised;
    for( const auto &spec : runner.availableTools() )
      advertised.append( spec.name );
    QVERIFY( advertised.contains( kKrigingToolAlias() ) );
    QVERIFY( advertised.contains( kSgsToolAlias() ) );
    QVERIFY( !QFileInfo::exists( h.snapshotRoot() ) );
    QSignalSpy finished( &runner, &AiChatToolRunner::toolFinished );
    // 驱动一次工具调用并等待 toolFinished（非 void lambda 不能用 QTRY 宏）。
    struct Reply
    {
      bool delivered = false;
      bool ok = false;
      QString callId;
      QJsonObject payload;
      QString operation() const { return payload.value( "operationId" ).toString(); }
    };
    int step = 0;
    const auto call = [&]( const QString &callId, const QString &tool, const QString &requestId,
                           const QJsonObject &arguments ) {
      finished.clear();
      runner.run( ChatToolCall{ callId, tool, compact( arguments ) } );
      Reply reply;
      reply.delivered = QTest::qWaitFor( [&] { return !finished.isEmpty(); }, 120000 );
      if( reply.delivered )
      {
        const QList<QVariant> event = finished.first();
        reply.ok = event.at( 1 ).toBool();
        reply.payload = QJsonDocument::fromJson( event.at( 2 ).toString().toUtf8() ).object();
        reply.callId = event.at( 0 ).value<ChatToolCall>().id;
      }
      record( ++step, tool, requestId, arguments, reply.payload );
      return reply;
    };
    const auto geostatCall = [&]( const QString &callId, const QString &tool, const QString &requestId,
                                  const QString &intent, const QVariantMap &parameters,
                                  const QVariantMap &inputs ) {
      return call( callId, tool, requestId, toolArguments( requestId, intent, parameters, inputs ) );
    };
    // ① ② 由现有查询取得受管样本版本（资产列表 → 版本详情）；不扫描样本。
    const Reply assets = call( QStringLiteral( "call-assets" ), QStringLiteral( "paleo.query_project" ),
                               QString(), QJsonObject{ { "topic", QStringLiteral( "assets" ) },
                                                       { "asset_type", QStringLiteral( "well_factor_points" ) } } );
    QVERIFY2( assets.delivered && assets.ok, qPrintable( compact( assets.payload ) ) );
    const Reply version = call( QStringLiteral( "call-version" ), QStringLiteral( "paleo.query_project" ),
                                QString(), QJsonObject{ { "topic", QStringLiteral( "version" ) },
                                                        { "version_id", fixture.wellsVersion } } );
    QVERIFY2( version.delivered && version.ok, qPrintable( compact( version.payload ) ) );
    // ③ prepare。
    const Reply prepare = geostatCall( QStringLiteral( "call-prepare" ), kKrigingToolAlias(),
                                       QStringLiteral( "i1-replay-prepare" ), QStringLiteral( "prepare" ),
                                       krigingParameters(), sampleInputs( fixture ) );
    QVERIFY2( prepare.delivered, "runner did not deliver prepare" );
    QCOMPARE( prepare.callId, QStringLiteral( "call-prepare" ) ); // 每个 tool_call_id 都有应答
    QVERIFY2( prepare.ok, qPrintable( compact( prepare.payload ) ) );
    QCOMPARE( prepare.payload.value( "code" ).toString(), QStringLiteral( "prepared" ) );
    QCOMPARE( prepare.payload.value( "contract" ).toString(), QStringLiteral( "paleo.operation.v2" ) );
    const QJsonObject facts = prepare.payload.value( "frozenPlan" ).toObject();
    QCOMPARE( facts.value( "sample" ).toObject().value( "count" ).toInt(), 25 );
    QCOMPARE( facts.value( "sample" ).toObject().value( "valueUnit" ).toString(), QStringLiteral( "m" ) );
    const QString prepareOperation = prepare.payload.value( "operationId" ).toString();
    QVERIFY( !prepareOperation.isEmpty() );
    QCOMPARE( h.service->recovery()->get( scope, prepareOperation ).value( "execution" ).toString(),
              QStringLiteral( "prepared" ) );
    QCOMPARE( journal( h ).lookupRecordById( prepareOperation ).record.callerScope, scope );
    // ④ compute-only（继承）。
    const Reply compute = geostatCall( QStringLiteral( "call-compute" ), kKrigingToolAlias(),
                                       QStringLiteral( "i1-replay-compute" ),
                                       QStringLiteral( "compute-only" ), QVariantMap{},
                                       QVariantMap{ { "source_operation_id", prepareOperation } } );
    QVERIFY2( compute.delivered && compute.ok, qPrintable( compact( compute.payload ) ) );
    QCOMPARE( compute.payload.value( "execution" ).toString(), QStringLiteral( "succeeded" ) );
    QCOMPARE( compute.payload.value( "commitState" ).toString(), QStringLiteral( "not_requested" ) );
    QCOMPARE( compute.payload.value( "sample_count" ).toInt(), 25 );
    QCOMPARE( compute.payload.value( "frozenPlan" ).toObject().value( "sample" ).toObject().value( "count" ).toInt(), 25 );
    QVERIFY( compact( compute.payload ).size() < 8192 ); // 有界回包
    QVERIFY( !compact( compute.payload ).contains( QStringLiteral( "result_path" ) ) );
    const QString computeOperation = compute.payload.value( "operationId" ).toString();
    // ⑤ ⑥ 用既有 operation 工具查看结果（不重算）。
    const Reply state = call( QStringLiteral( "call-operation-get" ), QStringLiteral( "paleo.operation_get" ),
                              QString(), QJsonObject{ { "operation_id", computeOperation } } );
    QVERIFY2( state.delivered && state.ok, qPrintable( compact( state.payload ) ) );
    QCOMPARE( state.payload.value( "execution" ).toString(), QStringLiteral( "succeeded" ) );
    QCOMPARE( state.payload.value( "computeCount" ).toInt(), 1 );
    const Reply results = call( QStringLiteral( "call-operation-results" ),
                                QStringLiteral( "paleo.operation_results" ), QString(),
                                QJsonObject{ { "operation_id", computeOperation } } );
    QVERIFY2( results.delivered && results.ok, qPrintable( compact( results.payload ) ) );
    QStringList resultPorts;
    for( const auto &port : results.payload.value( "ports" ).toArray() )
      resultPorts.append( port.toObject().value( "portId" ).toString() );
    resultPorts.sort();
    const QStringList krigingPorts{ QStringLiteral( "OUTPUT" ), QStringLiteral( "VARIANCE" ) };
    QCOMPARE( resultPorts, krigingPorts );
    QVERIFY( !compact( results.payload ).contains( QStringLiteral( "path" ) ) );
    for( const auto &port : results.payload.value( "ports" ).toArray() )
    {
      QCOMPARE( port.toObject().value( "axis" ).toString(), QStringLiteral( "XY" ) );
      QVERIFY( port.toObject().value( "unit" ).toString().contains( QStringLiteral( "factor value unit" ) ) );
    }
    // ⑦ 用新的请求号发布已计算结果；⑧ 重复发布请求复用 receipt、不重算。
    const QJsonObject publishArguments =
        toolArguments( QStringLiteral( "i1-replay-publish" ), QStringLiteral( "publish" ), QVariantMap{},
                       QVariantMap{ { "source_operation_id", computeOperation } } );
    const Reply committed = call( QStringLiteral( "call-publish" ), kKrigingToolAlias(),
                                  QStringLiteral( "i1-replay-publish" ), publishArguments );
    QVERIFY2( committed.delivered && committed.ok, qPrintable( compact( committed.payload ) ) );
    QCOMPARE( committed.payload.value( "commitState" ).toString(), QStringLiteral( "committed" ) );
    QVERIFY( !committed.payload.value( "commitReceiptRef" ).toString().isEmpty() );
    const Reply again = call( QStringLiteral( "call-publish-again" ), kKrigingToolAlias(),
                              QStringLiteral( "i1-replay-publish" ), publishArguments );
    QVERIFY2( again.delivered && again.ok, qPrintable( compact( again.payload ) ) );
    QCOMPARE( again.payload.value( "commitState" ).toString(), QStringLiteral( "committed" ) );
    QCOMPARE( again.operation(), committed.operation() );
    QCOMPARE( journal( h ).lookupRecordById( computeOperation ).record.computeCount, 1 );
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
    // ⑨ SGS：同一受管输入、固定种子；仅 OUTPUT/STD。
    QVariantMap sgsParameters{ { "VARIOGRAM_MODEL", QStringLiteral( "gaussian" ) },
                               { "RANGE", 30.0 }, { "REALIZATIONS", 2 }, { "SEED", QStringLiteral( "11" ) } };
    const Reply sgs = geostatCall( QStringLiteral( "call-sgs" ), kSgsToolAlias(), QStringLiteral( "i1-replay-sgs" ),
                                   QStringLiteral( "compute-only" ), sgsParameters, sampleInputs( fixture ) );
    QVERIFY2( sgs.delivered && sgs.ok, qPrintable( compact( sgs.payload ) ) );
    QCOMPARE( sgs.payload.value( "seed" ).toString(), QStringLiteral( "11" ) );
    const QStringList sgsPorts{ QStringLiteral( "OUTPUT" ), QStringLiteral( "STD" ) };
    QCOMPARE( portIds( journal( h ).lookupRecordById( sgs.payload.value( "operationId" ).toString() ).record ),
              sgsPorts );
    // ⑩ 首版边界在真实入口生效：显式 constraints_ref 被拒，且不立项。
    QVariantMap constrained = sampleInputs( fixture );
    constrained.insert( "constraints_ref", QStringLiteral( "catalog:" ) + fixture.wellsVersion );
    const Reply rejected = geostatCall( QStringLiteral( "call-constrained" ), kKrigingToolAlias(),
                                        QStringLiteral( "i1-replay-constrained" ),
                                        QStringLiteral( "compute-only" ), krigingParameters(), constrained );
    QVERIFY( rejected.delivered );
    QCOMPARE( rejected.callId, QStringLiteral( "call-constrained" ) );
    QVERIFY( !rejected.ok );
    QCOMPARE( rejected.payload.value( "code" ).toString(), QStringLiteral( "geostat_input_unsupported" ) );
    QCOMPARE( journal( h ).lookup( QStringLiteral( "doc-i1" ), scope, QStringLiteral( "i1-replay-constrained" ) ).code,
              QStringLiteral( "not_found" ) );
    // SGS 的 compute-only 同样不发布：目录里只有样本版本 + 克里金发布的两件成果。
    QCOMPARE( geostatVersions( h.catalog, fixture.wellsVersion ), 2 );
  }

  // 真实进程重启（子进程在独立进程里完成 prepare 后退出）：新进程按原 operation
  // 的冻结计划 compute，不重新解析样本/单元；同键重传按原请求回放。
  void processRestartAfterPrepareComputesFromOriginalOperation()
  {
    QTemporaryDir root{ QDir::temp().filePath( QStringLiteral( "i1-restart-XXXXXX" ) ) };
    QVERIFY( root.isValid() );
    QVERIFY( runChild( root.path(), QStringLiteral( "prepare" ), 0 ) );
    Host h( root.path() );
    QVERIFY( h.open( this ) );
    const OperationLog::Accept prepare = journal( h ).lookup( QStringLiteral( "doc-i1" ),
                                                             QStringLiteral( "agent" ),
                                                             QStringLiteral( "i1-prepare" ) );
    QCOMPARE( prepare.code, QStringLiteral( "found" ) );
    QCOMPARE( prepare.record.execution, QStringLiteral( "prepared" ) );
    const QString prepareOperation = prepare.record.operationId;
    const QJsonObject plan = planOf( prepare.record );
    QCOMPARE( plan.value( "sample" ).toObject().value( "sample_count" ).toInt(), 25 );
    // 方案与样本版本都是子进程建的：本进程只凭持久 operation 继续。
    const Decoded compute = computeRequest( QStringLiteral( "i1-compute" ), prepareOperation );
    QVERIFY( compute.ok );
    const ExecuteResult computed = h.run( compute.request, QStringLiteral( "i1-compute" ) );
    QVERIFY2( computed.ok, qPrintable( computed.code ) );
    QCOMPARE( computed.summary.value( "source_version_id" ).toString(),
              plan.value( "sample" ).toObject().value( "version_id" ).toString() );
    QCOMPARE( computed.summary.value( "value_unit" ).toString(), QStringLiteral( "m" ) );
    QCOMPARE( computeCount( h, computed.operationId ), 1 );
    const QStringList krigingPorts{ QStringLiteral( "OUTPUT" ), QStringLiteral( "VARIANCE" ) };
    QCOMPARE( portIds( journal( h ).lookupRecordById( computed.operationId ).record ), krigingPorts );
    // 同键重传 prepare：按原请求回放（不二次物化/立项）。
    const Decoded replay = prepareRequest( QStringLiteral( "i1-prepare" ), fixtureFrom( plan ) );
    QVERIFY( replay.ok );
    const auto replayed = h.service->prepare( replay.request, host::Caller::Agent );
    QVERIFY2( replayed.ok, qPrintable( replayed.code ) );
    QCOMPARE( replayed.code, QStringLiteral( "replay" ) );
    QCOMPARE( journal( h ).snapshotScope( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ) ).records.size(), qsizetype( 2 ) );
  }

  // 真实进程重启（子进程完成 compute 后退出）：新进程发布已计算结果，不重算。
  void processRestartAfterComputePublishesWithoutRecomputing()
  {
    QTemporaryDir root{ QDir::temp().filePath( QStringLiteral( "i1-restart-XXXXXX" ) ) };
    QVERIFY( root.isValid() );
    QVERIFY( runChild( root.path(), QStringLiteral( "computed" ), 0 ) );
    Host h( root.path() );
    QVERIFY( h.open( this ) );
    const OperationLog::Record compute =
        journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ), QStringLiteral( "i1-compute" ) ).record;
    QCOMPARE( compute.execution, QStringLiteral( "succeeded" ) );
    QCOMPARE( compute.computeCount, 1 );
    const QString sampleVersion =
        planOf( compute ).value( "sample" ).toObject().value( "version_id" ).toString();
    const Decoded publish = publishRequest( QStringLiteral( "i1-publish" ), compute.operationId );
    QVERIFY( publish.ok );
    const ExecuteResult committed = h.run( publish.request, QStringLiteral( "i1-publish" ) );
    QVERIFY2( committed.ok, qPrintable( committed.code ) );
    QCOMPARE( committed.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( computeCount( h, compute.operationId ), 1 );
    QCOMPARE( geostatVersions( h.catalog, sampleVersion ), 2 );
    const QStringList expectedParents{ sampleVersion };
    for( const auto &version : h.catalog.versions() )
      if( version.id != sampleVersion )
        QCOMPARE( version.parentVersionIds, expectedParents );
    // 已提交 receipt 跨进程可读；重发发布请求复用同一回执、不新增版本。
    const auto replay = h.service->prepare( publish.request, host::Caller::Agent );
    QVERIFY2( replay.ok, qPrintable( replay.code ) );
    QCOMPARE( replay.code, QStringLiteral( "replay" ) );
    const ExecuteResult again = h.start( replay.handle, publish.request, QStringLiteral( "publish-again" ) );
    QVERIFY( again.ok );
    QCOMPARE( again.commitState, QStringLiteral( "committed" ) );
    QCOMPARE( geostatVersions( h.catalog, sampleVersion ), 2 );
    QVERIFY( paleo::storage::operationRetention( h.projectDir ).complete );
  }

  // 真实进程在数值 worker 完成前消失：新进程按冻结计划显式 resume（走
  // validateFrozen / restoreInputValidator / restoreWorker）；快照缺失则明确不可恢复。
  void interruptedComputeResumesFromFrozenSnapshot()
  {
    for( const bool deleteSnapshot : { false, true } )
    {
      QTemporaryDir root{ QDir::temp().filePath( QStringLiteral( "i1-restart-XXXXXX" ) ) };
      QVERIFY( root.isValid() );
      QVERIFY( runChild( root.path(), QStringLiteral( "crash-running" ), kCrashExit ) );
      Host h( root.path() );
      QVERIFY( h.open( this ) );
      const OperationLog::Record crashed =
          journal( h ).lookup( QStringLiteral( "doc-i1" ), QStringLiteral( "agent" ), QStringLiteral( "i1-compute" ) ).record;
      QVERIFY( !crashed.operationId.isEmpty() );
      QCOMPARE( crashed.execution, QStringLiteral( "running" ) ); // 崩溃时磁盘上的状态
      QCOMPARE( crashed.computeCount, 1 );
      QString snapshot;
      if( deleteSnapshot )
      {
        const QJsonObject sample = planOf( crashed ).value( "sample" ).toObject();
        snapshot = h.projectDir + '/' + sample.value( "snapshot_path" ).toString();
        QVERIFY( QFile::remove( snapshot ) );
      }
      // 未显式 resume 不得重跑（不把中断当成功，也不自动重复计算）。
      QString error;
      QVERIFY( h.service->recovery()->start( QStringLiteral( "agent" ), crashed.operationId, this,
                                             QStringLiteral( "resume" ), []( ExecuteResult ) {}, false,
                                             &error ) == nullptr );
      QCOMPARE( error, QStringLiteral( "explicit_resume_required" ) );
      QCOMPARE( computeCount( h, crashed.operationId ), 1 );
      // 显式 resume：从冻结计划 + 私有快照恢复。
      ExecuteResult result;
      bool done = false;
      QVERIFY2( h.service->recovery()->start( QStringLiteral( "agent" ), crashed.operationId, this,
                                              QStringLiteral( "resume" ),
                                              [&]( ExecuteResult out ) { result = out; done = true; }, true,
                                              &error ),
                qPrintable( error ) );
      QTRY_VERIFY( done );
      QCOMPARE( computeCount( h, crashed.operationId ), 2 ); // 中断的一次 + 恢复的一次
      if( deleteSnapshot )
      {
        QVERIFY( !result.ok );
        QCOMPARE( result.code, QStringLiteral( "geostat_snapshot_unavailable" ) );
        QCOMPARE( result.execution, QStringLiteral( "failed" ) );
        continue;
      }
      QVERIFY2( result.ok, qPrintable( result.code ) );
      QCOMPARE( result.execution, QStringLiteral( "succeeded" ) );
      QCOMPARE( result.summary.value( "sample_count" ).toInt(), 25 );
      QVector<double> values;
      int cols = 0, rows = 0;
      QVERIFY( readRasterAll( artifactPath( h, journal( h ).lookupRecordById( crashed.operationId ).record,
                                            QStringLiteral( "OUTPUT" ) ),
                              &values, &cols, &rows ) );
      QVERIFY( std::abs( values[kSampleCell] - ( std::sin( 0.35 ) + 12.25 ) ) < 1e-3 );
    }
  }

private:
  bool runChild( const QString &root, const QString &phase, int expectedExit )
  {
    QProcess child;
    child.start( QCoreApplication::applicationFilePath(),
                 { QStringLiteral( "--i1-crash" ), root, phase } );
    if( !child.waitForStarted( 30000 ) || !child.waitForFinished( 180000 ) )
    {
      qWarning( "child did not finish: %s", qPrintable( child.errorString() ) );
      return false;
    }
    if( child.exitCode() != expectedExit )
      qWarning( "child exit %d (expected %d): %s", child.exitCode(), expectedExit,
                qPrintable( QString::fromUtf8( child.readAllStandardError() ).right( 2000 ) ) );
    return child.exitStatus() == QProcess::NormalExit && child.exitCode() == expectedExit;
  }
};

int main( int argc, char **argv )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH" ), true );
  app.initQgis();
  const QStringList args = app.arguments();
  int code = 0;
  if( args.size() == 4 && args.at( 1 ) == QLatin1String( "--i1-crash" ) )
    code = crashChild( args.at( 2 ), args.at( 3 ) );
  else
  {
    GeostatAtomOperationTests test;
    code = QTest::qExec( &test, argc, argv );
  }
  QgsApplication::exitQgis();
  return code;
}
#include "tst_geostat_atom_operation.moc"
