// 层：测试壳
#include <QtTest>
#include "app/agentproviderwiring.h"
#include "fixtures/documentsourcefixture.h"
#include "services/trackinginput.h"
#include <paleo_data/object_descriptor.h>

using namespace paleo;
namespace {
struct TraceFixture : document_fixture::Fixture {
  QString file;
  qlonglong volumeId = 0;
  bool initTrace() {
    file = temp.filePath("grid.sgy");
    if (!init() || !document_fixture::write(file, document_fixture::smallSegy()) || !importModel(file) || !bind()) return false;
    paleo::data::DataService service; std::string detail; std::vector<paleo::data::DataEntry> volumes;
    const auto source = session.sources().front();
    if (!service.openProject(source.modelDirectory.toStdString(), detail) ||
        !service.listData("volumes", 0, volumes, detail) || volumes.size() != 1) return false;
    volumeId = volumes[0].id; paleo::data::SeismicVolume volume;
    if (!service.readVolume(volumeId, volume, detail)) return false;
    volume.sampling.timeType = "TWT"; volume.sampling.timeReference = "explicit-fixture-datum";
    return service.saveVolume(volumeId, volume, detail);
  }
  tracking::ReadPlan plan() {
    tracking::ReadPlan plan; const auto source = session.sources().front();
    paleo::data::DataService service; std::string detail; paleo::data::DataObjectDescriptor object;
    if (!service.openProjectReadOnly(source.modelDirectory.toStdString(), detail) ||
        !service.describeDataObject(paleo::data::DataObjectKind::SeismicVolume, volumeId, object, detail)) return plan;
    document::InterpretationContext context;
    context.documentId = session.documentId(); context.modelStoreId = "area-model";
    context.purpose = document::RelationPurpose::WellTie;
    context.targetVolume = document::ModelObjectRef{source.ref, document::ObjectKind::SeismicVolume,
                                                   volumeId, QString::fromStdString(object.revision)};
    plan.interpretation.context = context; plan.interpretation.volume = source; return plan;
  }
};
}

class TestTrackingInput : public QObject {
  Q_OBJECT
private slots:
  void readsExactLineNumbersWithFrozenRevisionAndPhysicalAxis() {
    TraceFixture f; QVERIFY(f.initTrace()); const auto before = f.catalog.mutationSeq();
    const auto result = tracking::readWindow(f.plan(), 101, 200, 3, 2);
    QVERIFY2(result.ok, qPrintable(result.code + ":" + result.detail));
    QCOMPARE(result.traceOrdinal, qlonglong(2)); QCOMPARE(result.samples, QVector<float>({23, 24}));
    QCOMPARE(result.firstTimeMs, 6.0); QCOMPARE(result.sampleIntervalMs, 2.0);
    QCOMPARE(result.timeReference, QStringLiteral("explicit-fixture-datum"));
    QCOMPARE(f.catalog.mutationSeq(), before);
    QVERIFY(!tracking::readWindow(f.plan(), 102, 200, 3, 2).ok);
  }
  void rejectsBudgetCancellationAndChangedSourceWithoutSamples() {
    TraceFixture f; QVERIFY(f.initTrace()); const auto plan = f.plan();
    QCOMPARE(tracking::readWindow(plan, 101, 200, 0, 4097).code, QStringLiteral("tracking_window_budget"));
    QCOMPARE(tracking::readWindow(plan, 101, 200, 0, 2, [] { return true; }).code, QStringLiteral("cancelled"));
    QFile changed(f.file); QVERIFY(changed.open(QIODevice::Append)); QCOMPARE(changed.write("x"), qint64(1)); changed.close();
    const auto result = tracking::readWindow(plan, 101, 200, 0, 2);
    QVERIFY(!result.ok); QVERIFY(result.samples.isEmpty());
  }
  void productionProviderFreezesSelectionAndRejectsStalePresentation() {
    TraceFixture f; QVERIFY(f.initTrace()); const auto plan = f.plan();
    QVERIFY(f.session.selectInterpretation(*plan.interpretation.context, &f.error));
    const auto provider = trackingInputProvider(&f.session);
    AiChatToolRunner::TrackingInput input; QString error;
    QVERIFY2(provider({}, &input, &error), qPrintable(error)); QVERIFY(input.fetch); QVERIFY(input.ownerValidation);
    QVector<float> samples; QVERIFY2(input.fetch(101, 200, 3, 2, samples, error), qPrintable(error));
    QCOMPARE(samples, QVector<float>({23, 24})); QVERIFY(input.ownerValidation().isEmpty());
    QVERIFY(f.session.clearInterpretation(&f.error));
    QCOMPARE(input.ownerValidation(), QStringLiteral("tracking_binding_changed"));
    // A frozen value remains a read of its original input; it cannot enter the new selection's queue.
    QVERIFY(input.fetch(101, 200, 3, 2, samples, error));
  }
};

QTEST_GUILESS_MAIN(TestTrackingInput)
#include "tst_tracking_input.moc"
