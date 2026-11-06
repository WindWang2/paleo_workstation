// 层：测试壳
#include <QtTest/QtTest>

#include "algorithms/seisattrgroup.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

bool same(float a, float b) {
  if (std::isnan(a) && std::isnan(b))
    return true;
  return a == b;
}

std::vector<float> ramp(int n) {
  std::vector<float> values(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
    values[static_cast<std::size_t>(i)] = std::sin(0.2 * i);
  return values;
}

} // namespace

class TestSeisAttrGroup : public QObject {
  Q_OBJECT
private slots:
  void envelopeAndPhaseShareOneAnalyticCall();
  void rmsHaloMatchesFullTrace();
  void shortEnvelopeIsRefused();
  void nanStaysNan();
  void coherenceTilesMatchFullVolume();
  void budgetLowerBoundIsIndependentOfOutputWindow();
  void mixedCoherenceIsPricedAndShapeCannotOverflow();
  void tiledCoherenceMatchesFullVolume();
};

void TestSeisAttrGroup::envelopeAndPhaseShareOneAnalyticCall() {
  const std::vector<float> trace = ramp(32);
  paleo::seisattr::TraceGroupRequest request;
  request.traces = trace.data();
  request.nTraces = 1;
  request.traceLength = 32;
  request.stride = 32;
  request.sampleIntervalMs = 4;
  request.wantEnvelope = true;
  request.wantPhase = true;
  request.wantRms = true;
  request.rmsHalf = 2;
  request.fullTrace = true;
  const auto grouped = paleo::seisattr::computeAttributeGroup(request);
  QVERIFY(grouped.ok);
  QCOMPARE(grouped.analyticEvaluations, 1);
  QCOMPARE(grouped.samplesRead, 32);
  const auto analytic = paleo::seisattr::complexTraceAnalysis(trace.data(), 32, 4);
  std::vector<float> rms(32);
  paleo::seisattr::windowedRms(trace.data(), 32, 2, rms.data());
  for (int i = 0; i < 32; ++i) {
    QVERIFY(same(grouped.envelope[static_cast<std::size_t>(i)], analytic.envelope[static_cast<std::size_t>(i)]));
    QVERIFY(same(grouped.phase[static_cast<std::size_t>(i)], analytic.phaseDeg[static_cast<std::size_t>(i)]));
    QVERIFY(same(grouped.rms[static_cast<std::size_t>(i)], rms[static_cast<std::size_t>(i)]));
  }
  std::vector<float> viaService(32);
  paleo::seisattr::computeTraceAttribute(paleo::seisattr::TraceAttribute::Envelope, trace.data(), 32, 2, 4,
                                        viaService.data());
  for (int i = 0; i < 32; ++i)
    QVERIFY(same(viaService[static_cast<std::size_t>(i)], analytic.envelope[static_cast<std::size_t>(i)]));
}

void TestSeisAttrGroup::rmsHaloMatchesFullTrace() {
  const std::vector<float> trace = ramp(40);
  std::vector<float> full(40);
  paleo::seisattr::windowedRms(trace.data(), 40, 3, full.data());
  std::vector<float> tile(10);
  QVERIFY(paleo::seisattr::rmsInteriorFromHalo(trace.data(), 40, 3, 10, 20, tile.data()));
  for (int i = 0; i < 10; ++i)
    QVERIFY(same(tile[static_cast<std::size_t>(i)], full[static_cast<std::size_t>(10 + i)]));
}

void TestSeisAttrGroup::shortEnvelopeIsRefused() {
  const std::vector<float> trace = ramp(16);
  paleo::seisattr::TraceGroupRequest request;
  request.traces = trace.data();
  request.nTraces = 1;
  request.traceLength = 16;
  request.stride = 16;
  request.sampleIntervalMs = 2;
  request.wantEnvelope = true;
  request.fullTrace = false;
  const auto refused = paleo::seisattr::computeAttributeGroup(request);
  QVERIFY(!refused.ok);
  QCOMPARE(QString::fromStdString(refused.code), QStringLiteral("whole_trace_required"));
  QCOMPARE(refused.analyticEvaluations, 0);
}

