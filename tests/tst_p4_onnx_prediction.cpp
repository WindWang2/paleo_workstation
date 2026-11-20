// 层：测试壳
// P4（2026-11-19）：原生 ONNX 预测准入、血缘与发布闭环回归。
// 覆盖：ORT C API 准入探测（含确定性注入负向，不 QSKIP）、正式预测单元
// 准入（无单元/Boundary/伪造 S/aggregate/analysisLayerKey 绕过均前置拒绝）、
// 精确父引用（无 displayName 子串回退）、模型身份冻结（prepare 后换模型被
// compute 拒绝）、输出回读验证、发布顺序（先提交后声明；提交失败无悬空
// 声明；声明失败保留已提交产物并报「展示未完成」）、提交事实幂等（重复
// 回调不重复版本）、并发展示冲突不覆盖、取消/切工程不误删已提交产物。
// 无 ORT 构建（!PALEO_HAVE_ORT）：断言 onnx 算法在 prepare 明确不可用——
// 明确不可用 ≠ 成功推理，同样不 QSKIP。
#include <QtTest>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <gdal.h>
#include <qgsapplication.h>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/ai/modelregistry.h"
#include "../src/ai/onnxruntimeprobe.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/workflows.h"
#include "seismichorizonfixture.h"
#include "services/analysisunitcontext.h"
#include "services/seismicunitinput.h"
#include "services/seismicdataaccess.h"
#include "services/stratigraphicsubintervals.h"
#include "io/areamodel.h"

#include <paleo_data/service.h>

#include <cmath>

#if PALEO_HAVE_ORT
#include <onnxruntime_c_api.h>
#endif

using namespace paleo::strat;

namespace
{

const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
{
  const QVector<LayerDeclaration> decls = layers.declared();
  for ( const LayerDeclaration &d : decls )
    if ( d.layerId == layerId )
      return new LayerDeclaration( d );
  return nullptr;
}

// 真实单元夹具（tst_analysisunitcontext 同源）：合成 SEG-Y 体 + 层位 A/B +
// 方案（含 seismicHorizonResourceId 接线），单元取数走生产路径。
struct UnitFixture
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QgisProjectService projectSvc;
  PaleoProjectStore store;
  LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
  QgisLayerService layers{ &projectSvc, &manifest };
  QgisProcessingService proc{ &store };
#if PALEO_HAVE_ORT
  PaleoOnnxService onnx;
#endif
  paleo::data::DataService ds;
  paleo::data::StratigraphicScheme scheme;
  QString seismicSource;

#if PALEO_HAVE_ORT
  // models/manifest.json：钉 SHA 的 seg3 模型（生产注册表门控路径，非开发
  // 降级）。返回模型名。rePin：不重写模型文件，仅把 manifest 钉哈希换成
  // 给定值并重扫（模拟「prepare 之后注册表换钉新身份」）。
  QString installSegModel( const QString &name = QStringLiteral( "seg3" ),
                           const QString &shaOverride = QString() )
  {
    const QString modelDir = dir.filePath( QStringLiteral( "models" ) );
    if ( !QDir().mkpath( modelDir ) )
      return QString();
    const QString path = modelDir + QLatin1Char( '/' ) + name +
                         QStringLiteral( ".onnx" );
    if ( !OnnxFixtureWriter::writeSeg( path, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) )
      return QString();
    const QString sha = shaOverride.isEmpty() ? PaleoOnnxService::sha256OfFile( path )
                                              : shaOverride;
    return writeManifest( modelDir, name, sha );
  }

