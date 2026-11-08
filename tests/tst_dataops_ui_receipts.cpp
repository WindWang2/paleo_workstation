// 层：测试壳
// P1（data-commands 2026-11-08）W8：原生数据页交互级验证——真实 DataPage/
// DataListPanel + 真实工程目录，穿生产入口（菜单槽/拖放意图/批量侧车操作），
// 验证 成功/失败/撤销/重做 的用户可见结果与存储事实一致。
//   · 定向信号断言（statusMessage）+ 落盘核验（原始侧车内容/新实例重开），
//     不以截图代替状态检查；
//   · RAW 受管版本字节前后校验（数据不可变约束）。
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QItemSelectionModel>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>

#include "../src/io/dataimportservice.h"
#include "../src/services/jsonsidecar.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/pages/pagepanels.h"
#include "helpers/dataopsfixture.h"

using paleo::tests::dataops::DataOpsFixture;
using paleo::tests::dataops::driveModalNextTick;

namespace
{
// 状态栏信号捕获器（定向断言：成功/失败文案同一事实源）。
struct StatusSpy : public QObject
{
  explicit StatusSpy(DataListPanel *p) : panel(p)
  {
    connect(panel, &DataListPanel::statusMessage, this,
            [this](const QString &m) { messages << m; });
  }
  QString last() const { return messages.isEmpty() ? QString() : messages.last(); }
  bool anyContains(const QString &needle) const
  {
    for (const QString &m : messages)
      if (m.contains(needle))
        return true;
    return false;
  }
  DataListPanel *panel = nullptr;
  QStringList messages;
};
} // namespace

class TestDataOpsUiReceipts : public QObject
{
  Q_OBJECT
private slots:
  void menuBatchTagSuccessUndoRedo();
  void multiAssetDropMixedUndoScope();
  void batchSidecarFailureSurfacesHonestMessage();
  void softDeleteViaUiThenUndo();
  void readonlyGuardSurfacesInUi();

private:
  // 为 astResolved 造一个受管 RAW 版本（真实文件），返回绝对路径 + 内容摘要。
  QString seedManagedRaw(DataCatalog *cat, const QString &assetId,
                         const QString &projectDir, const QByteArray &bytes)
  {
    const QString verId = QStringLiteral("ver-raw-1");
    const QString abs = projectDir + QStringLiteral("/RAW/%1/%2/a1.las").arg(assetId, verId);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
      qWarning("seedManagedRaw: write failed: %s", qPrintable(abs));
    f.close();
    CatalogVersion v;
    v.id = verId;
    v.assetId = assetId;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = true;
    v.path = QStringLiteral("RAW/%1/%2/a1.las").arg(assetId, verId);
    v.fileName = QStringLiteral("a1.las");
    v.sourceUri = QStringLiteral("file://test-seed");
    v.sha256 = DataCatalog::sha256FileHex(abs);
    QString err;
    if (!cat->addVersion(v, &err))
      qWarning("seedManagedRaw: addVersion failed: %s", qPrintable(err));
    return abs;
  }
};

