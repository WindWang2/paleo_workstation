#include <QtTest>

#include "algorithms/seisattrgroup.h"
#include "fixtures/documentsourcefixture.h"
#include "services/capabilityseismicinput.h"
#include <paleo_data/object_descriptor.h>

using namespace paleo::capability;

namespace {
struct TraceFixture : document_fixture::Fixture {
  ResolvedSeismicTraceInput input;
  QString file;

  bool open() {
    file = temp.filePath(QStringLiteral("input.sgy"));
    if (!init() || !document_fixture::write(file, document_fixture::smallSegy()) || !importModel(file) || !bind())
      return false;
    const auto records = session.sources();
    if (records.size() != 1)
      return false;
    input.source = records.front();
    paleo::data::DataService service;
    std::vector<paleo::data::DataEntry> volumes;
    std::string detail;
    if (!service.openProjectReadOnly(input.source.modelDirectory.toUtf8().toStdString(), detail) ||
        !service.listData("volumes", 0, volumes, detail) || volumes.size() != 1) {
      error = QString::fromStdString(detail);
      return false;
    }
    paleo::data::DataObjectDescriptor object;
    if (!service.describeDataObject(paleo::data::DataObjectKind::SeismicVolume, volumes.front().id, object, detail) ||
        !object.revisionKnown) {
      error = QString::fromStdString(detail);
      return false;
    }
    input.selection = {{input.source.ref, paleo::document::ObjectKind::SeismicVolume, object.id,
                        QString::fromStdString(object.revision)}, 2, 3, 2};
    return true;
  }
};

paleo::seisattr::TraceGroupResult group(const SeismicTraceInputPlan &plan,
                                        const std::vector<float> &samples, bool analytic) {
  paleo::seisattr::TraceGroupRequest request;
  request.traces = samples.data();
  request.nTraces = 1;
  request.traceLength = plan.readSampleCount;
  request.stride = plan.readSampleCount;
  request.sampleIntervalMs = plan.sampleIntervalMs;
  request.rmsHalf = 1;
  request.outputBegin = plan.outputBegin;
  request.outputEnd = plan.outputEnd;
  request.wantRms = true;
  request.wantEnvelope = analytic;
  request.wantPhase = analytic;
  request.fullTrace = plan.fullTrace;
  return paleo::seisattr::computeAttributeGroup(request);
}
} // namespace

class TestCapabilitySeismicInput : public QObject {
  Q_OBJECT

private slots:
  void planningUsesOnlyMetadataAndChecksBounds() {
    TraceFixture fixture;
    QVERIFY2(fixture.open(), qPrintable(fixture.error));
    const auto &source = fixture.input.source;
    auto selection = fixture.input.selection;

    // An unavailable raw file cannot affect metadata-only planning.
    QVERIFY(QFile::rename(fixture.file, fixture.file + QStringLiteral(".offline")));
    const auto plan = planSeismicTraceInput(source, selection, 1, false, 4);
    QVERIFY2(plan.ok, qPrintable(plan.code));
    QCOMPARE(plan.firstReadSample, 2);
    QCOMPARE(plan.readSampleCount, 4);
    QCOMPARE(plan.outputBegin, 1);
    QCOMPARE(plan.outputEnd, 3);
    QCOMPARE(plan.firstReadTimeMs, 4.0);
    QCOMPARE(plan.sampleIntervalMs, 2.0);
    QVERIFY(!plan.fullTrace);
    QCOMPARE(planSeismicTraceInput(source, selection, 1, false, 3).code, QStringLiteral("budget_exceeded"));
    QCOMPARE(planSeismicTraceInput(source, selection, 1, true, 7).code, QStringLiteral("budget_exceeded"));
    std::vector<float> samples{42};
    QCOMPARE(readSeismicTraceInput(source, selection, plan, samples), QStringLiteral("source_revision_changed"));
    QVERIFY(samples.empty());

    selection.sampleCount = 6;
    QCOMPARE(planSeismicTraceInput(source, selection, 1, false, 8).code, QStringLiteral("seismic_window_out_of_range"));
    selection = fixture.input.selection;
    selection.volume.revision += QStringLiteral("-obsolete");
    QCOMPARE(planSeismicTraceInput(source, selection, 1, false, 8).code, QStringLiteral("object_revision_changed"));
  }

