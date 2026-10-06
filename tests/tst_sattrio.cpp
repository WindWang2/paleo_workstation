// 层：数据（测试壳位于 tests/，被测对象为 io 层属性容器读写）
#include <QtTest>
#include <QTemporaryDir>
#include <QDataStream>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <cmath>
#include <limits>
#include <vector>

#include "io/sattrio.h"

using paleo::sattr::SattrSectionHeader;
using paleo::sattr::SattrVolumeInfo;
using paleo::sattr::SattrVolumeReader;
using paleo::sattr::SattrVolumeWriter;

namespace
{

float cellValue(int il, int xl, int s)
{
  return float(il) * 1000.f + float(xl) * 100.f + float(s);
}

} // namespace

class TestSattrIo : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir tempDir_;

  SattrSectionHeader makeSectionHeader()
  {
    SattrSectionHeader h;
    h.attrId = QStringLiteral("envelope");
    h.section = QStringLiteral("il");
    h.sectionIndex = 1147;
    h.width = 9;
    h.height = 64;
    h.valueMin = -1.5;
    h.valueMax = 2.25;
    h.traceCount = 9;
    h.validTraceCount = 8;
    h.readMs = 12.5;
    h.computeMs = 34.25;
    h.sourceSgyPath = QStringLiteral("/tmp/survey.sgy");
    h.createdAt = QStringLiteral("2026-10-05T00:00:00.000Z");
    h.windowHalfSamples = 12;
    h.coherenceIlHalf = 2;
    h.coherenceXlHalf = 3;
    h.coherenceTimeHalf = 4;
    h.coherenceWeighting = 1;
    return h;
  }

  QVector<float> makeSectionValues(const SattrSectionHeader &h)
  {
    QVector<float> values(std::size_t(h.width) * h.height);
    for (int row = 0; row < h.height; ++row)
      for (int col = 0; col < h.width; ++col)
        values[std::size_t(row * h.width + col)] =
            float(row) * 10.f + float(col) +
            (row == 3 && col == 4
                 ? std::numeric_limits<float>::quiet_NaN()
                 : 0.f);
    return values;
  }