// 菜单操作（批量打标签槽 = 右键菜单/命令面板同一入口）→ 成功文案在执行
// 确认后出现且带「可撤销」；一次撤销/重做真实生效；RAW 原件未被改写。
void TestDataOpsUiReceipts::menuBatchTagSuccessUndoRedo()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(dir.path());
  DataOpsFixture fx;
  fx.build(svc.catalog());
  const QByteArray rawBytes = QByteArrayLiteral("RAW-BYTES-0123456789");
  const QString rawPath =
      seedManagedRaw(svc.catalog(), fx.astResolved, dir.path(), rawBytes);

  std::unique_ptr<DataPage> page(fx.makePage(&svc));
  auto *lp = page->findChild<DataListPanel *>();
  QVERIFY(lp);
  StatusSpy spy(lp);
  auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
  QVERIFY(table);
  // 多选：selectRow + selectionModel 追加（与既有 d1 用例同一口径）。
  table->selectRow(0);
  table->selectionModel()->select(
      table->model()->index(1, 0),
      QItemSelectionModel::Select | QItemSelectionModel::Rows);
  QTest::qWaitFor([lp] { return lp->currentAssetSelection().size() == 2; });

  driveModalNextTick([](QWidget *w) {
    if (auto *dlg = qobject_cast<QInputDialog *>(w))
    {
      dlg->setTextValue(QStringLiteral("验收标签"));
      QPointer<QInputDialog> guard(dlg);
      QTimer::singleShot(0, [guard] {
        if (guard)
          guard->accept();
      });
    }
  });
  lp->batchAddTag();

  // 成功确认：标签真实落盘（原始 JSON）+ 文案带可撤销 + 一条历史。
  QVERIFY(lp->opsContext().tags->tagsFor(fx.astResolved).contains(
      QStringLiteral("验收标签")));
  {
    QFile f(paleo::dataopsSidecarPath(svc.catalog(), QStringLiteral("asset_tags.json")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.readAll().contains("验收标签")); // 落盘事实，非内存假象
  }
  QVERIFY(spy.last().contains(QStringLiteral("可撤销")));
  QCOMPARE(lp->opStack()->depth(), 1);

  // 一次撤销：标签移除 + 文案「已撤销」+ 盘上一并回滚。
  lp->undoOp();
  QVERIFY(!lp->opsContext().tags->tagsFor(fx.astResolved).contains(
      QStringLiteral("验收标签")));
  QVERIFY(spy.last().contains(QStringLiteral("已撤销")));
  {
    QFile f(paleo::dataopsSidecarPath(svc.catalog(), QStringLiteral("asset_tags.json")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(!f.readAll().contains("验收标签"));
  }
  // 一次重做：恢复。
  lp->redoOp();
  QVERIFY(lp->opsContext().tags->tagsFor(fx.astResolved).contains(
      QStringLiteral("验收标签")));

  // RAW 原件未改写（数据不可变）。
  QFile rawAfter(rawPath);
  QVERIFY(rawAfter.open(QIODevice::ReadOnly));
  QCOMPARE(rawAfter.readAll(), rawBytes);
}

// 多资产拖放（拖到实体意图）混合批次：无链接 addLink（不可撤销）+ 已决转移
//（可撤销）分别汇报；撤销只回滚可撤销部分；目录重开核验。
void TestDataOpsUiReceipts::multiAssetDropMixedUndoScope()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(dir.path());
  DataOpsFixture fx;
  fx.build(svc.catalog());
  std::unique_ptr<DataPage> page(fx.makePage(&svc));
  auto *lp = page->findChild<DataListPanel *>();
  QVERIFY(lp);
  StatusSpy spy(lp);

  lp->applyEntityDrop({fx.astFree, fx.astResolved}, fx.wellB);

  // 混合批次文案：新挂 1、转移 1，且如实披露可撤销范围。
  QVERIFY(spy.last().contains(QStringLiteral("新挂 1")));
  QVERIFY(spy.last().contains(QStringLiteral("转移 1")));
  QVERIFY(spy.last().contains(QStringLiteral("不可撤销")));
  QCOMPARE(lp->opStack()->depth(), 1);

  // 撤销：转移回滚（astResolved 回 wellA），addLink 留存（真实不可撤销）。
  lp->undoOp();
  int resOnA = 0, freeOnB = 0;
  for (const EntityAssetLink &l : svc.catalog()->links())
  {
    if (l.assetId == fx.astResolved && !l.unresolved && l.entityId == fx.wellA)
      ++resOnA;
    if (l.assetId == fx.astFree && !l.unresolved && l.entityId == fx.wellB)
      ++freeOnB;
  }
  QCOMPARE(resOnA, 1);
  QCOMPARE(freeOnB, 1);

  // 目录独立重开核验（执行器与检查器分离）。
  DataCatalog ro;
  QVERIFY(ro.open(dir.path(), nullptr));
  int freeOnBRo = 0, resOnARo = 0;
  for (const EntityAssetLink &l : ro.links())
  {
    if (l.assetId == fx.astFree && !l.unresolved && l.entityId == fx.wellB)
      ++freeOnBRo;
    if (l.assetId == fx.astResolved && !l.unresolved && l.entityId == fx.wellA)
      ++resOnARo;
  }
  QCOMPARE(freeOnBRo, 1);
  QCOMPARE(resOnARo, 1);
}

// 批量侧车操作失败（注入：把 asset_tags.json 换成目录）：失败状态先出现、
// 不被成功文案覆盖、无虚假历史；解除后同入口重试成功。
void TestDataOpsUiReceipts::batchSidecarFailureSurfacesHonestMessage()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(dir.path());
  DataOpsFixture fx;
  fx.build(svc.catalog());
  std::unique_ptr<DataPage> page(fx.makePage(&svc));
  auto *lp = page->findChild<DataListPanel *>();
  QVERIFY(lp);
  StatusSpy spy(lp);
  auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
  QVERIFY(table);
  table->selectRow(0);
  QTest::qWaitFor([lp] { return !lp->currentAssetSelection().isEmpty(); });

  // 注入：sidecar 路径换成目录（QSaveFile 打不开——既有失败路径）。
  const QString tagsPath =
      paleo::dataopsSidecarPath(svc.catalog(), QStringLiteral("asset_tags.json"));
  QDir().mkpath(QFileInfo(tagsPath).absolutePath());
  QVERIFY(QDir().mkdir(tagsPath));

  driveModalNextTick([](QWidget *w) {
    if (auto *dlg = qobject_cast<QInputDialog *>(w))
    {
      dlg->setTextValue(QStringLiteral("堵"));
      QPointer<QInputDialog> guard(dlg);
      QTimer::singleShot(0, [guard] {
        if (guard)
          guard->accept();
      });
    }
  });
  lp->batchAddTag();

  // 失败如实上报：不出现「已给 … 打标签」的成功文案；无历史；内存与盘一致。
  QVERIFY(!spy.anyContains(QStringLiteral("已给")));
  QVERIFY(spy.anyContains(QStringLiteral("失败")));
  QCOMPARE(lp->opStack()->depth(), 0);
  QVERIFY(lp->opsContext().tags->tagsFor(fx.astResolved).isEmpty());

  // 解除堵塞 → 同一入口第二次操作成功（无残留失败态）。
  QVERIFY(QDir(tagsPath).removeRecursively());
  driveModalNextTick([](QWidget *w) {
    if (auto *dlg = qobject_cast<QInputDialog *>(w))
    {
      dlg->setTextValue(QStringLiteral("恢复"));
      QPointer<QInputDialog> guard(dlg);
      QTimer::singleShot(0, [guard] {
        if (guard)
          guard->accept();
      });
    }
  });
  lp->batchAddTag();
  QVERIFY(lp->opsContext().tags->tagsFor(fx.astResolved).contains(
      QStringLiteral("恢复")));
  QVERIFY(spy.last().contains(QStringLiteral("可撤销")));
  QCOMPARE(lp->opStack()->depth(), 1);
}

