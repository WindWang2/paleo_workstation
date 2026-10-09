#include <QtTest>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QtEndian>
#include <cmath>
#include <cstring>

#include "../src/services/seismichorizoncluster.h"

// 取窗契约锚（f0e7b10d「直接按原始层位提取地震窗并聚类」的文档化契约，
// docs/progress/seismichorizoncluster.md）：Scatter 路径按层位 TWT ±12ms
// 开窗（seismichorizoncluster.cpp:221-225）提均值/RMS/过零率三特征。
// 方言体（crossline@192 恒 0、xline 取 CDP@21、角点取 181-188——同
// tst_segy dialectLayout 回归的布局）× SMI 散点层位。
namespace
{
// 4 inline × 4 xline 方言合成体；dt=4ms、ns=50、t0=0。
// 第 (il,xl) 道在样本 22..28（=TWT 100ms ±12ms 窗）写入组签名：
//   xl 偶 → 恒 +3（低 RMS）；xl 奇 → ±30 交替（高 RMS/高过零率）。
// 其余样本恒 1.0。
QByteArray buildVolume()
{
  const int perLine = 4, lines = 4, ns = 50;
  QByteArray out(3200, ' ');
  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(static_cast<qint16>(4000), reinterpret_cast<uchar *>(binHdr.data() + 16)); // dt µs
  qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(binHdr.data() + 20));
  qToBigEndian<qint16>(qint16(5), reinterpret_cast<uchar *>(binHdr.data() + 24)); // IEEE f32
  qToBigEndian<qint16>(qint16(0x0100), reinterpret_cast<uchar *>(binHdr.data() + 300)); // rev1
  qToBigEndian<qint16>(qint16(0), reinterpret_cast<uchar *>(binHdr.data() + 304)); // 无扩展头
  out.append(binHdr);
  for (int i = 0; i < perLine * lines; ++i)
  {
    const int il = 100 + i / perLine, xl = 2000 + i % perLine;
    QByteArray trHdr(240, 0);
    qToBigEndian<qint32>(qint32(i + 1), reinterpret_cast<uchar *>(trHdr.data() + 0));
    qToBigEndian<qint32>(qint32(xl), reinterpret_cast<uchar *>(trHdr.data() + 20)); // CDP=方言 xline
    qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(trHdr.data() + 114));
    qToBigEndian<qint16>(static_cast<qint16>(4000), reinterpret_cast<uchar *>(trHdr.data() + 116));
    qToBigEndian<qint32>(qint32(10 * (xl - 2000)), reinterpret_cast<uchar *>(trHdr.data() + 180)); // CDP X
    qToBigEndian<qint32>(qint32(20 * (il - 100)), reinterpret_cast<uchar *>(trHdr.data() + 184)); // CDP Y
    qToBigEndian<qint32>(qint32(il), reinterpret_cast<uchar *>(trHdr.data() + 188)); // inline
    out.append(trHdr);
    for (int s = 0; s < ns; ++s)
    {
      float value = 1.0f;
      if (s >= 22 && s <= 28)
        value = (xl % 2 == 0) ? 3.0f : ((s % 2 == 0) ? 30.0f : -30.0f);
      quint32 raw;
      std::memcpy(&raw, &value, sizeof(float));
      const quint32 be = qToBigEndian<quint32>(raw);
      out.append(reinterpret_cast<const char *>(&be), sizeof(be));
    }
  }
  return out;
}

// SMI 散点层位：4×4 全网格，TWT 全部 timeMs（毫秒）。P1/P2/P3 与地震
// 测网角点一致（x=10·(xl-2000)、y=20·(il-100)，corner 校验 ≤1m）。
QByteArray buildHorizon(double timeMs)
{
  QByteArray text;
  text += "# Grid_size:4x4# Survey(Inline,Crossline,x,y)\n";
  text += "# P1:      100,      2000,     0.00000,     0.00000\n";
  text += "# P2:      100,      2003,    30.00000,     0.00000\n";
  text += "# P3:      103,      2003,    30.00000,    60.00000\n";
  text += "# Z_units: ms\n";
  for (int il = 100; il <= 103; ++il)
    for (int xl = 2000; xl <= 2003; ++xl)
      text += QStringLiteral("%1 %2 %3 %4 %5\n")
                  .arg(10.0 * (xl - 2000), 0, 'f', 5)
                  .arg(20.0 * (il - 100), 0, 'f', 5)
                  .arg(timeMs, 0, 'f', 1)
                  .arg(il)
                  .arg(xl)
                  .toUtf8();
  return text;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
  QSaveFile out(path);
  out.setDirectWriteFallback(false);
  if (!out.open(QIODevice::WriteOnly))
    return false;
  out.write(bytes);
  return out.commit();
}
}

class TestSeismicHorizonCluster : public QObject
{
  Q_OBJECT
private slots:
  // 窗内（±12ms）特征分离成两簇，相码按 RMS 升序钉序：低 RMS（xl 偶，
  // 恒 +3）→ codes[0]，高 RMS（xl 奇，±30 交替）→ codes[1]。
  void scatterWindowSeparatesInWindowSignatures()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("vol.sgy"));
    const QString smi = dir.filePath(QStringLiteral("h.dat"));
    QVERIFY(writeFile(sgy, buildVolume()));
    QVERIFY(writeFile(smi, buildHorizon(100.0)));
    QTemporaryDir cache;
    const auto result = clusterSeismicHorizon(
        sgy, smi, cache.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.columns, 4);
    QCOMPARE(result.rows, 4);
    QCOMPARE(result.validCells, 16);
    for (int il = 100; il <= 103; ++il)
      for (int xl = 2000; xl <= 2003; ++xl)
      {
        const int cell = (103 - il) * 4 + (xl - 2000);
        QCOMPARE(result.cells[cell], (xl % 2 == 0) ? 1 : 2);
      }
  }

  // 同一体、层位挪到 TWT 160ms：窗（148-172ms → 样本 37-43）落在恒 1.0
  // 背景上——窗外签名（88-112ms 异常）不得影响特征，全体同簇 codes[0]。
  void scatterWindowIgnoresSignaturesOutsideWindow()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("vol.sgy"));
    const QString smi = dir.filePath(QStringLiteral("h.dat"));
    QVERIFY(writeFile(sgy, buildVolume()));
    QVERIFY(writeFile(smi, buildHorizon(160.0)));
    QTemporaryDir cache;
    const auto result = clusterSeismicHorizon(
        sgy, smi, cache.path(), {1, 2}, {}, {}, SeismicHorizonSource::Scatter);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.validCells, 16);
    for (int cell = 0; cell < result.cells.size(); ++cell)
      QCOMPARE(result.cells[cell], 1);
  }
};

QTEST_GUILESS_MAIN(TestSeismicHorizonCluster)
#include "tst_seismichorizoncluster.moc"
