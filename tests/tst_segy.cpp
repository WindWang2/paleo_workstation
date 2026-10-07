#include <QCoreApplication>
#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>

#include "../src/io/perffixtures.h"
#include "../src/io/segyreader.h"

// ---------------------------------------------------------------------------
// Synthetic SEG-Y generator for testing
// Supports format 1 (IBM fp32) & format 5 (IEEE fp32), rev0 & rev1,
// extended text headers, and custom trace headers / sample arrays.
// ---------------------------------------------------------------------------
namespace
{
quint32 ieeeToIbm(float f)
{
  if (f == 0.0f)
    return 0;

  const quint32 sign = (f < 0.0f) ? 0x80000000u : 0u;
  const double d = std::fabs(static_cast<double>(f));

  int exp16 = static_cast<int>(std::floor(std::log2(d) / 4.0)) + 1;
  double f_norm = d / std::pow(16.0, exp16);
  while (f_norm >= 1.0)
  {
    f_norm /= 16.0;
    exp16++;
  }
  while (f_norm < 1.0 / 16.0 && exp16 > -64)
  {
    f_norm *= 16.0;
    exp16--;
  }

  int ibmExp = exp16 + 64;
  if (ibmExp < 0)
    return sign;
  if (ibmExp > 127)
    ibmExp = 127;

  quint32 mantissa = static_cast<quint32>(std::round(f_norm * 16777216.0));
  if (mantissa > 0x00ffffffu)
  {
    mantissa >>= 4;
    ibmExp++;
  }
  return sign | (static_cast<quint32>(ibmExp) << 24) | (mantissa & 0x00ffffffu);
}

struct SyntheticSegyConfig
{
  int formatCode = 5;      // 1 = IBM fp32, 5 = IEEE fp32
  int revNum = 0x0100;     // 0x0000 = rev0, 0x0100 = rev1
  int extHeaders = 0;      // count of 3200-byte extended text headers
  bool endText = true;     // final record marker when extHeaders is -1
  qint32 binLineNo = 1001; // binary header line number
  qint16 binDt = 2000;     // sample interval in microseconds (2000 us)
  qint16 binNs = 64;       // samples per trace
  int traceCount = 3;      // number of traces

  struct TraceData
  {
    qint32 tracl = 0;       // bytes 0-3 (0 = auto i+1)
    qint32 cdp = 0;         // bytes 20-23 (0 = auto 100+i)
    qint32 lineNo = 0;      // bytes 188-191 (0 = fallback to binLineNo)
    qint16 ns = 0;          // bytes 114-115 (0 = fallback to binNs)
    qint16 dt = 0;          // bytes 116-117 (0 = fallback to binDt)
    QVector<float> samples; // sample values
    qint32 cdpX = 0;        // bytes 180-183（CDP X；0 保持全零布局；末位避免
    qint32 cdpY = 0;        // bytes 184-187（CDP Y；破坏既有按位聚合初始化）
  };

