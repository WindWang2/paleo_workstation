#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <vector>
#include <filesystem>

#include "domain/seismic/sgyfilereader.h"
#include "domain/seismic/sgyrulelayout.h"
#include "domain/seismic/sgyindexbuilder.h"
#include "domain/seismic/sgyindexcache.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyreadplan.h"
#include "domain/seismic/sgyvolume.h"
#include "domain/seismic/sgysectionbuilder.h"
#include "domain/seismic/seismiccolormap.h"

#include <segyio/segy.h>

class TestSeismicCore : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir tempDir_;
  QString testFilePath_;

  static bool writeSyntheticVolumeSegy(const QString &filePath,
                                       int firstInline = 10, int lastInline = 13,
                                       int firstXline = 100, int lastXline = 104,
                                       int ns = 64, int dt = 2000)
  {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
      return false;

    // 1. 3200-byte Textual Header with declared range keywords
    QByteArray textHdr(3200, ' ');
    const QString banner = QString(
        "C01 SEG-Y TEST FILE First inline : %1 Last inline : %2 First xline : %3 Last xline : %4")
        .arg(firstInline).arg(lastInline).arg(firstXline).arg(lastXline);
    const QByteArray bannerBytes = banner.toUtf8();
    std::memcpy(textHdr.data(), bannerBytes.constData(), bannerBytes.size());
    file.write(textHdr);

    // 2. 400-byte Binary Header
    QByteArray binHdr(400, 0);
    qToBigEndian<qint32>(1, reinterpret_cast<uchar *>(binHdr.data()) + 4);       // Line number
    qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(binHdr.data()) + 16);     // Sample interval (us)
    qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(binHdr.data()) + 20);     // Samples per trace
    qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24);      // Format 5 = IEEE float
    qToBigEndian<qint16>(0x0100, reinterpret_cast<uchar *>(binHdr.data()) + 300); // rev 1.0
    file.write(binHdr);

    // 3. Traces: inline-major ordering
    int traceIndex = 0;
    const int inlineCount = lastInline - firstInline + 1;
    const int xlineCount = lastXline - firstXline + 1;

    for (int i = 0; i < inlineCount; ++i)
    {
      const int inlineNo = firstInline + i;
      for (int j = 0; j < xlineCount; ++j)
      {
        const int xlineNo = firstXline + j;

        QByteArray trHdr(240, 0);
        qToBigEndian<qint32>(traceIndex + 1, reinterpret_cast<uchar *>(trHdr.data()) + 0);  // TRACL
        qToBigEndian<qint32>(inlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 8);        // Field record
        qToBigEndian<qint32>(xlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 20);        // CDP

        // Source and group coordinates & scalar
        qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(trHdr.data()) + 70);              // Scalar = 1
        const qint32 coordX = 1000 + j * 25;
        const qint32 coordY = 5000 + i * 25;
        qToBigEndian<qint32>(coordX, reinterpret_cast<uchar *>(trHdr.data()) + 72);         // Source X
        qToBigEndian<qint32>(coordY, reinterpret_cast<uchar *>(trHdr.data()) + 76);         // Source Y
        qToBigEndian<qint32>(coordX, reinterpret_cast<uchar *>(trHdr.data()) + 180);        // CDP X
        qToBigEndian<qint32>(coordY, reinterpret_cast<uchar *>(trHdr.data()) + 184);        // CDP Y

        qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(trHdr.data()) + 114);
        qToBigEndian<qint16>(static_cast<qint16>(dt), reinterpret_cast<uchar *>(trHdr.data()) + 116);
        qToBigEndian<qint32>(inlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 188);      // INLINE
        qToBigEndian<qint32>(xlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 192);       // CROSSLINE
        file.write(trHdr);

        // Trace samples: IEEE 32-bit floats
        QByteArray samples(ns * 4, 0);
        for (int k = 0; k < ns; ++k)
        {
          const float val = static_cast<float>((i + 1) * 100 + (j + 1) * 10 + k);
          // Endian conversion for IEEE float:
          quint32 rawBits;
          std::memcpy(&rawBits, &val, 4);
          qToBigEndian<quint32>(rawBits, reinterpret_cast<uchar *>(samples.data()) + k * 4);
        }
        file.write(samples);

        ++traceIndex;
      }
    }

    file.close();
    return true;
  }

