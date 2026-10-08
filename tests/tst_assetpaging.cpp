#include <QtTest>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QElapsedTimer>
#include <QTimer>
#include <QComboBox>
#include <QTabWidget>
#include <atomic>
#include <thread>
#ifdef Q_OS_LINUX
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstring>
#endif
#include "../src/io/perffixtures.h"
#include "../src/io/dataimportservice.h"
#include "../src/services/previewdoc.h"
#include "../src/services/storagegovernance.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/dialogs/storagegovernancedialog.h"
#include "../src/ui/paleotheme.h"

#ifdef Q_OS_LINUX
namespace {
std::atomic<bool> traceStats{false};
std::atomic<int> uiArtifactStats{0};
std::thread::id guiThread;
void countStat(const char *path) {
  if (traceStats && path && std::this_thread::get_id() == guiThread &&
      (std::strstr(path, "/raw/") || std::strstr(path, "/derived/") ||
       std::strstr(path, "/artifacts/RAW/") || std::strstr(path, "/artifacts/DERIVED/"))) ++uiArtifactStats;
}
}
extern "C" int statx(int fd, const char *path, int flags, unsigned mask, struct statx *out) noexcept {
  static auto original = reinterpret_cast<int(*)(int, const char *, int, unsigned, struct statx *)>(dlsym(RTLD_NEXT, "statx"));
  countStat(path); return original(fd, path, flags, mask, out);
}
extern "C" int stat(const char *path, struct stat *out) noexcept {
  static auto original = reinterpret_cast<int(*)(const char *, struct stat *)>(dlsym(RTLD_NEXT, "stat"));
  countStat(path); return original(path, out);
}
extern "C" int lstat(const char *path, struct stat *out) noexcept {
  static auto original = reinterpret_cast<int(*)(const char *, struct stat *)>(dlsym(RTLD_NEXT, "lstat"));
  countStat(path); return original(path, out);
}
extern "C" int stat64(const char *path, struct stat64 *out) noexcept {
  static auto original = reinterpret_cast<int(*)(const char *, struct stat64 *)>(dlsym(RTLD_NEXT, "stat64"));
  countStat(path); return original(path, out);
}
extern "C" int lstat64(const char *path, struct stat64 *out) noexcept {
  static auto original = reinterpret_cast<int(*)(const char *, struct stat64 *)>(dlsym(RTLD_NEXT, "lstat64"));
  countStat(path); return original(path, out);
}
#endif

class TestAssetPaging : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    PaleoTheme::ensureApplicationFonts(); qApp->setFont(PaleoTheme::bodyFont());
    PaleoTheme::applyLightTheme();
#ifdef Q_OS_LINUX
    guiThread = std::this_thread::get_id();
#endif
  }
  void scaleRenderingFilteringSortingSelection() {
    const int n = qEnvironmentVariableIntValue("PALEO_CATALOG_SCALE");
    if (n < 10000) QSKIP("PALEO_CATALOG_SCALE=10000 enables viewport/legacy ratio evidence");
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), n));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(1); panel.show();
    auto *table = panel.findChild<QTableWidget *>("assetTable"); QVERIFY(table);
    QCoreApplication::processEvents();
    QElapsedTimer timer;
    // Same 10k catalog, the replaced full-row/full-cell-widget refresh baseline.
    QTableWidget legacy(0, 3); legacy.resize(640, 420);
    QVector<qint64> baseline, refreshed, filtered, located;
    int baselineStats = 0, refreshStats = 0;
#ifdef Q_OS_LINUX
    traceStats = true;
#endif
    for (int attempt = 0; attempt < 3; ++attempt) {
      timer.start();
      legacy.setRowCount(0);
      for (const auto &a : importer.catalog()->assets()) {
        const int row = legacy.rowCount(); legacy.insertRow(row);
        legacy.setItem(row, 0, new QTableWidgetItem(a.displayName));
        legacy.setItem(row, 1, new QTableWidgetItem(a.type));
        legacy.setItem(row, 2, new QTableWidgetItem(a.id));
        // Association controls were eager; use a minimal widget baseline.
        legacy.setCellWidget(row, 2, new QWidget(&legacy));
        QFileInfo(DataCatalog::resolvedVersionPath(dir.path(), importer.catalog()->currentVersion(a.id))).size();
      }
      baseline << timer.nsecsElapsed();
#ifdef Q_OS_LINUX
      baselineStats += uiArtifactStats.exchange(0);
#endif
      timer.restart(); panel.refreshAssetTable(); refreshed << timer.nsecsElapsed();
#ifdef Q_OS_LINUX
      refreshStats += uiArtifactStats.exchange(0);
#endif
    }
