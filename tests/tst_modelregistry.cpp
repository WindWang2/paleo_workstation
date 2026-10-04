#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "../src/ai/modelregistry.h"
#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"

// goal/ai-geological-assist 轮4：模型注册表扫描与如实降级。
// manifest 缺失 = 「未装模型」（非错误）；坏 JSON/缺文件/钉哈希不符逐条如实分类。
class TestModelRegistry : public QObject
{
  Q_OBJECT
private slots:
  void missingDirectoryDegradesToNotInstalled();
  void emptyModelsDirDegradesToNotInstalled();
  void emptyModelsArrayIsNotAnError();
  void validManifestListsRunnableModels();
  void missingFileClassifiedPerEntry();
  void pinnedShaMismatchDetected();
  void pinnedShaMatchStaysOk();
  void brokenJsonReportsManifestError();
  void entryWithoutNameIsManifestInvalid();
  void shapeNullBecomesDynamicDim();
  // #145：注册表门控生产加载路径 + 路径约束
  void traversalAndAbsoluteFileRejected();
  void duplicateNameRejected();
  void serviceExposesOnlyRunnableEntries();
  void serviceLoadsViaManifestFileField();
  void serviceRejectsReplacedModelAtLoad();
  void serviceRejectsUnsafeNameWithoutManifest();
  void brokenManifestBlocksAllModels();

private:
  static void writeText( const QString &path, const QByteArray &bytes )
  {
    QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile f( path );
    QVERIFY( f.open( QIODevice::WriteOnly ) );
    f.write( bytes );
    f.close();
  }
};

void TestModelRegistry::missingDirectoryDegradesToNotInstalled()
{
  const ModelRegistryScan scan = ModelRegistry::scan( QStringLiteral( "/nonexistent/models" ) );
  QVERIFY( !scan.manifestFound );
  QVERIFY( scan.manifestError.isEmpty() ); // 未装模型 ≠ 错误
  QVERIFY( scan.entries.isEmpty() );
  QVERIFY( !scan.anyRunnable() );
}

void TestModelRegistry::emptyModelsDirDegradesToNotInstalled()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QVERIFY( !scan.manifestFound );
  QVERIFY( scan.manifestError.isEmpty() );
  QVERIFY( !scan.anyRunnable() );
}

void TestModelRegistry::emptyModelsArrayIsNotAnError()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [] })" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QVERIFY( scan.manifestFound );
  QVERIFY( scan.manifestError.isEmpty() );
  QVERIFY( !scan.anyRunnable() ); // 结构合法、没有模型
}

void TestModelRegistry::validManifestListsRunnableModels()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeSeg( dir.filePath( QStringLiteral( "seg3.onnx" ) ),
                                       { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) );
  QVERIFY( OnnxFixtureWriter::writeTraceScorer( dir.filePath( QStringLiteral( "scorer.onnx" ) ),
                                                8.0f, 0.0f ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({
  "models": [
    { "name": "seg3", "file": "seg3.onnx", "version": "1.0", "task": "segmentation",
      "dataType": "amplitude",
      "input": { "name": "x", "dtype": "float32", "shape": [1, 1, null, null] },
      "outputs": { "semantics": "logits", "classes": 3 } },
    { "name": "scorer", "file": "scorer.onnx", "version": "0.9", "task": "trace_scorer",
      "dataType": "amplitude",
      "input": { "name": "t", "dtype": "float32", "shape": [1, 1, null] },
      "outputs": { "semantics": "probability", "classes": 1 } }
  ]
})" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QVERIFY( scan.manifestFound );
  QVERIFY2( scan.manifestError.isEmpty(), qPrintable( scan.manifestError ) );
  QCOMPARE( scan.entries.size(), 2 );
  QCOMPARE( scan.runnableNames(),
            QStringList() << QStringLiteral( "seg3" ) << QStringLiteral( "scorer" ) );
  const ModelRegistryEntry &seg = scan.entries.first();
  QCOMPARE( seg.status, ModelRegistryEntry::Status::Ok );
  QCOMPARE( seg.task, QStringLiteral( "segmentation" ) );
  QCOMPARE( seg.classes, 3 );
  QCOMPARE( seg.inputShape, QVector<int64_t>( { 1, 1, -1, -1 } ) );
  QVERIFY( seg.detail.isEmpty() );
  // 实测指纹填齐（无钉哈希也要给）。
  QCOMPARE( seg.sha256Actual, PaleoOnnxService::sha256OfFile( seg.absolutePath ) );
}

void TestModelRegistry::missingFileClassifiedPerEntry()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "ok.onnx" ) ) ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [
  { "name": "ok", "file": "ok.onnx" },
  { "name": "gone", "file": "gone.onnx" }
] })" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QCOMPARE( scan.entries.size(), 2 );
  QCOMPARE( scan.entries.at( 0 ).status, ModelRegistryEntry::Status::Ok );
  const ModelRegistryEntry &gone = scan.entries.at( 1 );
  QCOMPARE( gone.status, ModelRegistryEntry::Status::FileMissing );
  QVERIFY2( gone.detail.contains( QStringLiteral( "gone.onnx" ) ), qPrintable( gone.detail ) );
  QCOMPARE( scan.runnableNames(), QStringList() << QStringLiteral( "ok" ) );
}