private slots:
  void initTestCase()
  {
    QVERIFY(tempDir_.isValid());
    testFilePath_ = tempDir_.filePath(QStringLiteral("test_volume.sgy"));
    QVERIFY(writeSyntheticVolumeSegy(testFilePath_));
  }

  void fileReaderReadsSummary()
  {
    seismic::SgyFileSummary summary;
    std::string err;
    const std::filesystem::path stdPath(testFilePath_.toStdString());

    const bool ok = seismic::SgyFileReader::ReadSummary(stdPath, summary, err);
    QVERIFY2(ok, err.c_str());
    QCOMPARE(summary.traceCount, 20);
    QCOMPARE(summary.sampleCount, 64);
    QCOMPARE(summary.sampleIntervalUs, 2000);
    QCOMPARE(summary.formatCode, 5); // IEEE float
    QCOMPARE(summary.formatSizeBytes, 4);
    QVERIFY(summary.declaredRangeValid);
    QCOMPARE(summary.declaredInlineMin, 10);
    QCOMPARE(summary.declaredInlineMax, 13);
    QCOMPARE(summary.declaredXlineMin, 100);
    QCOMPARE(summary.declaredXlineMax, 104);

    // Formatter helpers
    QVERIFY(!seismic::DescribeSgyFormat(5).empty());
    QVERIFY(!seismic::DescribeSgyEndianness(summary.endianness).empty());
    QVERIFY(!seismic::DescribeSgyEncoding(summary.encoding).empty());
  }

  void ruleLayoutProbe()
  {
    const std::filesystem::path stdPath(testFilePath_.toStdString());
    seismic::SgyRuleLayout layout;
    std::string err;

    const bool ok = seismic::ProbeSgyRuleLayout(stdPath, 20, layout, err);
    QVERIFY2(ok, err.c_str());
    QVERIFY(layout.valid);
    QCOMPARE(layout.firstInline, 10);
    QCOMPARE(layout.lastInline, 13);
    QCOMPARE(layout.firstXline, 100);
    QCOMPARE(layout.lastXline, 104);
    QCOMPARE(layout.inlineCount, 4);
    QCOMPARE(layout.xlineCount, 5);
    QCOMPARE(layout.traceCount, 20);
    QVERIFY(layout.inlineMajor);

    QCOMPARE(layout.TraceIndexFor(10, 100), 0);
    QCOMPARE(layout.TraceIndexFor(10, 104), 4);
    QCOMPARE(layout.TraceIndexFor(13, 104), 19);
    QCOMPARE(layout.TraceIndexFor(99, 99), -1);

    int il = 0, xl = 0;
    QVERIFY(layout.ExpectedInlineXline(0, il, xl));
    QCOMPARE(il, 10);
    QCOMPARE(xl, 100);
    QVERIFY(layout.ExpectedInlineXline(19, il, xl));
    QCOMPARE(il, 13);
    QCOMPARE(xl, 104);
  }

  void indexBuilderAndCacheRoundtrip()
  {
    const std::filesystem::path stdPath(testFilePath_.toStdString());
    seismic::SgyIndexPtr index;
    std::string err;

    const bool buildOk = seismic::SgyIndexBuilder::Build(stdPath, index, err);
    QVERIFY2(buildOk, err.c_str());
    QVERIFY(index != nullptr);
    QVERIFY(index->complete);
    QCOMPARE(static_cast<int>(index->traces.size()), 20);
    QCOMPARE(index->inlineMin, 10);
    QCOMPARE(index->inlineMax, 13);
    QCOMPARE(index->xlineMin, 100);
    QCOMPARE(index->xlineMax, 104);

    const int found = index->FindTraceIndex(11, 102);
    QVERIFY(found >= 0);
    QCOMPARE(found, 7); // i=1, j=2 -> 1*5 + 2 = 7

    // Test Cache Save and Load
    const bool saveOk = seismic::SgyIndexCache::Save(index, err);
    QVERIFY2(saveOk, err.c_str());

    std::string loadReason;
    seismic::SgyIndexPtr cachedIndex = seismic::SgyIndexCache::Load(stdPath, loadReason);
    QVERIFY2(cachedIndex != nullptr, loadReason.c_str());
    QCOMPARE(cachedIndex->traceCount, index->traceCount);
    QCOMPARE(cachedIndex->sampleCount, index->sampleCount);
    QCOMPARE(cachedIndex->inlineMin, index->inlineMin);
    QCOMPARE(cachedIndex->inlineMax, index->inlineMax);
    QCOMPARE(cachedIndex->xlineMin, index->xlineMin);
    QCOMPARE(cachedIndex->xlineMax, index->xlineMax);

    // Cache Remove
    std::string removeErr;
    QVERIFY(seismic::SgyIndexCache::Remove(stdPath, removeErr));
  }

  void coordinateMapperFitAndCoverage()
  {
    const std::filesystem::path stdPath(testFilePath_.toStdString());
    seismic::SgyIndexPtr index;
    std::string err;
    QVERIFY(seismic::SgyIndexBuilder::Build(stdPath, index, err));

    seismic::SgyCoordinateMapper mapper = seismic::SgyCoordinateMapper::Fit(*index);
    QVERIFY(mapper.valid());
    QVERIFY(mapper.fit().rmsResidual < 0.1);

    // Inline 10, xline 100 -> (1000, 5000)
    double x = 0.0, y = 0.0;
    QVERIFY(mapper.MapInlineXline(10.0, 100.0, x, y));
    QCOMPARE(std::round(x), 1000.0);
    QCOMPARE(std::round(y), 5000.0);

    // Inline 12, xline 103 -> (1000 + 3*25 = 1075, 5000 + 2*25 = 5050)
    QVERIFY(mapper.MapInlineXline(12.0, 103.0, x, y));
    QCOMPARE(std::round(x), 1075.0);
    QCOMPARE(std::round(y), 5050.0);

    // Inverse Map
    double il = 0.0, xl = 0.0;
    QVERIFY(mapper.MapXY(1075.0, 5050.0, il, xl));
    QCOMPARE(std::round(il), 12.0);
    QCOMPARE(std::round(xl), 103.0);

    // InCoverage
    QVERIFY(mapper.InCoverage(1050.0, 5025.0));
    QVERIFY(!mapper.InCoverage(9999.0, 9999.0));
  }

  void engineReadPlanGeneration()
  {
    const std::filesystem::path stdPath(testFilePath_.toStdString());
    seismic::SgyIndexPtr index;
    std::string err;
    QVERIFY(seismic::SgyIndexBuilder::Build(stdPath, index, err));

    seismic::engine::ReadPlanOptions options;
    options.maxGapTraces = 0;
    options.sampleBegin = 0;
    options.sampleCount = 64;

    const std::vector<int> requested = { 0, 1, 2, 4 };
    seismic::engine::ReadPlan plan = seismic::engine::BuildReadPlan(*index, requested, options);

    QVERIFY(plan.planned);
    QCOMPARE(plan.stats.requestedTraces, 4ull);
    QCOMPARE(plan.stats.uniqueTraces, 4ull);
    QCOMPARE(plan.ranges.size(), 2ull); // [0..2] and [4..4]
  }

  void volumeAndSectionBuilder()
  {
    const std::filesystem::path stdPath(testFilePath_.toStdString());
    seismic::SgyIndexPtr index;
    std::string err;
    QVERIFY(seismic::SgyIndexBuilder::Build(stdPath, index, err));

    seismic::SgyVolume volume;
    volume.AdoptIndex(index);
    QVERIFY(volume.IsIndexComplete());

    // Extract inline slice
    seismic::SgySliceImage inlineSlice;
    QVERIFY2(volume.ExtractSlice(seismic::SgySliceType::Inline, 10, inlineSlice, err), err.c_str());
    QCOMPARE(inlineSlice.width, 5); // 5 crosslines
    QCOMPARE(inlineSlice.height, 64);
    QVERIFY(inlineSlice.Valid(0, 0));
    // Sample formula: (i+1)*100 + (j+1)*10 + k -> i=0, j=0
    // row = sampleCount - 1 - s: row 0 is s=63 (173.0), row 63 is s=0 (110.0)
    QCOMPARE(std::round(inlineSlice.Value(0, 0)), 173.0f);
    QCOMPARE(std::round(inlineSlice.Value(0, 63)), 110.0f);

    // Extract arbitrary line section diagonally: (10, 100) to (13, 104)
    std::vector<glm::ivec2> pathPoints = { {10, 100}, {13, 104} };
    seismic::SgySectionOptions secOptions;
    secOptions.maxColumns = 64;
    secOptions.interpolate = false;

    seismic::SgySliceImage secImage;
    seismic::SgySectionStats secStats;
    const bool secOk = seismic::BuildLineSection(volume, pathPoints, secOptions, secImage, secStats, err);
    QVERIFY2(secOk, err.c_str());
    QVERIFY(secStats.columns > 0);
    QCOMPARE(secImage.width, secStats.columns);
    QCOMPARE(secImage.height, 64);
    QVERIFY(!secImage.values.empty());
  }

  void seismicColorMapSampling()
  {
    const glm::vec3 c0 = seismic::ColorMap::SampleHorizonDepth(0.0f);
    const glm::vec3 cMid = seismic::ColorMap::SampleHorizonDepth(0.5f);
    const glm::vec3 c1 = seismic::ColorMap::SampleHorizonDepth(1.0f);

    QVERIFY(c0.r >= 0.0f && c0.r <= 1.0f);
    QVERIFY(c0.g >= 0.0f && c0.g <= 1.0f);
    QVERIFY(c0.b >= 0.0f && c0.b <= 1.0f);

    QVERIFY(cMid.r >= 0.0f && cMid.r <= 1.0f);
    QVERIFY(cMid.g >= 0.0f && cMid.g <= 1.0f);
    QVERIFY(cMid.b >= 0.0f && cMid.b <= 1.0f);

    QVERIFY(c1.r >= 0.0f && c1.r <= 1.0f);
    QVERIFY(c1.g >= 0.0f && c1.g <= 1.0f);
    QVERIFY(c1.b >= 0.0f && c1.b <= 1.0f);
  }

  // segyio 本地补丁（审计 03 D3）：rev2 扩展样点数（int32）× 样点字节数在 int 上溢出
  // 是 UB（UBSan: segy.c:1120 "1289994496 * 4 cannot be represented in type 'int'"）。
  // 现在 segy_collect_metadata 必须拒绝而不是带着回绕的 trace_bsize 继续。
  void segyioRejectsOverflowingTraceSize()
  {
    auto makeHeader = [](quint16 samples, qint32 extSamples) {
      QByteArray buf(3600 + 240 + 16, '\0');
      std::memset(buf.data(), ' ', 3200);
      uchar *bin = reinterpret_cast<uchar *>(buf.data()) + 3200;
      qToBigEndian<quint16>(4000, bin + 16);      // 采样间隔 µs
      qToBigEndian<quint16>(samples, bin + 20);   // SEGY_BIN_SAMPLES (3221)
      qToBigEndian<qint16>(5, bin + 24);          // IEEE float, 4 字节
      qToBigEndian<qint32>(extSamples, bin + 68); // SEGY_BIN_EXT_SAMPLES (3269)
      return buf;
    };
    {
      QByteArray ok = makeHeader(4, 0);
      segy_datasource *ds =
          segy_memopen(reinterpret_cast<unsigned char *>(ok.data()), size_t(ok.size()));
      QVERIFY(ds);
      QCOMPARE(segy_collect_metadata(ds, -1, -1, 0), int(SEGY_OK));
      QCOMPARE(ds->metadata.samplecount, 4);
      QCOMPARE(ds->metadata.trace_bsize, 16);
      segy_close(ds);
    }
    {
      // samples=0 → 采用扩展字段 1289994496；×4 字节 = 5.16e9 > INT_MAX
      QByteArray bad = makeHeader(0, 1289994496);
      segy_datasource *ds =
          segy_memopen(reinterpret_cast<unsigned char *>(bad.data()), size_t(bad.size()));
      QVERIFY(ds);
      QVERIFY(segy_collect_metadata(ds, -1, -1, 0) != SEGY_OK);
      segy_close(ds);
    }
    QCOMPARE(segy_trsize(SEGY_IEEE_FLOAT_4_BYTE, 1289994496), -1);
    QCOMPARE(segy_trsize(SEGY_IEEE_FLOAT_4_BYTE, 1000), 4000);
  }
};

QTEST_MAIN(TestSeismicCore)
#include "tst_seismic_core.moc"