  QString rePinManifest( const QString &name, const QString &sha )
  {
    return writeManifest( dir.filePath( QStringLiteral( "models" ) ), name, sha );
  }

private:
  QString writeManifest( const QString &modelDir, const QString &name, const QString &sha )
  {
    QFile manifestFile( modelDir + QStringLiteral( "/manifest.json" ) );
    if ( !manifestFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      return QString();
    manifestFile.write( QStringLiteral( R"({"models":[{"name":"%1","file":"%1.onnx",)"
                                         R"("version":"1.0.0","task":"segmentation",)"
                                         R"("dataType":"amplitude","input":{"name":"x","dtype":"float32","shape":[1,1,null,null]},)"
                                         R"("outputs":{"semantics":"logits","classes":3},)"
                                         R"("sha256":"%2"}]})" )
                             .arg( name, sha )
                             .toUtf8() );
    manifestFile.close();
    onnx.setModelRoot( modelDir );
    onnx.setModelRegistry( ModelRegistry::scan( modelDir ) );
    return name;
  }

public:
#endif

  bool init( bool seismic = true )
  {
    if ( !dir.isValid() || !catalog.open( dir.path() ) )
      return false;
    if ( !projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
      return false;
    if ( !manifest.open() )
      return false;
    const QString project = dir.path();
    std::string detail;
    const QString modelRoot = paleo::area_model::directory( project );
    if ( QFileInfo::exists( modelRoot + QStringLiteral( "/data.sqlite" ) )
             ? !ds.openProject( modelRoot.toStdString(), detail )
             : !ds.createProject( modelRoot.toStdString(),
                                  QStringLiteral( "p4" ).toStdString(), detail ) )
      return false;
    if ( seismic )
    {
      seismicSource = dir.filePath( QStringLiteral( "volume.sgy" ) );
      if ( !SeismicHorizonFixture::writeFile( seismicSource, SeismicHorizonFixture::buildVolume() ) )
        return false;
      paleo::data::ImportOptions opts;
      opts.copySources = false;
      paleo::data::ImportReport report;
      if ( !ds.importFile( seismicSource.toStdString(), opts, report, detail ) )
        return false;
      const QString input = dir.filePath( QStringLiteral( "picks.txt" ) );
      if ( !SeismicHorizonFixture::writeFile( input, QByteArrayLiteral( "synthetic" ) ) )
        return false;
      paleo::data::ParsedFile parsed;
      parsed.kind = "test";
      parsed.status = "parsed";
      for ( int k = 0; k < 2; ++k )
      {
        paleo::data::SeismicHorizon h;
        h.horizonName = k ? "B" : "A";
        h.timeUnit = "ms";
        h.timeType = "TWT";
        for ( int il = 100; il < 104; ++il )
          for ( int xl = 2000; xl < 2004; ++xl )
            h.points.push_back( { il, xl, double( ( xl - 2000 ) * 10 ),
                                  double( ( il - 100 ) * 20 ),
                                  il == 101 && xl == 2001 ? NAN : ( 80.0 + k * 80.0 ) } );
        parsed.horizons.push_back( h );
      }
      long long id = 0;
      if ( !ds.importPreparedFile( input.toStdString(), input.toStdString(),
                                   input.toStdString(),
                                   QCryptographicHash::hash( "synthetic", QCryptographicHash::Sha256 )
                                       .toHex()
                                       .toStdString(),
                                   parsed, id, detail ) )
        return false;
    }
    WellTopTable tops;
    tops[17] = { { QStringLiteral( "A" ), 1000 }, { QStringLiteral( "B" ), 1080 } };
    auto draft = draftScheme( QStringLiteral( "p4" ), QStringLiteral( "test" ),
                              { QStringLiteral( "A" ), QStringLiteral( "B" ) }, tops );
    if ( seismic )
    {
      std::vector<paleo::data::DataEntry> horizons;
      if ( !ds.listData( "horizons", 0, horizons, detail ) )
        return false;
      for ( auto &boundary : draft.boundaries )
        for ( const auto &entry : horizons )
          if ( entry.name == boundary.name )
            boundary.seismicHorizonResourceId = entry.id;
    }
    std::vector<paleo::data::FrameworkIssue> issues;
    return ds.commitStratigraphicScheme( draft, {}, scheme, issues, detail );
  }

  UnitSelection unit( int index = 1 ) const
  {
    return makeSelection( scheme, selectorOptions( scheme ).first().intervalId, index );
  }

  // 把地震体登记为 catalog 版本（受管血缘锚点），返回 version id。
  QString registerSeismicVersion()
  {
    CatalogAsset asset;
    asset.id = QStringLiteral( "ast-seismic" );
    asset.type = QStringLiteral( "seismic_volume" );
    asset.displayName = QStringLiteral( "合成地震体" );
    QString err;
    if ( !catalog.addAsset( asset, &err ) )
      return QString();
    QFile f( seismicSource );
    if ( !f.open( QIODevice::ReadOnly ) )
      return QString();
    CatalogVersion v;
    v.id = QStringLiteral( "ver-seismic" );
    v.assetId = asset.id;
    v.stage = QStringLiteral( "RAW" );
    v.managed = false;
    v.path = seismicSource;
    v.fileName = QFileInfo( seismicSource ).fileName();
    v.sha256 = QString::fromLatin1( QCryptographicHash::hash( f.readAll(), QCryptographicHash::Sha256 ).toHex() );
    if ( !catalog.addVersion( v, &err ) )
      return QString();
    return v.id;
  }

  // 干扰资产：显示名包含预测键相似文本（如 T7/T70/T71 族），但从不被本任务
  // 消费——精确父引用不得把它带进 parents。
  QString registerDecoyAsset( const QString &displayName )
  {
    CatalogAsset asset;
    asset.id = QStringLiteral( "ast-decoy-" ) + QString::number( catalog.assets().size() );
    asset.type = QStringLiteral( "horizon_raster" );
    asset.displayName = displayName;
    QString err;
    if ( !catalog.addAsset( asset, &err ) )
      return QString();
    const QString decoyPath = dir.filePath( asset.id + QStringLiteral( ".tif" ) );
    if ( !QFile::copy( seismicSource, decoyPath ) )
      return QString();
    CatalogVersion v;
    v.id = QStringLiteral( "ver-" ) + asset.id;
    v.assetId = asset.id;
    v.stage = QStringLiteral( "RAW" );
    v.managed = false;
    v.path = decoyPath;
    v.fileName = QFileInfo( decoyPath ).fileName();
    v.sha256 = QStringLiteral( "0" ).repeated( 64 );
    if ( !catalog.addVersion( v, &err ) )
      return QString();
    return v.id;
  }
};

} // namespace

namespace
{
// 探测注入 RAII 守卫：断言失败也保证还原真实探测，不污染后续用例。
struct ProbeOverrideGuard
{
  explicit ProbeOverrideGuard( const std::function<paleo::onnx::OnnxRuntimeProbeResult()> &fn )
  {
    paleo::onnx::OnnxRuntimeProbe::setTestOverride( fn );
  }
  ~ProbeOverrideGuard() { paleo::onnx::OnnxRuntimeProbe::setTestOverride( {} ); }
};
} // namespace

class TestP4OnnxPrediction : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY( QgisRuntime::isInitialized() );
    GDALAllRegister();
  }

  void cleanup()
  {
    // 同 tst_workflows 安全网：清 singleton-project 遗留。
  }

  // ---- P4-W1：准入探测 ----

