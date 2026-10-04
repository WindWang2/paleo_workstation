// 层：测试壳
// tst_lasperf — goal/data-perf：LAS 读面性能探针（方向 21 轮2/轮3）。
//
// 口径（为什么不写死绝对毫秒阈值）：本机负载/文件系统抖动会让绝对毫秒数在 CI
// 之间漂移，所以主门是**在同一进程内同时跑两条实现取比率**：
//   · 在测实现  = LasParser::parseDoc / parseRange（现在的字节级解析面）
//   · 参照实现  = referenceParseDataSection()：把优化前的算法原样重述（逐 token
//     QByteArray::fromRawData + toDouble + QVector::append），解析同一份文件。
// 退化到旧算法 → 比率回落到 ~1.0，必红；绝对上限只拦挂死。
//
// 另有 correctness 面：结果与参照实现逐位比对（memcmp 浮点位），
// 保证「提速不换语义」。
#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "../src/io/lasparser.h"
#include "../src/io/perffixtures.h"

namespace
{
  // 优化前的数据节算法（逐字节分行 → 逐 token QByteArray::fromRawData().toDouble()
  // → QVector<double>::append）。只作为性能参照与逐位等价基线，不给产品用。
  QList<LasCurve> referenceParseDataSection(const QByteArray &raw, qint64 asciiOffset,
                                            const QStringList &names, double nullValue)
  {
    QList<LasCurve> cols;
    cols.reserve(names.size());
    for (const QString &n : names)
      cols.append({n, QString(), QString(), {}});

    const int nCurves = cols.size();
    const qint64 n = raw.size();
    qint64 i = qMax<qint64>(0, asciiOffset);
    while (i < n)
    {
      qint64 eol = i;
      while (eol < n && raw.at(eol) != '\n' && raw.at(eol) != '\r')
        ++eol;
      if (eol > i)
      {
        int col = 0;
        qint64 p = i;
        while (p < eol && col < nCurves)
        {
          while (p < eol && (raw.at(p) == ' ' || raw.at(p) == '\t'))
            ++p;
          if (p >= eol)
            break;
          qint64 q = p;
          while (q < eol && raw.at(q) != ' ' && raw.at(q) != '\t')
            ++q;
          bool ok = false;
          const double v =
              QByteArray::fromRawData(raw.constData() + p, static_cast<int>(q - p)).toDouble(&ok);
          if (ok && v == v && v != nullValue)
            cols[col].values.append(v);
          else
            cols[col].values.append(std::nan(""));
          ++col;
          p = q;
        }
        if (col < nCurves)
          for (; col < nCurves; ++col)
            cols[col].values.append(std::nan(""));
      }
      if (eol >= n)
        break;
      i = eol + 1;
      if (raw.at(eol) == '\r' && i < n && raw.at(i) == '\n')
        ++i;
    }
    return cols;
  }

  double medianMs(const std::function<void()> &fn, int reps)
  {
    QVector<double> xs;
    xs.reserve(reps);
    for (int i = 0; i < reps; ++i)
    {
      QElapsedTimer c;
      c.start();
      fn();
      xs.append(c.nsecsElapsed() / 1.0e6);
    }
    std::sort(xs.begin(), xs.end());
    return xs.at(xs.size() / 2);
  }
} // namespace

class LasPerfTests : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(m_dir.isValid());
  }

  // 1M 行大 LAS：快路径 parseDoc 对旧算法的比率门。
  void millionRowParseRatioGate();

  // 优化后的解析结果必须与旧算法逐位一致（提速不换语义）。
  void parseResultIsBitIdenticalToReference();

  // parseRange 的区间读必须与全量读的同区间逐位一致。
  void parseRangeAgreesWithFullRead();

  // 多文件并发读（IO 可并行）：两文件并行 < 串行。
  void concurrentMultiFileReadIsFasterThanSerial();

private:
  QTemporaryDir m_dir;
};

void LasPerfTests::millionRowParseRatioGate()
{
  const QString las = m_dir.filePath(QStringLiteral("million.las"));
  QVERIFY2(PerfFixtures::makeSyntheticLas(las, 1000000),
           "synthetic 1M-row LAS fixture failed");
  const QFileInfo info(las);
  QVERIFY(info.size() > 0);

  QStringList names;
  LasDoc doc;
  const double nowMs = medianMs([&] { doc = LasParser::parseDoc(las); }, 3);
  QVERIFY2(doc.ok, qPrintable(doc.error));

  // 参照实现：读同一份文件的同一段，跑旧算法。
  double refMs = 0.0;
  QList<LasCurve> refCols;
  {
    QFile f(las);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    LasParser::SectionMap map;
    QVERIFY(LasParser::scanSections(raw, &map, nullptr));
    refMs = medianMs(
        [&] { refCols = referenceParseDataSection(raw, map.asciiDataOffset, map.header.curveNames,
                                                  map.header.nullValue); },
        3);
  }

  const double bytes = static_cast<double>(info.size());
  qInfo("lasperf 1M rows: parseDoc=%.1fms reference=%.1fms ratio=%.3f file=%.1fMB "
        "(%.1f MB/s vs %.1f MB/s)",
        nowMs, refMs, refMs > 0 ? nowMs / refMs : -1.0, bytes / (1024.0 * 1024.0),
        bytes / (1024.0 * 1024.0) / (nowMs / 1000.0), bytes / (1024.0 * 1024.0) / (refMs / 1000.0));

  QCOMPARE(doc.curves.size(), 5);
  QCOMPARE(doc.curves.first().values.size(), 1000000);

  // 主门（比率）：快路径不得退化到旧算法量级——退化则比率→1，必红。
  QVERIFY2(refMs > 0.0 && nowMs < refMs / 1.5,
           qPrintable(QStringLiteral("parseDoc %1ms is not >=1.5x faster than the reference %2ms "
                                     "(ratio %3) — LAS read面退化")
                          .arg(nowMs, 0, 'f', 1)
                          .arg(refMs, 0, 'f', 1)
                          .arg(refMs > 0 ? nowMs / refMs : -1.0, 0, 'f', 3)));
  // sanity 上限：只拦挂死，不判机器快慢。
  QVERIFY2(nowMs < 60000.0, qPrintable(QStringLiteral("parseDoc %1ms sanity").arg(nowMs)));
}