// 软删（确认框 Yes）→ 行消失 + 回收清单 1 条；一次撤销 → 行恢复。
void TestDataOpsUiReceipts::softDeleteViaUiThenUndo()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(dir.path());
  DataOpsFixture fx;
  fx.build(svc.catalog());
  std::unique_ptr<DataPage> page(fx.makePage(&svc));
  auto *lp = page->findChild<DataListPanel *>();
  QVERIFY(lp);
  StatusSpy spy(lp);
  auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
  QVERIFY(table);
  const int rows0 = table->rowCount();
  table->selectRow(rows0 - 1); // free.sgy（无链接资产）

  driveModalNextTick([](QWidget *w) {
    if (auto *mb = qobject_cast<QMessageBox *>(w))
      mb->button(QMessageBox::Yes)->click();
  });
  lp->batchRemoveSoft();

  QCOMPARE(table->rowCount(), rows0 - 1);
  QCOMPARE(lp->opsContext().recycle->entries().size(), 1);
  QVERIFY(spy.last().contains(QStringLiteral("可撤销")));

  lp->undoOp();
  QCOMPARE(table->rowCount(), rows0);
  QCOMPARE(lp->opsContext().recycle->entries().size(), 0);
  QVERIFY(spy.last().contains(QStringLiteral("已撤销")));
}

// 只读工程：确认框 Yes 后功能层守卫拒绝——错误状态不被成功文案覆盖。
void TestDataOpsUiReceipts::readonlyGuardSurfacesInUi()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataImportService svc(nullptr, nullptr);
  svc.setProjectDir(dir.path());
  DataOpsFixture fx;
  fx.build(svc.catalog());
  std::unique_ptr<DataPage> page(fx.makePage(&svc));
  auto *lp = page->findChild<DataListPanel *>();
  QVERIFY(lp);
  StatusSpy spy(lp);
  auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
  QVERIFY(table);
  const int rows0 = table->rowCount();
  table->selectRow(rows0 - 1);

  svc.catalog()->setLockedReadOnly(true);
  driveModalNextTick([](QWidget *w) {
    if (auto *mb = qobject_cast<QMessageBox *>(w))
      mb->button(QMessageBox::Yes)->click();
  });
  lp->batchRemoveSoft();

  // 守卫拒绝：行未消失、无成功文案、无历史。
  QCOMPARE(table->rowCount(), rows0);
  QVERIFY(!spy.anyContains(QStringLiteral("已移除")));
  QCOMPARE(lp->opStack()->depth(), 0);
  svc.catalog()->setLockedReadOnly(false);
}

QTEST_MAIN(TestDataOpsUiReceipts)
#include "tst_dataops_ui_receipts.moc"