  QVector<TraceData> customTraces;
};

QByteArray buildSyntheticSegy(const SyntheticSegyConfig &cfg)
{
  QByteArray out;

  // 1. Textual header: 3200 bytes
  QByteArray textHdr(3200, ' ');
  const char *banner = "C01 SYNTHETIC SEG-Y TEST FILE GENERATED FOR TDD";
  std::memcpy(textHdr.data(), banner, std::strlen(banner));
  out.append(textHdr);

  // 2. Binary header: 400 bytes (starting at byte 3200)
  QByteArray binHdr(400, 0);
  qToBigEndian<qint32>(cfg.binLineNo, reinterpret_cast<uchar *>(binHdr.data() + 4));
  qToBigEndian<qint16>(cfg.binDt, reinterpret_cast<uchar *>(binHdr.data() + 16));
  qToBigEndian<qint16>(cfg.binNs, reinterpret_cast<uchar *>(binHdr.data() + 20));
  qToBigEndian<qint16>(static_cast<qint16>(cfg.formatCode), reinterpret_cast<uchar *>(binHdr.data() + 24));
  qToBigEndian<qint16>(static_cast<qint16>(cfg.revNum), reinterpret_cast<uchar *>(binHdr.data() + 300));
  qToBigEndian<qint16>(static_cast<qint16>(cfg.extHeaders), reinterpret_cast<uchar *>(binHdr.data() + 304));
  out.append(binHdr);

  // 3. Extended text headers (if any)
  for (int e = 0; e < (cfg.extHeaders == -1 ? 2 : cfg.extHeaders); ++e)
  {
    QByteArray extHdr(3200, ' ');
    const char *extBanner = "C01 EXTENDED TEXT HEADER";
    std::memcpy(extHdr.data(), extBanner, std::strlen(extBanner));
    if (cfg.extHeaders == -1 && e == 1 && cfg.endText)
      std::memcpy(extHdr.data() + 80, "((SEG: EndText))", 16);
    out.append(extHdr);
  }

  // 4. Traces
  for (int i = 0; i < cfg.traceCount; ++i)
  {
    QByteArray trHdr(240, 0);
    const qint32 tracl = (i < cfg.customTraces.size() && cfg.customTraces[i].tracl != 0)
                             ? cfg.customTraces[i].tracl
                             : static_cast<qint32>(i + 1);
    const qint32 cdp = (i < cfg.customTraces.size() && cfg.customTraces[i].cdp != 0)
                           ? cfg.customTraces[i].cdp
                           : static_cast<qint32>(100 + i);
    const qint32 lineNo = (i < cfg.customTraces.size()) ? cfg.customTraces[i].lineNo : 0;
    const qint16 ns = (i < cfg.customTraces.size() && cfg.customTraces[i].ns != 0)
                          ? cfg.customTraces[i].ns
                          : cfg.binNs;
    const qint16 dt = (i < cfg.customTraces.size() && cfg.customTraces[i].dt != 0)
                          ? cfg.customTraces[i].dt
                          : cfg.binDt;

    qToBigEndian<qint32>(tracl, reinterpret_cast<uchar *>(trHdr.data() + 0));
    qToBigEndian<qint32>(cdp, reinterpret_cast<uchar *>(trHdr.data() + 20));
    qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(trHdr.data() + 114));
    qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(trHdr.data() + 116));
    qToBigEndian<qint32>(lineNo, reinterpret_cast<uchar *>(trHdr.data() + 188));
    if (i < cfg.customTraces.size())
    {
      // demo 工区方言布局：坐标只出现在 CDP X/Y（181-188），源点 73-78 与
      // crossline 193-196 保持全零，crossline 号走 CDP（21）。
      qToBigEndian<qint32>(cfg.customTraces[i].cdpX, reinterpret_cast<uchar *>(trHdr.data() + 180));
      qToBigEndian<qint32>(cfg.customTraces[i].cdpY, reinterpret_cast<uchar *>(trHdr.data() + 184));
    }
    out.append(trHdr);

    const int sampleCount = (ns > 0) ? ns : cfg.binNs;
    QVector<float> sampleVals;
    if (i < cfg.customTraces.size() && !cfg.customTraces[i].samples.isEmpty())
    {
      sampleVals = cfg.customTraces[i].samples;
    }
    else
    {
      sampleVals.resize(sampleCount);
      for (int s = 0; s < sampleCount; ++s)
        sampleVals[s] = 1.0f + 0.1f * static_cast<float>(i + 1) + 0.01f * static_cast<float>(s);
    }

