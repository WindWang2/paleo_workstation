// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>
#include <limits>
#include <cstring>

#include "domain/arearules.h"
#include "io/segyreader.h"
#include "algorithms/seismicattr.h"
#include "algorithms/inversion/bandlimit.h"

namespace
{
QString fixture(const QString &name)
{
  return QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
      QStringLiteral("fixtures/io_robustness/") + name);
}
bool write(const QString &path, const QByteArray &bytes)
{
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
void compareMissingAware(float a, float b)
{
  if (std::isnan(b))
    QVERIFY(std::isnan(a));
  else
    QCOMPARE(a, b);
}
}

class IoRobustnessTests : public QObject
{
  Q_OBJECT
private slots:
  void cleanup() { AreaRules::reset(); }
  void goodSamplesAreBitwiseUnchanged();
  void nonFiniteReadReportAndAttributes();
  void ibmOverflowIsNull();
  void readReportsDoNotAccumulate();
  void cancellationIsNotDecodeFailure();
  void truncatedFiles_data();
  void truncatedFiles();
  void decodedFailureIsReportedAfterFileChanges();
  void configuredHeaderWords();
  void changedHeaderWordsInvalidateCache();
  void exactSamplesSurviveMissingNeighbours();
};

void IoRobustnessTests::goodSamplesAreBitwiseUnchanged()
{
  SegyReader reader;
  QString error;
  QVERIFY2(reader.open(fixture(QStringLiteral("good.sgy")), &error), qPrintable(error));
  SegyReadReport report;
  const auto traces = reader.traces(nullptr, &report);
  QCOMPARE(traces.size(), 4);
  QCOMPARE(report.sanitizedSampleCount, 0);
  QCOMPARE(report.failedTraceOffsets.size(), 0);
  QVERIFY(report.message.isEmpty());
  for (int t = 0; t < 4; ++t)
    for (int s = 0; s < 16; ++s)
      QCOMPARE(traces[t].samples[s], float((t + 1) * 16 + s));
  QVERIFY(reader.open(fixture(QStringLiteral("finite_ieee_edges.sgy"))));
  const auto edges = reader.traces(nullptr, &report);
  QCOMPARE(report.sanitizedSampleCount, 0);
  const quint32 expected[] = {0x00000000, 0x80000000, 0x00000001, 0x80000001,
      0x00800000, 0x80800000, 0x7F7FFFFF, 0xFF7FFFFF,
      0x3F800000, 0xBF800000, 0x3DCCCCCD, 0xBDCCCCCD,
      0x3F000000, 0xBF000000, 0x41200000, 0xC1200000};
  QCOMPARE(edges.size(), 4);
  for (const auto &trace : edges)
    for (int sample = 0; sample < 16; ++sample)
    {
      quint32 actual;
      std::memcpy(&actual, &trace.samples[sample], sizeof(actual));
      QCOMPARE(actual, expected[sample]);
    }
}