void LasPerfTests::parseResultIsBitIdenticalToReference()
{
  const QString las = m_dir.filePath(QStringLiteral("agree.las"));
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 200000));

  const LasDoc doc = LasParser::parseDoc(las);
  QVERIFY2(doc.ok, qPrintable(doc.error));

  QFile f(las);
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QByteArray raw = f.readAll();
  LasParser::SectionMap map;
  QVERIFY(LasParser::scanSections(raw, &map, nullptr));
  const QList<LasCurve> ref =
      referenceParseDataSection(raw, map.asciiDataOffset, map.header.curveNames,
                                map.header.nullValue);

  QCOMPARE(doc.curves.size(), ref.size());
  for (int c = 0; c < ref.size(); ++c)
  {
    QCOMPARE(doc.curves.at(c).values.size(), ref.at(c).values.size());
    QVERIFY2(std::memcmp(doc.curves.at(c).values.constData(), ref.at(c).values.constData(),
                         static_cast<std::size_t>(ref.at(c).values.size()) * sizeof(double)) == 0,
             qPrintable(QStringLiteral("curve %1 (%2) differs bitwise from the reference conversion")
                            .arg(c)
                            .arg(map.header.curveNames.value(c))));
  }
}

void LasPerfTests::parseRangeAgreesWithFullRead()
{
  const QString las = m_dir.filePath(QStringLiteral("range.las"));
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 60000));

  const LasDoc full = LasParser::parseDoc(las);
  QVERIFY2(full.ok, qPrintable(full.error));

  QStringList partNames;
  QList<LasCurve> partCurves;
  QVERIFY(LasParser::parseRange(las, 1000, 9000, partNames, partCurves, nullptr, nullptr));
  QCOMPARE(partCurves.size(), full.curves.size());
  for (int c = 0; c < partCurves.size(); ++c)
  {
    QCOMPARE(partCurves.at(c).values.size(), 8000);
    QVERIFY2(std::memcmp(partCurves.at(c).values.constData(),
                         full.curves.at(c).values.constData() + 1000,
                         static_cast<std::size_t>(8000) * sizeof(double)) == 0,
             qPrintable(QStringLiteral("range window of curve %1 differs from the full read").arg(c)));
  }
}

void LasPerfTests::concurrentMultiFileReadIsFasterThanSerial()
{
  // 两口井各自的头 + 数据节；读 IO 天然可并行。corpus 取大到看得见差异的量。
  QVector<QString> paths;
  for (int i = 0; i < 4; ++i)
  {
    const QString p = m_dir.filePath(QStringLiteral("well_%1.las").arg(i));
    QVERIFY(PerfFixtures::makeSyntheticLas(p, 300000));
    paths.append(p);
  }

  auto readOne = [](const QString &p) -> int {
    QStringList names;
    QList<LasCurve> curves;
    if (!LasParser::parseRange(p, 0, -1, names, curves, nullptr, nullptr))
      return -1;
    return curves.isEmpty() ? 0 : curves.first().values.size();
  };

  const double serialMs = medianMs(
      [&] {
        for (const QString &p : paths)
          QVERIFY(readOne(p) == 300000);
      },
      3);

  // 并行：每个文件一条 std::thread（读 IO 天然可并行；无共享可变状态）。
  const double parallelMs = medianMs(
      [&] {
        std::vector<int> rows(paths.size(), -1);
        std::vector<std::thread> pool;
        pool.reserve(paths.size());
        for (std::size_t i = 0; i < static_cast<std::size_t>(paths.size()); ++i)
        {
          pool.emplace_back([&readOne, &paths, &rows, i] { rows[i] = readOne(paths.at(int(i))); });
        }
        for (std::thread &t : pool)
          t.join();
        for (int r : rows)
          QCOMPARE(r, 300000);
      },
      3);

  qInfo("lasperf concurrent: serial=%.1fms parallel=%.1fms ratio=%.3f (%d files)",
        serialMs, parallelMs, serialMs > 0 ? parallelMs / serialMs : -1.0, int(paths.size()));

  // 主门（比率）：并行读不得比串行慢；门留出线程池建立等固定开销的余量。
  QVERIFY2(serialMs > 0.0 && parallelMs < serialMs,
           qPrintable(QStringLiteral("parallel read %1ms is not faster than serial %2ms")
                          .arg(parallelMs, 0, 'f', 1)
                          .arg(serialMs, 0, 'f', 1)));
  // sanity 上限：只拦线程池挂死。
  QVERIFY2(parallelMs < 60000.0,
           qPrintable(QStringLiteral("parallel read %1ms sanity").arg(parallelMs)));
}

QTEST_MAIN(LasPerfTests)
#include "tst_lasperf.moc"
