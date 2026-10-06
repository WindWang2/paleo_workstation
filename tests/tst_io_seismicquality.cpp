// 层：测试壳
#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <algorithm>
#include <filesystem>

#include "Engine/Sdk.h"
#include "Engine/TranscodeJob.h"
#include "Engine/PagedPipeline.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include "algorithms/seismicattr.h"

namespace
{
QString fixture(const QString &name)
{
  return QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
      QStringLiteral("fixtures/io_robustness/") + name);
}
std::filesystem::path path(const QString &s)
{
#ifdef _WIN32
  return std::filesystem::path(s.toStdWString());
#else
  return std::filesystem::path(s.toStdString());
#endif
}
void compareTrace(const seismic::engine::TraceData &trace)
{
  QCOMPARE(trace.samples.size(), size_t(16));
  for (int i = 0; i < 16; ++i)
    if (i == 3 || i == 5 || i == 8 || i == 10)
      QVERIFY(std::isnan(trace.samples[i]));
    else
      QCOMPARE(trace.samples[i], float(16 + i));
}
}
class IoSeismicQualityTests : public QObject
{
  Q_OBJECT
private slots:
  void directSdkAndAttributeConsumption();
  void cancelledRequestsKeepDecodedQualityCounts();
  void transcodeReportsAndPayloads();
  void serviceReportIsVisibleAndPersistable();
};
void IoSeismicQualityTests::directSdkAndAttributeConsumption()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString source = dir.filePath(QStringLiteral("source.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("nonfinite_ieee.sgy")), source));
  seismic::sdk::OpenOptions options;
  options.backend = seismic::sdk::Backend::Direct;
  seismic::engine::Status status;
  auto dataset = seismic::sdk::Dataset::Open(path(source), options, status);
  QVERIFY2(dataset, status.message.c_str());
  seismic::engine::TraceData trace;
  QVERIFY(dataset->ReadTrace(100, 200, trace).ok());
  compareTrace(trace);
  QCOMPARE(dataset->Statistics().sanitizedSampleReads, std::uint64_t(4));
  seismic::engine::VoxelWindowRequest request;
  request.inlineBegin = 100; request.xlineBegin = 200;
  request.inlineCount = 2; request.xlineCount = 2; request.sampleCount = 16;
  seismic::engine::VoxelWindow window;
  QVERIFY(dataset->ReadVoxelWindow(request, window).ok());
  QCOMPARE(window.values.size(), size_t(64));
  QCOMPARE(dataset->Statistics().sanitizedSampleReads, std::uint64_t(8));
  // 实际属性任务使用 SDK voxel 读面；有限窗口与手工去除缺失的直算一致。
  float rms[16];
  paleo::seisattr::windowedRms(window.values.data(), 16, 0, rms);
  for (int i = 0; i < 16; ++i)
    if (i == 3 || i == 5 || i == 8 || i == 10) QVERIFY(std::isnan(rms[i]));
    else QCOMPARE(rms[i], float(16 + i));
  seismic::engine::Slice2D slice;
  QVERIFY(dataset->ReadInline(100, slice).ok());
  QCOMPARE(dataset->Statistics().sanitizedSampleReads, std::uint64_t(12));
  for (float value : slice.values) QVERIFY(!std::isinf(value));
  QVERIFY(dataset->ReadTimeSlice(8, slice).ok());
  QCOMPARE(dataset->Statistics().sanitizedSampleReads, std::uint64_t(13));
  QCOMPARE(int(std::count_if(slice.values.begin(), slice.values.end(), [](float v) { return std::isnan(v); })), 1);
  QVERIFY(std::isfinite(slice.valueMin) && std::isfinite(slice.valueMax));
  seismic::engine::SectionRequest section;
  section.pathPoints = {{100, 200}, {100, 201}};
  section.interpolate = false;
  section.useReadPlan = true;
  section.maxColumns = 2;
  QVERIFY(dataset->ReadSection(section, slice).ok());
  QCOMPARE(dataset->Statistics().sanitizedSampleReads, std::uint64_t(17));
  for (float value : slice.values) QVERIFY(!std::isinf(value));
}
void IoSeismicQualityTests::cancelledRequestsKeepDecodedQualityCounts()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString source = dir.filePath(QStringLiteral("source.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("nonfinite_ieee.sgy")), source));
  for (bool planned : {false, true})
  {
    seismic::sdk::OpenOptions options;
    options.backend = seismic::sdk::Backend::Direct;
    seismic::engine::Status status;
    auto dataset = seismic::sdk::Dataset::Open(path(source), options, status);
    QVERIFY2(dataset, status.message.c_str());
    seismic::engine::CancelToken cancel;
    int polls = 0;
    // 在第一道已解码、第二道尚未解码时协作取消；没有线程/墙钟竞态。
    cancel.SetPredicate([&] { return ++polls >= (planned ? 3 : 2); });
    if (planned)
    {
      seismic::engine::SectionRequest request;
      request.pathPoints = {{100, 200}, {100, 201}};
      request.maxColumns = 2;
      request.useReadPlan = true;
      seismic::engine::Slice2D result;
      status = dataset->ReadSection(request, result, &cancel);
      QVERIFY(result.values.empty());
    }
    else
    {
      seismic::engine::VoxelWindowRequest request;
      request.inlineBegin = 100; request.xlineBegin = 200;
      request.inlineCount = 2; request.xlineCount = 2; request.sampleCount = 16;
      seismic::engine::VoxelWindow result;
      status = dataset->ReadVoxelWindow(request, result, &cancel);
      QVERIFY(result.values.empty());
    }
    QCOMPARE(status.code, seismic::engine::StatusCode::Cancelled);
    QCOMPARE(dataset->Statistics().sanitizedSampleReads, std::uint64_t(4));
    QCOMPARE(dataset->Statistics().cancelledRequests, std::uint64_t(1));
  }
}
void IoSeismicQualityTests::transcodeReportsAndPayloads()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString source = dir.filePath(QStringLiteral("source.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("nonfinite_ieee.sgy")), source));
  for (bool multiPass : {false, true})
  {
    const QString base = dir.filePath(multiPass ? QStringLiteral("multi") : QStringLiteral("single"));
    seismic::engine::TranscodeOptions options;
    options.chunkInlines = 2; options.chunkXlines = 2;
    options.chunkSamples = multiPass ? 4 : 16;
    const auto result = seismic::engine::TranscodeSegyToWorkspace(path(source), path(base), options, nullptr);
    QVERIFY2(result.status.ok(), result.status.message.c_str());
    QCOMPARE(result.sanitizedSampleCount, std::uint64_t(4));
    QCOMPARE(result.sanitizedTraceCount, std::uint64_t(1));
    QCOMPARE(result.valueMin, 16.0f);
    QCOMPARE(result.valueMax, 79.0f);
    seismic::sdk::OpenOptions open;
    open.backend = seismic::sdk::Backend::Workspace;
    seismic::engine::Status status;
    auto dataset = seismic::sdk::Dataset::Open(path(base), open, status);
    QVERIFY2(dataset, status.message.c_str());
    seismic::engine::TraceData trace;
    QVERIFY(dataset->ReadTrace(100, 200, trace).ok());
    compareTrace(trace);
  }
  seismic::engine::PagedPipelineOptions options;
  options.buildLod1 = options.buildLod2 = false;
  const QString base = dir.filePath(QStringLiteral("paged.sf3p"));
  const auto result = seismic::engine::TranscodeSegyToPagedWorkspace(path(source), path(base), options, nullptr);
  QVERIFY2(result.status.ok(), result.status.message.c_str());
  QCOMPARE(result.sanitizedSampleCount, std::uint64_t(4));
  QCOMPARE(result.sanitizedTraceCount, std::uint64_t(1));
  QCOMPARE(result.valueMin, 16.0f); QCOMPARE(result.valueMax, 79.0f);
  seismic::sdk::OpenOptions open;
  open.backend = seismic::sdk::Backend::Paged;
  seismic::engine::Status status;
  auto dataset = seismic::sdk::Dataset::Open(path(base), open, status);
  QVERIFY2(dataset, status.message.c_str());
  seismic::engine::TraceData trace;
  QVERIFY(dataset->ReadTrace(100, 200, trace).ok());
  compareTrace(trace);
}
void IoSeismicQualityTests::serviceReportIsVisibleAndPersistable()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString source = dir.filePath(QStringLiteral("source.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("nonfinite_ieee.sgy")), source));
  PaleoTaskService tasks;
  seismic::SeismicTaskService service(&tasks, 16);
  bool finished = false, reported = false;
  seismic::SeismicTranscodeReport report;
  auto *task = service.startWorkspaceTranscodeDetailed(source, QString(),
      [&](bool ok, const QString &, const QString &) { finished = ok; },
      [&](const auto &result) { report = result; reported = true; });
  QVERIFY(task);
  QTRY_VERIFY_WITH_TIMEOUT(finished && reported, 30000);
  QCOMPARE(report.sanitizedSamples, qint64(4));
  QCOMPARE(report.sanitizedTraces, qint64(1));
  QVERIFY(report.summaryLine().contains(QStringLiteral("清洗 4")));
  const auto json = QJsonDocument::fromJson(report.toJsonLine().mid(QStringLiteral("PALEO-SEISMIC-TRANSCODE ").size()).toUtf8()).object();
  QCOMPARE(json.value(QStringLiteral("sanitizedSamples")).toInt(), 4);
  QCOMPARE(json.value(QStringLiteral("sanitizedTraces")).toInt(), 1);
  QVERIFY(report.validValues);
  QCOMPARE(report.valueMin, 16.0f); QCOMPARE(report.valueMax, 79.0f);
}
QTEST_GUILESS_MAIN(IoSeismicQualityTests)
#include "tst_io_seismicquality.moc"
