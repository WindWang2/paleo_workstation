// tst_domain_importrows — FolderPreviewRow/FolderRowResult 的默认值锚点。
// 两个 DTO 的真实生产面（分类/跳过原因/实体归位推导）在 src/io/
// dataimport_folder.cpp 的 previewRowsFromPlan——由
// tests/tst_io_importrows_derivation.cpp 覆盖（本套件链 paleo_domain，
// 够不到 io 侧符号）；这里只钉默认值，删掉了原先三个「本函数内赋字面量
// →同函数读回」的恒真槽。
#include <QtTest>

#include "../src/domain/importrows.h"

class TestDomainImportRows : public QObject
{
  Q_OBJECT

private slots:
  void previewRowDefaults();
  void rowResultOutcomeEnum();
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

void TestDomainImportRows::mutationDemonstration_defaultOutcome()
{
  // 变异测试示范：未初始化的结果行必须默认为 Skipped，绝不默认为 Imported
  FolderRowResult res;
  QCOMPARE(res.outcome, FolderRowResult::Outcome::Skipped);
  QVERIFY(res.outcome != FolderRowResult::Outcome::Imported);
}

QTEST_GUILESS_MAIN(TestDomainImportRows)
#include "tst_domain_importrows.moc"
