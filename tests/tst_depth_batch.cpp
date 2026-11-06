// 层：测试壳
#include <QtTest/QtTest>

#include "algorithms/depthbatch.h"

#include <cmath>
#include <limits>

using paleo::depthbatch::BatchRequest;
using paleo::depthbatch::DepthSemantic;
using paleo::depthbatch::Extrapolation;
using paleo::depthbatch::IntervalRef;
using paleo::depthbatch::SampleRef;
using paleo::depthbatch::TimeSemantic;

namespace {

paleo::velmodel::VelocityModel fitOne(const QString &id, double t0, double z0, double t1, double z1) {
  paleo::velmodel::VelocityWellControl well;
  well.wellId = id;
  well.x = 0;
  well.y = 0;
  well.knots = {{t0, z0}, {t1, z1}};
  QString error;
  return paleo::velmodel::VelocityModel::fit({well}, paleo::velmodel::ModelType::IntervalAverage, &error);
}

BatchRequest baseRequest() {
  BatchRequest request;
  request.time = TimeSemantic::TwtMs;
  request.depth = DepthSemantic::TvdM;
  request.extrapolation = Extrapolation::RejectOutside;
  request.modelRevision = "rev-1";
  return request;
}

SampleRef sample(const char *id, const char *well, double time) {
  SampleRef row;
  row.sampleId = id;
  row.wellId = well;
  row.occurrenceId = "occ";
  row.time = time;
  return row;
}

} // namespace

class TestDepthBatch : public QObject {
  Q_OBJECT
private slots:
  void legacyMdFallbackIsNamedAndNotAConversion();
  void knotsMustIncrease();
  void matchesWellDepthOracle();
  void refusesOwtMdAndSurfaceMerge();
  void wellsAndOccurrencesStaySeparate();
  void qualityAndEmptyAndCancel();
};

void TestDepthBatch::legacyMdFallbackIsNamedAndNotAConversion() {
  QCOMPARE(paleo::depthbatch::legacyControlDepth(true, 10.0, true, 99.0), 10.0);
  QCOMPARE(paleo::depthbatch::legacyControlDepth(false, 0.0, true, 99.0), 99.0);
  QVERIFY(std::isnan(paleo::depthbatch::legacyControlDepth(false, 0.0, false, 0.0)));
  QCOMPARE(QString::fromLatin1(paleo::depthbatch::rasterExtrapolationPolicy()),
           QStringLiteral("spatial_idw_extended_v1"));
}

void TestDepthBatch::knotsMustIncrease() {
  QCOMPARE(QString::fromStdString(paleo::depthbatch::auditKnotOrder({{100, 200}, {200, 500}})), QString());
  QCOMPARE(QString::fromStdString(paleo::depthbatch::auditKnotOrder({{100, 200}, {100, 300}})),
           QStringLiteral("knots_not_strictly_increasing"));
  paleo::velmodel::VelocityWellControl well;
  well.wellId = QStringLiteral("W1");
  well.x = 0;
  well.y = 0;
  well.knots = {{100, 300}, {200, 250}};
  QString error;
  const auto model = paleo::velmodel::VelocityModel::fit({well}, paleo::velmodel::ModelType::IntervalAverage, &error);
  QVERIFY(!model.isValid());
  BatchRequest request = baseRequest();
  request.samples = {sample("a", "W1", 150)};
  const auto result = paleo::depthbatch::convertWellBatch(model, request);
  QCOMPARE(QString::fromStdString(result.code), QStringLiteral("model_invalid"));
}

void TestDepthBatch::matchesWellDepthOracle() {
  const auto model = fitOne(QStringLiteral("W1"), 100, 200, 200, 500);
  QVERIFY(model.isValid());
  const double oracle = model.wellDepthForTwt(model.wells().first(), 150);
  QCOMPARE(oracle, 350.0);
  BatchRequest request = baseRequest();
  request.samples = {sample("a", "W1", 150), sample("b", "W1", 50)};
  const auto result = paleo::depthbatch::convertWellBatch(model, request);
  QVERIFY(result.ok);
  QCOMPARE(result.samples[0].depthM, oracle);
  QVERIFY(!result.samples[1].valid);
  QCOMPARE(QString::fromStdString(result.samples[1].code), QStringLiteral("outside"));
  request.extrapolation = Extrapolation::IntervalExtendedV1;
  request.samples = {sample("c", "W1", 50)};
  const auto extended = paleo::depthbatch::convertWellBatch(model, request);
  QCOMPARE(extended.samples[0].depthM, model.wellDepthForTwtExtended(model.wells().first(), 50));
}

