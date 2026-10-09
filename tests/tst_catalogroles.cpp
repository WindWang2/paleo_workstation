// 层：测试壳
#include <QtTest>
#include <QHash>
#include <QSet>

#include "../src/catalog/catalogroles.h"

// 方向 92：catalogRoles() 常量词表单测——表完整性（新角色落组/序唯一）、
// 查询面（树角色序等价保持）、词表外未分组兜底、AI 清单生成。
class TestCatalogRoles : public QObject
{
  Q_OBJECT
private slots:
  void tableIntegrity();
  void wellFamilyOrderPreservesTreeSequence();
  void lookupFaces();
  void ungroupedFallback();
  void aiListingSkipsEmptyFamilies();
};

void TestCatalogRoles::tableIntegrity()
{
  const QVector<CatalogRoleInfo> roles = catalogRoles();
  QVERIFY(!roles.isEmpty());

  QSet<QString> groupKeys;
  for (const CatalogRoleGroup &g : catalogRoleGroups())
    groupKeys.insert(g.key);
  // 四族键齐备（mapproduct 当前空族，键预留）。
  QVERIFY(groupKeys.contains(QStringLiteral("well")));
  QVERIFY(groupKeys.contains(QStringLiteral("seismic")));
  QVERIFY(groupKeys.contains(QStringLiteral("mapproduct")));
  QVERIFY(groupKeys.contains(QStringLiteral("misc")));

  QSet<int> orders;
  QSet<QString> seenRoles;
  for (const CatalogRoleInfo &r : roles)
  {
    QVERIFY2(!r.role.isEmpty(), "角色名非空");
    QVERIFY2(!r.display.isEmpty(), qPrintable(r.role + " 显示名非空"));
    QVERIFY2(groupKeys.contains(r.group),
             qPrintable(r.role + " 分组 " + r.group + " 不在族表"));
    QVERIFY2(!orders.contains(r.order),
             qPrintable(r.role + " 序号 " + QString::number(r.order) + " 重复"));
    orders.insert(r.order);
    QVERIFY2(r.order < kUngroupedRoleOrder, "表内序必须小于未分组兜底序");
    seenRoles.insert(r.role);
  }

  // 新角色落组：各族成员清单精确对拍（新角色必须先进表、进对族）。
  QStringList wellFamily, seismicFamily, miscFamily;
  for (const CatalogRoleInfo &r : roles)
  {
    if (r.group == QLatin1String("well"))
      wellFamily << r.role;
    else if (r.group == QLatin1String("seismic"))
      seismicFamily << r.role;
    else if (r.group == QLatin1String("misc"))
      miscFamily << r.role;
    else
      QFAIL("mapproduct 族当前必须为空（产物挂接角色出现时先扩文档再入表）");
  }
  QCOMPARE(wellFamily,
           QStringList({QStringLiteral("well_log"), QStringLiteral("tops"),
                        QStringLiteral("time_depth"), QStringLiteral("well_head"),
                        QStringLiteral("core"), QStringLiteral("trajectory"),
                        QStringLiteral("cuttings"), QStringLiteral("lab_analysis"),
                        QStringLiteral("interpretation"), QStringLiteral("qc"),
                        QStringLiteral("other")}));
  QCOMPARE(seismicFamily,
           QStringList({QStringLiteral("seismic_volume"), QStringLiteral("horizon"),
                        QStringLiteral("geometry"), QStringLiteral("velocity"),
                        QStringLiteral("fault"), QStringLiteral("interpretation"),
                        QStringLiteral("other")}));
  QCOMPARE(miscFamily,
           QStringList({QStringLiteral("reference"), QStringLiteral("framework_unit"),
                        QStringLiteral("siting_note")}));

  // 同名跨族角色裸查命中表序首个（井段在前，与 RoleRegistry 约定一致）。
  const CatalogRoleInfo *interp = catalogRoleInfo(QStringLiteral("interpretation"));
  QVERIFY(interp);
  QCOMPARE(interp->group, QStringLiteral("well"));
}

void TestCatalogRoles::wellFamilyOrderPreservesTreeSequence()
{
  // 等价重构红线：树面既有井节点角色序 well_log(0) < tops(1) <
  // time_depth(2) < well_head(3) 原样保持。
  QVERIFY(catalogRoleOrder(QStringLiteral("well_log")) == 0);
  QVERIFY(catalogRoleOrder(QStringLiteral("well_log")) <
          catalogRoleOrder(QStringLiteral("tops")));
  QVERIFY(catalogRoleOrder(QStringLiteral("tops")) <
          catalogRoleOrder(QStringLiteral("time_depth")));
  QVERIFY(catalogRoleOrder(QStringLiteral("time_depth")) <
          catalogRoleOrder(QStringLiteral("well_head")));
}

void TestCatalogRoles::lookupFaces()
{
  QCOMPARE(catalogRoleGroup(QStringLiteral("well_head")), QStringLiteral("well"));
  QCOMPARE(catalogRoleGroup(QStringLiteral("horizon")), QStringLiteral("seismic"));
  QCOMPARE(catalogRoleGroup(QStringLiteral("reference")), QStringLiteral("misc"));
  // 显示名以树面现状为准（等价对拍锚点）。
  QCOMPARE(catalogRoleDisplay(QStringLiteral("tops")), QStringLiteral("井分层"));
  QCOMPARE(catalogRoleDisplay(QStringLiteral("core")), QStringLiteral("岩心照片"));
  QCOMPARE(catalogRoleDisplay(QStringLiteral("well_log")), QStringLiteral("测井曲线"));
  QVERIFY(catalogRoleInfo(QStringLiteral("well_log")));
  QVERIFY(!catalogRoleInfo(QStringLiteral("no_such_role")));
}

void TestCatalogRoles::ungroupedFallback()
{
  const QString custom = QStringLiteral("my_custom_role");
  QCOMPARE(catalogRoleGroup(custom), QString::fromLatin1(kUngroupedRoleGroup));
  QCOMPARE(catalogRoleOrder(custom), kUngroupedRoleOrder);
  // 词表外显示名原样返回（不臆造翻译）。
  QCOMPARE(catalogRoleDisplay(custom), custom);
  QVERIFY(catalogRoleOrder(custom) > catalogRoleOrder(QStringLiteral("other")));
}

void TestCatalogRoles::aiListingSkipsEmptyFamilies()
{
  const QString listing = catalogRoleListForAi();
  QVERIFY(!listing.isEmpty());
  // 角色清单覆盖三实族 + 分组名单源（族显示名进词面）。
  QVERIFY(listing.contains(QStringLiteral("well_head")));
  QVERIFY(listing.contains(QStringLiteral("well_log")));
  QVERIFY(listing.contains(QStringLiteral("tops")));
  QVERIFY(listing.contains(QStringLiteral("seismic_volume")));
  QVERIFY(listing.contains(QStringLiteral("reference")));
  QVERIFY(listing.contains(QStringLiteral("井：")));
  QVERIFY(listing.contains(QStringLiteral("地震：")));
  QVERIFY(listing.contains(QStringLiteral("辅助：")));
  // 空族（mapproduct）不进 AI 词面。
  QVERIFY(!listing.contains(QStringLiteral("成果图件")));
}

QTEST_MAIN(TestCatalogRoles)
#include "tst_catalogroles.moc"
