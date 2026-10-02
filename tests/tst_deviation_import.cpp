// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>

#include <memory>

#include "catalog/datacatalog.h"
#include "domain/projectclassifier.h"
#include "domain/wellrecords.h"
#include "io/dataimportservice.h"
#include "io/ingestplan.h"
#include "io/wellfileparsers.h"
#include "metadata/paleoprojectstore.h"

// goal/well-trajectory 轮2：测斜导入链验收（Oracle 第 2 条）——
// 分类判据（目录段/文件名/XML 井斜工作表）、文本站表解析、
// trajectory 角色链接（primary/已决）、未决诚实面（不建井不猜）。
class tst_deviation_import : public QObject
{
  Q_OBJECT

private slots:
  void classifiesDeviationPaths();
  void classifiesDeviationXml();
  void parsesDeviationText();
  void importsTrajectoryLink();
  void unresolvedFaceDoesNotCreateWell();
  void ingestPlanProposesTrajectory();

private:
  struct Stack
  {
    PaleoProjectStore store;
    std::unique_ptr<DataImportService> svc;
  };

  // 轻栈：无 QGIS 工程——导入只写 catalog.json + 受管 RAW（project store
  // 仅持有路径，不触 .qgz）。预置井 A1 先经独立 DataCatalog 落盘。
  std::unique_ptr<Stack> makeStack(const QString &projectDir, bool seedWellA1)
  {
    if (seedWellA1)
    {
      DataCatalog pre;
      QString err;
      if (!pre.open(projectDir, &err))
        return nullptr;
      CatalogEntity w;
      w.id = QStringLiteral("well-A1");
      w.entityType = QStringLiteral("well");
      w.name = QStringLiteral("A1");
      if (!pre.addEntity(w, &err))
        return nullptr;
    }
    auto s = std::make_unique<Stack>();
    s->store.setProjectPaths(QDir(projectDir).filePath(QStringLiteral("proj.qgz")),
                             QDir(projectDir).filePath(QStringLiteral("project.gpkg")),
                             QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite")));
    s->svc = std::make_unique<DataImportService>(&s->store);
    s->svc->setProjectDir(projectDir);
    return s;
  }

  static QString writeFile(const QString &path, const QByteArray &content)
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    f.write(content);
    f.close();
    return path;
  }

  static const char *kDeviationDat;
  static const char *kDeviationXml;
};

const char *tst_deviation_import::kDeviationDat =
    "# Well : A1\n"
    "# MD  INCL  AZI\n"
    "0.0 0.0 90.0\n"
    "500.0 6.0 90.0\n"
    "1000.0 12.0 90.0\n"
    "1500.0 -99999 90.0\n"   // 哨兵井斜：整行不进站表
    "2000.0 24.0 90.0\n";

const char *tst_deviation_import::kDeviationXml =
    "<?xml version=\"1.0\"?>\n"
    "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\"\n"
    "          xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n"
    " <Worksheet ss:Name=\"井斜数据\">\n"
    "  <Table>\n"
    "   <Row><Cell><Data ss:Type=\"String\">测深</Data></Cell>"
    "<Cell><Data ss:Type=\"String\">井斜角</Data></Cell>"
    "<Cell><Data ss:Type=\"String\">方位角</Data></Cell></Row>\n"
    "   <Row><Cell><Data ss:Type=\"Number\">0</Data></Cell>"
    "<Cell><Data ss:Type=\"Number\">0</Data></Cell>"
    "<Cell><Data ss:Type=\"Number\">45</Data></Cell></Row>\n"
    "   <Row><Cell><Data ss:Type=\"Number\">1000</Data></Cell>"
    "<Cell><Data ss:Type=\"Number\">30</Data></Cell>"
    "<Cell><Data ss:Type=\"Number\">45</Data></Cell></Row>\n"
    "  </Table>\n"
    " </Worksheet>\n"
    "</Workbook>\n";

