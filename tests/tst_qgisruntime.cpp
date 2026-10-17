// tst_qgisruntime — QgisRuntime 纯解析面的契约测试（#483）：prefix 优先级
// 「QGIS_PREFIX_PATH env > 显式实参 > 缺省序」（env-first 历史语义保持——
// 数十个测试 main 以 "/usr" 兜底、依赖 CI env 压制；CLI --prefix 压 env 的
// 产品行为由 main() 先 qputenv 实现）。不触碰 QgsApplication——
// initialize/shutdown 的进程级生命周期由 tst_mapbook 等整装测试覆盖。
#include <QtTest>

#include "../src/qgis/qgisruntime.h"

class TestQgisRuntime : public QObject
{
  Q_OBJECT
private slots:
  void envBeatsExplicit();
  void explicitBeatsDefault();
  void emptyFallsBackToDefault();
};

void TestQgisRuntime::envBeatsExplicit()
{
  QCOMPARE(QgisRuntime::resolvePrefixPath(QStringLiteral("/arg/prefix"),
                                          QStringLiteral("/env/prefix")),
           QStringLiteral("/env/prefix"));
}

void TestQgisRuntime::explicitBeatsDefault()
{
  QCOMPARE(QgisRuntime::resolvePrefixPath(QStringLiteral("/arg/prefix"), QString()),
           QStringLiteral("/arg/prefix"));
}

void TestQgisRuntime::emptyFallsBackToDefault()
{
  const QString resolved = QgisRuntime::resolvePrefixPath(QString(), QString());
  QVERIFY(!resolved.isEmpty());
  QCOMPARE(resolved, QgisRuntime::defaultPrefixPath());
}

QTEST_GUILESS_MAIN(TestQgisRuntime)
#include "tst_qgisruntime.moc"
