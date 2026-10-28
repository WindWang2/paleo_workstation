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
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTabWidget>
#include <QApplication>
#include <QDialog>
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
#include "../src/workflow/importledger.h"

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

namespace {
// 顶级导航根按文本前缀定位：#565 之后树顶新增「工区概况」「现有古地理图」
// 「工区图件输出」三根，实体分组整体后移——索引式断言随根结构反复脆断
// （曾致 topLevelItem(1) 指向空根、child(0) 空指针段错误），改按文本查找。
QTreeWidgetItem *topLevelRootByPrefix(QTreeWidget *tree, const QString &prefix)
{
  if (!tree)
    return nullptr;
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
    if (tree->topLevelItem(i)->text(0).startsWith(prefix))
      return tree->topLevelItem(i);
  return nullptr;
}
} // namespace

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
    // 阶段计时（任务 B 证据）：夹具生成 / 目录打开 与刷新链分开计时——
    // 不把夹具或打开成本算进刷新；旧实现对照是独立 QTableWidget 的同样本
    // 3 次循环。全部绝对值以毫秒报告（比值之外保留原始量）。
    QElapsedTimer timer;
    const qint64 tFixtureStart = timer.nsecsElapsed();
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), n));
    const qint64 tFixture = timer.nsecsElapsed() - tFixtureStart;
    timer.restart();
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    const qint64 tOpen = timer.nsecsElapsed();
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(1); panel.show();
    auto *table = panel.findChild<QTableWidget *>("assetTable"); QVERIFY(table);
    QCoreApplication::processEvents();
    // Same 10k catalog, the replaced full-row/full-cell-widget refresh baseline.
    QTableWidget legacy(0, 3); legacy.resize(640, 420);
    QVector<qint64> baseline, refreshed, filtered, located;
    qint64 tSort = 0, tRestore = 0;
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
    timer.restart(); panel.refreshAssetTable(); tRestore = timer.nsecsElapsed();
    QVERIFY(panel.currentAssetSelection().contains(target.id));
    auto *search = panel.findChild<QLineEdit *>("assetSearchEdit"); QVERIFY(search);
    timer.restart(); search->setText(target.displayName); panel.applyListFilter(); filtered << timer.nsecsElapsed();
    QCOMPARE(table->rowCount(), 1); QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toString(), target.id);
    QVERIFY(panel.currentAssetSelection().contains(target.id));
    search->clear(); panel.applyListFilter(); QVERIFY(panel.currentAssetSelection().contains(target.id));
    QVERIFY(search->text().isEmpty()); QVERIFY(table->rowCount() > 1);
    // Sort all logical rows, then first/last page navigation matches the global order.
    timer.restart();
    QMetaObject::invokeMethod(table->horizontalHeader(), "sectionClicked", Q_ARG(int, 0));
    QMetaObject::invokeMethod(table->horizontalHeader(), "sectionClicked", Q_ARG(int, 0));
    tSort = timer.nsecsElapsed();
    const auto last = *std::max_element(assets.begin(), assets.end(), [](const auto &a, const auto &b) { return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0; });
    panel.selectAssetInViews(last.id); QVERIFY(panel.currentAssetSelection().contains(last.id));
    auto *previous = panel.findChild<QPushButton *>("assetPreviousPage");
    while (previous->isEnabled()) previous->click();
    QCOMPARE(table->item(0, 0)->text(), last.displayName);
    for (qint64 cost : filtered + located) QVERIFY(double(cost) / median(baseline) < 1.0);
    const qint64 toMs = 1000000;
    qInfo() << "SCALE asset table" << n << "rendered" << rendered << "pageSize" << pageSize
            << "legacyStats" << baselineStats << "refreshStats" << refreshStats << "refresh/legacy" << refreshRatio << "filter/legacy" << double(filtered.first())/median(baseline)
            << "locate/legacy" << double(located.first())/median(baseline)
            << "ms: fixture" << tFixture / toMs << "open" << tOpen / toMs
            << "legacy" << median(baseline) / toMs << "refresh" << median(refreshed) / toMs
            << "filter" << filtered.first() / toMs << "sort" << tSort / toMs
            << "locate" << located.first() / toMs << "restoreRefresh" << tRestore / toMs;
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
  // 任务 B（树刷新挂脏延迟）行为守卫：表格视图下的刷新不重建导航树
  //（万条资产实测 ~250ms/次的瓶颈段）；切回树视图必须补建到**最新**目录
  // 状态（含表格视图期间新增的资产）并按持久选中身份还原树选中——树可见
  // 期间数据从不陈旧。
  void treeDeferredRefreshRecoversOnViewSwitch() {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 300));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(1); panel.show();
    panel.refreshAssetTable(); QCoreApplication::processEvents();
    auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    // 表格视图：树从未构建（挂脏路径真的跳过了全量重建）。
    QCOMPARE(tree->topLevelItemCount(), 0);
    // 表格视图下选中一个资产（选中身份持久在 m_pageSelection）。
    const QString id = importer.catalog()->assets().constFirst().id;
    panel.selectAssetInViews(id);
    QVERIFY(panel.currentAssetSelection().contains(id));
    // 表格视图期间目录前进：新增资产后再刷新（树继续挂脏）。
    CatalogAsset extra;
    extra.id = QStringLiteral("defer-extra-1");
    extra.type = QStringLiteral("document");
    extra.format = QStringLiteral("pdf");
    extra.displayName = QStringLiteral("defer-extra.pdf");
    QString err;
    QVERIFY2(importer.catalog()->addAsset(extra, &err), qPrintable(err));
    panel.refreshAssetTable();
    QCOMPARE(tree->topLevelItemCount(), 0); // 仍不重建
    // 切回树视图：补建到最新状态（新资产可见）且选中还原。
    panel.setViewMode(0);
    QCoreApplication::processEvents();
    QVERIFY2(tree->topLevelItemCount() >= 5,
             qPrintable(QStringLiteral("top=%1").arg(tree->topLevelItemCount())));
    bool found = false, selectedPreserved = false;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
      const QString itemId = (*it)->data(0, Qt::UserRole).toString();
      found |= itemId == QStringLiteral("defer-extra-1");
      selectedPreserved |= itemId == id && (*it)->isSelected();
    }
    QVERIFY2(found, "树补建未包含表格视图期间新增的资产");
    QVERIFY2(selectedPreserved, "切回树视图后选中未还原");
    // 树视图模式下刷新回到立即重建路径（零变更 → Unchanged；现有 navTree*
    // 测试覆盖树内增删改的正确性）。
    panel.refreshAssetTable();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::Unchanged));
  }

  // 数据导航树（用户契约）：大 catalog（>200，原降级分页树阈值）不再走
  // 「测区+当页拍平」——完整实体树，实体分组（测井/地震/成果图件/辅助资料
  // /计划井）全部默认收拢；#565 起树顶另有「工区概况」（按设计默认展开）、
  // 「现有古地理图」「工区图件输出」三根；已挂井曲线只在井节点下（无
  // 「未关联曲线」重复面）；树形视图下分页器隐藏（树滚动，不分页）。
  void navTreeFullStructureBeyondPagingThreshold() {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 300));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(0); panel.show();
    panel.refreshAssetTable(); panel.applyListFilter(); QCoreApplication::processEvents();

    auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    QVERIFY2(tree->topLevelItemCount() >= 5,
             qPrintable(QStringLiteral("top=%1").arg(tree->topLevelItemCount())));
    // 顶级根按文本定位（#565 新增三根后索引整体后移，不再钉位次）。
    auto *overviewRoot = topLevelRootByPrefix(tree, QStringLiteral("工区概况"));
    auto *paleomapRoot = topLevelRootByPrefix(tree, QStringLiteral("现有古地理图"));
    auto *outputRoot = topLevelRootByPrefix(tree, QStringLiteral("工区图件输出"));
    auto *seismicRoot = topLevelRootByPrefix(tree, QStringLiteral("地震 ("));
    auto *productRoot = topLevelRootByPrefix(tree, QStringLiteral("成果图件 ("));
    auto *auxRoot = topLevelRootByPrefix(tree, QStringLiteral("辅助资料 ("));
    QVERIFY(overviewRoot && paleomapRoot && outputRoot);
    QVERIFY(seismicRoot && productRoot && auxRoot);
    QTreeWidgetItem *wellRoot = topLevelRootByPrefix(tree, QStringLiteral("测井 ("));
    QVERIFY2(wellRoot != nullptr, "测井根缺失");
    QCOMPARE(wellRoot->text(0), QStringLiteral("测井 (300 井)"));
    QCOMPARE(seismicRoot->text(0), QStringLiteral("地震 (0)"));
    QCOMPARE(productRoot->text(0), QStringLiteral("成果图件 (0)"));
    QCOMPARE(auxRoot->text(0), QStringLiteral("辅助资料 (0)"));
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
      QTreeWidgetItem *root = tree->topLevelItem(i);
      // 「工区概况」按 #565 设计默认展开（看板/工区地图常驻视野）；
      // 「辅助资料」有子节点时按产品规则展开（datalist_tree.cpp 渲染段：
      // 避免已导入文档停在收拢组里），同样不参与收拢断言。
      const QString t = root->text(0);
      if (t == QStringLiteral("工区概况") || t.startsWith(QStringLiteral("辅助资料 (")))
        continue;
      QVERIFY2(!root->isExpanded(), qPrintable(t));
    }

    // 300 口井全挂在「测井」组下（合成夹具每井一条已决 LAS，无未关联面）。
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
    QTreeWidgetItem *wellRoot = topLevelRootByPrefix(tree, QStringLiteral("测井 ("));
    QVERIFY2(wellRoot != nullptr, "测井根缺失");
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
    QTreeWidgetItem *wellRoot2 = topLevelRootByPrefix(tree2, QStringLiteral("测井 ("));
    QVERIFY2(wellRoot2 != nullptr, "测井根缺失");
    QVERIFY2(wellRoot2->isExpanded(),
             "rebuild must preserve expanded top-level groups");
    QVERIFY2(wellRoot2->child(0)->isExpanded(),
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
    QTreeWidgetItem *wellRoot = topLevelRootByPrefix(tree, QStringLiteral("测井 ("));
    QVERIFY2(wellRoot != nullptr, "测井根缺失");
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
    QTreeWidgetItem *wellRoot2 = topLevelRootByPrefix(tree2, QStringLiteral("测井 ("));
    QVERIFY2(wellRoot2 != nullptr, "测井根缺失");
    QTreeWidgetItem *w0r = findWell(wellRoot2, wells.at(0).id);
    QTreeWidgetItem *w1r = findWell(wellRoot2, wells.at(1).id);
    QVERIFY(w0r && w1r);
    QVERIFY2(w0r->isExpanded() && w0r->child(1)->isExpanded(),
             "expanded photo branch must survive rebuild");
    QVERIFY2(!w1r->child(1)->isExpanded(),
             "sibling well's collapsed branch must not be restored by key collision");
  }

  // 方向 92：树增量通道——资产级增/改/删落节点级更新（QSignalSpy 行级证据
  // + 结构断言 + 节点指针稳定性）；#270 展开态在增量路径原地保留；变更面
  // 超阈值回全量兜底。
  void navTreeIncrementalAddUpdateRemove() {
    // 5 井夹具：初建全量插入 20 节点 > 兜底下限 16——初建必走全量通道。
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 5));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(0); panel.show();
    panel.refreshAssetTable(); panel.applyListFilter(); QCoreApplication::processEvents();
    auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    QTreeWidgetItem *wellRoot = topLevelRootByPrefix(tree, QStringLiteral("测井 ("));
    QVERIFY2(wellRoot != nullptr, "测井根缺失");
    QCOMPARE(wellRoot->childCount(), 5);
    // 初建（空树 → 全量插入超阈值）走全量兜底。
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::FullRebuild));
    // 零变更再刷 → Unchanged（不 clear、不碰展开/滚动）。
    panel.refreshAssetTable();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::Unchanged));

    DataCatalog *cat = importer.catalog();
    const QString wellId = cat->entities(QStringLiteral("well")).at(1).id;
    const auto wellItemOf = [wellRoot](const QString &wid) -> QTreeWidgetItem * {
      for (int i = 0; i < wellRoot->childCount(); ++i)
        if (wellRoot->child(i)->data(0, Qt::UserRole + 1).toString() == wid)
          return wellRoot->child(i);
      return nullptr;
    };
    QTreeWidgetItem *wellItem = wellItemOf(wellId);
    QVERIFY(wellItem);
    const int before = wellItem->childCount();
    QCOMPARE(wellItem->child(0)->data(0, Qt::UserRole + 2).toString(), QStringLiteral("well_log"));

    // ---- 增：新资产 + 词表内角色链接 → 节点级插入（井节点指针不变）----
    QSignalSpy spyInserted(tree->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy spyRemoved(tree->model(), &QAbstractItemModel::rowsRemoved);
    CatalogAsset a;
    a.id = QStringLiteral("tops-extra-1");
    a.type = QStringLiteral("tops");
    a.format = QStringLiteral("csv");
    a.displayName = QStringLiteral("extra-tops.csv");
    QString err;
    QVERIFY2(cat->addAsset(a, &err), qPrintable(err));
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = wellId;
    l.assetId = a.id;
    l.role = QStringLiteral("tops");
    QVERIFY2(cat->addLink(l, &err), qPrintable(err));
    wellRoot->setExpanded(true);
    wellItem->setExpanded(true);
    panel.refreshAssetTable(); QCoreApplication::processEvents();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::Incremental));
    // #270 语义：增量不 clear——展开态与节点身份原地保留。
    QVERIFY(wellRoot->isExpanded());
    QVERIFY(wellItem->isExpanded());
    QCOMPARE(wellItemOf(wellId), wellItem);
    QCOMPARE(wellItem->childCount(), before + 1);
    QTreeWidgetItem *added = wellItem->child(before);
    QCOMPARE(added->data(0, Qt::UserRole).toString(), QStringLiteral("tops-extra-1"));
    QCOMPARE(added->data(0, Qt::UserRole + 1).toString(), wellId);
    QCOMPARE(added->data(0, Qt::UserRole + 2).toString(), QStringLiteral("tops"));
    QVERIFY(added->text(0).contains(QStringLiteral("井分层")));
    QVERIFY(added->text(0).contains(QStringLiteral("extra-tops.csv")));
    // 行级证据：一次插入、零删除（全量重建必然伴随整树删除）。
    QCOMPARE(spyInserted.count(), 1);
    QCOMPARE(spyRemoved.count(), 0);

    // ---- 改：主文件显式切换 → 两个叶就地改文案（无增无删）----
    CatalogAsset b;
    b.id = QStringLiteral("log-extra-2");
    b.type = QStringLiteral("well_log");
    b.format = QStringLiteral("las");
    b.displayName = QStringLiteral("extra-run2.las");
    QVERIFY2(cat->addAsset(b, &err), qPrintable(err));
    EntityAssetLink l2;
    l2.entityType = QStringLiteral("well");
    l2.entityId = wellId;
    l2.assetId = b.id;
    l2.role = QStringLiteral("well_log");
    l2.isPrimary = false;
    l2.ordinal = 1;
    QVERIFY2(cat->addLink(l2, &err), qPrintable(err));
    panel.refreshAssetTable();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::Incremental));
    QCOMPARE(wellItem->childCount(), before + 2);
    QVERIFY(wellItem->child(0)->text(0).contains(QStringLiteral("主文件")));
    QVERIFY(wellItem->child(1)->text(0).contains(QStringLiteral("成员")));

    QSignalSpy spyChanged(tree, &QTreeWidget::itemChanged);
    QSignalSpy spyInserted2(tree->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy spyRemoved2(tree->model(), &QAbstractItemModel::rowsRemoved);
    // setLinkPrimary 的 index 是 links() 全表序（非 linksForEntity 子集序）。
    int promotedIdx = -1;
    const QVector<EntityAssetLink> allLinks = cat->links();
    for (int i = 0; i < allLinks.size(); ++i)
      if (allLinks.at(i).assetId == b.id && !allLinks.at(i).unresolved)
        promotedIdx = i;
    QVERIFY(promotedIdx >= 0);
    QVERIFY2(cat->setLinkPrimary(promotedIdx, &err), qPrintable(err));
    panel.refreshAssetTable();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::Incremental));
    QCOMPARE(wellItem->childCount(), before + 2);
    QVERIFY(wellItem->child(0)->text(0).contains(QStringLiteral("成员")));
    QVERIFY(wellItem->child(1)->text(0).contains(QStringLiteral("主文件")));
    // 就地改写证据：既有叶 itemChanged ≥ 2，无行增删。
    QVERIFY2(spyChanged.count() >= 2,
             qPrintable(QStringLiteral("itemChanged=%1").arg(spyChanged.count())));
    QCOMPARE(spyInserted2.count(), 0);
    QCOMPARE(spyRemoved2.count(), 0);

    // ---- 删：removeAsset → 叶级删除 ----
    QSignalSpy spyInserted3(tree->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy spyRemoved3(tree->model(), &QAbstractItemModel::rowsRemoved);
    QVERIFY2(cat->removeAsset(b.id, &err), qPrintable(err));
    panel.refreshAssetTable();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::Incremental));
    QCOMPARE(wellItem->childCount(), before + 1);
    QCOMPARE(spyInserted3.count(), 0);
    QCOMPARE(spyRemoved3.count(), 1);
    // 删除后井节点指针与展开态仍在。
    QCOMPARE(wellItemOf(wellId), wellItem);
    QVERIFY(wellItem->isExpanded());

    // ---- 兜底阈值：一批变更数超 max(16, 节点数/4) → 全量重建 ----
    {
      DataCatalog::BatchSave batch(cat);
      for (int i = 0; i < DataListPanel::kNavReconcileFloor + 4; ++i)
      {
        CatalogAsset m;
        m.id = QStringLiteral("aux-flood-%1").arg(i);
        m.type = QStringLiteral("document");
        m.format = QStringLiteral("pdf");
        m.displayName = QStringLiteral("flood-%1.pdf").arg(i);
        QVERIFY2(cat->addAsset(m, &err), qPrintable(err));
      }
      QVERIFY2(batch.flush(&err), qPrintable(err));
    }
    panel.refreshAssetTable();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::FullRebuild));
    // 全量重建后结构一致（20 份文档进辅助资料参考资料分支）。
    int auxAdded = 0;
    for (int t = 0; t < tree->topLevelItemCount(); ++t)
      if (tree->topLevelItem(t)->text(0).startsWith(QStringLiteral("辅助资料")))
        for (int c = 0; c < tree->topLevelItem(t)->childCount(); ++c)
          auxAdded += tree->topLevelItem(t)->child(c)->childCount();
    QCOMPARE(auxAdded, DataListPanel::kNavReconcileFloor + 4);
  }

  // 方向 92：词表外角色（工程自定义）落「未分组」可见桶——不静默藏匿、
  // 不与词表角色混排；桶分支随角色出现/消失走节点级增删。
  void navTreeUngroupedBucketForUnknownRole() {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 3));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    auto *cat = importer.catalog();
    const QString wellId = cat->entities(QStringLiteral("well")).at(0).id;
    CatalogAsset a;
    a.id = QStringLiteral("custom-doc-1");
    a.type = QStringLiteral("document");
    a.format = QStringLiteral("dat");
    a.displayName = QStringLiteral("custom-thing.dat");
    QString err;
    QVERIFY2(cat->addAsset(a, &err), qPrintable(err));
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = wellId;
    l.assetId = a.id;
    l.role = QStringLiteral("my_exotic_role");
    QVERIFY2(cat->addLink(l, &err), qPrintable(err));

    DataListPanel panel; panel.setDocService(&doc); panel.resize(640, 740); panel.setViewMode(0); panel.show();
    panel.refreshAssetTable(); panel.applyListFilter(); QCoreApplication::processEvents();
    auto *tree = panel.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
    QVERIFY(tree);
    QTreeWidgetItem *wellRoot = topLevelRootByPrefix(tree, QStringLiteral("测井 ("));
    QVERIFY2(wellRoot != nullptr, "测井根缺失");
    QTreeWidgetItem *wellItem = nullptr;
    for (int i = 0; i < wellRoot->childCount(); ++i)
      if (wellRoot->child(i)->data(0, Qt::UserRole + 1).toString() == wellId)
        wellItem = wellRoot->child(i);
    QVERIFY(wellItem);
    QCOMPARE(wellItem->childCount(), 2); // 词表内 LAS 叶 + 「未分组」桶
    QTreeWidgetItem *las = wellItem->child(0);
    QCOMPARE(las->data(0, Qt::UserRole + 2).toString(), QStringLiteral("well_log"));
    QTreeWidgetItem *bucket = wellItem->child(1);
    QCOMPARE(bucket->text(0), QStringLiteral("未分组 (1)"));
    QCOMPARE(bucket->data(0, Qt::UserRole + 2).toString(), QStringLiteral("category"));
    QCOMPARE(bucket->data(0, Qt::UserRole + 3).toString(), QStringLiteral("ungrouped"));
    QCOMPARE(bucket->childCount(), 1);
    QTreeWidgetItem *leaf = bucket->child(0);
    QCOMPARE(leaf->data(0, Qt::UserRole).toString(), QStringLiteral("custom-doc-1"));
    QCOMPARE(leaf->data(0, Qt::UserRole + 1).toString(), wellId);
    QCOMPARE(leaf->data(0, Qt::UserRole + 2).toString(), QStringLiteral("my_exotic_role"));
    QCOMPARE(leaf->text(0), QStringLiteral("custom-thing.dat"));
    QCOMPARE(leaf->text(1), QStringLiteral("my_exotic_role")); // 词表外角色名如实展示

    // 桶随资产删除而消失（节点级增删，不走全量）。
    QVERIFY2(cat->removeAsset(a.id, &err), qPrintable(err));
    panel.refreshAssetTable();
    QCOMPARE(int(panel.lastTreeRefreshMode()), int(DataListPanel::TreeRefreshMode::Incremental));
    QCOMPARE(wellItem->childCount(), 1);
    QCOMPARE(wellItem->child(0)->data(0, Qt::UserRole + 2).toString(), QStringLiteral("well_log"));
  }

  // A3（方向 01）：批量改型（视图层改写）后表视图类型列、类型过滤的可见性
  // 必须即时就位——增量通道只更新虚拟模型不清过滤口径的旧缺陷回归；同时
  // 钉住「同集同序同数据跳过 setRows」：applyListFilter 不再无差别重置
  // 虚拟视图选区。
  void batchTypeOverrideRefreshesViewsAndKeepsFilter() {
    QTemporaryDir dir;
    QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 6));
    DataImportService importer; importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc);
    panel.resize(640, 740); panel.setViewMode(3); panel.show();
    panel.refreshAssetTable();
    QCoreApplication::processEvents();
    auto *table = panel.findChild<QTableWidget *>("assetTable"); QVERIFY(table);
    const QString target = importer.catalog()->assets().constFirst().id;
    panel.selectAssetInViews(target);
    QVERIFY(panel.currentAssetSelection().contains(target));

    // 模态驱动：确认对话框选 tops 并确认（无捕获 lambda——重试定时器不悬垂）。
    static const auto driveBatchType = [] {
      auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dlg)
        return false;
      auto *combo = dlg->findChild<QComboBox *>("batchTypeCombo");
      auto *box = dlg->findChild<QDialogButtonBox *>();
      if (!combo || !box)
        return false;
      const int tops = combo->findData(QStringLiteral("tops"));
      if (tops < 0) return false;
      combo->setCurrentIndex(tops);
      box->button(QDialogButtonBox::Ok)->click();
      return true;
    };
    QTimer::singleShot(0, [] {
      if (!driveBatchType())
        QTimer::singleShot(20, nullptr, [] { driveBatchType(); });
    });
    QMetaObject::invokeMethod(&panel, "batchChangeType");
    QCoreApplication::processEvents();

    // 表视图类型列即时显示新类型（旧缺陷：保持 well_log 直到任意下次刷新）。
    int targetRow = -1;
    for (int r = 0; r < table->rowCount(); ++r)
      if (table->item(r, 0)->data(Qt::UserRole).toString() == target)
        targetRow = r;
    QVERIFY(targetRow >= 0);
    QCOMPARE(table->item(targetRow, 1)->text(), QStringLiteral("tops"));
    // 选区保持：增量分支快照 m_pageSelection → applyListFilter 重灌表体后
    // 由 m_selKeep 还原表选中（sameRowsAs 只在数据零变化时跳过 setRows，
    // 改型路径 effectiveType 已变、重建是预期行为）。
    QVERIFY(panel.currentAssetSelection().contains(target));

    // 类型过滤按生效类型重算：筛 well_log 时目标行隐藏（旧缺陷：仍可见）。
    auto *typeFilter = panel.findChild<QComboBox *>("assetTypeFilter"); QVERIFY(typeFilter);
    const int wl = typeFilter->findData(QStringLiteral("well_log"));
    QVERIFY(wl >= 0);
    typeFilter->setCurrentIndex(wl);
    panel.applyListFilter();
    for (int r = 0; r < table->rowCount(); ++r)
      if (table->item(r, 0)->data(Qt::UserRole).toString() == target)
        QVERIFY(table->isRowHidden(r));
    // 筛掉的目标不参与批量命令；清过滤后按身份恢复选区。
    QVERIFY(!panel.currentAssetSelection().contains(target));
    typeFilter->setCurrentIndex(0);
    panel.applyListFilter();
    QVERIFY(panel.currentAssetSelection().contains(target));
  }

  // C1（方向 01）：批量打标签 N 条命令一次收口刷新——旧缺陷 N 项选中即
  // N 次整表重建（与 dataopsundo.h「刷新由栈信号订阅方统一做」的设计注释
  // 相悖）。观察面：entityRefreshRequested 恰一次 + sidecar 功能不变。
  void batchAddTagRefreshesOnce() {
    QTemporaryDir dir;
    QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 4));
    DataImportService importer; importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc);
    panel.resize(640, 740); panel.setViewMode(1); panel.show();
    panel.refreshAssetTable();
    QCoreApplication::processEvents();
    panel.selectAllVisibleAssets();
    QCOMPARE(int(panel.currentAssetSelection().size()), 4);

    QSignalSpy refreshSpy(&panel, &DataListPanel::entityRefreshRequested);
    static const auto driveTagInput = [] {
      auto *dlg = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
      if (!dlg)
        return false;
      dlg->setTextValue(QStringLiteral("batch-tag"));
      dlg->accept();
      return true;
    };
    QTimer::singleShot(0, [] {
      if (!driveTagInput())
        QTimer::singleShot(20, nullptr, [] { driveTagInput(); });
    });
    QMetaObject::invokeMethod(&panel, "batchAddTag");
    QCoreApplication::processEvents();
    QCOMPARE(refreshSpy.count(), 1); // 4 项一次收口（旧缺陷：4 次）

    // 功能面不回归：sidecar 四个资产都带上标签。
    QFile tags(QDir(dir.path()).filePath(QStringLiteral(".paleo/asset_tags.json")));
    QVERIFY(tags.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(tags.readAll()).object();
    const QJsonObject tagMap = root.value(QStringLiteral("tags")).toObject();
    QCOMPARE(tagMap.size(), 4);
    for (auto it = tagMap.constBegin(); it != tagMap.constEnd(); ++it)
      QVERIFY(it.value().toArray().contains(QStringLiteral("batch-tag")));
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
  // opt-01 U2：损坏导入台账在查看器入口给出可解释原因（不再静默显示 0 批）。
  void importLedgerViewerReportsCorruptLedger() {
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 8));
    DataImportService importer; importer.setProjectDir(dir.path()); PreviewDocService doc(&importer);
    DataListPanel panel; panel.setDocService(&doc); panel.show(); QVERIFY(QTest::qWaitForWindowExposed(&panel)); panel.refreshAssetTable();
    paleo::imports::ImportLedger probe; probe.load(importer.catalog());
    QVERIFY(!probe.filePath().isEmpty());
    QDir().mkpath(QFileInfo(probe.filePath()).absolutePath());
    { QFile f(probe.filePath()); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{broken"); }
    QSignalSpy status(&panel, &DataListPanel::statusMessage);
    bool sawDialog = false;
    QString banner;
    QTimer::singleShot(0, [&sawDialog, &banner] {
      if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        sawDialog = true;
        if (auto *w = dlg->findChild<QLabel *>(QStringLiteral("importLedgerWarning")))
          banner = w->textFormat() == Qt::PlainText ? w->text() : QStringLiteral("<非纯文本>");
        dlg->reject();
      }
    });
    panel.showImportLedger();
    QVERIFY(sawDialog);
    QVERIFY2(banner.contains(QStringLiteral("损坏")), qPrintable(banner)); // 查看器内可见，不只状态栏
    bool reported = false;
    for (const auto &args : status) reported = reported || args.at(0).toString().contains(QStringLiteral("损坏"));
    QVERIFY2(reported, "损坏台账未给出原因");
    QFile f(probe.filePath()); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), QByteArray("{broken")); // 只读查看不改原件
  }
};
QTEST_MAIN(TestAssetPaging)
#include "tst_assetpaging.moc"
