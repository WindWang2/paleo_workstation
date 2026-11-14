// 层：测试壳
#include <QtTest>
#include "ai/onnxfixture.h"
#include "ai/onnxpredictionservice.h"
#include "ai/tileinference.h"
#include "ai/horizonsuggest.h"
#include "ai/onnxmodelbytes.h"
#include <QFile>
#include <onnxruntime_cxx_api.h>
#include <QTemporaryDir>

class TestPinnedInference : public QObject {
  Q_OBJECT
private slots:
  void actualOutputChannelsAreCheckedBeforeAnyTileFetch_data() {
    QTest::addColumn<int>("channels");QTest::addColumn<QString>("expected");
    QTest::newRow("512-real-2-declared")<<512<<QString("tile_model_output_classes_out_of_range");
    QTest::newRow("3-real-2-declared")<<3<<QString("tile_model_output_classes_mismatch");
    QTest::newRow("unknown-real-2-declared")<<-1<<QString("tile_model_output_classes_unknown");
  }
  void actualOutputChannelsAreCheckedBeforeAnyTileFetch() {
    QFETCH(int,channels);QFETCH(QString,expected);QTemporaryDir temp;QString error;
    const auto path=temp.filePath("model.onnx");
    if(channels>0)QVERIFY(OnnxFixtureWriter::writeSeg(path,QVector<float>(channels,1),QVector<float>(channels,0)));
    else QVERIFY(OnnxFixtureWriter::writeIdentity(path,{1,-1,-1,-1}));
    PaleoOnnxService service;service.setModelRoot(temp.path());ModelRegistryScan scan;scan.manifestFound=true;
    ModelRegistryEntry entry;entry.name="model";entry.file="model.onnx";entry.absolutePath=path;
    entry.sha256Pinned=PaleoOnnxService::sha256OfFile(path);entry.classes=2;entry.outputSemantics="logits";
    scan.entries.append(entry);service.setModelRegistry(scan);OnnxModelMeta meta;
    QCOMPARE(service.loadModelMeta("model",&meta,&error),OnnxLoadStatus::Ok);
    QCOMPARE(meta.classes,2);QCOMPARE(meta.outputShape,QVector<int64_t>({1,channels,-1,-1}));
    QCOMPARE(meta.outputElemType,1);QVERIFY(meta.warmupMs>=0);QVERIFY2(meta.warmupError.isEmpty(),qPrintable(meta.warmupError));
    // Dynamic H/W are warmed with 1x1 only; the 1024² request must never fetch or allocate its real output.
    TileInferenceRequest request;request.model="model";request.gridRows=1024;request.gridCols=1024;
    request.tileRows=1024;request.tileCols=1024;request.expectedClassCount=2;request.requireProbabilityContract=true;
    int reads=0;request.fetch=[&](int,int,int rows,int cols,QVector<float> &values,QString &) {
      ++reads;values.fill(1,rows*cols);return true;
    };
    TileInferenceResult result;
    QVERIFY(!runTileInference(&service,request,&result,{},nullptr,&error));QCOMPARE(error,expected);
    QCOMPARE(reads,0);QCOMPARE(result.tilesDone,0);
  }
  void oversizedStaticOutputSkipsWarmupAndRunBeforeAllocation() {
    QTemporaryDir temp;QString error;
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),QVector<float>(64,1),QVector<float>(64,0),8,&error,1024,1024));
    PaleoOnnxService service;service.setModelRoot(temp.path());OnnxModelMeta meta;
    QCOMPARE(service.loadModelMeta("model",&meta,&error),OnnxLoadStatus::Ok);
    QCOMPARE(meta.outputShape,QVector<int64_t>({1,64,1024,1024}));QCOMPARE(meta.warmupMs,qint64(-1));
    QCOMPARE(meta.warmupError,QString("model_output_budget_exceeded"));
    const auto output=service.runPinnedTensor("model",meta.sha256,meta.inputName,QVector<float>(1024*1024,0),{1,1,1024,1024},&error);
    QVERIFY(output.values.isEmpty());QCOMPARE(error,QString("model_output_budget_exceeded"));
  }
  void exactModelBytesRejectReplacementAndOversizedFiles() {
    QTemporaryDir temp;const auto path=temp.filePath("model.onnx");
    QVERIFY(OnnxFixtureWriter::writeAddScalar(path,40));const auto sha=PaleoOnnxService::sha256OfFile(path);QString error;
    QCOMPARE(paleo::onnx::boundedFingerprint(path,&error),sha);
    QVERIFY(!paleo::onnx::readPinnedBytes(path,sha,&error).isEmpty());
    QVERIFY(OnnxFixtureWriter::writeAddScalar(path,80));QVERIFY(paleo::onnx::readPinnedBytes(path,sha,&error).isEmpty());
    QCOMPARE(error,QString("model_bytes_changed"));QFile large(temp.filePath("large.onnx"));QVERIFY(large.open(QIODevice::WriteOnly));
    QVERIFY(large.resize(paleo::onnx::maxModelBytes+1));large.close();PaleoOnnxService service;service.setModelRoot(temp.path());
    QCOMPARE(service.loadModelMeta("large",nullptr,&error),OnnxLoadStatus::CreateFailed);QCOMPARE(error,QString("model_byte_budget_exceeded"));
  }
  void externalWeightsCannotMasqueradeAsTheMainFileFingerprint() {
    QTemporaryDir temp;QString error;QVERIFY(OnnxFixtureWriter::writeExternalAddScalar(temp.filePath("model.onnx"),&error));
    { // Prove this is a valid external-weight model when its file path is supplied.
      Ort::Env env(ORT_LOGGING_LEVEL_WARNING,"external-fixture");Ort::SessionOptions options;options.SetIntraOpNumThreads(1);
#ifdef Q_OS_WIN
      const auto path=temp.filePath("model.onnx").toStdWString();
#else
      const auto path=temp.filePath("model.onnx").toStdString();
#endif
      Ort::Session direct(env,path.c_str(),options);QCOMPARE(direct.GetInputCount(),size_t(1));
    }
    PaleoOnnxService service;service.setModelRoot(temp.path());OnnxModelMeta meta;
    QVERIFY(service.loadModelMeta("model",&meta,&error)!=OnnxLoadStatus::Ok);QVERIFY(!error.isEmpty());QCOMPARE(service.sessionPoolSize(),0);
  }
  void sameNameReplacementCannotSatisfyAnOldFingerprint() {
    QTemporaryDir first, second;
    QVERIFY(OnnxFixtureWriter::writeAddScalar(first.filePath("model.onnx"), 40));
    QVERIFY(OnnxFixtureWriter::writeAddScalar(second.filePath("model.onnx"), 80));
    PaleoOnnxService service; service.setModelRoot(first.path()); QString error;
    OnnxModelMeta original, replacement;
    QCOMPARE(service.loadModelMeta("model", &original, &error), OnnxLoadStatus::Ok);
    const auto good = service.runPinnedTensor("model", original.sha256, original.inputName, {2}, {1}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(good.values, QVector<float>({42}));
    service.setModelRoot(second.path());
    QCOMPARE(service.loadModelMeta("model", &replacement, &error), OnnxLoadStatus::Ok);
    QVERIFY(original.sha256 != replacement.sha256);
    QVERIFY(service.runPinnedTensor("model", original.sha256, original.inputName, {2}, {1}, &error).values.isEmpty());
    QCOMPARE(error, QStringLiteral("model_revision_changed"));
    const auto current = service.runPinnedTensor("model", replacement.sha256, replacement.inputName, {2}, {1}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(current.values, QVector<float>({82}));
  }
  void tileOperationRejectsARevisionChangeBetweenLoadAndRun() {
    QTemporaryDir first, second;
    QVERIFY(OnnxFixtureWriter::writeSeg(first.filePath("model.onnx"), {1, -1}, {0, 0}));
    QVERIFY(OnnxFixtureWriter::writeSeg(second.filePath("model.onnx"), {2, -2}, {0, 0}));
    PaleoOnnxService service; service.setModelRoot(first.path());
    TileInferenceRequest request; request.model = "model"; request.gridRows = 2; request.gridCols = 2;
    request.tileRows = 2; request.tileCols = 2;
    request.fetch = [&](int, int, int rows, int cols, QVector<float> &values, QString &error) {
      values.fill(1, rows * cols); service.setModelRoot(second.path());
      return service.loadModel("model", &error);
    };
    TileInferenceResult result; QString error;
    QVERIFY(!runTileInference(&service, request, &result, {}, nullptr, &error));
    QVERIFY2(error.contains("model_revision_changed"), qPrintable(error));
    QCOMPARE(result.tilesDone, 0);
  }
  void trackingOperationRejectsARevisionChangeBeforeItsFirstScore() {
    QTemporaryDir first, second;
    QVERIFY(OnnxFixtureWriter::writeTraceScorer(first.filePath("model.onnx"), 1, 0));
    QVERIFY(OnnxFixtureWriter::writeTraceScorer(second.filePath("model.onnx"), 2, 0));
    PaleoOnnxService service; service.setModelRoot(first.path());
    TraceWindowFetcher fetch = [&](int, int, int, int count, QVector<float> &values, QString &error) {
      values.fill(1, count); service.setModelRoot(second.path());
      return service.loadModel("model", &error);
    };
    QVector<TrackingSuggestion> suggestions; QString error, usedFingerprint;
    QVERIFY(!suggestHorizonTracking(&service, "model", {{100, 200, 4}}, 4, 1, fetch, {},
                                     &suggestions, &error, &usedFingerprint));
    QVERIFY2(error.contains("model_revision_changed"), qPrintable(error));
    QVERIFY(suggestions.isEmpty()); QVERIFY(!usedFingerprint.isEmpty());
  }
  void fixedModelDimensionsCannotForceUnboundedPadding() {
    QTemporaryDir temp;QString error;
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),{1,-1},{0,0},8,&error,100000,1));
    PaleoOnnxService service;service.setModelRoot(temp.path());
    TileInferenceRequest request;request.model="model";request.gridRows=1;request.gridCols=1;
    int reads=0;request.fetch=[&](int,int,int rows,int cols,QVector<float> &values,QString &) {
      ++reads;values.fill(1,rows*cols);return true;
    };
    TileInferenceResult result;
    QVERIFY(!runTileInference(&service,request,&result,{},nullptr,&error));
    QCOMPARE(error,QString("model_input_budget_exceeded"));QCOMPARE(reads,0);
  }
};

QTEST_GUILESS_MAIN(TestPinnedInference)
#include "tst_pinned_inference.moc"