void tst_deviation_import::classifiesDeviationPaths()
{
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/测斜/A1.dat")).type,
           QStringLiteral("well_deviation"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/dev/A1.dat")).type,
           QStringLiteral("well_deviation"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/井斜/deviates.dat")).type,
           QStringLiteral("well_deviation"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/data/A1-deviation.dat")).type,
           QStringLiteral("well_deviation"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/data/A1-trajectory.dat")).type,
           QStringLiteral("well_deviation"));
  // 无判据命中不虚判（tabular），语义目录优先级保持（井位里的 dev 文件
  // 仍按井位分类——表序尾部追加零扰动）。
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/data/A1.dat")).type,
           QStringLiteral("tabular"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/井位/dev.dat")).type,
           QStringLiteral("well_head"));
  // 词表登记（确认表词表一致性由 isClassifierType 消费）。
  QVERIFY(isClassifierType(QStringLiteral("well_deviation")));
}

void tst_deviation_import::classifiesDeviationXml()
{
  const QByteArray devXml(kDeviationXml);
  QCOMPARE(sniffWellXml(devXml), WellXmlKind::WellDeviation);
  const ProjectClassification cls = classifyProjectImport(QStringLiteral("a.xml"), devXml);
  QCOMPARE(cls.type, QStringLiteral("well_deviation"));
  QCOMPARE(cls.format, QStringLiteral("xml"));

  // 综合柱状图（测井曲线表 + 井斜表并存）：仍判 well_log——井斜表只在其
  // 独立成文件时才走测斜导入。
  const QByteArray both(
      "<?xml version=\"1.0\"?><Workbook "
      "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">"
      "<Worksheet ss:Name=\"测井曲线\"><Table></Table></Worksheet>"
      "<Worksheet ss:Name=\"井斜数据\"><Table></Table></Worksheet></Workbook>");
  QCOMPARE(sniffWellXml(both), WellXmlKind::WellLog);

  const QByteArray neither(QStringLiteral("<?xml version=\"1.0\"?><foo/>").toUtf8());
  QCOMPARE(sniffWellXml(neither), WellXmlKind::Unknown);
}

void tst_deviation_import::parsesDeviationText()
{
  const DeviationTable table = parseDeviationText(QByteArray(kDeviationDat));
  QCOMPARE(table.wellName, QStringLiteral("A1"));
  QCOMPARE(table.stations.size(), 4); // 哨兵行 + 表头行不进站表
  QCOMPARE(table.stations.at(0).md, 0.0);
  QCOMPARE(table.stations.at(1).inclinationDeg, 6.0);
  QCOMPARE(table.stations.at(3).azimuthDeg, 90.0);

  // 无井名注释：wellName 空（导入侧回退文件名主名）。
  const DeviationTable anon = parseDeviationText(QByteArrayLiteral("0 0 0\n100 5 10\n"));
  QVERIFY(anon.wellName.isEmpty());
  QCOMPARE(anon.stations.size(), 2);
}

void tst_deviation_import::importsTrajectoryLink()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir, true);
  QVERIFY(stack != nullptr);
  DataImportService &svc = *stack->svc;

  const QString dat = writeFile(tmp.filePath(QStringLiteral("A1-deviation.dat")),
                                QByteArray(kDeviationDat));
  QVERIFY(!dat.isEmpty());
  QString err;
  const auto result = svc.importProjectFileEx(dat, &err);
  QVERIFY2(result.outcome == DataImportService::ImportOutcome::Imported,
           qPrintable(err + QStringLiteral(" | ") + result.message));
  QVERIFY(!result.assetId.isEmpty());

  DataCatalog cat;
  QVERIFY(cat.open(projectDir, &err));
  const QVector<EntityAssetLink> links = cat.linksForEntity(QStringLiteral("well-A1"));
  const EntityAssetLink *traj = nullptr;
  for (const EntityAssetLink &l : links)
    if (l.role == QLatin1String("trajectory"))
      traj = &l;
  QVERIFY2(traj != nullptr, "trajectory link missing");
  QVERIFY(!traj->unresolved);
  QVERIFY(traj->isPrimary);
  QCOMPARE(traj->entityType, QStringLiteral("well"));

  // 受管版本可回读：站点逐值对拍（含哨兵行剔除语义）。
  const CatalogVersion v = cat.currentVersion(traj->assetId);
  QVERIFY(!v.id.isEmpty());
  const QString stored = DataCatalog::resolvedVersionPath(projectDir, v);
  QVERIFY(!stored.isEmpty() && QFileInfo(stored).isFile());
  QFile f(stored);
  QVERIFY(f.open(QIODevice::ReadOnly));
  const DeviationTable back = parseDeviationText(f.readAll());
  QCOMPARE(back.wellName, QStringLiteral("A1"));
  QCOMPARE(back.stations.size(), 4);
  QCOMPARE(back.stations.at(2).md, 1000.0);
  QCOMPARE(back.stations.at(2).inclinationDeg, 12.0);
}

void tst_deviation_import::unresolvedFaceDoesNotCreateWell()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir, false); // 不预置井
  QVERIFY(stack != nullptr);
  DataImportService &svc = *stack->svc;

  const QString dat = writeFile(tmp.filePath(QStringLiteral("GHOST-deviation.dat")),
                                QByteArrayLiteral("# Well : GHOST\n0 0 0\n500 10 45\n"));
  QString err;
  const auto result = svc.importProjectFileEx(dat, &err);
  QVERIFY2(result.outcome == DataImportService::ImportOutcome::Imported,
           qPrintable(err)); // 资产已入库——未决的是链接，不是导入失败

  DataCatalog cat;
  QVERIFY(cat.open(projectDir, &err));
  bool sawUnresolvedTrajectory = false;
  for (const EntityAssetLink &l : cat.links())
    if (l.role == QLatin1String("trajectory"))
    {
      QVERIFY(l.unresolved);
      QVERIFY(l.entityId.isEmpty());
      QVERIFY(!l.note.isEmpty()); // 候选/未匹配名如实记录
      sawUnresolvedTrajectory = true;
    }
  QVERIFY(sawUnresolvedTrajectory);
  // 未决不建井：没有任何井实体。
  QVERIFY(cat.entities(QStringLiteral("well")).isEmpty());
}

