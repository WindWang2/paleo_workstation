#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>

#include "../src/catalog/datacatalog.h"
#include "../src/catalog/roleregistry.h"

// A 包（docs/DATA_FABRIC_ADOPTION.md）：工程作用域角色词表。
// 覆盖：内置 well/seismic_survey 词表、forEntity 过滤、未知角色如实空、
// fromJson 覆盖/新增语义、open() 装载 project_area.json roles 节与回退。
class TestRoles : public QObject
{
  Q_OBJECT

private slots:
  void defaultsContainWellAndSurveyVocab();
  void forEntityFiltersByEntityType();
  void unknownRoleIsNotFound();
  void fromJsonOverridesAndAdds();
  void openLoadsProjectAreaRoles();
  void openWithoutAreaFileYieldsDefaults();
  void corruptAreaFileStillOpensWithDefaults();
};

void TestRoles::defaultsContainWellAndSurveyVocab()
{
  const RoleRegistry reg = RoleRegistry::defaults();
  // well 9 角色 + survey 7 角色全量已知——缺一个都算词表缺口。
  const QStringList wellRoles{
      QStringLiteral("well_head"), QStringLiteral("well_log"),
      QStringLiteral("trajectory"), QStringLiteral("tops"),
      QStringLiteral("time_depth"), QStringLiteral("core"),
      QStringLiteral("interpretation"), QStringLiteral("qc"),
      QStringLiteral("other")};
  const QStringList surveyRoles{
      QStringLiteral("seismic_volume"), QStringLiteral("geometry"),
      QStringLiteral("velocity"), QStringLiteral("horizon"),
      QStringLiteral("fault"), QStringLiteral("interpretation"),
      QStringLiteral("other")};
  for (const QString &r : wellRoles)
  {
    QVERIFY(reg.isKnown(r));
    QVERIFY(reg.find(r) != nullptr);
  }
  for (const QString &r : surveyRoles)
    QVERIFY(reg.isKnown(r));

  // 定义带中文显示名与 RAW 建议阶段；well_log 是词表唯一 ordered=true 的
  // 角色；内置成员数一律不限（上游 0..N 移植，见 roleregistry.h 注释）。
  const RoleDef *log = reg.find(QStringLiteral("well_log"));
  QVERIFY(log != nullptr);
  QCOMPARE(log->display, QStringLiteral("测井曲线"));
  QCOMPARE(log->entityTypes, QStringList{QStringLiteral("well")});
  QCOMPARE(log->maxCount, 0);
  QVERIFY(log->ordered);
  QCOMPARE(log->stageDefault, QStringLiteral("RAW"));
  QVERIFY(!reg.find(QStringLiteral("well_head"))->display.isEmpty());
  QVERIFY(!reg.find(QStringLiteral("seismic_volume"))->description.isEmpty());

  // 裸角色 find 命中表序首个定义（well 段在前）——与上游约定一致。
  QCOMPARE(reg.find(QStringLiteral("interpretation"))->display,
           QStringLiteral("井周解释"));
  QCOMPARE(reg.find(QStringLiteral("other"))->entityTypes,
           QStringList{QStringLiteral("well")});
}

void TestRoles::forEntityFiltersByEntityType()
{
  const RoleRegistry reg = RoleRegistry::defaults();
  const QVector<RoleDef> wellDefs = reg.forEntity(QStringLiteral("well"));
  QCOMPARE(wellDefs.size(), 9);
  QCOMPARE(wellDefs.first().role, QStringLiteral("well_head"));
  QCOMPARE(wellDefs.last().role, QStringLiteral("other")); // other 收尾
  for (const RoleDef &d : wellDefs)
    QVERIFY(d.entityTypes.contains(QStringLiteral("well")));

  const QVector<RoleDef> surveyDefs =
      reg.forEntity(QStringLiteral("seismic_survey"));
  QCOMPARE(surveyDefs.size(), 7);
  QCOMPARE(surveyDefs.first().role, QStringLiteral("seismic_volume"));
  QCOMPARE(surveyDefs.last().role, QStringLiteral("other"));
  for (const RoleDef &d : surveyDefs)
    QVERIFY(d.entityTypes.contains(QStringLiteral("seismic_survey")));

  // 同名跨表角色过滤后各归各域。
  QCOMPARE(wellDefs.at(6).role, QStringLiteral("interpretation"));
  QCOMPARE(wellDefs.at(6).display, QStringLiteral("井周解释"));
  QCOMPARE(surveyDefs.at(5).role, QStringLiteral("interpretation"));
  QCOMPARE(surveyDefs.at(5).display, QStringLiteral("调查解释"));

  // 无词表的实体类型与空类型 → 如实空集。
  QVERIFY(reg.forEntity(QStringLiteral("auxiliary")).isEmpty());
  QVERIFY(reg.forEntity(QStringLiteral("sequence_boundary")).isEmpty());
  QVERIFY(reg.forEntity(QString()).isEmpty());
}

void TestRoles::unknownRoleIsNotFound()
{
  const RoleRegistry reg = RoleRegistry::defaults();
  QVERIFY(reg.find(QStringLiteral("nonsense")) == nullptr);
  QVERIFY(!reg.isKnown(QStringLiteral("nonsense")));
  QVERIFY(reg.find(QString()) == nullptr);
  QVERIFY(!reg.isKnown(QString()));
}