void TestModelRegistry::pinnedShaMismatchDetected()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeSeg( dir.filePath( QStringLiteral( "seg3.onnx" ) ),
                                       { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [ { "name": "seg3", "file": "seg3.onnx",
  "sha256": "0000000000000000000000000000000000000000000000000000000000000000" } ] })" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QCOMPARE( scan.entries.size(), 1 );
  QCOMPARE( scan.entries.at( 0 ).status, ModelRegistryEntry::Status::FingerprintMismatch );
  QVERIFY( scan.entries.at( 0 ).detail.contains( QStringLiteral( "钉哈希不符" ) ) );
  QVERIFY( !scan.anyRunnable() ); // 被换过的模型不上算法列表
}

void TestModelRegistry::pinnedShaMatchStaysOk()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString model = dir.filePath( QStringLiteral( "seg3.onnx" ) );
  QVERIFY( OnnxFixtureWriter::writeSeg( model, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) );
  const QString sha = PaleoOnnxService::sha256OfFile( model );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             QStringLiteral( R"({ "models": [ { "name": "seg3", "file": "seg3.onnx", "sha256": "%1" } ] })" )
               .arg( sha )
               .toUtf8() );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QCOMPARE( scan.entries.size(), 1 );
  QCOMPARE( scan.entries.at( 0 ).status, ModelRegistryEntry::Status::Ok );
  QCOMPARE( scan.runnableNames(), QStringList() << QStringLiteral( "seg3" ) );
}

void TestModelRegistry::brokenJsonReportsManifestError()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             QByteArrayLiteral( "{ not json" ) );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QVERIFY( scan.manifestFound );
  QVERIFY2( !scan.manifestError.isEmpty(), "坏 manifest 必须如实报错" );
  QVERIFY( scan.entries.isEmpty() );
}

void TestModelRegistry::entryWithoutNameIsManifestInvalid()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [ { "file": "x.onnx" } ] })" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QCOMPARE( scan.entries.size(), 1 );
  QCOMPARE( scan.entries.at( 0 ).status, ModelRegistryEntry::Status::ManifestInvalid );
  QVERIFY( !scan.entries.at( 0 ).detail.isEmpty() );
}

void TestModelRegistry::shapeNullBecomesDynamicDim()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "m.onnx" ) ) ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [ { "name": "m", "file": "m.onnx",
  "input": { "name": "x", "dtype": "float32", "shape": [1, null] } } ] })" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QCOMPARE( scan.entries.at( 0 ).inputShape, QVector<int64_t>( { 1, -1 } ) );
}

void TestModelRegistry::traversalAndAbsoluteFileRejected()
{
  QTemporaryDir outer;
  QVERIFY( outer.isValid() );
  const QString modelsDir = QDir( outer.path() ).filePath( QStringLiteral( "models" ) );
  QDir().mkpath( modelsDir );
  // models/ 外放一个真模型——旧代码会把 ../evil.onnx 判为 Ok。
  const QString evil = QDir( outer.path() ).filePath( QStringLiteral( "evil.onnx" ) );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( evil ) );
  writeText( QDir( modelsDir ).filePath( QStringLiteral( "manifest.json" ) ),
             QStringLiteral( R"({ "models": [
  { "name": "up", "file": "../evil.onnx" },
  { "name": "abs", "file": "%1" },
  { "name": "../x", "file": "x.onnx" } ] })" )
               .arg( evil )
               .toUtf8() );
  const ModelRegistryScan scan = ModelRegistry::scan( modelsDir );
  QCOMPARE( scan.entries.size(), 3 );
  for ( const ModelRegistryEntry &e : scan.entries )
    QVERIFY2( e.status == ModelRegistryEntry::Status::ManifestInvalid, qPrintable( e.name + e.detail ) );
  QVERIFY( !scan.anyRunnable() );
#ifndef Q_OS_WIN
  // 符号链接逃逸：词法合法但 canonical 落在 models/ 外。
  QVERIFY( QFile::link( evil, QDir( modelsDir ).filePath( QStringLiteral( "link.onnx" ) ) ) );
  writeText( QDir( modelsDir ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [ { "name": "link", "file": "link.onnx" } ] })" );
  const ModelRegistryScan scan2 = ModelRegistry::scan( modelsDir );
  QCOMPARE( scan2.entries.size(), 1 );
  QCOMPARE( scan2.entries.at( 0 ).status, ModelRegistryEntry::Status::ManifestInvalid );
#endif
}

void TestModelRegistry::duplicateNameRejected()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "a.onnx" ) ) ) );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "b.onnx" ) ) ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [ { "name": "m", "file": "a.onnx" }, { "name": "m", "file": "b.onnx" } ] })" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QCOMPARE( scan.entries.size(), 2 );
  QCOMPARE( scan.entries.at( 0 ).status, ModelRegistryEntry::Status::Ok );
  QCOMPARE( scan.entries.at( 1 ).status, ModelRegistryEntry::Status::ManifestInvalid );
}