void TestSeisAttrGroup::nanStaysNan() {
  std::vector<float> trace = ramp(12);
  trace[5] = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> rms(12);
  paleo::seisattr::computeTraceAttribute(paleo::seisattr::TraceAttribute::Rms, trace.data(), 12, 1, 2, rms.data());
  QVERIFY(std::isnan(rms[5]));
  QVERIFY(std::isnan(rms[4]));
  QVERIFY(std::isnan(rms[6]));
  std::vector<float> envelope(12);
  paleo::seisattr::computeTraceAttribute(paleo::seisattr::TraceAttribute::Envelope, trace.data(), 12, 0, 2,
                                        envelope.data());
  QVERIFY(std::isnan(envelope[5]));
  QVERIFY(std::isfinite(envelope[1]));
}

void TestSeisAttrGroup::coherenceTilesMatchFullVolume() {
  constexpr int nIl = 5;
  constexpr int nXl = 7;
  constexpr int nS = 6;
  std::vector<float> volume(static_cast<std::size_t>(nIl * nXl * nS));
  for (int i = 0; i < nIl * nXl * nS; ++i)
    volume[static_cast<std::size_t>(i)] = std::sin(0.1 * i);
  volume[20] = std::numeric_limits<float>::quiet_NaN();
  paleo::seisattr::CoherenceTile full;
  full.volume = volume.data();
  full.nIl = nIl;
  full.nXl = nXl;
  full.nS = nS;
  full.ilHalf = 1;
  full.xlHalf = 1;
  full.timeHalf = 1;
  full.ilSpacing = 25;
  full.xlSpacing = 12.5;
  const auto reference = paleo::seisattr::coherenceInterior(full);
  QVERIFY(reference.ok);
  std::vector<float> tile(static_cast<std::size_t>(nIl * 4 * nS));
  for (int il = 0; il < nIl; ++il) {
    for (int xl = 0; xl < 4; ++xl) {
      for (int s = 0; s < nS; ++s)
        tile[static_cast<std::size_t>(((il * 4) + xl) * nS + s)] =
            volume[static_cast<std::size_t>(((il * nXl) + xl) * nS + s)];
    }
  }
  paleo::seisattr::CoherenceTile part = full;
  part.volume = tile.data();
  part.nXl = 4;
  const auto cropped = paleo::seisattr::coherenceInterior(part);
  QVERIFY(cropped.ok);
  QCOMPARE(cropped.samplesRead, nIl * 4 * nS);
  QVERIFY(cropped.samplesRead > cropped.interiorIl * cropped.interiorXl * nS);
  auto weighted = paleo::seisattr::coherenceInterior(full, paleo::seisattr::CoherenceWeightMode::InverseDistance);
  QVERIFY(weighted.ok);
  bool weightChangesValue = false;
  for (std::size_t i = 0; i < reference.interior.size() && i < weighted.interior.size(); ++i) {
    if (reference.interior[i] != weighted.interior[i])
      weightChangesValue = true;
  }
  QVERIFY(weightChangesValue);
  const int refXl = reference.interiorXl;
  for (int il = 0; il < cropped.interiorIl; ++il) {
    for (int xl = 0; xl < cropped.interiorXl; ++xl) {
      for (int s = 0; s < nS; ++s) {
        const float got = cropped.interior[static_cast<std::size_t>(((il * cropped.interiorXl) + xl) * nS + s)];
        const float expect = reference.interior[static_cast<std::size_t>(((il * refXl) + xl) * nS + s)];
        QVERIFY(same(got, expect));
      }
    }
  }
}

