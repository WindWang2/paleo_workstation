#include <QtTest>

#include "../src/domain/wellsitingplan.h"

using namespace paleo::siting;

class TestDomainWellSitingPlan : public QObject
{
  Q_OBJECT

private slots:
  void scenarioRoundTripMap();
  void scenarioSetEmptyAndIndex();
  void scenarioSetUpsertAppendAndReplace();
  void scenarioSetRemove();
  void retiredAndDisplayNameResolution();
  void scenarioSetSerializationRoundTrip();
  void mutationDemonstration_noDuplicateOnUpsert();
};

void TestDomainWellSitingPlan::scenarioRoundTripMap()
{
  SitingScenario s;
  s.id = QStringLiteral("scenario-1");
  s.name = QStringLiteral("方案A");
  s.plannedWellIds = QStringList{QStringLiteral("w-1"), QStringLiteral("w-2")};
  s.params.insert(QStringLiteral("radius"), 500.0);
  s.metricsBefore.insert(QStringLiteral("hole_count"), 3);
  s.metricsAfter.insert(QStringLiteral("hole_count"), 5);
  s.contributions.insert(QStringLiteral("w-1"), 1234.5);
  s.updatedMs = 1700000000000LL;

  const QVariantMap m = s.toMap();
  const SitingScenario parsed = SitingScenario::fromMap(m);

  QCOMPARE(parsed.id, s.id);
  QCOMPARE(parsed.name, s.name);
  QCOMPARE(parsed.plannedWellIds, s.plannedWellIds);
  QCOMPARE(parsed.params.value(QStringLiteral("radius")).toDouble(), 500.0);
  QCOMPARE(parsed.metricsBefore.value(QStringLiteral("hole_count")).toInt(), 3);
  QCOMPARE(parsed.metricsAfter.value(QStringLiteral("hole_count")).toInt(), 5);
  QCOMPARE(parsed.updatedMs, s.updatedMs);
}

void TestDomainWellSitingPlan::scenarioSetEmptyAndIndex()
{
  ScenarioSet set;
  QVERIFY(set.isEmpty());
  QCOMPARE(set.indexOf(QStringLiteral("nonexistent")), -1);
}

void TestDomainWellSitingPlan::scenarioSetUpsertAppendAndReplace()
{
  ScenarioSet set;
  SitingScenario s1;
  s1.id = QStringLiteral("sc-1");
  s1.name = QStringLiteral("First");

  SitingScenario s2;
  s2.id = QStringLiteral("sc-2");
  s2.name = QStringLiteral("Second");

  set.upsert(s1);
  set.upsert(s2);
  QCOMPARE(set.scenarios.size(), 2);
  QCOMPARE(set.indexOf(QStringLiteral("sc-1")), 0);
  QCOMPARE(set.indexOf(QStringLiteral("sc-2")), 1);

  // 同 ID 替换保位
  SitingScenario s1Updated = s1;
  s1Updated.name = QStringLiteral("First Renamed");
  set.upsert(s1Updated);

  QCOMPARE(set.scenarios.size(), 2);
  QCOMPARE(set.indexOf(QStringLiteral("sc-1")), 0);
  QCOMPARE(set.scenarios.at(0).name, QStringLiteral("First Renamed"));
}

void TestDomainWellSitingPlan::scenarioSetRemove()
{
  ScenarioSet set;
  SitingScenario s1;
  s1.id = QStringLiteral("sc-1");
  set.upsert(s1);

  QVERIFY(!set.remove(QStringLiteral("sc-nonexistent")));
  QCOMPARE(set.scenarios.size(), 1);

  QVERIFY(set.remove(QStringLiteral("sc-1")));
  QVERIFY(set.isEmpty());
}

void TestDomainWellSitingPlan::retiredAndDisplayNameResolution()
{
  ScenarioSet set;
  set.retiredWellIds = QStringList{QStringLiteral("w-dead-1"), QStringLiteral("w-dead-2")};
  set.wellRenames.insert(QStringLiteral("w-1"), QStringLiteral("井1号"));

  QVERIFY(set.isRetired(QStringLiteral("w-dead-1")));
  QVERIFY(!set.isRetired(QStringLiteral("w-1")));

  QCOMPARE(set.displayName(QStringLiteral("w-1"), QStringLiteral("默认")), QStringLiteral("井1号"));
  QCOMPARE(set.displayName(QStringLiteral("w-other"), QStringLiteral("默认")), QStringLiteral("默认"));
}

void TestDomainWellSitingPlan::scenarioSetSerializationRoundTrip()
{
  ScenarioSet set;
  SitingScenario s;
  s.id = QStringLiteral("sc-1");
  s.name = QStringLiteral("Plan 1");
  set.upsert(s);
  set.retiredWellIds << QStringLiteral("retired-1");
  set.wellRenames.insert(QStringLiteral("w-100"), QStringLiteral("CustomName"));

  const QVariantMap m = set.toMap();
  const ScenarioSet parsed = ScenarioSet::fromMap(m);

  QCOMPARE(parsed.scenarios.size(), 1);
  QCOMPARE(parsed.scenarios.at(0).id, QStringLiteral("sc-1"));
  QCOMPARE(parsed.retiredWellIds, QStringList{QStringLiteral("retired-1")});
  QCOMPARE(parsed.wellRenames.value(QStringLiteral("w-100")).toString(), QStringLiteral("CustomName"));
}

void TestDomainWellSitingPlan::mutationDemonstration_noDuplicateOnUpsert()
{
  // 变异测试示范：多次 upsert 同一 ID 绝不能产生列表冗余增长
  ScenarioSet set;
  SitingScenario s;
  s.id = QStringLiteral("sc-unique");
  s.name = QStringLiteral("Initial");

  for (int i = 0; i < 10; ++i)
  {
    s.name = QStringLiteral("Update %1").arg(i);
    set.upsert(s);
  }

  QCOMPARE(set.scenarios.size(), 1);
  QCOMPARE(set.scenarios.at(0).name, QStringLiteral("Update 9"));
}

QTEST_GUILESS_MAIN(TestDomainWellSitingPlan)
#include "tst_domain_wellsitingplan.moc"