#if PALEO_HAVE_ORT
  void probeReportsVendoredRuntimeOk()
  {
    const paleo::onnx::OnnxRuntimeProbeResult r = PaleoOnnxService::runtimeDiagnostics();
    QVERIFY2( r.ok(), qPrintable( r.summaryText() ) );
    QVERIFY( PaleoOnnxService::runtimeAvailable() );
    QCOMPARE( r.requiredApiVersion, int( ORT_API_VERSION ) );
    QVERIFY( !r.runtimeVersion.isEmpty() );
#ifdef Q_OS_LINUX
    QVERIFY2( r.loadedModulePath.contains( QStringLiteral( "libonnxruntime" ) ),
              qPrintable( r.loadedModulePath ) );
#endif
    QVERIFY( !r.controlledPrefix.isEmpty() );
  }

  // 确定性注入：GetApi 返回空（旧运行库）——任何 Ort 包装对象构造之前得到
  // 具体错误，进程不崩溃；负向结论不靠 QSKIP。
  void probeInjectionApiMismatchFailsBeforeAnyWrapper()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PaleoOnnxService svc;
    svc.setModelRoot( f.dir.filePath( QStringLiteral( "models" ) ) );
    svc.setModelRegistry( ModelRegistry::scan( f.dir.filePath( QStringLiteral( "models" ) ) ) );
    QString err;
    OnnxModelMeta meta;
    {
    const ProbeOverrideGuard apiMismatchGuard( [] {
      paleo::onnx::OnnxRuntimeProbeResult r;
      r.status = paleo::onnx::OnnxRuntimeProbeResult::Status::ApiVersionMismatch;
      r.requiredApiVersion = int( ORT_API_VERSION );
      r.runtimeVersion = QStringLiteral( "onnxruntime.dll 1.2.3 (旧运行库)" );
      r.loadedModulePath = QStringLiteral( "C:\\Windows\\System32\\onnxruntime.dll" );
      return r;
    } );

    QCOMPARE( PaleoOnnxService::runtimeDiagnostics().status,
              paleo::onnx::OnnxRuntimeProbeResult::Status::ApiVersionMismatch );
    QVERIFY( !PaleoOnnxService::runtimeAvailable() );
    QCOMPARE( svc.loadModelMeta( QStringLiteral( "seg3" ), &meta, &err ),
              OnnxLoadStatus::RuntimeIncompatible );
    QVERIFY2( err.contains( QStringLiteral( "runtime_api_mismatch" ) ), qPrintable( err ) );
    QVERIFY2( err.contains( QString::number( int( ORT_API_VERSION ) ) ),
              "错误必须带所需 API 版号" );
    QVERIFY( err.contains( QStringLiteral( "旧运行库" ) ) ); // 实际运行库版本
    QVERIFY( err.contains( QStringLiteral( "onnxruntime.dll" ) ) ); // 实测加载模块
    QVERIFY( !svc.isModelLoaded( QStringLiteral( "seg3" ) ) ); // 未构造任何会话
    }

    QVERIFY( PaleoOnnxService::runtimeDiagnostics().ok() ); // 守卫析构已还原
    QCOMPARE( svc.loadModelMeta( QStringLiteral( "seg3" ), &meta, &err ), OnnxLoadStatus::Ok );
  }

  // 确定性注入：OrtGetApiBase 本身不可用（进程内可观测的库不可用状态）。
  void probeInjectionApiUnavailableFailsBeforeAnyWrapper()
  {
    const ProbeOverrideGuard apiUnavailableGuard( [] {
      paleo::onnx::OnnxRuntimeProbeResult r;
      r.status = paleo::onnx::OnnxRuntimeProbeResult::Status::ApiUnavailable;
      r.requiredApiVersion = int( ORT_API_VERSION );
      return r;
    } );
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PaleoOnnxService svc;
    svc.setModelRoot( f.dir.filePath( QStringLiteral( "models" ) ) );
    QString err;
    OnnxModelMeta meta;
    QCOMPARE( svc.loadModelMeta( QStringLiteral( "seg3" ), &meta, &err ),
              OnnxLoadStatus::RuntimeIncompatible );
    QVERIFY2( err.contains( QStringLiteral( "runtime_api_unavailable" ) ), qPrintable( err ) );
    QVERIFY( !svc.isModelLoaded( QStringLiteral( "seg3" ) ) );

    // 部署边界（文档化事实）：ORT 动态链接缺库时进程在 main 之前被 OS 终止，
    // 进程内 guard 不可覆盖——此处只能断言进程已启动后两类 API 状态的区分。
  }

  // prepare 与服务/页面同一诊断：准入失败在准备阶段就拒绝（W7）。
  void prepareRejectsWhenAdmissionFails()
  {
    const ProbeOverrideGuard admissionGuard( [] {
      paleo::onnx::OnnxRuntimeProbeResult r;
      r.status = paleo::onnx::OnnxRuntimeProbeResult::Status::ApiVersionMismatch;
      r.requiredApiVersion = int( ORT_API_VERSION );
      r.runtimeVersion = QStringLiteral( "old-runtime" );
      return r;
    } );
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY( !wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "runtime_api_mismatch" ) ), qPrintable( err ) );
  }
