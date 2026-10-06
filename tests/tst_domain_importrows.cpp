#include <QtTest>

#include "../src/domain/importrows.h"

class TestDomainImportRows : public QObject
{
  Q_OBJECT

private slots:
  void previewRowDefaults();
  void previewRowSkipState();
  void previewRowVocabAndDisplay();
  void rowResultOutcomeEnum();
  void rowResultEntityMapping();
  void mutationDemonstration_defaultOutcome();
};

void TestDomainImportRows::previewRowDefaults()
{
  FolderPreviewRow row;
  QVERIFY(!row.skipped);
  QVERIFY(row.typeEditable);
  QCOMPARE(row.sizeBytes, -1LL);
  QVERIFY(row.path.isEmpty());
  QVERIFY(row.typeVocab.isEmpty());
}

void TestDomainImportRows::previewRowSkipState()
{
  FolderPreviewRow row;
  row.path = QStringLiteral("/data/broken_link.las");
  row.skipped = true;
  row.skipReason = QStringLiteral("符号链接指向不存在文件");
  row.typeEditable = false;
  row.decision = QStringLiteral("skip");

  QVERIFY(row.skipped);
  QVERIFY(!row.typeEditable);
  QCOMPARE(row.decision, QStringLiteral("skip"));
  QCOMPARE(row.skipReason, QStringLiteral("符号链接指向不存在文件"));
}

void TestDomainImportRows::previewRowVocabAndDisplay()
{
  FolderPreviewRow row;
  row.classifiedType = QStringLiteral("well_log");
  row.displayType = QStringLiteral("测井曲线");
  row.typeVocab = QStringList{QStringLiteral("well_log"), QStringLiteral("reference"), QStringLiteral("unknown")};
  row.entityPreview = QStringLiteral("井 A1（既有）");

  QCOMPARE(row.displayType, QStringLiteral("测井曲线"));
  QCOMPARE(row.typeVocab.size(), 3);
  QCOMPARE(row.entityPreview, QStringLiteral("井 A1（既有）"));
}

void TestDomainImportRows::rowResultOutcomeEnum()
{
  FolderRowResult res;
  QCOMPARE(res.outcome, FolderRowResult::Outcome::Skipped);

  res.outcome = FolderRowResult::Outcome::Imported;
  QCOMPARE(res.outcome, FolderRowResult::Outcome::Imported);

  res.outcome = FolderRowResult::Outcome::Unresolved;
  QCOMPARE(res.outcome, FolderRowResult::Outcome::Unresolved);

  res.outcome = FolderRowResult::Outcome::Failed;
  QCOMPARE(res.outcome, FolderRowResult::Outcome::Failed);
}

void TestDomainImportRows::rowResultEntityMapping()
{
  FolderRowResult res;
  res.path = QStringLiteral("c:/wells/w1_log.las");
  res.classifiedType = QStringLiteral("well_log");
  res.entityName = QStringLiteral("Well_A1, Well_A2");
  res.outcome = FolderRowResult::Outcome::Imported;
  res.message = QStringLiteral("已导入 24 条曲线");

  QCOMPARE(res.entityName, QStringLiteral("Well_A1, Well_A2"));
  QCOMPARE(res.outcome, FolderRowResult::Outcome::Imported);
  QVERIFY(res.message.contains(QStringLiteral("24")));
}

void TestDomainImportRows::mutationDemonstration_defaultOutcome()
{
  // 变异测试示范：未初始化的结果行必须默认为 Skipped，绝不默认为 Imported
  FolderRowResult res;
  QCOMPARE(res.outcome, FolderRowResult::Outcome::Skipped);
  QVERIFY(res.outcome != FolderRowResult::Outcome::Imported);
}

QTEST_GUILESS_MAIN(TestDomainImportRows)
#include "tst_domain_importrows.moc"