#ifdef Q_OS_LINUX
    traceStats = false;
    QVERIFY(baselineStats >= n); // Proves the intercept observes actual Qt filesystem calls.
    QCOMPARE(refreshStats, 0);   // Refresh does no per-version file stat on the GUI thread.
    const auto storageReport = paleo::storage::scan(paleo::storage::snapshot(importer.catalog()));
    QVERIFY(storageReport.complete);
    traceStats = true;
    const auto emptyPreview = paleo::storage::preview(storageReport, {}, {});
    traceStats = false;
    QVERIFY(!emptyPreview.valid); QCOMPARE(uiArtifactStats.exchange(0), 0); // Preview reuses worker-normalized references.
#endif
    const auto median = [](QVector<qint64> v) { std::sort(v.begin(), v.end()); return v.at(v.size()/2); };
    const double refreshRatio = double(median(refreshed)) / median(baseline);
    QVERIFY2(refreshRatio < 1.0, qPrintable(QString("refresh/legacy ratio=%1").arg(refreshRatio)));
    const int pageSize = table->property("paleo.pageSize").toInt();
    QVERIFY(table->rowCount() <= pageSize); QVERIFY(table->rowCount() * 100 < n);
    QCOMPARE(table->property("paleo.totalAssets").toInt(), n);
    const int rendered = table->rowCount();
    auto assets = importer.catalog()->assets(); const auto target = assets.last();
    timer.restart(); panel.selectAssetInViews(target.id); located << timer.nsecsElapsed();
    QVERIFY(panel.currentAssetSelection().contains(target.id));
    bool found = false; for (int row = 0; row < table->rowCount(); ++row)
      found |= table->item(row, 0)->data(Qt::UserRole).toString() == target.id;
    QVERIFY(found);
    panel.refreshAssetTable(); QVERIFY(panel.currentAssetSelection().contains(target.id));
    auto *search = panel.findChild<QLineEdit *>("assetSearchEdit"); QVERIFY(search);
    timer.restart(); search->setText(target.displayName); panel.applyListFilter(); filtered << timer.nsecsElapsed();
    QCOMPARE(table->rowCount(), 1); QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toString(), target.id);
    QVERIFY(panel.currentAssetSelection().contains(target.id));
    search->clear(); panel.applyListFilter(); QVERIFY(panel.currentAssetSelection().contains(target.id));
    QVERIFY(search->text().isEmpty()); QVERIFY(table->rowCount() > 1);
    // Sort all logical rows, then first/last page navigation matches the global order.
    QMetaObject::invokeMethod(table->horizontalHeader(), "sectionClicked", Q_ARG(int, 0));
    QMetaObject::invokeMethod(table->horizontalHeader(), "sectionClicked", Q_ARG(int, 0));
    const auto last = *std::max_element(assets.begin(), assets.end(), [](const auto &a, const auto &b) { return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0; });
    panel.selectAssetInViews(last.id); QVERIFY(panel.currentAssetSelection().contains(last.id));
    auto *previous = panel.findChild<QPushButton *>("assetPreviousPage");
    while (previous->isEnabled()) previous->click();
    QCOMPARE(table->item(0, 0)->text(), last.displayName);
    for (qint64 cost : filtered + located) QVERIFY(double(cost) / median(baseline) < 1.0);
    qInfo() << "SCALE asset table" << n << "rendered" << rendered << "pageSize" << pageSize
            << "legacyStats" << baselineStats << "refreshStats" << refreshStats << "refresh/legacy" << refreshRatio << "filter/legacy" << double(filtered.first())/median(baseline)
            << "locate/legacy" << double(located.first())/median(baseline);
  }
  void viewportControlsBoundedAndSelectionRestored() {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 300));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    // Every asset is unresolved, exercising actual per-row entity combos.
    { DataCatalog::BatchSave batch(importer.catalog());
      const int count = importer.catalog()->links().size();
      for (int i = 0; i < count; ++i) QVERIFY(importer.catalog()->setLinkUnresolved(i));
    }
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(1); panel.show();
    panel.refreshAssetTable(); QCoreApplication::processEvents();
    auto *table = panel.findChild<QTableWidget *>("assetTable");
    QVERIFY(table->rowCount() <= table->property("paleo.pageSize").toInt());
    int controls = 0; for (int r = 0; r < table->rowCount(); ++r) controls += table->cellWidget(r, 2) != nullptr;
    QVERIFY(controls > 0); QVERIFY(controls <= table->property("paleo.pageSize").toInt());
    const auto choices = table->findChildren<QComboBox *>("resolveEntityCombo");
    QVERIFY(!choices.isEmpty());
    for (auto *combo : choices) { QCOMPARE(combo->model(), choices.first()->model()); QCOMPARE(combo->count(), 301); }
    const QString id = table->item(0, 0)->data(Qt::UserRole).toString(); table->selectRow(0);
    panel.refreshAssetTable(); QVERIFY(panel.currentAssetSelection().contains(id));
    auto *next = panel.findChild<QPushButton *>("assetNextPage"); QVERIFY(next->isEnabled()); next->click();
    QVERIFY(panel.currentAssetSelection().contains(id));
    panel.selectAssetInViews(id); QVERIFY(table->item(0, 0)->isSelected());
    panel.resize(640, 850); QCoreApplication::processEvents();
    QVERIFY(table->rowCount() <= table->property("paleo.pageSize").toInt());
  }
  // 数据导航树（用户契约）：大 catalog（>200，原降级分页树阈值）不再走
  // 「测区+当页拍平」——完整实体树，顶级五组（测区/测井/地震/成果图件/
  // 辅助资料）全部默认收拢；已挂井曲线只在井节点下（无「未关联曲线」
  // 重复面）；树形视图下分页器隐藏（树滚动，不分页）。
  void navTreeFullStructureBeyondPagingThreshold() {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 300));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(0); panel.show();
    panel.refreshAssetTable(); panel.applyListFilter(); QCoreApplication::processEvents();

    auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    QVERIFY2(tree->topLevelItemCount() >= 5,
             qPrintable(QStringLiteral("top=%1").arg(tree->topLevelItemCount())));
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("测区"));
    QVERIFY(tree->topLevelItem(1)->text(0).startsWith(QStringLiteral("测井 (300 井)")));
    QCOMPARE(tree->topLevelItem(2)->text(0), QStringLiteral("地震 (0)"));
    QCOMPARE(tree->topLevelItem(3)->text(0), QStringLiteral("成果图件 (0)"));
    QCOMPARE(tree->topLevelItem(4)->text(0), QStringLiteral("辅助资料 (0)"));
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      QVERIFY2(!tree->topLevelItem(i)->isExpanded(),
               qPrintable(tree->topLevelItem(i)->text(0)));

    // 300 口井全挂在「测井」组下（合成夹具每井一条已决 LAS，无未关联面）。
    QTreeWidgetItem *wellRoot = tree->topLevelItem(1);
    QCOMPARE(wellRoot->childCount(), 300);
    for (int i = 0; i < wellRoot->childCount(); ++i) {
      QVERIFY(!wellRoot->child(i)->text(0).startsWith(QStringLiteral("未关联曲线")));
      QVERIFY(!wellRoot->child(i)->text(0).startsWith(QStringLiteral("综合柱状图")));
      QVERIFY(wellRoot->child(i)->childCount() >= 1); // 曲线挂井节点下（一次，不重复）
    }

    // 树形视图（0）分页器隐藏——滚动条管全程，不翻页。
    auto *pager = panel.findChild<QWidget *>(QStringLiteral("assetPager"));
    QVERIFY(pager);
    QVERIFY2(!pager->isVisibleTo(&panel), "pager must hide in tree view");
    // 切到表视图（1）分页器回来（>200 仍分页，出界行为不变）。
    panel.setViewMode(1); panel.applyListFilter(); QCoreApplication::processEvents();
    QVERIFY(pager->isVisibleTo(&panel));
  }

  // 回归：树内点选三级资产（井 → 测井曲线）触发的选中联动不得把用户手动
  // 展开的一级组收掉——原实现 applyListFilter 的 !filtering 分支每次都把
  // 全部一级收拢，点击三级节点会把树跳回一级。
  void navTreeSelectionKeepsExpandedGroups()
  {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 300));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(0); panel.show();
    panel.refreshAssetTable(); panel.applyListFilter(); QCoreApplication::processEvents();

    auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    QTreeWidgetItem *wellRoot = tree->topLevelItem(1);
    wellRoot->setExpanded(true);
    wellRoot->child(0)->setExpanded(true);
    const QString lasId =
        wellRoot->child(0)->child(0)->data(0, Qt::UserRole).toString();
    QVERIFY(!lasId.isEmpty());

    panel.selectAssetInViews(lasId);
    QVERIFY2(wellRoot->isExpanded(),
             "tree-sourced selection must not collapse expanded top-level groups");
    QVERIFY2(wellRoot->child(0)->isExpanded(),
             "selected child's parent chain must stay expanded");

    // catalog.changed → refreshAssetTable → refreshAssetTree 全量重建路径
    // 同样不得抹掉展开态（重建前快照、建后按键还原）。
    panel.refreshAssetTable();
    auto *tree2 = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree2);
    QVERIFY2(tree2->topLevelItem(1)->isExpanded(),
             "rebuild must preserve expanded top-level groups");
    QVERIFY2(tree2->topLevelItem(1)->child(0)->isExpanded(),
             "rebuild must preserve expanded child nodes");
  }

  // 岩心照片（core 角色，高频多实例）收第四级：井 → 「岩心照片 (N)」分支
  // （L3，category 无 id）→ 照片叶（L4，assetId/wellId/core 三元不变——
  // 激活/拖放/过滤语义与平铺叶一致）；无照片井不出分支；跨井同名分支
  // 经父链键区分，重建后展开态互不串。
  void navTreeCorePhotosFourthLevel()
  {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 300));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    auto *cat = importer.catalog();
    const auto wells = cat->entities(QStringLiteral("well"));
    QVERIFY(wells.size() >= 3);
    QString err;
    for (int wIdx = 0; wIdx < 2; ++wIdx)
      for (int p = 0; p < 2; ++p)
      {
        CatalogAsset a;
        a.id = QStringLiteral("photo-%1-%2").arg(wIdx).arg(p);
        a.type = QStringLiteral("image");
        a.format = QStringLiteral("jpg");
        a.displayName = QStringLiteral("井%1,%2m.JPG").arg(wIdx + 1).arg(1250 + p * 10);
        QVERIFY2(cat->addAsset(a, &err), qPrintable(err));
        EntityAssetLink l;
        l.entityType = QStringLiteral("well");
        l.entityId = wells.at(wIdx).id;
        l.assetId = a.id;
        l.role = QStringLiteral("core");
        l.isPrimary = false;
        l.ordinal = p;
        QVERIFY2(cat->addLink(l, &err), qPrintable(err));
      }

    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(0); panel.show();
    panel.refreshAssetTable(); panel.applyListFilter(); QCoreApplication::processEvents();
    auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    const auto findWell = [](QTreeWidgetItem *root, const QString &wid) -> QTreeWidgetItem * {
      for (int i = 0; i < root->childCount(); ++i)
        if (root->child(i)->data(0, Qt::UserRole + 1).toString() == wid)
          return root->child(i);
      return nullptr;
    };
    QTreeWidgetItem *wellRoot = tree->topLevelItem(1);
    QTreeWidgetItem *w0 = findWell(wellRoot, wells.at(0).id);
    QTreeWidgetItem *w1 = findWell(wellRoot, wells.at(1).id);
    QTreeWidgetItem *w2 = findWell(wellRoot, wells.at(2).id);
    QVERIFY(w0 && w1 && w2);

    // w0：LAS 平铺叶 + 末尾「岩心照片 (2)」分支；照片叶在第四级。
    QCOMPARE(w0->childCount(), 2);
    QCOMPARE(w0->child(0)->data(0, Qt::UserRole + 2).toString(), QStringLiteral("well_log"));
    QTreeWidgetItem *branch0 = w0->child(1);
    QCOMPARE(branch0->text(0), QStringLiteral("岩心照片 (2)"));
    QVERIFY(branch0->data(0, Qt::UserRole).toString().isEmpty());
    QCOMPARE(branch0->data(0, Qt::UserRole + 2).toString(), QStringLiteral("category"));
    QCOMPARE(branch0->childCount(), 2);
    for (int p = 0; p < 2; ++p)
    {
      QTreeWidgetItem *leaf = branch0->child(p);
      QCOMPARE(leaf->data(0, Qt::UserRole).toString(), QStringLiteral("photo-0-%1").arg(p));
      QCOMPARE(leaf->data(0, Qt::UserRole + 1).toString(), wells.at(0).id);
      QCOMPARE(leaf->data(0, Qt::UserRole + 2).toString(), QStringLiteral("core"));
      QCOMPARE(leaf->text(0), QStringLiteral("井1,%1m.JPG").arg(1250 + p * 10));
    }
    // 无照片井不出分支。
    QCOMPARE(w2->childCount(), 1);

    // 跨井同名分支的展开态还原互不串（父链键）：只展开 w0 的分支 →
    // 全量重建后 w0 分支仍展开、w1 分支保持收拢。
    wellRoot->setExpanded(true);
    w0->setExpanded(true);
    branch0->setExpanded(true);
    panel.refreshAssetTable();
    auto *tree2 = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QTreeWidgetItem *wellRoot2 = tree2->topLevelItem(1);
    QTreeWidgetItem *w0r = findWell(wellRoot2, wells.at(0).id);
    QTreeWidgetItem *w1r = findWell(wellRoot2, wells.at(1).id);
    QVERIFY(w0r && w1r);
    QVERIFY2(w0r->isExpanded() && w0r->child(1)->isExpanded(),
             "expanded photo branch must survive rebuild");
    QVERIFY2(!w1r->child(1)->isExpanded(),
             "sibling well's collapsed branch must not be restored by key collision");
  }

  void governanceUiPreviewCancelAndTokens() {
    paleo::storage::Report report; report.complete = true; report.scannedRoots = {"artifacts/RAW"};
    paleo::storage::FileFact f; f.relativePath = "artifacts/RAW/unreferenced.bin"; f.sizeBytes = 42;
    report.orphans = {f};
    StorageGovernanceDialog dialog; dialog.setReport(report); dialog.show();
    auto *preview = dialog.findChild<QPushButton *>("storagePreviewButton"); QVERIFY(preview && preview->isEnabled());
    QSignalSpy confirm(&dialog, &StorageGovernanceDialog::confirmRequested);
    paleo::storage::Preview plan; plan.orphanFiles = {f}; plan.files = {f}; plan.bytes = 42; plan.valid = true;
    dialog.setPreview(plan);
    auto *confirmation = dialog.findChild<QDialog *>("storageCleanupPreview"); QVERIFY(confirmation);
    auto *summary = confirmation->findChild<QLabel *>("storagePreviewSummary"); QVERIFY(summary->text().contains("42 B"));
    const QString previewCapture = qEnvironmentVariable("PALEO_GOV_PREVIEW_CAPTURE");
    if (!previewCapture.isEmpty()) { QCoreApplication::processEvents(); QVERIFY(confirmation->grab().save(previewCapture)); }
    confirmation->reject(); QCOMPARE(confirm.size(), 0);
    dialog.setBusy(true, true); QVERIFY(dialog.findChild<QPushButton *>("storageCancelButton")->isEnabled());
    dialog.setBusy(true, false); QVERIFY(!dialog.findChild<QPushButton *>("storageCancelButton")->isEnabled());
    for (const char *name : {"storageOrphans", "storageStale", "storageByType", "storageProgress", "storagePreviewButton"}) {
      auto *widget = dialog.findChild<QWidget *>(name); QVERIFY(widget); QVERIFY(!widget->accessibleName().isEmpty());
    }
    const QString capture = qEnvironmentVariable("PALEO_GOV_CAPTURE");
    if (!capture.isEmpty()) { dialog.findChild<QTabWidget *>("storageTabs")->setCurrentIndex(2); dialog.setBusy(false, true); QCoreApplication::processEvents(); QVERIFY(dialog.grab().save(capture)); }
  }
};
QTEST_MAIN(TestAssetPaging)
#include "tst_assetpaging.moc"