void TestRoles::fromJsonOverridesAndAdds()
{
  QJsonObject roles;
  // 不带 entity_types 的覆盖 = 字段级补丁：写了的键替换，没写的保留。
  roles.insert(QStringLiteral("well_log"),
               QJsonObject{{QStringLiteral("display"),
                            QStringLiteral("改名单曲线")}});
  // 带 entity_types 且 (role, 域) 无内置定义 → 追加同域新定义；
  // well 的 tops 原样不动。
  roles.insert(QStringLiteral("tops"),
               QJsonObject{
                   {QStringLiteral("entity_types"),
                    QJsonArray{QStringLiteral("seismic_survey")}},
                   {QStringLiteral("display"), QStringLiteral("调查分层")}});
  // 全新角色追加进词表。
  roles.insert(QStringLiteral("custom_marker"),
               QJsonObject{
                   {QStringLiteral("entity_types"),
                    QJsonArray{QStringLiteral("well")}},
                   {QStringLiteral("max_count"), 1},
                   {QStringLiteral("ordered"), true},
                   {QStringLiteral("display"), QStringLiteral("定制标记")},
                   {QStringLiteral("stage_default"), QStringLiteral("DERIVED")},
                   {QStringLiteral("description"),
                    QStringLiteral("工区自定义角色")}});

  const RoleRegistry reg = RoleRegistry::fromJson(roles);

  const RoleDef *log = reg.find(QStringLiteral("well_log"));
  QVERIFY(log != nullptr);
  QCOMPARE(log->display, QStringLiteral("改名单曲线"));
  QCOMPARE(log->entityTypes, QStringList{QStringLiteral("well")}); // 域保留
  QVERIFY(log->ordered);                                           // 未写字段保留

  QCOMPARE(reg.find(QStringLiteral("tops"))->display, QStringLiteral("分层顶"));
  QCOMPARE(reg.forEntity(QStringLiteral("well")).size(), 10);     // 9 + custom
  QCOMPARE(reg.forEntity(QStringLiteral("seismic_survey")).size(), 8); // 7 + tops
  QCOMPARE(reg.forEntity(QStringLiteral("seismic_survey")).last().display,
           QStringLiteral("调查分层"));

  const RoleDef *custom = reg.find(QStringLiteral("custom_marker"));
  QVERIFY(custom != nullptr);
  QCOMPARE(custom->entityTypes, QStringList{QStringLiteral("well")});
  QCOMPARE(custom->maxCount, 1);
  QVERIFY(custom->ordered);
  QCOMPARE(custom->stageDefault, QStringLiteral("DERIVED"));
  QCOMPARE(custom->display, QStringLiteral("定制标记"));
  QVERIFY(reg.isKnown(QStringLiteral("custom_marker")));

  // 非对象值/空角色名不产出定义；空 roles 对象 = 纯 defaults。
  QJsonObject junk;
  junk.insert(QStringLiteral("not_an_object"), QStringLiteral("x"));
  junk.insert(QString(), QJsonObject{{QStringLiteral("display"),
                                      QStringLiteral("空名")}});
  const RoleRegistry junkReg = RoleRegistry::fromJson(junk);
  QVERIFY(!junkReg.isKnown(QStringLiteral("not_an_object")));
  QVERIFY(!junkReg.isKnown(QString()));
  const RoleRegistry plain = RoleRegistry::fromJson(QJsonObject());
  QCOMPARE(plain.forEntity(QStringLiteral("well")).size(), 9);
}

// catalog.open() 读 <projectDir>/project_area.json 的 roles 节——工程自定义
// 角色经 roleRegistry() 暴露，内置词表仍是底。
void TestRoles::openLoadsProjectAreaRoles()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QFile f(dir.filePath(QStringLiteral("project_area.json")));
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QStringLiteral(
              "{\"area_name\":\"测试工区\",\"roles\":{"
              "\"seismic_volume\":{\"display\":\"三维数据体\"},"
              "\"survey_bom\":{\"entity_types\":[\"seismic_survey\"],"
              "\"max_count\":1,\"display\":\"爆破表\"}}}")
              .toUtf8());
  f.close();

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY(err.isEmpty());
  const RoleRegistry &reg = cat.roleRegistry();
  QVERIFY(reg.isKnown(QStringLiteral("survey_bom")));
  QCOMPARE(reg.find(QStringLiteral("survey_bom"))->display,
           QStringLiteral("爆破表"));
  QCOMPARE(reg.find(QStringLiteral("survey_bom"))->maxCount, 1);
  QCOMPARE(reg.find(QStringLiteral("seismic_volume"))->display,
           QStringLiteral("三维数据体"));
  QVERIFY(reg.isKnown(QStringLiteral("well_log"))); // 内置词表仍在
  QCOMPARE(reg.forEntity(QStringLiteral("seismic_survey")).size(), 8);
}

void TestRoles::openWithoutAreaFileYieldsDefaults()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  // 无 project_area.json → 内置词表原样可用。
  QVERIFY(cat.roleRegistry().isKnown(QStringLiteral("well_head")));
  QVERIFY(cat.roleRegistry().isKnown(QStringLiteral("seismic_volume")));
  QVERIFY(!cat.roleRegistry().isKnown(QStringLiteral("survey_bom")));
  QCOMPARE(cat.roleRegistry().forEntity(QStringLiteral("well")).size(), 9);
}

void TestRoles::corruptAreaFileStillOpensWithDefaults()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QFile f(dir.filePath(QStringLiteral("project_area.json")));
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArrayLiteral("{ not json at all"));
  f.close();
  DataCatalog cat;
  QString err;
  // 词表文件坏掉绝不阻塞工程打开——静默回退 defaults()。
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY(cat.roleRegistry().isKnown(QStringLiteral("well_head")));
  QVERIFY(!cat.roleRegistry().isKnown(QStringLiteral("survey_bom")));
}

QTEST_MAIN(TestRoles)
#include "tst_roles.moc"