void TestDepthBatch::refusesOwtMdAndSurfaceMerge() {
  const auto model = fitOne(QStringLiteral("W1"), 100, 200, 200, 500);
  BatchRequest request = baseRequest();
  request.time = TimeSemantic::OwtMs;
  request.samples = {sample("a", "W1", 75)};
  QCOMPARE(QString::fromStdString(paleo::depthbatch::convertWellBatch(model, request).code),
           QStringLiteral("time_domain_unsupported"));
  request.owtPolicy = "owt_double_to_twt_v1";
  const auto doubled = paleo::depthbatch::convertWellBatch(model, request);
  QCOMPARE(doubled.samples[0].depthM, model.wellDepthForTwt(model.wells().first(), 150));
  request = baseRequest();
  request.depth = DepthSemantic::MdM;
  request.samples = {sample("a", "W1", 150)};
  QCOMPARE(QString::fromStdString(paleo::depthbatch::convertWellBatch(model, request).code),
           QStringLiteral("depth_domain_unsupported"));
  request = baseRequest();
  IntervalRef surface;
  surface.intervalId = "T70";
  surface.occurrenceId = "occ-1";
  surface.kind = "surface";
  request.intervals = {surface};
  QCOMPARE(QString::fromStdString(paleo::depthbatch::convertWellBatch(model, request).code),
           QStringLiteral("surface_is_not_interval"));
  request.intervals.clear();
  request.mergeDiscontinuous = true;
  QCOMPARE(QString::fromStdString(paleo::depthbatch::planIntervals(request).code),
           QStringLiteral("discontinuous_merge_refused"));
}

void TestDepthBatch::wellsAndOccurrencesStaySeparate() {
  paleo::velmodel::VelocityWellControl a;
  a.wellId = QStringLiteral("A");
  a.x = 0;
  a.y = 0;
  a.knots = {{100, 200}, {200, 400}};
  paleo::velmodel::VelocityWellControl b = a;
  b.wellId = QStringLiteral("B");
  b.knots = {{100, 1000}, {200, 2000}};
  QString error;
  const auto model = paleo::velmodel::VelocityModel::fit({a, b}, paleo::velmodel::ModelType::IntervalAverage, &error);
  BatchRequest request = baseRequest();
  IntervalRef first;
  first.intervalId = "iv-1";
  first.occurrenceId = "occ-1";
  first.kind = "interval";
  IntervalRef second = first;
  second.occurrenceId = "occ-2";
  request.intervals = {first, second};
  request.samples = {sample("s1", "A", 150)};
  request.samples[0].occurrenceId = "occ-1";
  const auto result = paleo::depthbatch::convertWellBatch(model, request);
  QCOMPARE(result.occurrenceIds.size(), 2);
  QCOMPARE(QString::fromStdString(result.occurrenceIds[0]), QStringLiteral("occ-1"));
  QCOMPARE(QString::fromStdString(result.occurrenceIds[1]), QStringLiteral("occ-2"));
  QCOMPARE(result.samples[0].depthM, model.wellDepthForTwt(model.wells().first(), 150));
  QVERIFY(result.samples[0].depthM < 1000.0);
  BatchRequest missing = baseRequest();
  missing.samples = {sample("s2", "C", 150)};
  QCOMPARE(QString::fromStdString(paleo::depthbatch::convertWellBatch(model, missing).samples[0].code),
           QStringLiteral("unknown_well"));
}

void TestDepthBatch::qualityAndEmptyAndCancel() {
  const auto model = fitOne(QStringLiteral("W1"), 100, 200, 200, 500);
  BatchRequest request = baseRequest();
  const auto empty = paleo::depthbatch::convertWellBatch(model, request);
  QVERIFY(empty.ok);
  QCOMPARE(QString::fromStdString(empty.code), QStringLiteral("empty_window"));
  SampleRef bad = sample("q", "W1", 150);
  bad.quality = std::numeric_limits<double>::quiet_NaN();
  request.samples = {bad};
  const auto nanQuality = paleo::depthbatch::convertWellBatch(model, request);
  QVERIFY(nanQuality.ok);
  QVERIFY(std::isnan(nanQuality.samples[0].depthM));
  QCOMPARE(QString::fromStdString(nanQuality.samples[0].code), QStringLiteral("nan_quality"));
  request.samples = {sample("a", "W1", 150), sample("b", "W1", 160)};
  int seen = 0;
  const auto cancelled = paleo::depthbatch::convertWellBatch(model, request, [&seen] {
    return ++seen > 0;
  });
  QCOMPARE(QString::fromStdString(cancelled.code), QStringLiteral("cancelled"));
  QVERIFY(cancelled.samples.empty());
}

QTEST_MAIN(TestDepthBatch)
#include "tst_depth_batch.moc"
