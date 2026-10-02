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

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestModelRegistry tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_modelregistry.moc"