void IoRobustnessTests::nonFiniteReadReportAndAttributes()
{
  SegyReader reader;
  QVERIFY(reader.open(fixture(QStringLiteral("nonfinite_ieee.sgy"))));
  SegyReadReport report;
  const auto traces = reader.traces(nullptr, &report);
  QCOMPARE(traces.size(), 4);
  QCOMPARE(report.sanitizedSampleCount, 4);
  QCOMPARE(report.sanitizedTraces.size(), 1);
  QCOMPARE(report.sanitizedTraces[0].traceOffset, 3600);
  QCOMPARE(report.sanitizedTraces[0].sampleIndices, QVector<int>({3, 5, 8, 10}));
  QVERIFY(!report.message.isEmpty());
  QVERIFY(reader.badTraceOffsets().isEmpty()); // 被保留的道不得混进索引跳过列表
  const auto &samples = traces[0].samples;
  QVector<float> manual(16);
  for (int s = 0; s < 16; ++s)
  {
    manual[s] = (s == 3 || s == 5 || s == 8 || s == 10)
                    ? std::numeric_limits<float>::quiet_NaN() : float(16 + s);
    compareMissingAware(samples[s], manual[s]);
    QVERIFY(!std::isinf(samples[s]));
  }
  QVector<float> rms(16), energy(16), maxAbs(16);
  paleo::seisattr::windowedRms(samples.constData(), 16, 1, rms.data());
  paleo::seisattr::windowedMeanEnergy(samples.constData(), 16, 1, energy.data());
  paleo::seisattr::windowedMaxAbs(samples.constData(), 16, 1, maxAbs.data());
  for (int s = 0; s < 16; ++s)
  {
    double sum = 0, max = 0;
    int count = 0;
    bool missing = false;
    for (int j = qMax(0, s - 1); j <= qMin(15, s + 1); ++j)
    {
      missing |= std::isnan(manual[j]);
      sum += double(manual[j]) * manual[j];
      max = std::max(max, std::fabs(double(manual[j])));
      ++count;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    compareMissingAware(rms[s], missing ? nan : float(std::sqrt(sum / count)));
    compareMissingAware(energy[s], missing ? nan : float(sum / count));
    compareMissingAware(maxAbs[s], missing ? nan : float(max));
  }
  // 手工缺失掩码对拍反演，不压缩时间轴、不补振幅。
  paleo::inversion::BandlimitedOptions options;
  options.amplitudeScale = 1000;
  const auto actual = paleo::inversion::bandlimitedInversion(samples.constData(), 16, 2, nullptr, nullptr, options);
  const auto expected = paleo::inversion::bandlimitedInversion(manual.constData(), 16, 2, nullptr, nullptr, options);
  QVERIFY2(actual.ok, actual.reason.c_str());
  QVERIFY(expected.ok);
  QCOMPARE(actual.impedance.size(), expected.impedance.size());
  for (size_t s = 0; s < actual.impedance.size(); ++s)
    compareMissingAware(actual.impedance[s], expected.impedance[s]);
}

void IoRobustnessTests::ibmOverflowIsNull()
{
  SegyReader reader;
  QVERIFY(reader.open(fixture(QStringLiteral("nonfinite_ibm.sgy"))));
  SegyReadReport report;
  const auto traces = reader.traces(nullptr, &report);
  QCOMPARE(report.sanitizedSampleCount, 1);
  QVERIFY(std::isnan(traces[0].samples[3]));
  for (int t = 0; t < 4; ++t)
    for (int s = 0; s < 16; ++s)
      if (t != 0 || s != 3)
        QCOMPARE(traces[t].samples[s], float((t + 1) * 16 + s));
  QVERIFY(reader.open(fixture(QStringLiteral("finite_ieee_edges.sgy"))));
  const auto edges = reader.traces(nullptr, &report);
  QCOMPARE(report.sanitizedSampleCount, 0);
  const quint32 expected[] = {0x00000000, 0x80000000, 0x00000001, 0x80000001,
      0x00800000, 0x80800000, 0x7F7FFFFF, 0xFF7FFFFF,
      0x3F800000, 0xBF800000, 0x3DCCCCCD, 0xBDCCCCCD,
      0x3F000000, 0xBF000000, 0x41200000, 0xC1200000};
  QCOMPARE(edges.size(), 4);
  for (const auto &trace : edges)
    for (int sample = 0; sample < 16; ++sample)
    {
      quint32 actual;
      std::memcpy(&actual, &trace.samples[sample], sizeof(actual));
      QCOMPARE(actual, expected[sample]);
    }
}

void IoRobustnessTests::readReportsDoNotAccumulate()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  SegyReader reader;
  QVERIFY(reader.openCached(fixture(QStringLiteral("nonfinite_ieee.sgy")), dir.path()));
  SegyReadReport report;
  QVector<SegyTrace> traces;
  for (int i = 0; i < 2; ++i)
  {
    QVERIFY(reader.readInline(100, &traces, nullptr, nullptr, &report));
    QCOMPARE(report.sanitizedSampleCount, 4);
    QCOMPARE(report.sanitizedTraces.size(), 1);
  }
  QVERIFY(reader.readInline(101, &traces, nullptr, nullptr, &report));
  QCOMPARE(report.sanitizedSampleCount, 0);
  QVERIFY(report.message.isEmpty());
  QVERIFY(reader.openCached(fixture(QStringLiteral("nonfinite_ieee.sgy")), dir.path()));
  QVERIFY(reader.readInline(100, &traces, nullptr, nullptr, &report));
  QCOMPARE(report.sanitizedSampleCount, 4);
}

void IoRobustnessTests::cancellationIsNotDecodeFailure()
{
  SegyReader reader;
  QVERIFY(reader.open(fixture(QStringLiteral("good.sgy"))));
  SegyOptions options;
  options.cancel = [] { return true; };
  SegyReadReport report;
  QVector<SegyTrace> traces;
  QString error;
  QVERIFY(!reader.readInline(100, &traces, &error, &options, &report));
  QVERIFY(report.cancelled);
  QVERIFY(report.failedTraceOffsets.isEmpty());
  QCOMPARE(report.requestedTraceCount, 2);
  QCOMPARE(report.decodedTraceCount, 0);
  QVERIFY(error.contains(QStringLiteral("取消")));
  options.cancel = [] { return false; };
  QVERIFY(reader.readInline(100, &traces, &error, &options, &report));
  QVERIFY(!report.cancelled);
  QVERIFY(report.message.isEmpty());
}