private slots:
  void initTestCase() { QVERIFY(tempDir_.isValid()); }

  void sectionRoundTrip()
  {
    const SattrSectionHeader out = makeSectionHeader();
    const QVector<float> values = makeSectionValues(out);
    const QString path = tempDir_.filePath(QStringLiteral("env_il_1147.sattr"));
    QString err;
    QVERIFY2(paleo::sattr::writeSattrSection(path, out, values, &err),
             qPrintable(err));

    SattrSectionHeader in;
    QVector<float> readBack;
    QVERIFY2(paleo::sattr::readSattrSection(path, &in, &readBack, &err),
             qPrintable(err));
    // Oracle#1：写端产物读回参数/几何/数据逐字段一致。
    QCOMPARE(in.attrId, out.attrId);
    QCOMPARE(in.section, out.section);
    QCOMPARE(in.sectionIndex, out.sectionIndex);
    QCOMPARE(in.width, out.width);
    QCOMPARE(in.height, out.height);
    QCOMPARE(in.valueMin, out.valueMin);
    QCOMPARE(in.valueMax, out.valueMax);
    QCOMPARE(in.traceCount, out.traceCount);
    QCOMPARE(in.validTraceCount, out.validTraceCount);
    QCOMPARE(in.readMs, out.readMs);
    QCOMPARE(in.computeMs, out.computeMs);
    QCOMPARE(in.sourceSgyPath, out.sourceSgyPath);
    QCOMPARE(in.createdAt, out.createdAt);
    QCOMPARE(in.windowHalfSamples, out.windowHalfSamples);
    QCOMPARE(in.coherenceIlHalf, out.coherenceIlHalf);
    QCOMPARE(in.coherenceXlHalf, out.coherenceXlHalf);
    QCOMPARE(in.coherenceTimeHalf, out.coherenceTimeHalf);
    QCOMPARE(in.coherenceWeighting, out.coherenceWeighting);
    QCOMPARE(readBack.size(), values.size());
    for (qsizetype i = 0; i < values.size(); ++i)
    {
      const float a = values[int(i)], b = readBack[i];
      QVERIFY2(std::isnan(a) == std::isnan(b),
               qPrintable(QStringLiteral("NaN 位不一致 @%1").arg(i)));
      if (!std::isnan(a))
        QCOMPARE(b, a); // 小端 f32 逐位 round-trip
    }
  }

  void sectionRejectsCorruption()
  {
    const SattrSectionHeader h = makeSectionHeader();
    const QVector<float> values = makeSectionValues(h);
    const QString path = tempDir_.filePath(QStringLiteral("bad.sattr"));
    QVERIFY(paleo::sattr::writeSattrSection(path, h, values));

    SattrSectionHeader in;
    QVector<float> v;
    QString err;
    // 魔数损坏
    {
      QByteArray blob;
      {
        QFile src(path);
        QVERIFY(src.open(QIODevice::ReadOnly));
        blob = src.readAll();
      }
      blob[0] = 'X';
      QFile f(tempDir_.filePath(QStringLiteral("bad_magic.sattr")));
      f.open(QIODevice::WriteOnly);
      f.write(blob);
      f.close();
      QVERIFY(!paleo::sattr::readSattrSection(f.fileName(), &in, &v, &err));
      QVERIFY(err.contains(QStringLiteral("魔数")));
      err.clear();
    }
    // 值块截断
    {
      QByteArray blob;
      {
        QFile src(path);
        QVERIFY(src.open(QIODevice::ReadOnly));
        blob = src.readAll();
      }
      blob.chop(64);
      QFile f(tempDir_.filePath(QStringLiteral("bad_trunc.sattr")));
      f.open(QIODevice::WriteOnly);
      f.write(blob);
      f.close();
      QVERIFY(!paleo::sattr::readSattrSection(f.fileName(), &in, &v, &err));
      QVERIFY(err.contains(QStringLiteral("大小")) ||
              err.contains(QStringLiteral("截断")));
      err.clear();
    }
    // 写端尺寸守卫：值块与头不符
    {
      QVector<float> shortValues;
      QVERIFY(!paleo::sattr::writeSattrSection(
          tempDir_.filePath(QStringLiteral("bad_len.sattr")), h, shortValues,
          &err));
      QVERIFY(err.contains(QStringLiteral("尺寸")));
    }
  }

  void sectionLegacyMinimalHeader()
  {
    // crossplot 既有夹具形态：极简 JSON 头（无 params/valueMin 等）——读端
    // 缺键按缺省（等权相干、半窗 8/1/1/2）兼容，不拒读。
    const QString path = tempDir_.filePath(QStringLiteral("legacy.sattr"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const auto json = QJsonDocument(QJsonObject{{"section", "xl"},
                                                {"sectionIndex", 2000},
                                                {"attrId", "RMS"},
                                                {"sourceSgyPath", "/tmp/x.sgy"}})
                          .toJson(QJsonDocument::Compact);
    stream.writeRawData("SATR", 4);
    stream << quint32(1) << qint32(3) << qint32(2) << quint32(json.size());
    stream.writeRawData(json.constData(), json.size());
    for (int i = 0; i < 6; ++i)
      stream << float(i);
    file.close();

    SattrSectionHeader h;
    QVector<float> v;
    QString err;
    QVERIFY2(paleo::sattr::readSattrSection(path, &h, &v, &err),
             qPrintable(err));
    QCOMPARE(h.section, QStringLiteral("xl"));
    QCOMPARE(h.sectionIndex, 2000);
    QCOMPARE(h.attrId, QStringLiteral("RMS"));
    QCOMPARE(h.width, 3);
    QCOMPARE(h.height, 2);
    QCOMPARE(h.coherenceWeighting, 0);
    QCOMPARE(h.windowHalfSamples, 8);
    QCOMPARE(v.size(), qsizetype(6));
    QCOMPARE(v[4], 4.f);
  }

  void volumeRoundTripAndExtraction()
  {
    SattrVolumeInfo info;
    info.attrId = QStringLiteral("instfreq");
    info.nIl = 7;
    info.nXl = 5;
    info.nS = 13;
    for (int i = 0; i < info.nIl; ++i)
      info.ilValues.append(100 + i * 2); // 稀疏轴如实记录
    for (int j = 0; j < info.nXl; ++j)
      info.xlValues.append(200 + j * 3);
    info.sampleIntervalMs = 2.0;
    info.startTimeMs = 100.0;
    info.blockIl = 3; // 块 3/3/1——末块短路径
    info.valueMin = 0.0;
    info.valueMax = 7612.0;
    info.validCells = 7 * 5 * 13 - 4;
    info.readMs = 100.0;
    info.computeMs = 200.0;
    info.sourceSgyPath = QStringLiteral("/tmp/survey.sgy");
    info.createdAt = QStringLiteral("2026-10-05T00:00:00.000Z");
    info.paramHash = QStringLiteral("deadbeefcafe0123");
    info.windowHalfSamples = 9;
    info.coherenceIlHalf = 2;
    info.coherenceXlHalf = 1;
    info.coherenceTimeHalf = 3;
    info.coherenceWeighting = 1;
    info.ilTraceSpacing = 110.0;
    info.xlTraceSpacing = 55.0;

    const QString path = tempDir_.filePath(QStringLiteral("instfreq.sattr"));
    SattrVolumeWriter writer;
    QString err;
    QVERIFY2(writer.begin(path, info, &err), qPrintable(err));
    std::vector<float> trace(std::size_t(info.nXl) * info.nS);
    for (int il = 0; il < info.nIl; ++il)
    {
      for (int xl = 0; xl < info.nXl; ++xl)
        for (int s = 0; s < info.nS; ++s)
          trace[std::size_t(xl) * info.nS + std::size_t(s)] =
              (il == 2 && xl == 3 && s == 5)
                  ? std::numeric_limits<float>::quiet_NaN()
                  : cellValue(il, xl, s);
      QVERIFY2(writer.writeInline(trace.data(), &err), qPrintable(err));
    }
    QVERIFY2(writer.finish(info.valueMin, info.valueMax, info.validCells, &err),
             qPrintable(err));
    QCOMPARE(writer.inlinesWritten(), qint64(7));

    // 文件层：SATV 魔数（容器身份由魔数区分剖面 SATR）
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.read(4) == QByteArray("SATV", 4));
    f.close();

    SattrVolumeReader reader;
    QVERIFY2(reader.open(path, &err), qPrintable(err));
    const SattrVolumeInfo &back = reader.info();
    QCOMPARE(back.attrId, info.attrId);
    QCOMPARE(back.nIl, info.nIl);
    QCOMPARE(back.nXl, info.nXl);
    QCOMPARE(back.nS, info.nS);
    QCOMPARE(back.blockIl, info.blockIl);
    QCOMPARE(back.blockCount, 3);
    QCOMPARE(back.ilValues, info.ilValues);
    QCOMPARE(back.xlValues, info.xlValues);
    QCOMPARE(back.sampleIntervalMs, info.sampleIntervalMs);
    QCOMPARE(back.startTimeMs, info.startTimeMs);
    QCOMPARE(back.valueMin, info.valueMin);
    QCOMPARE(back.valueMax, info.valueMax);
    QCOMPARE(back.validCells, info.validCells);
    QCOMPARE(back.paramHash, info.paramHash);
    QCOMPARE(back.windowHalfSamples, info.windowHalfSamples);
    QCOMPARE(back.coherenceIlHalf, info.coherenceIlHalf);
    QCOMPARE(back.coherenceXlHalf, info.coherenceXlHalf);
    QCOMPARE(back.coherenceTimeHalf, info.coherenceTimeHalf);
    QCOMPARE(back.coherenceWeighting, info.coherenceWeighting);
    QCOMPARE(back.ilTraceSpacing, info.ilTraceSpacing);
    QCOMPARE(back.xlTraceSpacing, info.xlTraceSpacing);
    QCOMPARE(back.sourceSgyPath, info.sourceSgyPath);

    // Oracle#3：任意 IL/XL/切片面抽取与直算一致（块表寻址正确性）。
    std::vector<float> plane;
    for (int il : {0, 2, 3, 6})
    {
      QVERIFY2(reader.extractInline(il, &plane, &err), qPrintable(err));
      QCOMPARE(plane.size(), std::size_t(info.nXl) * info.nS);
      for (int xl = 0; xl < info.nXl; ++xl)
        for (int s = 0; s < info.nS; ++s)
        {
          const float got = plane[std::size_t(xl) * info.nS + std::size_t(s)];
          if (il == 2 && xl == 3 && s == 5)
            QVERIFY(std::isnan(got));
          else
            QCOMPARE(got, cellValue(il, xl, s));
        }
    }
    for (int xl : {0, 3, 4})
    {
      QVERIFY2(reader.extractXline(xl, &plane, &err), qPrintable(err));
      QCOMPARE(plane.size(), std::size_t(info.nIl) * info.nS);
      for (int il = 0; il < info.nIl; ++il)
        for (int s = 0; s < info.nS; ++s)
        {
          const float got = plane[std::size_t(il) * info.nS + std::size_t(s)];
          if (il == 2 && xl == 3 && s == 5)
            QVERIFY(std::isnan(got));
          else
            QCOMPARE(got, cellValue(il, xl, s));
        }
    }
    for (int s : {0, 5, 12})
    {
      QVERIFY2(reader.extractTime(s, &plane, &err), qPrintable(err));
      QCOMPARE(plane.size(), std::size_t(info.nIl) * info.nXl);
      for (int il = 0; il < info.nIl; ++il)
        for (int xl = 0; xl < info.nXl; ++xl)
        {
          const float got = plane[std::size_t(il) * info.nXl + std::size_t(xl)];
          if (il == 2 && xl == 3 && s == 5)
            QVERIFY(std::isnan(got));
          else
            QCOMPARE(got, cellValue(il, xl, s));
        }
    }
    float cell = 0.f;
    QVERIFY(reader.readCell(6, 4, 12, &cell, &err));
    QCOMPARE(cell, cellValue(6, 4, 12));
    QVERIFY(reader.readCell(2, 3, 5, &cell, &err));
    QVERIFY(std::isnan(cell));
    // 越界如实拒绝
    QVERIFY(!reader.extractInline(7, &plane));
    QVERIFY(!reader.extractXline(5, &plane));
    QVERIFY(!reader.extractTime(13, &plane));
    QVERIFY(!reader.readCell(-1, 0, 0, &cell));
  }

  void volumeRejectsCorruption()
  {
    SattrVolumeInfo info;
    info.attrId = QStringLiteral("rms");
    info.nIl = 2;
    info.nXl = 3;
    info.nS = 4;
    info.ilValues = {10, 11};
    info.xlValues = {20, 21, 22};
    info.blockIl = 8; // 单块（> nIl，末块短路径）
    const QString path = tempDir_.filePath(QStringLiteral("vol_bad.sattr"));
    SattrVolumeWriter writer;
    QString err;
    QVERIFY(writer.begin(path, info, &err));
    std::vector<float> trace(std::size_t(info.nXl) * info.nS, 1.f);
    QVERIFY(writer.writeInline(trace.data(), &err));
    QVERIFY(writer.writeInline(trace.data(), &err));
    QVERIFY(writer.finish(1.0, 1.0, info.nIl * info.nXl * info.nS, &err));

    SattrVolumeReader reader;
    std::vector<float> plane;
    // 魔数损坏
    {
      QByteArray blob;
      {
        QFile src(path);
        QVERIFY(src.open(QIODevice::ReadOnly));
        blob = src.readAll();
      }
      blob[2] = 'X';
      const QString p2 = tempDir_.filePath(QStringLiteral("vol_magic.sattr"));
      QFile f(p2);
      f.open(QIODevice::WriteOnly);
      f.write(blob);
      f.close();
      QVERIFY(!reader.open(p2, &err));
      QVERIFY(err.contains(QStringLiteral("魔数")));
    }
    // payload 截断
    {
      QByteArray blob;
      {
        QFile src(path);
        QVERIFY(src.open(QIODevice::ReadOnly));
        blob = src.readAll();
      }
      blob.chop(10);
      const QString p3 = tempDir_.filePath(QStringLiteral("vol_trunc.sattr"));
      QFile f(p3);
      f.open(QIODevice::WriteOnly);
      f.write(blob);
      f.close();
      QVERIFY(!reader.open(p3, &err));
      QVERIFY(err.contains(QStringLiteral("块表")) ||
              err.contains(QStringLiteral("不符")));
    }
    // 块表寻址被篡改（偏移 +1 → 与解析期望不符）
    {
      QByteArray blob;
      {
        QFile src(path);
        QVERIFY(src.open(QIODevice::ReadOnly));
        blob = src.readAll();
      }
      const quint32 jsonSize =
          qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(blob.constData()) + 8);
      const qint64 tableAt = 12 + qint64(jsonSize) + 4;
      uchar *slot = reinterpret_cast<uchar *>(blob.data()) + tableAt + 8;
      const quint64 bumped =
          qFromLittleEndian<quint64>(slot) + quint64(1);
      qToLittleEndian<quint64>(bumped, slot);
      const QString p4 = tempDir_.filePath(QStringLiteral("vol_table.sattr"));
      QFile f(p4);
      f.open(QIODevice::WriteOnly);
      f.write(blob);
      f.close();
      QVERIFY(!reader.open(p4, &err));
      QVERIFY(err.contains(QStringLiteral("块表")));
    }
    // 写端守卫：未写满 finish 拒绝；越界 writeInline 拒绝；未 begin 拒绝
    {
      SattrVolumeWriter w2;
      QVERIFY(!w2.finish(0.0, 1.0, 1, &err));
      QVERIFY(err.contains(QStringLiteral("未 begin")));
      QVERIFY(w2.begin(tempDir_.filePath(QStringLiteral("vol_short.sattr")),
                       info, &err));
      QVERIFY(w2.writeInline(trace.data(), &err));
      QVERIFY(!w2.finish(0.0, 1.0, 1, &err)); // 少一条
      QVERIFY(err.contains(QStringLiteral("未写满")));
    }
    {
      SattrVolumeWriter w3;
      QVERIFY(w3.begin(tempDir_.filePath(QStringLiteral("vol_over.sattr")),
                       info, &err));
      QVERIFY(w3.writeInline(trace.data(), &err));
      QVERIFY(w3.writeInline(trace.data(), &err));
      QVERIFY(!w3.writeInline(trace.data(), &err)); // 多一条
      QVERIFY(err.contains(QStringLiteral("越界")));
    }
  }
};

QTEST_MAIN(TestSattrIo)
#include "tst_sattrio.moc"