  void finiteHaloAndWholeTraceHaveTheSameRmsWindow() {
    TraceFixture fixture;
    QVERIFY2(fixture.open(), qPrintable(fixture.error));
    const auto &source = fixture.input.source;
    auto selection = fixture.input.selection;
    const auto halo = planSeismicTraceInput(source, selection, 1, false, 8);
    const auto whole = planSeismicTraceInput(source, selection, 1, true, 8);
    QVERIFY2(halo.ok, qPrintable(halo.code));
    QVERIFY2(whole.ok, qPrintable(whole.code));
    QVERIFY(whole.fullTrace);
    QCOMPARE(whole.firstReadSample, 0);
    QCOMPARE(whole.readSampleCount, 8);
    QCOMPARE(whole.outputBegin, 3);
    QCOMPARE(whole.outputEnd, 5);
    std::vector<float> haloSamples, wholeSamples;
    QString error = readSeismicTraceInput(source, selection, halo, haloSamples);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    error = readSeismicTraceInput(source, selection, whole, wholeSamples);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(haloSamples, (std::vector<float>{22, 23, 24, 25}));
    QCOMPARE(wholeSamples, (std::vector<float>{20, 21, 22, 23, 24, 25, 26, 27}));
    const auto cropped = group(halo, haloSamples, false);
    const auto analytic = group(whole, wholeSamples, true);
    QVERIFY(cropped.ok);
    QVERIFY(analytic.ok);
    QCOMPARE(analytic.analyticEvaluations, 1);
    QCOMPARE(analytic.envelope.size(), std::size_t(2));
    QCOMPARE(analytic.phase.size(), std::size_t(2));
    QCOMPARE(cropped.rms, analytic.rms);

    selection.firstSample = 7;
    selection.sampleCount = 1;
    const auto edge = planSeismicTraceInput(source, selection, 1, false, 2);
    QVERIFY2(edge.ok, qPrintable(edge.code));
    QCOMPARE(edge.firstReadSample, 6);
    QCOMPARE(edge.readSampleCount, 2);
    QCOMPARE(edge.outputBegin, 1);
    error = readSeismicTraceInput(source, selection, edge, haloSamples);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(haloSamples, (std::vector<float>{26, 27}));
  }

  void readRechecksLiveRevisionAndHonorsCancellation() {
    TraceFixture fixture;
    QVERIFY2(fixture.open(), qPrintable(fixture.error));
    const auto &source = fixture.input.source;
    const auto &selection = fixture.input.selection;
    const auto plan = planSeismicTraceInput(source, selection, 1, true, 8);
    QVERIFY2(plan.ok, qPrintable(plan.code));

    std::vector<float> samples{42};
    QCOMPARE(readSeismicTraceInput(source, selection, plan, samples, [] { return true; }), QStringLiteral("cancelled"));
    QVERIFY(samples.empty());
    paleo::data::DataService service;
    paleo::data::SeismicVolume volume;
    std::string error;
    QVERIFY2(service.openProject(source.modelDirectory.toUtf8().toStdString(), error), error.c_str());
    QVERIFY2(service.readVolume(selection.volume.id, volume, error), error.c_str());
    volume.volumeName += " updated";
    QVERIFY2(service.saveVolume(selection.volume.id, volume, error), error.c_str());
    QCOMPARE(readSeismicTraceInput(source, selection, plan, samples), QStringLiteral("object_revision_changed"));
    QVERIFY(samples.empty());
  }
};

QTEST_GUILESS_MAIN(TestCapabilitySeismicInput)
#include "tst_capability_seismic_input.moc"