void IoRobustnessTests::truncatedFiles_data()
{
  QTest::addColumn<QString>("name");
  for (const char *name : {"truncated_binary_header.sgy", "truncated_trace_header.sgy", "truncated_samples.sgy", "truncated_bad_ns.sgy"})
    QTest::newRow(name) << QString::fromLatin1(name);
}
void IoRobustnessTests::truncatedFiles()
{
  QFETCH(QString, name);
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  for (bool cached : {false, true})
  {
    SegyReader reader;
    QString error;
    const bool ok = cached ? reader.openCached(fixture(name), dir.path(), &error)
                           : reader.open(fixture(name), &error);
    QVERIFY2(!ok, qPrintable(name));
    QVERIFY(!error.isEmpty());
    QCOMPARE(reader.traceCount(), 0);
  }
}

void IoRobustnessTests::decodedFailureIsReportedAfterFileChanges()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QFile f(fixture(QStringLiteral("good.sgy")));
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QString path = dir.filePath(QStringLiteral("changed.sgy"));
  QVERIFY(write(path, f.readAll()));
  SegyReader reader;
  QVERIFY(reader.open(path));
  QFile changed(path);
  QVERIFY(changed.open(QIODevice::ReadWrite));
  QVERIFY(changed.resize(3600 + 3 * 304 + 240 + 63));
  changed.close();
  SegyReadReport report;
  const auto traces = reader.traces(nullptr, &report);
  QCOMPARE(traces.size(), 3);
  QCOMPARE(report.failedTraceOffsets, QVector<qint64>({3600 + 3 * 304}));
  QVERIFY(!report.message.isEmpty());
}

void IoRobustnessTests::configuredHeaderWords()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  for (const auto &pair : {QPair<QString, QByteArray>{QStringLiteral("custom_words.sgy"), "{\"segy_indexing\":{\"inline_word_offset\":180,\"crossline_word_offset\":184}}"},
                          QPair<QString, QByteArray>{QStringLiteral("swapped_words.sgy"), "{\"segy_indexing\":{\"inline_word_offset\":192,\"crossline_word_offset\":188}}"}})
  {
    QVERIFY(write(dir.filePath(QStringLiteral("project_area.json")), pair.second));
    AreaRules::setProjectDir(dir.path());
    QVERIFY(AreaRules::lastError().isEmpty());
    SegyReader reader;
    QVERIFY(reader.open(fixture(pair.first)));
    QCOMPARE(reader.inlineNumbers(), QVector<qint32>({100, 101}));
    QCOMPARE(reader.crosslineNumbers(), QVector<qint32>({200, 201}));
  }
}

void IoRobustnessTests::changedHeaderWordsInvalidateCache()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString cache = dir.filePath(QStringLiteral("cache"));
  const QString path = fixture(QStringLiteral("swapped_words.sgy"));
  SegyReader before;
  QVERIFY(before.openCached(path, cache));
  QCOMPARE(before.inlineNumbers(), QVector<qint32>({200, 201}));
  QVERIFY(write(dir.filePath(QStringLiteral("project_area.json")),
                "{\"segy_indexing\":{\"inline_word_offset\":192,\"crossline_word_offset\":188}}"));
  AreaRules::setProjectDir(dir.path());
  QVERIFY(AreaRules::lastError().isEmpty());
  SegyReader after;
  QVERIFY(after.openCached(path, cache));
  QCOMPARE(after.inlineNumbers(), QVector<qint32>({100, 101}));
  QCOMPARE(after.crosslineNumbers(), QVector<qint32>({200, 201}));
}

void IoRobustnessTests::exactSamplesSurviveMissingNeighbours()
{
  SegyTrace trace;
  trace.samples = {1.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f};
  trace.sampleIntervalUs = 2000;
  trace.startTimeMs = 0;
  SegySectionGrid grid;
  grid.startMs = 0; grid.stepMs = 1; grid.rows = 5;
  float value = 0;
  QVERIFY(grid.sampleAt(trace, 0, 2000, 0, &value));
  QCOMPARE(value, 1.0f);
  QVERIFY(!grid.sampleAt(trace, 1, 2000, 0, &value));
  QVERIFY(!grid.sampleAt(trace, 2, 2000, 0, &value));
  QVERIFY(grid.sampleAt(trace, 4, 2000, 0, &value));
  QCOMPARE(value, 3.0f);
}

QTEST_GUILESS_MAIN(IoRobustnessTests)
#include "tst_io_robustness.moc"