    for (int s = 0; s < sampleCount; ++s)
    {
      float val = (s < sampleVals.size()) ? sampleVals[s] : 0.0f;
      if (cfg.formatCode == 5)
      {
        quint32 raw;
        std::memcpy(&raw, &val, sizeof(float));
        quint32 be = qToBigEndian<quint32>(raw);
        out.append(reinterpret_cast<const char *>(&be), sizeof(be));
      }
      else if (cfg.formatCode == 1)
      {
        quint32 ibm = ieeeToIbm(val);
        quint32 be = qToBigEndian<quint32>(ibm);
        out.append(reinterpret_cast<const char *>(&be), sizeof(be));
      }
    }
  }

  return out;
}

bool writeSegyFile(const QString &filePath, const QByteArray &bytes)
{
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  return file.write(bytes) == bytes.size();
}
} // namespace

class TestSegy : public QObject
{
  Q_OBJECT

  private slots:
    // a) Header fields: traceCount=3, samplesPerTrace=64, sampleIntervalUs=2000.0f, rev1, IEEE fp32
    void headerFieldsIeeeRev1()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("header_ieee.sgy"));

      SyntheticSegyConfig cfg;
      cfg.formatCode = 5;
      cfg.revNum = 0x0100;
      cfg.binLineNo = 1001;
      cfg.binDt = 2000;
      cfg.binNs = 64;
      cfg.traceCount = 3;

      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));

      SegyReader reader;
      QString error;
      QVERIFY2(reader.open(path, &error), qPrintable(error));
      QCOMPARE(reader.traceCount(), 3);
      QCOMPARE(reader.samplesPerTrace(), 64);
      QCOMPARE(reader.sampleIntervalUs(), 2000.0f);
    }

    // a) Header fields: format 1 (IBM fp32), rev0 (0x0000), sampleIntervalUs=4000.0f
    void headerFieldsIbmRev0()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("header_ibm.sgy"));

      SyntheticSegyConfig cfg;
      cfg.formatCode = 1;
      cfg.revNum = 0x0000;
      cfg.binLineNo = 2002;
      cfg.binDt = 4000;
      cfg.binNs = 64;
      cfg.traceCount = 3;

      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));

      SegyReader reader;
      QString error;
      QVERIFY2(SegyReader::open(path, reader, &error), qPrintable(error));
      QCOMPARE(reader.traceCount(), 3);
      QCOMPARE(reader.samplesPerTrace(), 64);
      QCOMPARE(reader.sampleIntervalUs(), 4000.0f);
    }

    // b) Traces() data and sample values for IEEE format:
    //    check CDP at bytes 20-23, lineNo (real inline word; #233：inline 0 是
    //    合法编号，不再回退二进制头行号), tracl at bytes 0-3, sample values
    void tracesDataAndSampleValuesIeee()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("traces_ieee.sgy"));

      SyntheticSegyConfig cfg;
      cfg.formatCode = 5;
      cfg.binLineNo = 5000;
      cfg.binNs = 64;
      cfg.traceCount = 3;

      SyntheticSegyConfig::TraceData t0;
      t0.tracl = 11;
      t0.cdp = 201;
      t0.lineNo = 0; // inline 字 = 0：0 基测网的合法编号（#233 起如实返回 0）
      t0.samples.resize(64);
      t0.samples[0] = 1.25f;
      t0.samples[1] = -3.5f;
      t0.samples[63] = 42.0f;

      SyntheticSegyConfig::TraceData t1;
      t1.tracl = 12;
      t1.cdp = 202;
      t1.lineNo = 7777; // inline override
      t1.samples.resize(64);
      t1.samples[0] = 0.0f;
      t1.samples[1] = 99.75f;
      t1.samples[63] = -123.456f;

      SyntheticSegyConfig::TraceData t2;
      t2.tracl = 13;
      t2.cdp = 203;
      t2.lineNo = 0; // 同上：lineNo 如实为 0
      t2.samples.resize(64);
      t2.samples[0] = -0.5f;
      t2.samples[1] = 1000.0f;
      t2.samples[63] = 0.001f;

      cfg.customTraces = {t0, t1, t2};

      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));

      SegyReader reader;
      QString error;
      QVERIFY2(reader.open(path, &error), qPrintable(error));

      const QVector<SegyTrace> traces = reader.traces();
      QCOMPARE(traces.size(), 3);

      // Trace 0
      QCOMPARE(traces[0].tracl, 11);
      QCOMPARE(traces[0].cdp, 201);
      QCOMPARE(traces[0].lineNo, 0);
      QCOMPARE(traces[0].samples.size(), 64);
      QCOMPARE(traces[0].samples[0], 1.25f);
      QCOMPARE(traces[0].samples[1], -3.5f);
      QCOMPARE(traces[0].samples[63], 42.0f);

      // Trace 1
      QCOMPARE(traces[1].tracl, 12);
      QCOMPARE(traces[1].cdp, 202);
      QCOMPARE(traces[1].lineNo, 7777);
      QCOMPARE(traces[1].samples.size(), 64);
      QCOMPARE(traces[1].samples[0], 0.0f);
      QCOMPARE(traces[1].samples[1], 99.75f);
      QCOMPARE(traces[1].samples[63], -123.456f);

      // Trace 2
      QCOMPARE(traces[2].tracl, 13);
      QCOMPARE(traces[2].cdp, 203);
      QCOMPARE(traces[2].lineNo, 0);
      QCOMPARE(traces[2].samples.size(), 64);
      QCOMPARE(traces[2].samples[0], -0.5f);
      QCOMPARE(traces[2].samples[1], 1000.0f);
      QCOMPARE(traces[2].samples[63], 0.001f);
    }

    // b) Traces() data and sample values for IBM format:
    //    check sample conversions for known numbers (1.0, -1.0, 0.5, 2.0, -0.75, 0.0)
    void tracesDataAndSampleValuesIbm()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("traces_ibm.sgy"));

      SyntheticSegyConfig cfg;
      cfg.formatCode = 1;
      cfg.binLineNo = 3000;
      cfg.binNs = 64;
      cfg.traceCount = 3;

      SyntheticSegyConfig::TraceData t0;
      t0.tracl = 1;
      t0.cdp = 100;
      t0.lineNo = 3000;
      t0.samples.resize(64);
      t0.samples[0] = 1.0f;
      t0.samples[1] = -1.0f;
      t0.samples[2] = 0.5f;
      t0.samples[3] = 2.0f;
      t0.samples[4] = -0.75f;
      t0.samples[5] = 0.0f;

      cfg.customTraces = {t0};

      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));

      SegyReader reader;
      QString error;
      QVERIFY2(reader.open(path, &error), qPrintable(error));

      const QVector<SegyTrace> traces = reader.traces();
      QCOMPARE(traces.size(), 3);

      QCOMPARE(traces[0].tracl, 1);
      QCOMPARE(traces[0].cdp, 100);
      QCOMPARE(traces[0].lineNo, 3000);
      QVERIFY(std::abs(traces[0].samples[0] - 1.0f) < 1e-5f);
      QVERIFY(std::abs(traces[0].samples[1] - (-1.0f)) < 1e-5f);
      QVERIFY(std::abs(traces[0].samples[2] - 0.5f) < 1e-5f);
      QVERIFY(std::abs(traces[0].samples[3] - 2.0f) < 1e-5f);
      QVERIFY(std::abs(traces[0].samples[4] - (-0.75f)) < 1e-5f);
      QCOMPARE(traces[0].samples[5], 0.0f);
    }

    // Extended text headers: extHeaders * 3200 bytes skipped correctly
    void extendedTextHeadersSkipped()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("ext_headers.sgy"));

      SyntheticSegyConfig cfg;
      cfg.formatCode = 5;
      cfg.revNum = 0x0100;
      cfg.extHeaders = 2; // skip 2 * 3200 = 6400 bytes
      cfg.binLineNo = 8008;
      cfg.binNs = 64;
      cfg.traceCount = 3;

      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));

      SegyReader reader;
      QString error;
      QVERIFY2(reader.open(path, &error), qPrintable(error));
      QCOMPARE(reader.traceCount(), 3);
      // #233：inline 字恒 0 → ordinal 道号索引首线编号 0；lineNo 如实返回 0，
      // 不再回退二进制头行号 8008。
      QCOMPARE(reader.traces()[0].lineNo, 0);
    }

    void variableExtendedTextHeadersSkipped()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      SyntheticSegyConfig cfg;
      cfg.extHeaders = -1;
      const QString path = dir.filePath(QStringLiteral("endtext.sgy"));
      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));
      SegyReader reader;
      QString error;
      QVERIFY2(reader.open(path, &error), qPrintable(error));
      QCOMPARE(reader.traceCount(), cfg.traceCount);
      cfg.endText = false;
      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));
      QVERIFY(!reader.open(path, &error));
      QVERIFY(error.contains(QStringLiteral("EndText")));
    }

    // c) Truncated file handling:
    //    file < 3600 bytes, file cut off in trace header or samples -> returns false, sets non-empty error
    void truncatedFileHandling()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());

      SyntheticSegyConfig cfg;
      cfg.formatCode = 5;
      cfg.binNs = 64;
      cfg.traceCount = 3;
      const QByteArray full = buildSyntheticSegy(cfg);
      QVERIFY(full.size() > 3600);

      // Case 1: file < 3600 bytes
      const QString p1 = dir.filePath(QStringLiteral("trunc_header.sgy"));
      QVERIFY(writeSegyFile(p1, full.left(3599)));
      SegyReader r1;
      QString err1;
      QVERIFY(!r1.open(p1, &err1));
      QVERIFY(!err1.isEmpty());

      // Case 2: cut off in trace header (e.g. 3600 + 120 bytes, header needs 240)
      const QString p2 = dir.filePath(QStringLiteral("trunc_tr_hdr.sgy"));
      QVERIFY(writeSegyFile(p2, full.left(3600 + 120)));
      SegyReader r2;
      QString err2;
      QVERIFY(!r2.open(p2, &err2));
      QVERIFY(!err2.isEmpty());

      // Case 3: cut off in trace samples (e.g. 3600 + 240 + 20 bytes, samples need 64*4=256)
      const QString p3 = dir.filePath(QStringLiteral("trunc_samples.sgy"));
      QVERIFY(writeSegyFile(p3, full.left(3600 + 240 + 20)));
      SegyReader r3;
      QString err3;
      QVERIFY(!r3.open(p3, &err3));
      QVERIFY(!err3.isEmpty());

      // Case 4: cut off inside extended text headers
      SyntheticSegyConfig cfgExt;
      cfgExt.extHeaders = 1; // needs 3600 + 3200 = 6800 bytes
      const QByteArray fullExt = buildSyntheticSegy(cfgExt);
      const QString p4 = dir.filePath(QStringLiteral("trunc_ext.sgy"));
      QVERIFY(writeSegyFile(p4, fullExt.left(4000)));
      SegyReader r4;
      QString err4;
      QVERIFY(!r4.open(p4, &err4));
      QVERIFY(!err4.isEmpty());
    }

    // d) Non-SEG-Y / invalid file handling:
    //    empty file, non-existent file, invalid format code, ns <= 0, 0 traces -> returns false, sets non-empty error
    void nonSegyAndInvalidFileHandling()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());

      // Non-existent file
      SegyReader rNonExist;
      QString errNonExist;
      QVERIFY(!rNonExist.open(dir.filePath(QStringLiteral("ghost.sgy")), &errNonExist));
      QVERIFY(!errNonExist.isEmpty());

      // Empty file (0 bytes)
      const QString pEmpty = dir.filePath(QStringLiteral("empty.sgy"));
      QVERIFY(writeSegyFile(pEmpty, QByteArray()));
      SegyReader rEmpty;
      QString errEmpty;
      QVERIFY(!rEmpty.open(pEmpty, &errEmpty));
      QVERIFY(!errEmpty.isEmpty());

      // Invalid format code (e.g. 2 = 32-bit fixed point, not supported)
      SyntheticSegyConfig cfgBadFmt;
      cfgBadFmt.formatCode = 2;
      const QString pBadFmt = dir.filePath(QStringLiteral("bad_fmt.sgy"));
      QVERIFY(writeSegyFile(pBadFmt, buildSyntheticSegy(cfgBadFmt)));
      SegyReader rBadFmt;
      QString errBadFmt;
      QVERIFY(!rBadFmt.open(pBadFmt, &errBadFmt));
      QVERIFY(!errBadFmt.isEmpty());

      // Invalid ns in binary header (ns <= 0)
      SyntheticSegyConfig cfgBadNs;
      cfgBadNs.binNs = 0;
      const QString pBadNs = dir.filePath(QStringLiteral("bad_ns.sgy"));
      QVERIFY(writeSegyFile(pBadNs, buildSyntheticSegy(cfgBadNs)));
      SegyReader rBadNs;
      QString errBadNs;
      QVERIFY(!rBadNs.open(pBadNs, &errBadNs));
      QVERIFY(!errBadNs.isEmpty());

      // Exactly 3600 bytes with 0 traces
      SyntheticSegyConfig cfgZeroTraces;
      cfgZeroTraces.traceCount = 0;
      const QString pZero = dir.filePath(QStringLiteral("zero_traces.sgy"));
      QVERIFY(writeSegyFile(pZero, buildSyntheticSegy(cfgZeroTraces)));
      SegyReader rZero;
      QString errZero;
      QVERIFY(!rZero.open(pZero, &errZero));
      QVERIFY(!errZero.isEmpty());

      // Corrupt ns in trace header (ns <= 0)
      SyntheticSegyConfig cfgCorruptTraceNs;
      cfgCorruptTraceNs.traceCount = 1;
      cfgCorruptTraceNs.customTraces = {{1, 100, 0, -5, 0, {}}};
      const QString pCorruptNs = dir.filePath(QStringLiteral("corrupt_trace_ns.sgy"));
      QVERIFY(writeSegyFile(pCorruptNs, buildSyntheticSegy(cfgCorruptTraceNs)));
      SegyReader rCorruptNs;
      QString errCorruptNs;
      QVERIFY(!rCorruptNs.open(pCorruptNs, &errCorruptNs));
      QVERIFY(!errCorruptNs.isEmpty());
    }

    // P1-03 / BIZ-03: Partial scan resumption with bad traces resumes from exact byte offset
    void partialScanResumptionWithBadTraces()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("resume_bad.sgy"));
      const QString cacheDir = dir.filePath(QStringLiteral("idx"));

      const int inl = 60, xl = 60, samples = 40;
      const int totalTraces = inl * xl;
      QVERIFY(PerfFixtures::makeSyntheticSegy(path, inl, xl, samples) > 0);

      const qint64 traceSize = 240 + samples * 4;
      // Patch 2 bad traces within the first 256 traces (at trace 5 and trace 15)
      QFile f(path);
      QVERIFY(f.open(QIODevice::ReadWrite));
      const qint64 badTrace1 = 3600 + 5 * traceSize;
      const qint64 badTrace2 = 3600 + 15 * traceSize;
      const char badNs[2] = {static_cast<char>(0x80), 0x00}; // -32768
      f.seek(badTrace1 + 114);
      f.write(badNs, 2);
      f.seek(badTrace2 + 114);
      f.write(badNs, 2);
      f.close();

      // Step 1: Scan with cancel triggered after 256 traces
      SegyReader r1;
      SegyOptions opts;
      std::atomic_int shard0Traces{0};
      opts.cancel = [&]() {
        return shard0Traces.load() >= 256;
      };
      opts.progress = [&](qint64 current, qint64 total) {
        Q_UNUSED(total);
        const qint64 tr = (current - 3600) / traceSize;
        if (tr >= 0 && tr < 500)
        {
          int prev = shard0Traces.load();
          while (tr > prev && !shard0Traces.compare_exchange_weak(prev, static_cast<int>(tr)))
            ;
        }
      };

      QString err1;
      const bool ok1 = r1.openCached(path, cacheDir, &err1, &opts);
      QVERIFY(!ok1);
      QVERIFY(r1.lastScanPartial());

      SegyIndexStore::StoredIndex snap;
      QVERIFY(r1.snapshot(&snap));
      QCOMPARE(snap.badTraceOffsets.size(), 2);
      // Validate that scannedOffset includes both valid traces and bad traces without lagging
      const qint64 expectedScannedOffset =
          3600 + static_cast<qint64>(snap.inlineNos.size() + snap.badTraceOffsets.size()) * traceSize;
      QCOMPARE(snap.scannedOffset, expectedScannedOffset);

      // Step 2: Resume scan to completion
      SegyReader r2;
      QString err2;
      const bool ok2 = r2.openCached(path, cacheDir, &err2);
      QVERIFY2(ok2, qPrintable(err2));
      QVERIFY(!r2.lastScanPartial());
      QCOMPARE(r2.traceCount(), totalTraces - 2);
      QCOMPARE(r2.badTraceOffsets().size(), 2);

      // Verify no duplicate or lagging reads: trace offsets must be strictly monotonically increasing
      SegyIndexStore::StoredIndex finalSnap;
      QVERIFY(r2.snapshot(&finalSnap));
      QCOMPARE(finalSnap.offsets.size(), totalTraces - 2);
      for (int i = 1; i < finalSnap.offsets.size(); ++i)
      {
        QVERIFY2(finalSnap.offsets[i] > finalSnap.offsets[i - 1],
                 qPrintable(QStringLiteral("Duplicate or backward offset at index %1: %2 <= %3")
                                .arg(i)
                                .arg(finalSnap.offsets[i])
                                .arg(finalSnap.offsets[i - 1])));
      }
    }
    // P? 回归（2026-10 智能预测「地震体号域不构成二维测网」）：demo 工区方言体
    // （crossline 193 恒 0、CDP 21 变化、坐标只在 CDP X/Y 181-188）此前只有顺序
    // open() 应用方言；>1MB 体走 openCached 并行扫描时号域退化为 xline 全 0、
    // 角点全 0。并行路径必须与顺序路径产出同一几何，缓存热读同样保持。
    void dialectLayoutParallelOpenCachedMatchesSequential()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("dialect_parallel.sgy"));

      const int perLine = 50, lines = 60, ns = 64;
      SyntheticSegyConfig cfg;
      cfg.formatCode = 5;
      cfg.binNs = ns;
      cfg.traceCount = perLine * lines; // 3000 道 × 496B ≈ 1.49MB → 并行资格
      QVector<SyntheticSegyConfig::TraceData> traces(cfg.traceCount);
      for (int i = 0; i < cfg.traceCount; ++i)
      {
        traces[i].lineNo = 100 + i / perLine;   // inline 变化（非 ordinal 方言）
        traces[i].cdp = 2000 + i % perLine;     // CDP 变化 → 方言 crossline 源
        traces[i].cdpX = 10 * (i % perLine);    // 坐标只出现在 181-188（73-78 恒 0）
        traces[i].cdpY = 20 * (i / perLine);
      }
      cfg.customTraces = traces;
      QVERIFY(writeSegyFile(path, buildSyntheticSegy(cfg)));

      SegyReader seq;
      QString err;
      QVERIFY2(seq.open(path, &err), qPrintable(err));
      const auto g0 = seq.geometry();
      QCOMPARE(g0.inlineMin, 100);
      QCOMPARE(g0.inlineMax, 100 + lines - 1);
      QCOMPARE(g0.xlineMin, 2000);
      QCOMPARE(g0.xlineMax, 2000 + perLine - 1);

      QTemporaryDir cacheDir;
      SegyReader cold;
      QVERIFY2(cold.openCached(path, cacheDir.path(), &err), qPrintable(err));
      const auto g1 = cold.geometry();
      QCOMPARE(g1.inlineMin, g0.inlineMin);
      QCOMPARE(g1.inlineMax, g0.inlineMax);
      QCOMPARE(g1.xlineMin, g0.xlineMin);
      QCOMPARE(g1.xlineMax, g0.xlineMax);
      for (int c = 0; c < 4; ++c)
      {
        QCOMPARE(g1.cornerX[c], g0.cornerX[c]);
        QCOMPARE(g1.cornerY[c], g0.cornerY[c]);
      }
      QCOMPARE(g1.cornerX[2], double(10 * (perLine - 1)));
      QCOMPARE(g1.cornerY[2], double(20 * (lines - 1)));

      // 缓存热读（身份命中免扫）同样不得回退成退化几何。
      SegyReader hot;
      QVERIFY2(hot.openCached(path, cacheDir.path(), &err), qPrintable(err));
      const auto g2 = hot.geometry();
      QCOMPARE(g2.xlineMin, 2000);
      QCOMPARE(g2.xlineMax, 2000 + perLine - 1);
      QCOMPARE(g2.cornerX[2], double(10 * (perLine - 1)));
      QCOMPARE(g2.cornerY[2], double(20 * (lines - 1)));
    }
    // e) #42.1 收口（WP2）：open 成功后文件被截断（索引已建）——丢道不再
    //    静默：readInline 以「failed to decode N of M」如实失败，且
    //    readByIndexList 对被丢弃的道 qWarning 留痕（诚实降级面——
    //    traces()/未来调用方拿部分结果时日志里有账可查）。
    void postOpenTruncationDroppedTracesSurface()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());

      SyntheticSegyConfig cfg;
      cfg.formatCode = 5;
      cfg.binNs = 64;
      cfg.traceCount = 3;
      cfg.customTraces = {{1, 101, 10, 0, 0, {}},
                          {2, 102, 10, 0, 0, {}},
                          {3, 103, 10, 0, 0, {}}};
      const QByteArray full = buildSyntheticSegy(cfg);
      const QString p = dir.filePath(QStringLiteral("trunc_post_open.sgy"));
      QVERIFY(writeSegyFile(p, full));
      SegyReader r;
      QString err;
      QVERIFY(r.open(p, &err));
      QVector<SegyTrace> line;
      QVERIFY2(r.readInline(10, &line, &err), qPrintable(err));
      QCOMPARE(int(line.size()), 3);

      // 截断第三道中部（道字节 = 240 头 + 64*4 样本；留头 + 64 样本字节）。
      const int traceBytes = 240 + 64 * 4;
      QFile f(p);
      QVERIFY(f.open(QIODevice::ReadWrite));
      QVERIFY(f.resize(3600 + 2 * traceBytes + 240 + 64));
      f.close();

      line.clear();
      err.clear();
      QTest::ignoreMessage(QtWarningMsg,
                           QRegularExpression("dropped 1 of 3 traces"));
      QVERIFY(!r.readInline(10, &line, &err));
      QVERIFY2(err.contains(QStringLiteral("failed to decode 1 of 3")),
               qPrintable(err));
    }

    // f) SeismicPreviewPanel::loadLineFromFile integration test removed with
    //    T30 — the panel is retired; single-line preview + SHA verify live on
    //    the data-page preview tabs (datapreviewtabs).
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QCoreApplication app(argc, argv);
  TestSegy tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_segy.moc"