void TestModelRegistry::serviceExposesOnlyRunnableEntries()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "good.onnx" ) ) ) );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "swapped.onnx" ) ) ) );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "stray.onnx" ) ) ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [ { "name": "good", "file": "good.onnx" },
  { "name": "swapped", "file": "swapped.onnx",
    "sha256": "0000000000000000000000000000000000000000000000000000000000000000" } ] })" );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  svc.setModelRegistry( ModelRegistry::scan( dir.path() ) );
  QCOMPARE( svc.availableModels(), QStringList() << QStringLiteral( "good" ) );
  QString err;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "swapped" ), nullptr, &err ), OnnxLoadStatus::NotFound );
  QVERIFY2( err.contains( QStringLiteral( "注册表" ) ), qPrintable( err ) );
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "stray" ), nullptr, &err ), OnnxLoadStatus::NotFound );
  QVERIFY2( err.contains( QStringLiteral( "未在" ) ), qPrintable( err ) );
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "good" ), nullptr, &err ), OnnxLoadStatus::Ok );
}

void TestModelRegistry::serviceLoadsViaManifestFileField()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( QDir( dir.path() ).mkpath( QStringLiteral( "sub" ) ) );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "sub/model_v2.onnx" ) ) ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             R"({ "models": [ { "name": "seg", "file": "sub/model_v2.onnx" } ] })" );
  const ModelRegistryScan scan = ModelRegistry::scan( dir.path() );
  QCOMPARE( scan.runnableNames(), QStringList() << QStringLiteral( "seg" ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  svc.setModelRegistry( scan );
  QString err;
  // 旧代码按 <root>/seg.onnx 加载 → NotFound。
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "seg" ), nullptr, &err ), OnnxLoadStatus::Ok );
  QVERIFY( svc.isModelLoaded( QStringLiteral( "seg" ) ) );
  const OnnxTensor out = svc.runTensorOn( QStringLiteral( "seg" ), QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
  QVERIFY2( out.values.size() == 1, qPrintable( err ) );
  QCOMPARE( out.values.at( 0 ), 42.0f );
}

void TestModelRegistry::serviceRejectsReplacedModelAtLoad()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString model = dir.filePath( QStringLiteral( "seg3.onnx" ) );
  QVERIFY( OnnxFixtureWriter::writeSeg( model, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) );
  const QString sha = PaleoOnnxService::sha256OfFile( model );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ),
             QStringLiteral( R"({ "models": [ { "name": "seg3", "file": "seg3.onnx", "sha256": "%1" } ] })" )
               .arg( sha )
               .toUtf8() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  svc.setModelRegistry( ModelRegistry::scan( dir.path() ) );
  QString err;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "seg3" ), nullptr, &err ), OnnxLoadStatus::Ok );
  // 扫描之后文件被替换：加载时复核钉哈希必须拒绝（池命中也不放行）。
  QVERIFY( OnnxFixtureWriter::writeSeg( model, { 9.0f, 9.0f, 9.0f }, { 0.0f, 0.0f, 0.0f } ) );
  QVERIFY( PaleoOnnxService::sha256OfFile( model ) != sha );
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "seg3" ), nullptr, &err ), OnnxLoadStatus::NotFound );
  QVERIFY2( err.contains( QStringLiteral( "钉哈希不符" ) ), qPrintable( err ) );
}

void TestModelRegistry::serviceRejectsUnsafeNameWithoutManifest()
{
  QTemporaryDir outer;
  QVERIFY( outer.isValid() );
  const QString modelsDir = QDir( outer.path() ).filePath( QStringLiteral( "models" ) );
  QDir().mkpath( modelsDir );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( QDir( outer.path() ).filePath( QStringLiteral( "evil.onnx" ) ) ) );
  PaleoOnnxService svc;
  svc.setModelRoot( modelsDir );
  svc.setModelRegistry( ModelRegistry::scan( modelsDir ) ); // 无 manifest：开发降级
  QString err;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "../evil" ), nullptr, &err ), OnnxLoadStatus::NotFound );
  QVERIFY2( err.contains( QStringLiteral( "非法模型名" ) ), qPrintable( err ) );
}

void TestModelRegistry::brokenManifestBlocksAllModels()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "m.onnx" ) ) ) );
  writeText( QDir( dir.path() ).filePath( QStringLiteral( "manifest.json" ) ), QByteArrayLiteral( "{ not json" ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  svc.setModelRegistry( ModelRegistry::scan( dir.path() ) );
  QVERIFY( svc.availableModels().isEmpty() );
  QString err;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "m" ), nullptr, &err ), OnnxLoadStatus::NotFound );
  QVERIFY2( err.contains( QStringLiteral( "manifest" ) ), qPrintable( err ) );
}

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestModelRegistry tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_modelregistry.moc"