#else
  // 无 ORT 构建：明确不可用，不当作成功推理（同样不 QSKIP）。
  void noOrtBuildReportsExplicitlyUnavailable()
  {
    UnitFixture f;
    QVERIFY( f.init( false ) );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );
    PredictionWorkflow::PredictionJob job;
    QString err;
    QVERIFY( !wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       QVariantMap(), &job, &err ) );
    QVERIFY( err.contains( QStringLiteral( "lacks ONNX Runtime" ) ) );
    QVERIFY( !job.prepared );
  }
#endif

  // ---- P4-W2：正式预测单元准入 ----

#if PALEO_HAVE_ORT
  void noUnitFormalPredictionRejectedAtPrepare()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    // 正式 ONNX 预测（onnx 引擎）：无单元在 prepare 前置拒绝——Boundary 名
    // （T70）、analysisLayerKey、horizon 字符串都不能替代单元身份。
    {
      PredictionWorkflow::PredictionJob job;
      QString err;
      QVERIFY( !wf.preparePredictionJob( QStringLiteral( "T70" ), QStringLiteral( "onnx:seg3" ),
                                         QVariantMap(), &job, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "冻结分析单元" ) ), qPrintable( err ) );
      QVERIFY( !job.prepared );
      QVERIFY( !job.staging.isValid() ); // 未 stage——准入失败不占受管目录
    }
    // 带 analysisLayerKey 也不能绕过（仍无单元）。
    {
      PredictionWorkflow::PredictionJob job;
      QString err;
      QVariantMap params;
      params.insert( QStringLiteral( "analysisLayerKey" ), QStringLiteral( "T70" ) );
      QVERIFY( !wf.preparePredictionJob( QStringLiteral( "T70" ), QStringLiteral( "onnx:seg3" ),
                                         params, &job, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "冻结分析单元" ) ), qPrintable( err ) );
    }
    // 空 unitSelection map（共享段按无单元走 legacy 路径）同样在 ONNX 分支
    // 前置拒绝——不能靠空 map 既绕过冻结又通过 contains 判定。
    {
      PredictionWorkflow::PredictionJob job;
      QString err;
      QVariantMap params;
      params.insert( QStringLiteral( "unitSelection" ), QVariantMap() );
      QVERIFY( !wf.preparePredictionJob( QStringLiteral( "T70" ), QStringLiteral( "onnx:seg3" ),
                                         params, &job, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "冻结分析单元" ) ), qPrintable( err ) );
      QVERIFY( !job.staging.isValid() );
    }
    QCOMPARE( failSpy.count(), 3 );

    // 低层独立张量 API 保持原合同：不经正式发布入口也能数值验证。
    QString tensorErr;
    QVERIFY( f.onnx.loadModel( QStringLiteral( "seg3" ), &tensorErr ) );
    const OnnxTensor t = f.onnx.runTensorOn( QStringLiteral( "seg3" ), QStringLiteral( "x" ),
                                             { 0.5f, 0.5f, 0.5f, 0.5f }, { 1, 1, 2, 2 },
                                             &tensorErr );
    QVERIFY2( !t.values.isEmpty(), qPrintable( tensorErr ) );
  }

  void analysisLayerKeyMismatchAndForgedUnitRejected()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    // analysisLayerKey 绕过：键必须来自冻结单元。
    {
      PredictionWorkflow::PredictionJob job;
      QString err;
      QVariantMap params;
      params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
      params.insert( QStringLiteral( "analysisLayerKey" ), QStringLiteral( "T70" ) );
      QVERIFY( !wf.preparePredictionJob( QStringLiteral( "T70" ), QStringLiteral( "onnx:seg3" ),
                                         params, &job, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "analysisLayerKey" ) ), qPrintable( err ) );
    }
    // 伪造 S 编号：不在方案内的子层段被 freezeAnalysisUnit 拒绝。
    {
      auto forged = f.unit();
      forged.subIntervalId = QStringLiteral( "S9" );
      forged.subIndex = 9;
      PredictionWorkflow::PredictionJob job;
      QString err;
      QVariantMap params;
      params.insert( QStringLiteral( "unitSelection" ), toMap( forged ) );
      QVERIFY( !wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                         params, &job, &err ) );
      QVERIFY( !err.isEmpty() );
    }
    // aggregate：本链分析单位是 S1–S4，显式综合不被默认受理。
    {
      auto agg = f.unit();
      agg.aggregate = true;
      agg.subIntervalId.clear();
      PredictionWorkflow::PredictionJob job;
      QString err;
      QVariantMap params;
      params.insert( QStringLiteral( "unitSelection" ), toMap( agg ) );
      QVERIFY( !wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                         params, &job, &err ) );
      QVERIFY( !err.isEmpty() );
    }
  }

  // ---- P4-W3/W4：真实单元端到端 + 精确血缘 + 冻结模型 + 输出验证 ----

  void unitOnnxPredictionEndToEndWithPreciseLineage()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    const QString seismicVersion = f.registerSeismicVersion();
    QVERIFY( !seismicVersion.isEmpty() );
    const QString decoy = f.registerDecoyAsset( QStringLiteral( "T70–T71 旧平面" ) );
    QVERIFY( !decoy.isEmpty() );
    const QString decoy2 = f.registerDecoyAsset( QStringLiteral( "T7 时间面" ) );
    QVERIFY( !decoy2.isEmpty() );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    const UnitSelection selection = f.unit();
    const QString key = analysisLayerKey( selection );
    QVERIFY( !key.isEmpty() );

    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( selection ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    QCOMPARE( job.horizon, key );
    QCOMPARE( job.layerId, QStringLiteral( "pred.%1.onnx.seg3" ).arg( key ) );
    // 精确父引用：只有地震源版本；干扰资产绝不进入 parents。
    QCOMPARE( job.parentVersionIds, QStringList{ seismicVersion } );
    QVERIFY( !job.parentVersionIds.contains( decoy ) );
    QVERIFY( !job.parentVersionIds.contains( decoy2 ) );
    // 模型身份冻结：注册表钉 SHA（轻查询，不加载模型）。
    QCOMPARE( job.expectedModelSha256, PaleoOnnxService::sha256OfFile(
                  f.dir.filePath( QStringLiteral( "models/seg3.onnx" ) ) ) );
    QVERIFY( job.modelSha256.isEmpty() ); // compute 之前不加载模型

    QVERIFY2( wf.computePredictionJob( &job ), qPrintable( job.error ) );
    QVERIFY2( wf.publishPredictionJob( job, &err ), qPrintable( err ) );
    QCOMPARE( failSpy.count(), 0 );
    QCOMPARE( doneSpy.count(), 1 );

    // 展示声明指向 catalog 记录的同一已验证版本。
    const LayerDeclaration *d = findDecl( f.layers, job.layerId );
    QVERIFY2( d != nullptr, "prediction layer not declared" );
    const CatalogVersion committed = f.catalog.versionById( job.committedVersionId );
    QVERIFY( !committed.id.isEmpty() );
    QCOMPARE( QDir::cleanPath( d->source ),
              QDir::cleanPath( f.catalog.versionFilePath( committed ) ) );
    QVERIFY( QFile::exists( d->source ) );

    // provenance：单元/模型/引擎/实际输入源/回读验证摘要。
    const QVariantMap extra = committed.extra;
    QCOMPARE( fromMap( extra.value( QStringLiteral( "unitSelection" ) ).toMap() ), selection );
    const QVariantMap modelBlock = extra.value( QStringLiteral( "model_registry" ) ).toMap();
    QCOMPARE( modelBlock.value( QStringLiteral( "sha256_pinned" ) ).toString(),
              job.expectedModelSha256 );
    QCOMPARE( extra.value( QStringLiteral( "model_sha256" ) ).toString(),
              job.expectedModelSha256 );
    QCOMPARE( extra.value( QStringLiteral( "execution_engine" ) ).toString(),
              QStringLiteral( "onnxruntime" ) );
    QCOMPARE( extra.value( QStringLiteral( "input_attribute" ) ).toString(),
              QStringLiteral( "layer_window_rms" ) );
    QCOMPARE( extra.value( QStringLiteral( "input_sources" ) ).toMap()
                  .value( QStringLiteral( "seismic_path" ) ).toString(),
              f.seismicSource );
    const QVariantMap verified = extra.value( QStringLiteral( "verified_output" ) ).toMap();
    QVERIFY( verified.contains( QStringLiteral( "finite_cells" ) ) );
    QVERIFY( verified.value( QStringLiteral( "finite_cells" ) ).toInt() > 0 );

    // 栅格与单元 RMS 网格一致（64×64），有效像元与回读摘要一致。
    GDALDatasetH ds = GDALOpen( d->source.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, qPrintable( d->source ) );
    QCOMPARE( GDALGetRasterXSize( ds ), 64 );
    QCOMPARE( GDALGetRasterYSize( ds ), 64 );
    GDALClose( ds );
    delete d;

    // 同层段 S2 预测：身份与版本不碰撞（S1/S2 键不同、layerId 不同）。
    PredictionWorkflow::PredictionJob job2;
    QVariantMap params2;
    params2.insert( QStringLiteral( "unitSelection" ), toMap( f.unit( 2 ) ) );
    params2.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params2, &job2, &err ),
              qPrintable( err ) );
    QVERIFY( job2.layerId != job.layerId );
    QVERIFY( job2.horizon != job.horizon );
    QVERIFY2( wf.computePredictionJob( &job2 ), qPrintable( job2.error ) );
    QVERIFY2( wf.publishPredictionJob( job2, &err ), qPrintable( err ) );
    QVERIFY( job2.committedVersionId != job.committedVersionId );
    const LayerDeclaration *d2 = findDecl( f.layers, job2.layerId );
    QVERIFY2( d2 != nullptr, "S2 layer not declared" );
    delete d2;
  }

  void modelSwapAfterPrepareRejectedAtCompute()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    const QString frozenSha = job.expectedModelSha256;
    QVERIFY( !frozenSha.isEmpty() );

    // prepare 之后注册表换钉新模型文件（不同内容）——compute 按冻结契约拒绝。
    const QString modelPath = f.dir.filePath( QStringLiteral( "models/seg3.onnx" ) );
    QVERIFY( QFile::remove( modelPath ) );
    QVERIFY( OnnxFixtureWriter::writeSeg( modelPath, { 2.0f, -2.0f, 0.5f }, { 0.0f, 0.0f, 0.0f } ) );
    const QString newSha = PaleoOnnxService::sha256OfFile( modelPath );
    QVERIFY( newSha != frozenSha );
    QVERIFY2( !f.rePinManifest( QStringLiteral( "seg3" ), newSha ).isEmpty(), "re-pin failed" );

    QVERIFY( !wf.computePredictionJob( &job ) );
    QVERIFY2( job.error.contains( QStringLiteral( "model_revision_changed" ) ),
              qPrintable( job.error ) );
    QVERIFY( job.error.contains( frozenSha ) ); // 明确指出哪一项冻结契约变化
  }

  // 单元路径形状门禁：固定 2×2 输入的模型与 64×64 单元 RMS 网格不一致
  // ——compute 拒绝，不写栅格、不登记（旧 411×641 工区门禁的单元路径承接；
  // writeSeg 的输入/输出形状绑定生成，输入侧拒绝即证明门禁生效）。
  void scopedModelShapeMismatchRefused()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    const QString modelDir = f.dir.filePath( QStringLiteral( "models" ) );
    QVERIFY( QDir().mkpath( modelDir ) );
    const QString fixedPath = modelDir + QStringLiteral( "/fixed22.onnx" );
    QVERIFY( OnnxFixtureWriter::writeSeg( fixedPath, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f },
                                          8, nullptr, 2, 2 ) );
    QFile mf( modelDir + QStringLiteral( "/manifest.json" ) );
    QVERIFY( mf.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    mf.write( QStringLiteral( R"({"models":[{"name":"fixed22","file":"fixed22.onnx",)"
                             R"("version":"1.0.0","task":"segmentation",)"
                             R"("dataType":"amplitude","input":{"name":"x","dtype":"float32","shape":[1,1,2,2]},)"
                             R"("outputs":{"semantics":"logits","classes":3},)"
                             R"("sha256":"%1"}]})" )
                 .arg( PaleoOnnxService::sha256OfFile( fixedPath ) )
                 .toUtf8() );
    mf.close();
    f.onnx.setModelRoot( modelDir );
    f.onnx.setModelRegistry( ModelRegistry::scan( modelDir ) );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:fixed22" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    QVERIFY( !wf.computePredictionJob( &job ) );
    QVERIFY2( job.error.contains( QStringLiteral( "模型输入与当前层段的 RMS 网格不匹配" ) ),
              qPrintable( job.error ) );
    QVERIFY( job.error.contains( QStringLiteral( "[1,1,64,64]" ) ) );
    QString pubErr;
    QVERIFY( !wf.publishPredictionJob( job, &pubErr ) ); // 未产出 → 发布拒绝
    QVERIFY( findDecl( f.layers, job.layerId ) == nullptr );
  }

  // 未登记模型：prepare 在注册表门控处拒绝（不加载模型文件、不 stage）。
  void ghostModelRejectedAtPrepareWithoutLoading()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY( !wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:ghost" ),
                                       params, &job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "ghost" ) ) && err.contains( QStringLiteral( "注册表" ) ),
              qPrintable( err ) );
    QVERIFY( !job.prepared );
    QVERIFY( !job.staging.isValid() );
    QVERIFY( job.modelSha256.isEmpty() );
  }

  // ---- P4-W5/W6：发布编排与故障矩阵 ----

  // Catalog 提交失败：无悬空声明（先提交后声明），只有本任务未提交暂存被清。
  void catalogCommitFailureLeavesNoDanglingDeclaration()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    // v1 正常发布。
    PredictionWorkflow::PredictionJob v1;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &v1, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &v1 ), qPrintable( v1.error ) );
    QVERIFY2( wf.publishPredictionJob( v1, &err ), qPrintable( err ) );
    const LayerDeclaration *decl1 = findDecl( f.layers, v1.layerId );
    QVERIFY( decl1 != nullptr );
    const QString v1Source = decl1->source;
    delete decl1;
    QVERIFY( QFile::exists( v1Source ) );

    // v2：注入提交失败（父版本引用不存在的版本 → commit 拒绝）。
    PredictionWorkflow::PredictionJob v2;
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &v2, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &v2 ), qPrintable( v2.error ) );
    v2.parentVersionIds = QStringList{ QStringLiteral( "ver-ghost" ) };
    QVERIFY( !wf.publishPredictionJob( v2, &err ) );
    QVERIFY( v2.committedVersionId.isEmpty() ); // 未提交
    // v1 声明/文件/版本不受失败 v2 影响。
    const LayerDeclaration *declAgain = findDecl( f.layers, v1.layerId );
    QVERIFY( declAgain != nullptr );
    QCOMPARE( declAgain->source, v1Source );
    QVERIFY( QFile::exists( v1Source ) );
    delete declAgain;
    QVERIFY( f.catalog.versionById( v2.staging.versionId ).id.isEmpty() );
    // 未提交暂存被清（stagedPath 上的文件不复存在）。
    QVERIFY( !QFile::exists( v2.staging.absolutePath ) );
    QVERIFY( failSpy.count() >= 1 );
  }

  // v2 提交成功但声明失败：文件与 Catalog 记录保留，状态=展示未完成，重试
  // 只恢复展示（不重复提交、不重复推理计数）。
  void declareFailureAfterCommitKeepsResultAndRetryRestoresDisplay()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &job ), qPrintable( job.error ) );

    // 注入声明失败：清空 layerId（manifest 声明拒绝空 id——既有注入手法）。
    const QString realLayerId = job.layerId;
    job.layerId.clear();
    QVERIFY( !wf.publishPredictionJob( job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "已提交" ) ) && err.contains( QStringLiteral( "展示未完成" ) ),
              qPrintable( err ) );
    QVERIFY( err.contains( job.committedVersionId ) ); // 可恢复的精确产物引用
    // 文件与 Catalog 记录保留。
    const CatalogVersion committed = f.catalog.versionById( job.committedVersionId );
    QVERIFY( !committed.id.isEmpty() );
    QVERIFY( QFile::exists( f.catalog.versionFilePath( committed ) ) );

    // 重试只恢复展示：不追加新版本。
    const int versionsBefore = f.catalog.versions().size();
    job.layerId = realLayerId;
    QVERIFY2( wf.publishPredictionJob( job, &err ), qPrintable( err ) );
    QCOMPARE( f.catalog.versions().size(), versionsBefore );
    const LayerDeclaration *d = findDecl( f.layers, realLayerId );
    QVERIFY2( d != nullptr, "retry did not restore display" );
    QCOMPARE( QDir::cleanPath( d->source ),
              QDir::cleanPath( f.catalog.versionFilePath( committed ) ) );
    delete d;
    QVERIFY( doneSpy.count() == 1 );
    QVERIFY( failSpy.count() == 1 );
  }

  // 同一任务结果重复回调：不重做提交、不追加版本、不覆盖较新展示。
  void duplicateCallbackIsIdempotent()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &job ), qPrintable( job.error ) );
    QVERIFY2( wf.publishPredictionJob( job, &err ), qPrintable( err ) );
    const int versionsAfterFirst = f.catalog.versions().size();

    // 重复回调（同 job 再 publish）。
    QVERIFY2( wf.publishPredictionJob( job, &err ), qPrintable( err ) );
    QCOMPARE( f.catalog.versions().size(), versionsAfterFirst ); // 无重复版本
    QCOMPARE( job.committedVersionId, job.staging.versionId );
  }

  // 运行期间展示目标被另一有效运行更新：拒绝覆盖、保留结果供查看。
  void concurrentDisplayWinnerNotOverwritten()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );

    PredictionWorkflow::PredictionJob job1;
    QString err;
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job1, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &job1 ), qPrintable( job1.error ) );
    QVERIFY2( wf.publishPredictionJob( job1, &err ), qPrintable( err ) );

    // job2 在 job1 完成后 prepare（快照=job1 的声明源）。
    const QString job1CommittedPath = f.catalog.versionFilePath(
        f.catalog.versionById( job1.committedVersionId ) );
    PredictionWorkflow::PredictionJob job2;
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job2, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &job2 ), qPrintable( job2.error ) );
    QCOMPARE( job2.declaredSourceAtPrepare, job1CommittedPath ); // 冻结了真实声明现状

    // job2 publish 之前，另一有效运行更新了同一展示目标（按现有目标状态
    // 模拟：声明指向另一个真实存在的有效产物文件）。
    const QString winnerSource = f.dir.filePath( QStringLiteral( "concurrent-winner.tif" ) );
    QVERIFY( QFile::copy( job1CommittedPath, winnerSource ) );
    LayerDeclaration winnerDecl;
    winnerDecl.layerId = job2.layerId;
    winnerDecl.horizon = job2.horizon;
    winnerDecl.type = QStringLiteral( "raster" );
    winnerDecl.source = winnerSource;
    winnerDecl.group = job2.layerGroup;
    QVERIFY2( f.layers.declare( winnerDecl, &err ), qPrintable( err ) );

    // 迟到的 job2：提交成功（catalog 有其版本）但展示不覆盖并发胜者。
    QVERIFY2( wf.publishPredictionJob( job2, &err ), qPrintable( err ) );
    QVERIFY( !job2.committedVersionId.isEmpty() );
    const LayerDeclaration *after = findDecl( f.layers, job2.layerId );
    QVERIFY( after != nullptr );
    QCOMPARE( after->source, winnerSource ); // 较新展示未被覆盖
    delete after;
    QVERIFY( !f.catalog.versionById( job2.committedVersionId ).id.isEmpty() ); // 结果保留可查
  }

  // ---- P4-W7：取消/切工程清理审计 ----

  void cancelBeforeCommitPublishesNothing()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &job ), qPrintable( job.error ) );
    job.isCancelled = [] { return true; };
    QVERIFY( !wf.publishPredictionJob( job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "提交前" ) ), qPrintable( err ) );
    QVERIFY( job.committedVersionId.isEmpty() );
    QVERIFY( f.catalog.versionById( job.staging.versionId ).id.isEmpty() );
  }

  // 取消发生在提交之后：提交凭证被尊重，守卫清理不误删已提交文件。
  void cancelAfterCommitKeepsCommittedFact()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &job ), qPrintable( job.error ) );
    QVERIFY2( wf.publishPredictionJob( job, &err ), qPrintable( err ) );
    const QString committedPath = f.catalog.versionFilePath(
        f.catalog.versionById( job.committedVersionId ) );
    QVERIFY( QFile::exists( committedPath ) );

    // 上层清理（attach_predict 语义）：committedVersionId 非空 → 跳过 discard。
    if ( job.committedVersionId.isEmpty() )
      PaleoWorkflowDiscardPredictionStagingDir( job.staging.versionId, job.outputPath );
    QVERIFY( QFile::exists( committedPath ) ); // 已提交产物未被清理误删

    // 提交后取消的 publish 重放：如实报「已提交、取消在提交之后」，不删文件。
    job.isCancelled = [] { return true; };
    QVERIFY( !wf.publishPredictionJob( job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "已提交" ) ), qPrintable( err ) );
    QVERIFY( QFile::exists( committedPath ) );
  }

  // 工程切换在提交之后：不把已发生的提交当未提交暂存清掉。
  void projectSwitchAfterCommitKeepsCommittedArtifact()
  {
    UnitFixture f;
    QVERIFY( f.init() );
    QVERIFY2( !f.installSegModel().isEmpty(), "model install failed" );
    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setOnnxService( &f.onnx );
    wf.setCatalog( &f.catalog, f.dir.path() );

    PredictionWorkflow::PredictionJob job;
    QString err;
    QVariantMap params;
    params.insert( QStringLiteral( "unitSelection" ), toMap( f.unit() ) );
    params.insert( QStringLiteral( "seismicPath" ), f.seismicSource );
    QVERIFY2( wf.preparePredictionJob( QStringLiteral( "A" ), QStringLiteral( "onnx:seg3" ),
                                       params, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computePredictionJob( &job ), qPrintable( job.error ) );
    QVERIFY2( wf.publishPredictionJob( job, &err ), qPrintable( err ) );
    const QString committedPath = f.catalog.versionFilePath(
        f.catalog.versionById( job.committedVersionId ) );

    // 同路径重开（新会话 = 新工程代次，tst_workflows 同口径）。
    wf.setCatalog( &f.catalog, f.dir.path() );
    QString switchErr;
    QVERIFY( !wf.publishPredictionJob( job, &switchErr ) );
    QVERIFY2( switchErr.contains( QStringLiteral( "已提交" ) ), qPrintable( switchErr ) );
    QVERIFY( QFile::exists( committedPath ) ); // 提交事实未被「工程已切换」清理
  }
#endif
};

int main( int argc, char *argv[] )
{
  if ( !QgisRuntime::isInitialized() )
    QgisRuntime::initialize( QStringLiteral( "/usr" ) );
  QgsApplication::processingRegistry();
  GDALAllRegister();
  const QByteArray logPath = QByteArray( QT_TESTCASE_BUILDDIR ) + "/tst_p4_onnx_prediction-result.txt";
  QList<QByteArray> forwarded;
  forwarded << QByteArray( argv[0] );
  bool hasFileLogger = false;
  for ( int i = 1; i < argc; ++i )
  {
    forwarded << QByteArray( argv[i] );
    if ( QByteArray( argv[i] ) == "-o" || QByteArray( argv[i] ) == "-output" )
      hasFileLogger = true;
  }
  if ( !hasFileLogger )
    forwarded << QByteArray( "-o" ) << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve( forwarded.size() );
  for ( QByteArray &a : forwarded )
    cargv << a.data();
  TestP4OnnxPrediction tc;
  const int rc = QTest::qExec( &tc, cargv.size(), cargv.data() );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_p4_onnx_prediction.moc"