void tst_deviation_import::ingestPlanProposesTrajectory()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  // 预置两口井制造双候选（A1 与 a-1 规范化同名）→ 歧义如实标注。
  {
    DataCatalog pre;
    QString err;
    QVERIFY(pre.open(projectDir, &err));
    for (const QString &id : {QStringLiteral("well-A1"), QStringLiteral("well-A1b")})
    {
      CatalogEntity w;
      w.id = id;
      w.entityType = QStringLiteral("well");
      w.name = id == QLatin1String("well-A1") ? QStringLiteral("A1") : QStringLiteral("a-1");
      QVERIFY(pre.addEntity(w, &err));
    }
  }
  DataCatalog cat;
  QString err;
  QVERIFY(cat.open(projectDir, &err));

  const QString dir = tmp.filePath(QStringLiteral("测斜"));
  QVERIFY(QDir().mkpath(dir));
  const QString dat = writeFile(QDir(dir).filePath(QStringLiteral("A1.dat")),
                                QByteArray(kDeviationDat));
  QVERIFY(!dat.isEmpty());

  const IngestPlan plan = buildIngestPlan(dat, cat);
  QCOMPARE(plan.items.size(), 1);
  const PlannedItem &item = plan.items.constFirst();
  QCOMPARE(item.type, QStringLiteral("well_deviation"));
  QCOMPARE(item.role, QStringLiteral("trajectory"));
  QCOMPARE(item.entityType, QStringLiteral("well"));
  QVERIFY(item.entityAmbiguous); // 双候选：歧义不猜
  QVERIFY(!item.suggestedPrimary);
}

QTEST_MAIN(tst_deviation_import)
#include "tst_deviation_import.moc"
