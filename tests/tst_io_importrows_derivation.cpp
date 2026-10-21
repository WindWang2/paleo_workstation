// tst_io_importrows_derivation — FolderPreviewRow 的真实生产面：
// DataImportService::previewFiles（→ planForFiles → previewRowsFromPlan，
// src/io/dataimport_folder.cpp）按分类器谓词与路径规则推导确认表行的
// displayType / typeVocab / typeEditable / entityPreview。
// tst_domain_importrows 只钉两个 DTO 的默认值（不触产品逻辑），本套件覆盖
// 推导本身。
#include <QtTest>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include "../src/domain/projectclassifier.h"
#include "../src/io/dataimportservice.h"

class TestIoImportRowsDerivation : public QObject
{
  Q_OBJECT

private slots:
  void displayTypeFollowsClassifierAndPathRules();
  void unresolvedWellRowPreviewsPendingName();
};

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return false;
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(content) == content.size();
}

FolderPreviewRow rowFor(const QVector<FolderPreviewRow> &rows, const QString &fileName)
{
  for (const FolderPreviewRow &r : rows)
    if (QFileInfo(r.path).fileName() == fileName)
      return r;
  return FolderPreviewRow();
}

} // namespace

void TestIoImportRowsDerivation::displayTypeFollowsClassifierAndPathRules()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString proj = dir.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(proj));
  // 四个路径面：普通目录 / 「参考资料」目录段 / HZ28-6-1 固定辅助命名 /
  // 非井类文档。
  const QString plain = dir.filePath(QStringLiteral("import/A1.Las"));
  const QString refDir = dir.filePath(QStringLiteral("import/参考资料/A2.Las"));
  const QString fixedAux = dir.filePath(QStringLiteral("import/HZ28-6-1 附表.las"));
  const QString doc = dir.filePath(QStringLiteral("import/说明.pdf"));
  const QByteArray lasStub("~VERSION INFORMATION\n");
  QVERIFY(writeFile(plain, lasStub));
  QVERIFY(writeFile(refDir, lasStub));
  QVERIFY(writeFile(fixedAux, lasStub));
  QVERIFY(writeFile(doc, QByteArrayLiteral("%PDF-1.4\n")));

  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(proj);
  QString err;
  const QVector<FolderPreviewRow> rows =
      svc.previewFiles({plain, refDir, fixedAux, doc}, &err);
  QVERIFY2(rows.size() == 4, qPrintable(err));

  // 普通路径：显示分类器原类型（.las → well_log），类型可改。
  const FolderPreviewRow plainRow = rowFor(rows, QStringLiteral("A1.Las"));
  QCOMPARE(plainRow.classifiedType, QStringLiteral("well_log"));
  QCOMPARE(plainRow.displayType, QStringLiteral("well_log"));
  QVERIFY(plainRow.typeEditable);
  QCOMPARE(plainRow.typeVocab, projectClassifierTypes()); // 词表内类型不追加
  QVERIFY(!plainRow.skipped);

  // 「参考资料」目录段内：井类默认显示「参考」，但仍可改（与固定辅助不同，
  // 改动作为 override 送达后端）。
  const FolderPreviewRow refRow = rowFor(rows, QStringLiteral("A2.Las"));
  QCOMPARE(refRow.displayType, QStringLiteral("reference"));
  QVERIFY(refRow.typeEditable);

  // HZ28-6-1 固定辅助：显示「参考」且锁死——确认表禁改类型。
  const FolderPreviewRow auxRow = rowFor(rows, QStringLiteral("HZ28-6-1 附表.las"));
  QCOMPARE(auxRow.displayType, QStringLiteral("reference"));
  QVERIFY(!auxRow.typeEditable);

  // 非井类文档：显示分类器原类型，不受参考资料规则影响。
  const FolderPreviewRow docRow = rowFor(rows, QStringLiteral("说明.pdf"));
  QCOMPARE(docRow.classifiedType, QStringLiteral("document"));
  QCOMPARE(docRow.displayType, QStringLiteral("document"));
  QVERIFY(docRow.typeEditable);
}

void TestIoImportRowsDerivation::unresolvedWellRowPreviewsPendingName()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString proj = dir.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(proj));
  const QString las = dir.filePath(QStringLiteral("import/A1.Las"));
  QVERIFY(writeFile(las, QByteArrayLiteral("~VERSION INFORMATION\n")));

  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(proj);
  QString err;
  const QVector<FolderPreviewRow> rows = svc.previewFiles({las}, &err);
  QCOMPARE(rows.size(), 1);
  // 新工程（零井实体）：井类行零匹配 → 预显「未决（名字）」，不猜、不静默。
  QVERIFY2(rows.first().entityPreview.startsWith(QStringLiteral("未决")),
           qPrintable(rows.first().entityPreview));
}

QTEST_GUILESS_MAIN(TestIoImportRowsDerivation)
#include "tst_io_importrows_derivation.moc"