void TestSeisAttrGroup::budgetLowerBoundIsIndependentOfOutputWindow() {
  const int n = 128;
  const qint64 outputOnly = static_cast<qint64>(n) * static_cast<qint64>(sizeof(float));
  const auto tooSmall = paleo::seisattr::planAttributeBudget(4, n, 2, true, false, false, 1, 1, 0, 0, outputOnly);
  QCOMPARE(QString::fromStdString(tooSmall.code), QStringLiteral("budget_exceeded"));
  QVERIFY(tooSmall.minimumBytes > outputOnly);
  const auto room = paleo::seisattr::planAttributeBudget(4, n, 2, true, false, false, 1, 1, 0, 0, tooSmall.minimumBytes * 2);
  QVERIFY(room.code == "ok" || room.code == "split");
  QVERIFY(room.estimatedBytes <= tooSmall.minimumBytes * 2);
  QVERIFY(room.traceBatch >= 1);
  QVERIFY(room.traceBatch < 4);
  const int nXl = 64;
  const int nS = 32;
  const qint64 inlineBytes = static_cast<qint64>(nXl) * nS * static_cast<qint64>(sizeof(float));
  const auto split = paleo::seisattr::planAttributeBudget(0, nS, 0, false, false, true, 2, nXl, 1, 1, inlineBytes);
  QVERIFY(split.code == "split" || split.code == "budget_exceeded");
  QVERIFY(split.minimumBytes >= static_cast<qint64>(3 * 3) * nS * static_cast<qint64>(sizeof(float)));
  const auto impossible =
      paleo::seisattr::planAttributeBudget(0, nS, 0, false, false, true, 2, nXl, 1, 1, 64);
  QCOMPARE(QString::fromStdString(impossible.code), QStringLiteral("budget_exceeded"));

  const int traces = 16;
  const auto envelopeOnly =
      paleo::seisattr::planAttributeBudget(1, traces, 0, true, false, false, 0, 0, 0, 0, 1 << 20);
  QVERIFY(envelopeOnly.code == "ok" || envelopeOnly.code == "split");
  const auto mixed = paleo::seisattr::planAttributeBudget(1, traces, 0, true, false, true, 8, 8, 1, 1,
                                                         envelopeOnly.estimatedBytes, 64);
  QCOMPARE(QString::fromStdString(mixed.code), QStringLiteral("budget_exceeded"));
  const auto overflow = paleo::seisattr::planAttributeBudget(0, 1, 0, false, false, true, 2000000000, 2000000000, 0, 0,
                                                            1LL << 62, 2000000000);
  QCOMPARE(QString::fromStdString(overflow.code), QStringLiteral("budget_exceeded"));
}

void TestSeisAttrGroup::mixedCoherenceIsPricedAndShapeCannotOverflow() {
  float sample = 1.f;
  paleo::seisattr::CoherenceTile tile;
  tile.volume = &sample;
  tile.nIl = 2000000000;
  tile.nXl = 2000000000;
  tile.nS = 2000000000;
  const auto wrapped = paleo::seisattr::coherenceInterior(tile);
  QVERIFY(!wrapped.ok);
  QCOMPARE(QString::fromStdString(wrapped.code), QStringLiteral("invalid_shape"));
  QCOMPARE(wrapped.samplesRead, 0);

  tile.nIl = 100000;
  tile.nXl = 100000;
  tile.nS = 1000;
  int count = 1;
  QVERIFY(!paleo::seisattr::volumeIndexCount(tile.nIl, tile.nXl, tile.nS, &count));
  QCOMPARE(count, 1);
  const auto wide = paleo::seisattr::coherenceInterior(tile);
  QVERIFY(!wide.ok);
  QCOMPARE(QString::fromStdString(wide.code), QStringLiteral("invalid_shape"));
  QCOMPARE(wide.samplesRead, 0);
}

void TestSeisAttrGroup::tiledCoherenceMatchesFullVolume() {
  const int nIl = 5;
  const int nXl = 8;
  const int nS = 6;
  std::vector<float> volume(static_cast<std::size_t>(nIl * nXl * nS));
  for (int i = 0; i < nIl * nXl * nS; ++i)
    volume[static_cast<std::size_t>(i)] = std::sin(0.17 * i);
  paleo::seisattr::CoherenceTile tile;
  tile.volume = volume.data();
  tile.nIl = nIl;
  tile.nXl = nXl;
  tile.nS = nS;
  tile.ilHalf = 1;
  tile.xlHalf = 1;
  tile.timeHalf = 1;
  tile.ilSpacing = 25;
  tile.xlSpacing = 12.5;
  const auto full = paleo::seisattr::coherenceInterior(tile);
  const auto tiled = paleo::seisattr::computeCoherenceTiled(tile, paleo::seisattr::CoherenceWeightMode::Equal, 2);
  QVERIFY(full.ok);
  QVERIFY2(tiled.ok, tiled.code.c_str());
  QCOMPARE(tiled.interior.size(), full.interior.size());
  for (std::size_t i = 0; i < full.interior.size(); ++i)
    QVERIFY(same(full.interior[i], tiled.interior[i]));
  const auto split = paleo::seisattr::planAttributeBudget(0, nS, 0, false, false, true, nIl, nXl, 1, 1,
                                                         static_cast<qint64>(nIl * nXl * nS * sizeof(float)), nS);
  QVERIFY(split.code == "split" || split.code == "budget_exceeded");
}

QTEST_MAIN(TestSeisAttrGroup)
#include "tst_seisattr_group.moc"
