// tests/tst_catalog_scale — B4（wave/deepen-perf）：catalog.sqlite 查询索引
// （ADR 0056 / TODOS P3）触发条件实测。按需运行（重量级夹具）：
//   PALEO_CATALOG_SCALE=100000 ctest -R catalog_scale   # 100k 资产
//   PALEO_CATALOG_SCALE=10000  ctest -R catalog_scale   # 10k 对照
// 未设 env 时 QSKIP——常规 ctest 不吃这几十秒灌库成本。
// 口径：打开（JSON 全量解析）/ 列表枚举 / 1000 次 entityById（邻接索引）/
// 1000 次 linksForEntity / 类型计数 / 索引对账。数字进 qInfo 与
// docs/perf/BASELINE.md（评估记录），机器无关退化由 tst_perf_regress 的
// 5k/1k 比率门持续看护。
#include <QtTest>
#include <QElapsedTimer>
#include <QHash>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/perffixtures.h"

class TestCatalogScale : public QObject
{
  Q_OBJECT

  private slots:

    void scaleMeasurements()
    {
      bool okScale = false;
      const int n = qEnvironmentVariableIntValue("PALEO_CATALOG_SCALE", &okScale);
      if (!okScale || n <= 0)
        QSKIP("PALEO_CATALOG_SCALE not set — heavy fixture measurement skipped");
      QVERIFY(n >= 1000);

      QTemporaryDir dir;
      QElapsedTimer t;
      t.start();
      QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), n));
      qInfo("PERF catalog build %d assets(ms): %lld", n, t.elapsed());

      DataCatalog cat;
      QString err;
      t.start();
      QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
      qInfo("PERF catalog open %d(ms): %lld", n, t.elapsed());

      t.start();
      qint64 nameBytes = 0;
      const auto entities = cat.entities();
      for (const CatalogEntity &e : entities)
        nameBytes += e.name.size();
      qInfo("PERF list entities %d(ms): %lld", int(entities.size()), t.elapsed());
      QCOMPARE(entities.size(), n);

      t.start();
      for (int i = 1; i <= 1000; ++i)
        cat.entityById(QStringLiteral("well-%1").arg((i * 7) % n + 1, 6, 10,
                                                     QLatin1Char('0')));
      qInfo("PERF 1000x entityById(ms): %lld", t.elapsed());

      t.start();
      int linkTotal = 0;
      for (int i = 1; i <= 1000; ++i)
        linkTotal += cat.linksForEntity(
                         QStringLiteral("well-%1").arg((i * 7) % n + 1, 6, 10,
                                                       QLatin1Char('0')))
                         .size();
      qInfo("PERF 1000x linksForEntity(ms): %lld total=%d", t.elapsed(), linkTotal);

      t.start();
      const QHash<QString, int> counts = cat.entityCountsByType();
      qInfo("PERF countsByType(ms): %lld wells=%d", t.elapsed(),
            int(counts.value(QStringLiteral("well"))));
      QCOMPARE(counts.value(QStringLiteral("well")), n);

      QString mismatch;
      QVERIFY2(cat.indexHealthy(&mismatch), qPrintable(mismatch));

      // 夹具身份自证：每资产 1 链接（populateSyntheticCatalog 契约）。
      QCOMPARE(int(cat.links().size()), n);
      QVERIFY(nameBytes > 0);
    }
};

int main(int argc, char *argv[])
{
  // 100k 资产灌库（每资产一个受管 RAW 文件 + JSON 落盘）>300s——QtTest 的
  // 函数级默认超时会误杀本测量；未显式设置时放宽到 20 分钟。
  if (qEnvironmentVariableIsEmpty("QTEST_FUNCTION_TIMEOUT"))
    qputenv("QTEST_FUNCTION_TIMEOUT", "1200000");
  TestCatalogScale tc;
  return QTest::qExec(&tc, argc, argv);
}
#include "tst_catalog_scale.moc"
