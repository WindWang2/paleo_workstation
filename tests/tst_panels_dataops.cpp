// 层：测试壳
// #503 巨型测试拆分第一步：tst_panels.cpp（5337 行）按被测域拆出第一个新
// 目标——DataOps 数据页 D1–D10 用例族（wave/data-page-operations：多选/操作
// 矩阵/过滤/拖拽/撤销栈/命令面板/视图形态/导入队列）。用例逐字搬运，断言
// 与注释零改动；原 TestPanels 私有段的本域夹具已随迁至
// tests/helpers/dataopsfixture.h（与 tst_panels.cpp 共用）。
// 注册（根 CMakeLists，主 Agent 统一落地）：add_paleo_test(tst_panels_dataops)

#include "../src/qgis/layervocabulary.h"
#include "perfbudget_relax.h"
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QResizeEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "../src/services/previewdoc.h"
#include "../src/ui/dialogs/folderconfirm.h"
#include "../src/workflow/folderimport.h"
#include "../src/ui/pages/pagepanels.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/workflow/workflows.h"
#include "uipolish_capture.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/io/dataimportservice.h"
#include "../src/domain/projectclassifier.h"
#include "../src/catalog/datacatalog.h"
// P3 dataops 测试面（wave/data-page-operations）：pages/dataops 头直连。
#include "../src/ui/pages/dataopspanelops.h"
#include "../src/ui/pages/dataopspanelextra.h"
#include "../src/ui/pages/dataopspalette.h"
#include "../src/ui/pages/dataopsimportui.h"
#include "../src/ui/pages/dataopsviews.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/pages/datanavtree.h"
#include "../src/ui/pages/dataopswidgets.h"
#include "helpers/dataopsfixture.h"

using paleo::tests::dataops::DataOpsFixture;
using paleo::tests::dataops::driveModalNextTick;
using paleo::tests::dataops::writeFile;
using paleo::tests::dataops::tableRowForPath;

class TestPanelsDataOps : public QObject
{
  Q_OBJECT

  private slots:
    void filteredOutSelectionCannotReachBatchCommands()
    {
        QTemporaryDir dir;
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->listPanel();
        lp->setViewMode(2);
        lp->selectAssetInViews(fx.astResolved);
        QVERIFY(lp->currentAssetSelection().contains(fx.astResolved));
        lp->setViewMode(1);
        paleo::dataops::FilterGroup filter;
        filter.conditions.append({paleo::dataops::FilterDim::Search,
                                  QStringLiteral("no-such-asset"), false});
        lp->applyFilterGroup(filter);
        QVERIFY(lp->currentAssetSelection().isEmpty());
    }

    void entityPrimaryActionUsesSharedUndoStack()
    {
        QTemporaryDir dir;
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        EntityAssetLink member;
        member.entityType = QStringLiteral("well");
        member.entityId = fx.wellA;
        member.assetId = fx.astFree;
        member.role = QStringLiteral("well_log");
        member.isPrimary = false;
        QVERIFY(svc.catalog()->addLink(member));
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *panel = qobject_cast<EntityPanel *>(page->entityViewSection());
        QVERIFY(panel);
        const auto primary = [&] {
            for (const auto &link : svc.catalog()->linksForEntity(fx.wellA))
                if (link.role == member.role && link.isPrimary) return link.assetId;
            return QString();
        };
        QCOMPARE(primary(), fx.astResolved);
        emit panel->wellLogSetPrimaryRequested(fx.wellA, fx.astFree);
        QTRY_COMPARE(primary(), fx.astFree);
        page->listPanel()->undoOp();
        QCOMPARE(primary(), fx.astResolved);
        page->listPanel()->redoOp();
        QCOMPARE(primary(), fx.astFree);
    }

    // ---- D1 多选框架 -----------------------------------------------------
    void dataops_d1_extendedSelectionOnTreeAndTable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("dataTree"));
        QVERIFY(lp && table && tree);
        // D1.1：两视图都是 ExtendedSelection。
        QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
        QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);
        // 跨行多选（模拟 Ctrl 逐行加选）。
        table->selectionModel()->select(
            table->model()->index(0, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        table->selectionModel()->select(
            table->model()->index(1, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(table->selectionModel()->selectedRows().size(), 2);
        const QSet<QString> ids = lp->currentAssetSelection();
        QCOMPARE(ids.size(), 2);
        QVERIFY(ids.contains(fx.astResolved) || ids.contains(fx.astPending));
    }

    void dataops_d1_selectionBadgeAndCountSignal()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        auto *badge = page->findChild<QLabel *>(QStringLiteral("selectionBadge"));
        QVERIFY(lp && table && badge);
        QVERIFY(badge->isHidden()); // 单/零选中不占位
        QSignalSpy spy(lp, &DataListPanel::selectionCountChanged);
        table->selectRow(0);
        table->selectionModel()->select(
            table->model()->index(1, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QVERIFY(spy.count() >= 1);
        // 徽标显示「已选 2 项」。
        QVERIFY(!badge->isHidden());
        QVERIFY(badge->text().contains(QStringLiteral("2")));
        table->clearSelection();
        QVERIFY(badge->isHidden());
    }

    void dataops_d1_contextMenuActionMatrix()
    {
        using namespace paleo::dataops;
        // D1.3：四态菜单矩阵（纯逻辑面）。
        ContextMenuSpec singleResolved;
        singleResolved.hasAssets = true;
        singleResolved.assetCount = 1;
        singleResolved.singleAssetResolved = true;
        const QStringList singleActs = contextMenuActions(singleResolved);
        QVERIFY(singleActs.contains(QStringLiteral("detachLink")));
        QVERIFY(singleActs.contains(QStringLiteral("setPrimary")));
        QVERIFY(singleActs.contains(QStringLiteral("editRole")));
        QVERIFY(singleActs.contains(QStringLiteral("openExternal")));
        // 数据对象重命名：单资产（非 mixed）给入口，紧随「打开预览」。
        QVERIFY(singleActs.contains(QStringLiteral("renameAsset")));
        QCOMPARE(singleActs.indexOf(QStringLiteral("renameAsset")),
                 singleActs.indexOf(QStringLiteral("openPreview")) + 1);
        QVERIFY(!singleActs.contains(QStringLiteral("removeSoft")));
        // 索引级删除（只删数据对象，文件一律不动）：单/多/混合资产面都给。
        QVERIFY(singleActs.contains(QStringLiteral("removeFromCatalog")));
        // goal/gridding-surface-ops：非层位单资产不给「网格化」。
        QVERIFY(!singleActs.contains(QStringLiteral("gridHorizon")));
        ContextMenuSpec singleHorizon = singleResolved;
        singleHorizon.singleAssetIsHorizon = true;
        QVERIFY(contextMenuActions(singleHorizon).contains(QStringLiteral("gridHorizon")));
        // 方向 32：井分层单资产给「编辑分层」，其它类型不给。
        QVERIFY(!singleActs.contains(QStringLiteral("editTops")));
        ContextMenuSpec singleTops = singleResolved;
        singleTops.singleAssetIsTops = true;
        const QStringList topsActs = contextMenuActions(singleTops);
        QVERIFY(topsActs.contains(QStringLiteral("editTops")));
        QVERIFY(!topsActs.contains(QStringLiteral("gridHorizon")));

        ContextMenuSpec multi;
        multi.hasAssets = true;
        multi.assetCount = 5;
        const QStringList multiActs = contextMenuActions(multi);
        QVERIFY(multiActs.contains(QStringLiteral("attachToEntity")));
        QVERIFY(multiActs.contains(QStringLiteral("exportManifest")));
        QVERIFY(multiActs.contains(QStringLiteral("removeSoft")));
        QVERIFY(multiActs.contains(QStringLiteral("removeFromCatalog")));
        QVERIFY(multiActs.contains(QStringLiteral("openPreviewAll")));
        QVERIFY(!multiActs.contains(QStringLiteral("openExternal")));
        // 异构选中：资产 + 实体 → 公共子集（资产操作）+ 实体定位，不给实体 CRUD。
        ContextMenuSpec mixed;
        mixed.hasAssets = true;
        mixed.hasEntities = true;
        mixed.assetCount = 2;
        mixed.entityCount = 1;
        const QStringList mixedActs = contextMenuActions(mixed);
        QVERIFY(mixedActs.contains(QStringLiteral("focusEntities")));
        QVERIFY(mixedActs.contains(QStringLiteral("removeFromCatalog")));
        QVERIFY(!mixedActs.contains(QStringLiteral("renameEntity")));
        QVERIFY(!mixedActs.contains(QStringLiteral("deleteEntity")));
        // 纯实体：实体 CRUD 面。
        ContextMenuSpec entitiesOnly;
        entitiesOnly.hasEntities = true;
        entitiesOnly.entityCount = 1;
        const QStringList entActs = contextMenuActions(entitiesOnly);
        QVERIFY(entActs.contains(QStringLiteral("renameEntity")));
        QVERIFY(entActs.contains(QStringLiteral("deleteEntity")));
        QVERIFY(entActs.contains(QStringLiteral("editCoords")));
        QVERIFY(!entActs.contains(QStringLiteral("openPreview")));
        // 空选择无菜单。
        QVERIFY(contextMenuActions(ContextMenuSpec()).isEmpty());
    }

    void dataops_d1_batchAttachViaDropCore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        DataCatalog *cat = svc.catalog();
        // 未决 → 挂到 well-B2；已决 → 转移到 well-B2；无链接 → 建引用链接。
        lp->applyEntityDrop({fx.astPending, fx.astResolved, fx.astFree}, fx.wellB);
        QCOMPARE(cat->links().size(), 3); // 原 2 + free 的 reference（挂接/转移复用原链接行）
        bool sawResolved = false, sawFree = false;
        for (const EntityAssetLink &l : cat->linksForAsset(fx.astPending))
        {
            QVERIFY(!l.unresolved);
            QCOMPARE(l.entityId, fx.wellB);
        }
        for (const EntityAssetLink &l : cat->linksForAsset(fx.astResolved))
            if (l.entityId == fx.wellB)
                sawResolved = true;
        QVERIFY(sawResolved);
        for (const EntityAssetLink &l : cat->linksForAsset(fx.astFree))
            if (l.entityId == fx.wellB && l.role == QLatin1String("reference"))
                sawFree = true;
        QVERIFY(sawFree);
        // D5：撤销一次 = 最后一条 push 的命令回退（转移）。列表里 well-A1 的
        // 挂接恢复——这里只验证栈可继续回退到全部干净。
        lp->undoOp();
        lp->undoOp();
        lp->undoOp();
        // 未决那条回到未决。
        bool pendingBack = false;
        for (const EntityAssetLink &l : cat->linksForAsset(fx.astPending))
            if (l.unresolved)
                pendingBack = true;
        QVERIFY(pendingBack);
    }

    void dataops_d1_entityPickerDialogSelection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        paleo::dataops::EntityPickerDialog dlg;
        dlg.loadEntities(svc.catalog(), paleo::dataops::EntityOverrideStore(),
                         QStringLiteral("well"));
        QCOMPARE(dlg.findChild<QTableWidget *>(QStringLiteral("entityPickerList"))
                     ->rowCount(), 2);
        // 搜索过滤：输 A1 → B2 行隐藏。
        dlg.findChild<QLineEdit *>(QStringLiteral("entityPickerSearch"))
            ->setText(QStringLiteral("A1"));
        auto *list = dlg.findChild<QTableWidget *>(QStringLiteral("entityPickerList"));
        QVERIFY(list->isRowHidden(1));
        QVERIFY(!list->isRowHidden(0));
        list->selectRow(0);
        QCOMPARE(dlg.selectedEntityId(), fx.wellA);
    }

    void dataops_d1_batchChangeTypeWithUndo()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        // 选中 ast-free（seismic）→ 类型改写为 well_log。
        const int freeRow = [table, &fx]() {
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 0)->data(Qt::UserRole).toString() == fx.astFree)
                    return r;
            return -1;
        }();
        QVERIFY(freeRow >= 0);
        table->selectRow(freeRow);
        // 类型改写走 pushCommand 路径（对话框交互在此直接用命令面对等验证）。
        paleo::dataops::DataOpsContext ctx = lp->opsContext();
        QVERIFY(ctx.valid());
        lp->pushCommand(new paleo::dataops::TypeOverrideCmd(
            ctx, fx.astFree, QStringLiteral("well_log"), QString()));
        QCOMPARE(table->item(freeRow, 1)->text(), QStringLiteral("well_log"));
        // catalog 原型不动（改写只落视图层 sidecar）。
        QCOMPARE(svc.catalog()->assetById(fx.astFree).type, QStringLiteral("seismic"));
        // 撤销 → 回原型。
        lp->undoOp();
        QCOMPARE(table->item(freeRow, 1)->text(), QStringLiteral("seismic"));
    }

    void dataops_d1_softDeleteRecycleAndRestore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        QCOMPARE(table->rowCount(), 3);
        lp->pushCommand(new paleo::dataops::SoftDeleteCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("free.sgy"),
            QStringLiteral("seismic"), true));
        // 软删：行消失 + sidecar 记录。
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(lp->opsContext().recycle->entries().size(), 1);
        QCOMPARE(lp->opsContext().recycle->entries().front().assetId, fx.astFree);
        // 撤销：行回来。
        lp->undoOp();
        QCOMPARE(table->rowCount(), 3);
        QVERIFY(lp->opsContext().recycle->entries().isEmpty());
        // 再删一次（永久路径走 RecycleBin 面）+ 恢复。
        lp->pushCommand(new paleo::dataops::SoftDeleteCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("free.sgy"),
            QStringLiteral("seismic"), true));
        lp->pushCommand(new paleo::dataops::SoftDeleteCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("free.sgy"),
            QStringLiteral("seismic"), false));
        QCOMPARE(table->rowCount(), 3);
    }

    void dataops_d1_exportManifestContent()
    {
        using namespace paleo::dataops;
        QVector<AssetRowInfo> rows(2);
        rows[0].displayName = QStringLiteral("A,1.las");
        rows[0].fileName = QStringLiteral("A,1.las");
        rows[0].effectiveType = QStringLiteral("well_log");
        rows[0].status = QStringLiteral("RAW");
        rows[0].currentVersionNo = 2;
        rows[0].entityNames = {QStringLiteral("A1"), QStringLiteral("B2")};
        rows[0].tags = {QStringLiteral("核心")};
        rows[0].sizeBytes = 1024;
        rows[1].displayName = QStringLiteral("z.sgy");
        rows[1].effectiveType = QStringLiteral("seismic");
        rows[1].status = QStringLiteral("DERIVED");
        const ExportFields f;
        const QString csv = exportCsv(rows, f);
        // CSV 转义：逗号在值内被引号包裹。
        QVERIFY(csv.startsWith(QStringLiteral("\"path\",\"type\",\"version\"")));
        QVERIFY(csv.contains(QStringLiteral("\"A,1.las\"")));
        QVERIFY(csv.contains(QStringLiteral("\"A1;B2\"")));
        QVERIFY(csv.contains(QStringLiteral("well_log")));
        const QByteArray json = exportJson(rows, f);
        QVERIFY(json.contains("\"assetId\""));
        QVERIFY(json.contains("\"count\": 2"));
        QVERIFY(json.contains("\"A1\""));
    }

    // D.1：>200 资产的性能守卫跳过 stat 后 sizeBytes = -1 / lastModified 无效——
    // 导出件必须是空值（CSV 空单元 / JSON null），不能把内部哨兵 -1 写成错值。
    void dataops_d1_exportUnstatEmptyValues()
    {
        using namespace paleo::dataops;
        QVector<AssetRowInfo> rows(2);
        rows[0].displayName = QStringLiteral("known.las");
        rows[0].fileName = QStringLiteral("known.las");
        rows[0].effectiveType = QStringLiteral("well_log");
        rows[0].sizeBytes = 2048;
        rows[0].lastModified = QDateTime::fromString(QStringLiteral("2026-01-02T03:04:05"),
                                                     Qt::ISODate);
        rows[1].displayName = QStringLiteral("unstat.las");
        rows[1].fileName = QStringLiteral("unstat.las");
        rows[1].effectiveType = QStringLiteral("well_log");
        rows[1].sizeBytes = -1; // 同 buildAssetRow 的 statFile=false 哨兵
        rows[1].lastModified = QDateTime(); // 无效 = 未 stat
        const ExportFields f;
        const QString csv = exportCsv(rows, f);
        const QStringList lines = csv.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 3); // 表头 + 两行
        // sizeBytes 列与 version 同口径：裸数字（未 stat 才是引号空单元）；
        // 末列 lastModified 是字符串列，始终引号包裹。
        QVERIFY(lines[1].endsWith(QStringLiteral("2048,\"2026-01-02T03:04:05\"")));
        QVERIFY(lines[2].endsWith(QStringLiteral("\"\",\"\"")));
        const QByteArray json = exportJson(rows, f);
        QVERIFY(json.contains("\"sizeBytes\": 2048"));
        QVERIFY(json.contains("\"lastModified\": \"2026-01-02T03:04:05\""));
        // 未 stat 行：null 空值（写出 -1 即是错值——本修复的断言面）。
        QVERIFY(json.contains("\"sizeBytes\": null"));
        QVERIFY(json.contains("\"lastModified\": null"));
    }

    void dataops_d1_batchOpenPreviewPlan()
    {
        using namespace paleo::dataops;
        QStringList ids;
        for (int i = 0; i < 12; ++i)
            ids << QStringLiteral("ast-%1").arg(i);
        const auto plan = batchOpenPreviewPlan(ids);
        QCOMPARE(plan.first.size(), 8);   // 前 8 项进标签
        QCOMPARE(plan.second, 4);         // 超出 4 提示
        QCOMPARE(plan.first.front(), QStringLiteral("ast-0"));
        // 少量选中 = 全开。
        const auto few = batchOpenPreviewPlan({QStringLiteral("a"), QStringLiteral("b")});
        QCOMPARE(few.first.size(), 2);
        QCOMPARE(few.second, 0);
    }

    void dataops_d1_selectAllInvertAndFiltered()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        // D1.9 全选可见。
        lp->selectAllVisibleAssets();
        QCOMPARE(lp->currentAssetSelection().size(), 3);
        // 反选：全选后反选 = 空；再反选 = 全。
        lp->invertAssetSelection();
        QCOMPARE(table->selectionModel()->selectedRows().size(), 0);
        lp->invertAssetSelection();
        QCOMPARE(lp->currentAssetSelection().size(), 3);
        // 类型过滤后「按过滤器选中」= 只选可见。
        auto *typeFilter = page->findChild<QComboBox *>(QStringLiteral("assetTypeFilter"));
        const int idx = typeFilter->findData(QStringLiteral("tops"));
        QVERIFY(idx >= 0);
        typeFilter->setCurrentIndex(idx);
        lp->selectAllVisibleAssets();
        QCOMPARE(lp->currentAssetSelection().size(), 1);
        QVERIFY(lp->currentAssetSelection().contains(fx.astPending));
    }

    void dataops_d1_selectionPersistsAcrossRefresh()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        // 选 2 行 → 刷新（catalog 没变，走重建管线）→ 选中还原。
        table->selectRow(0);
        table->selectionModel()->select(
            table->model()->index(1, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        const QSet<QString> before = lp->currentAssetSelection();
        QCOMPARE(before.size(), 2);
        page->refreshAssetTable();
        const QSet<QString> after = lp->currentAssetSelection();
        QCOMPARE(after, before);
        // 树侧同样还原（选中镜像到树）。
        int treeSelected = 0;
        QTreeWidgetItemIterator it(page->findChild<QTreeWidget *>(QStringLiteral("dataTree")));
        while (*it)
        {
            if ((*it)->isSelected())
                ++treeSelected;
            ++it;
        }
        QVERIFY(treeSelected >= 2);
    }

    // ---- D2 搜索/过滤/组织 --------------------------------------------------
    void dataops_d2_filterConditionDimensions()
    {
        using namespace paleo::dataops;
        AssetRowInfo row;
        row.displayName = QStringLiteral("A1.las");
        row.fileName = QStringLiteral("A1.las");
        row.effectiveType = QStringLiteral("well_log");
        row.status = QStringLiteral("RAW");
        row.roles = {QStringLiteral("well_log")};
        row.entityNames = {QStringLiteral("A1")};
        row.tags = {QStringLiteral("核心")};
        FilterCondition c;
        c.dim = FilterDim::Type;
        c.value = QStringLiteral("well_log");
        QVERIFY(c.matches(row));
        c.dim = FilterDim::Status;
        QVERIFY(!c.matches(row)); // RAW ≠ DERIVED
        c.value = QStringLiteral("RAW");
        QVERIFY(c.matches(row));
        c.dim = FilterDim::Role;
        c.value = QStringLiteral("well_log");
        QVERIFY(c.matches(row));
        c.dim = FilterDim::Entity;
        c.value = QStringLiteral("a1"); // 大小写不敏感
        QVERIFY(c.matches(row));
        c.dim = FilterDim::Tag;
        c.value = QStringLiteral("核心");
        QVERIFY(c.matches(row));
        c.dim = FilterDim::Search;
        c.value = QStringLiteral("A1");
        QVERIFY(c.matches(row));
        c.dim = FilterDim::Regex;
        c.value = QStringLiteral("A\\d\\.las");
        QVERIFY(c.matches(row));
        c.value = QStringLiteral("[bad");
        QVERIFY(!c.matches(row)); // #421：无效模式退字面量——row 无 "[bad" 字面子串
        c.dim = FilterDim::Unlinked;
        QVERIFY(!c.matches(row));
        AssetRowInfo unlinked;
        unlinked.effectiveType = QStringLiteral("x");
        c.value.clear();
        QVERIFY(c.matches(unlinked));
        c.dim = FilterDim::Warned;
        row.unresolved = true;
        QVERIFY(c.matches(row));
        c.dim = FilterDim::UnknownType;
        row.effectiveType = QStringLiteral("unknown");
        QVERIFY(c.matches(row));
        // 取反（D2.2）。
        c.dim = FilterDim::Type;
        c.value = QStringLiteral("well_log");
        c.negate = true;
        row.effectiveType = QStringLiteral("well_log");
        QVERIFY(!c.matches(row));
        row.effectiveType = QStringLiteral("tops");
        QVERIFY(c.matches(row));
    }

    void dataops_d2_andOrComposition()
    {
        using namespace paleo::dataops;
        AssetRowInfo row;
        row.effectiveType = QStringLiteral("well_log");
        row.status = QStringLiteral("RAW");
        FilterGroup g;
        g.add({FilterDim::Type, QStringLiteral("well_log"), false});
        g.add({FilterDim::Status, QStringLiteral("DERIVED"), false});
        QVERIFY(!g.matches(row)); // AND：类型对但状态不对
        g.orMode = true;
        QVERIFY(g.matches(row)); // OR：类型命中即可
        g.clearDim(FilterDim::Status);
        QVERIFY(g.valuesOf(FilterDim::Status).isEmpty());
        QVERIFY(g.matches(row));
    }

    void dataops_d2_chipRemoveToggleAndBar()
    {
        using namespace paleo::dataops;
        qRegisterMetaType<FilterCondition>();
        FilterGroup g;
        g.add({FilterDim::Type, QStringLiteral("well_log"), false});
        g.add({FilterDim::Tag, QStringLiteral("核心"), false});
        QCOMPARE(g.chipTexts().size(), 2);
        QVERIFY(g.chipTexts().at(0).contains(QStringLiteral("类型")));
        // chip 单删。
        QVERIFY(g.removeOne({FilterDim::Tag, QStringLiteral("核心"), false}));
        QCOMPARE(g.conditions.size(), 1);
        QVERIFY(!g.removeOne({FilterDim::Tag, QStringLiteral("核心"), false}));
        // chip 行部件信号面。
        FilterChipBar bar;
        QSignalSpy removed(&bar, &FilterChipBar::chipRemoved);
        QSignalSpy toggled(&bar, &FilterChipBar::chipToggled);
        FilterGroup g2;
        g2.add({FilterDim::Type, QStringLiteral("tops"), false});
        g2.add({FilterDim::Status, QStringLiteral("RAW"), true});
        bar.setConditions(g2);
        const QList<QPushButton *> chips =
            bar.findChildren<QPushButton *>(QStringLiteral("filterChip"));
        QCOMPARE(chips.size(), 2);
        QVERIFY(bar.isVisible());
        // 点击 chip[0] → chipRemoved 带 FilterCondition。
        chips.at(0)->click();
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(0).value<FilterCondition>().dim, FilterDim::Type);
    }

    void dataops_d2_pendingQuickBarFiltersAndCounts()
    {
        using namespace paleo::dataops;
        qRegisterMetaType<FilterDim>();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        QCOMPARE(table->rowCount(), 3);
        // 计数徽标（未挂接 1 = free；有警告 1 = pending；类型未知 0）。
        auto *quick = page->findChild<QWidget *>(QStringLiteral("pendingQuickBar"));
        QVERIFY(quick);
        auto *unlinkedBtn = quick->findChild<QPushButton *>(QStringLiteral("quickUnlinked"));
        auto *warnedBtn = quick->findChild<QPushButton *>(QStringLiteral("quickWarned"));
        QVERIFY(unlinkedBtn && warnedBtn);
        // 未挂接 = 无已决实体（free.sgy 无链接 + Z9.las 未决链接）→ 2；
        // 有警告 = 存在未决链接 → 1（Z9.las）；类型未知 → 0。
        QVERIFY2(unlinkedBtn->text().contains(QStringLiteral("(2)")),
                 qPrintable(unlinkedBtn->text()));
        QVERIFY2(warnedBtn->text().contains(QStringLiteral("(1)")),
                 qPrintable(warnedBtn->text()));
        QVERIFY(quick->findChild<QPushButton *>(QStringLiteral("quickUnknownType"))
                    ->text()
                    .contains(QStringLiteral("(0)")));
        // 点「未挂接」→ 只剩 free.sgy + Z9.las 两行。
        unlinkedBtn->click();
        QStringList visibleNames;
        for (int r = 0; r < table->rowCount(); ++r)
            if (!table->isRowHidden(r) && table->item(r, 0)->data(Qt::UserRole).isValid() &&
                !table->item(r, 0)->data(Qt::UserRole).toString().isEmpty())
                visibleNames << table->item(r, 0)->text();
        QCOMPARE(visibleNames.size(), 2);
        QVERIFY(visibleNames.contains(QStringLiteral("free.sgy")));
        QVERIFY(visibleNames.contains(QStringLiteral("Z9.las")));
        // 关掉 → 全回来。
        unlinkedBtn->click();
        QCOMPARE(table->rowCount(), 3);
    }

    void dataops_d2_tagStoreRoundtripAndCloud()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        DataCatalog *cat = svc.catalog();
        TagStore tags;
        tags.load(cat);
        QVERIFY(tags.addTag(fx.astFree, QStringLiteral("核心")));
        QVERIFY(tags.addTag(fx.astFree, QStringLiteral("参考")));
        QVERIFY(!tags.addTag(fx.astFree, QStringLiteral("核心"))); // 幂等
        QVERIFY(!tags.addTag(fx.astFree, QStringLiteral("  ")));  // 空白拒
        QVERIFY(tags.addTag(fx.astResolved, QStringLiteral("核心")));
        QVERIFY(tags.save());
        // 重开（新实例从盘读回）。
        TagStore again;
        again.load(cat);
        QCOMPARE(again.tagsFor(fx.astFree),
                 QStringList({QStringLiteral("核心"), QStringLiteral("参考")}));
        const auto cloud = again.tagCloud();
        QCOMPARE(cloud.size(), 2);
        QCOMPARE(cloud.front().first, QStringLiteral("核心")); // 计数高在前
        QCOMPARE(cloud.front().second, 2);
        QCOMPARE(again.assetsWithTag(QStringLiteral("核心")).size(), 2);
        QVERIFY(again.removeTag(fx.astFree, QStringLiteral("核心")));
        QVERIFY(!again.removeTag(fx.astFree, QStringLiteral("核心")));
        again.save();
        TagStore third;
        third.load(cat);
        QCOMPARE(third.tagsFor(fx.astFree), QStringList{QStringLiteral("参考")});
    }

    void dataops_d2_treeSortMemory()
    {
        using namespace paleo::dataops;
        // D2.5：排序记忆走 QSettings。
        QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
        s.remove(QStringLiteral("dataops/treeSort"));
        QCOMPARE(recalledTreeSort(), TreeSortKind::Name);
        rememberTreeSort(TreeSortKind::Time);
        QCOMPARE(recalledTreeSort(), TreeSortKind::Time);
        rememberTreeSort(TreeSortKind::Size);
        QCOMPARE(recalledTreeSort(), TreeSortKind::Size);
        rememberTreeSort(TreeSortKind::Type);
        QCOMPARE(recalledTreeSort(), TreeSortKind::Type);
        s.remove(QStringLiteral("dataops/treeSort"));
    }

    void dataops_d2_savedFilterPresets()
    {
        using namespace paleo::dataops;
        QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
        s.remove(QStringLiteral("dataops/filterPresets"));
        QVERIFY(savedFilterNames().isEmpty());
        FilterGroup g;
        g.add({FilterDim::Type, QStringLiteral("well_log"), false});
        g.add({FilterDim::Status, QStringLiteral("DERIVED"), true});
        g.orMode = true;
        QVERIFY(saveFilterPreset(QStringLiteral("派生测井"), g));
        QVERIFY(!saveFilterPreset(QStringLiteral("  "), g)); // 空名拒
        QCOMPARE(savedFilterNames(), QStringList{QStringLiteral("派生测井")});
        const FilterGroup loaded = loadFilterPreset(QStringLiteral("派生测井"));
        QCOMPARE(loaded.conditions.size(), 2);
        QVERIFY(loaded.orMode);
        QCOMPARE(loaded.conditions.at(1).value, QStringLiteral("DERIVED"));
        QVERIFY(loaded.conditions.at(1).negate);
        QVERIFY(deleteFilterPreset(QStringLiteral("派生测井")));
        QVERIFY(!deleteFilterPreset(QStringLiteral("派生测井")));
        QVERIFY(savedFilterNames().isEmpty());
    }

    void dataops_d2_matchRangesForHighlight()
    {
        using namespace paleo::dataops;
        const auto ranges = matchRanges(QStringLiteral("A1.las a12 a1x"), QStringLiteral("a1"));
        QCOMPARE(ranges.size(), 3);
        QCOMPARE(ranges.at(0).first, 0);
        QCOMPARE(ranges.at(1).first, 7);
        QCOMPARE(ranges.at(2).first, 11);
        QVERIFY(matchRanges(QStringLiteral("abc"), QString()).isEmpty());
        QVERIFY(matchRanges(QString(), QStringLiteral("x")).isEmpty());
    }

    void dataops_d2_filterStateStringRoundtrip()
    {
        using namespace paleo::dataops;
        FilterGroup g;
        g.add({FilterDim::Search, QStringLiteral("测井 曲线"), false});
        g.add({FilterDim::Type, QStringLiteral("well_log"), false});
        g.add({FilterDim::Tag, QStringLiteral("核心"), true});
        g.orMode = true;
        const QString s1 = g.toStateString();
        QVERIFY(s1.startsWith(QStringLiteral("paleo://dataops-filter?")));
        const FilterGroup back = FilterGroup::fromStateString(s1);
        QCOMPARE(back.conditions.size(), 3);
        QVERIFY(back.orMode);
        QCOMPARE(back.conditions.at(0).value, QStringLiteral("测井 曲线"));
        QCOMPARE(back.conditions.at(2).dim, FilterDim::Tag);
        QVERIFY(back.conditions.at(2).negate);
        // 坏串不炸：回空组。
        QVERIFY(FilterGroup::fromStateString(QStringLiteral("http://nope")).isEmpty());
        QVERIFY(FilterGroup::fromStateString(QString()).isEmpty());
    }

    void dataops_d2_emptyStateAndRelax()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        auto *empty = page->findChild<QWidget *>(QStringLiteral("filterEmptyState"));
        QVERIFY(empty);
        QVERIFY(empty->isHidden()); // 无过滤 → 空态不出现
        // 过滤到零命中 → D2.9 空态 + 放宽/清除按钮。
        page->findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"))
            ->setText(QStringLiteral("不存在的名字xyz"));
        QVERIFY(!empty->isHidden());
        auto *relax = empty->findChild<QPushButton *>(QStringLiteral("filterRelaxButton"));
        QVERIFY(relax);
        relax->click(); // 逐级放宽（去掉最后一个条件 = 搜索词）
        QVERIFY(empty->isHidden());
        QCOMPARE(table->rowCount(), 3);
    }

    void dataops_d2_filterPerformance10k()
    {
        using namespace paleo::dataops;
        // D2.8：10k 资产 × 3 条件过滤 < 100ms。
        QVector<AssetRowInfo> rows;
        rows.reserve(10000);
        for (int i = 0; i < 10000; ++i)
        {
            AssetRowInfo r;
            r.assetId = QStringLiteral("ast-%1").arg(i);
            r.displayName = QStringLiteral("well_%1.las").arg(i);
            r.fileName = r.displayName;
            r.effectiveType = i % 3 == 0 ? QStringLiteral("well_log")
                                         : QStringLiteral("tops");
            r.status = i % 2 ? QStringLiteral("RAW") : QStringLiteral("DERIVED");
            r.tags = {QStringLiteral("t%1").arg(i % 7)};
            rows.append(r);
        }
        FilterGroup g;
        g.add({FilterDim::Type, QStringLiteral("well_log"), false});
        g.add({FilterDim::Status, QStringLiteral("DERIVED"), false});
        g.add({FilterDim::Regex, QStringLiteral("well_\\d+"), false});
        QElapsedTimer t;
        t.start();
        int hits = 0;
        for (const AssetRowInfo &r : rows)
            if (g.matches(r))
                ++hits;
        const qint64 elapsed = t.elapsed();
        // D2.8 预算 100ms；300ms 上限为机器争用容限（多 goal-loop worktree
        // 并行构建/测试时实测 108ms——空载 8/8 过、争用下偶发超。回归语义
        // 不变：数量级劣化（>3×）才红）。
        QVERIFY2(elapsed < paleo::perfbudget::relaxedBudgetMs(300),
                 qPrintable(QStringLiteral("%1ms").arg(elapsed)));
        QVERIFY(hits > 0);
    }

    // ---- D3 拖拽 ----------------------------------------------------------
    void dataops_d3_assetMimeRoundtrip()
    {
        using namespace paleo::dataops;
        const QStringList ids = {QStringLiteral("ast-1"), QStringLiteral("ast-2")};
        QMimeData *mime = DataNavTree::mimeForAssetIds(ids);
        QVERIFY(DataNavTree::isAssetMime(mime));
        QCOMPARE(DataNavTree::assetIdsFromMime(mime), ids);
        QVERIFY(mime->hasText()); // 拖到外部目标的文本回退
        QVERIFY(!DataNavTree::isAssetMime(nullptr));
        QMimeData other;
        QVERIFY(!DataNavTree::isAssetMime(&other));
        delete mime;
    }

    void dataops_d3_dropOnWellNodeAttaches()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *tree = page->findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree"));
        auto *lp = page->findChild<DataListPanel *>();
        QVERIFY(tree && lp);
        tree->resize(400, 600);
        tree->expandAll();
        tree->show();
        QTest::qWait(30);
        // 找 well-B2 井节点。
        QTreeWidgetItem *wellBItem = nullptr;
        QTreeWidgetItemIterator it(tree);
        while (*it)
        {
            if ((*it)->data(0, Qt::UserRole + 1).toString() == fx.wellB &&
                (*it)->data(0, Qt::UserRole + 2).toString() == QLatin1String("well"))
                wellBItem = *it;
            ++it;
        }
        QVERIFY(wellBItem);
        const QRect rect = tree->visualItemRect(wellBItem);
        QVERIFY(rect.isValid());
        // D3.1：未决资产拖到井节点 = 挂接。
        QMimeData *mime = paleo::dataops::DataNavTree::mimeForAssetIds({fx.astPending});
        QDropEvent drop(QPointF(rect.center()), Qt::MoveAction, mime,
                        Qt::LeftButton, Qt::NoModifier);
        QSignalSpy spy(lp, &DataListPanel::statusMessage);
        tree->handleDrop(&drop);
        delete mime;
        bool resolvedToB = false;
        for (const EntityAssetLink &l : svc.catalog()->linksForAsset(fx.astPending))
            if (l.entityId == fx.wellB && !l.unresolved)
                resolvedToB = true;
        QVERIFY2(resolvedToB, "drop on well node must attach the pending link");
        QVERIFY(spy.count() >= 1); // D3.1 确认 toast
        // 撤销回未决。
        lp->undoOp();
        bool backToPending = false;
        for (const EntityAssetLink &l : svc.catalog()->linksForAsset(fx.astPending))
            if (l.unresolved)
                backToPending = true;
        QVERIFY(backToPending);
    }

    void dataops_d3_dropOnTagNodeAddsTag()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        // 先打个标签让标签分组出现。
        lp->pushCommand(new TagCmd(lp->opsContext(), fx.astFree,
                                   QStringLiteral("重要"), true));
        page->refreshAssetTable();
        auto *tree = page->findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree"));
        tree->resize(400, 600);
        tree->expandAll();
        tree->show();
        QTest::qWait(30);
        QTreeWidgetItem *tagLeaf = nullptr;
        QTreeWidgetItemIterator it(tree);
        while (*it)
        {
            if ((*it)->data(0, Qt::UserRole + 2).toString() == QLatin1String("tag_leaf"))
                tagLeaf = *it;
            ++it;
        }
        QVERIFY(tagLeaf);
        // D3.5：资产拖到标签节点 = 打标签（多选拖拽带全部 D3.7）。
        QMimeData *mime = paleo::dataops::DataNavTree::mimeForAssetIds(
            {fx.astPending, fx.astResolved});
        QDropEvent drop(QPointF(tree->visualItemRect(tagLeaf).center()),
                        Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        tree->handleDrop(&drop);
        delete mime;
        QCOMPARE(lp->opsContext().tags->tagsFor(fx.astPending),
                 QStringList{QStringLiteral("重要")});
        QCOMPARE(lp->opsContext().tags->tagsFor(fx.astResolved),
                 QStringList{QStringLiteral("重要")});
    }

    void dataops_d3_externalFilesDroppedSignal()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *tree = page->findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree"));
        auto *lp = page->findChild<DataListPanel *>();
        tree->resize(400, 600);
        tree->show();
        QTest::qWait(30);
        QMimeData mime;
        QList<QUrl> urls;
        const QString f1 = QDir(dir.path()).filePath(QStringLiteral("a.las"));
        const QString f2 = QDir(dir.path()).filePath(QStringLiteral("b.sgy"));
        QVERIFY(writeFile(f1, QByteArray("x")));
        QVERIFY(writeFile(f2, QByteArray("y")));
        urls << QUrl::fromLocalFile(f1) << QUrl::fromLocalFile(f2);
        mime.setUrls(urls);
        // D3.2：外部文件拖入 → externalImportRequested（壳接导入流）。
        QSignalSpy pageSpy(page.get(), &DataPage::externalImportRequested);
        QSignalSpy lpSpy(lp, &DataListPanel::externalImportRequested);
        QDropEvent drop(QPointF(100, 50), Qt::CopyAction, &mime, Qt::LeftButton,
                        Qt::NoModifier);
        tree->handleDrop(&drop);
        QCOMPARE(lpSpy.count(), 1);
        QCOMPARE(pageSpy.count(), 1);
        QCOMPARE(lpSpy.at(0).at(0).toStringList().size(), 2);
        // 导入队列登记（D8.1 入口；runner 未注入时保持排队）。
        QVERIFY(lp->findChild<QWidget *>(QStringLiteral("importQueuePanel")));
    }

    void dataops_d3_invalidTargetGivesReason()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *tree = page->findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree"));
        tree->resize(400, 600);
        tree->expandAll();
        tree->show();
        QTest::qWait(30);
        // D3.6：拖到分类节点 = 拒绝 + tooltip 原因。
        QTreeWidgetItem *category = nullptr;
        QTreeWidgetItemIterator it(tree);
        while (*it)
        {
            if ((*it)->data(0, Qt::UserRole + 2).toString() == QLatin1String("category"))
            {
                category = *it;
                break;
            }
            ++it;
        }
        QVERIFY(category);
        QMimeData *mime = paleo::dataops::DataNavTree::mimeForAssetIds({fx.astFree});
        QDropEvent drop(QPointF(tree->visualItemRect(category).center()),
                        Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        tree->handleDrop(&drop);
        delete mime;
        QVERIFY(!tree->lastDropRejectReason().isEmpty());
        QVERIFY(tree->viewport()->toolTip().contains(QStringLiteral("分类")));
    }

    void dataops_d3_dragLeaveClearsFeedback()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *tree = page->findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree"));
        tree->resize(400, 600);
        tree->expandAll();
        tree->show();
        QTest::qWait(30);
        // 先造一次非法落点（dragMove 拒绝 → 反馈挂上），再 dragLeave 清掉。
        QTreeWidgetItem *category = nullptr;
        QTreeWidgetItemIterator it(tree);
        while (*it)
        {
            if ((*it)->data(0, Qt::UserRole + 2).toString() == QLatin1String("category"))
            {
                category = *it;
                break;
            }
            ++it;
        }
        QVERIFY(category);
        QMimeData *mime = paleo::dataops::DataNavTree::mimeForAssetIds({fx.astFree});
        QDragMoveEvent move(tree->visualItemRect(category).center(),
                            Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        tree->handleDragMove(&move);
        QVERIFY(!tree->viewport()->toolTip().isEmpty());
        // D3.8：拖出窗口/取消 → dragLeave 清反馈。
        QDragLeaveEvent leave;
        tree->handleDragLeave(&leave);
        QVERIFY(tree->viewport()->toolTip().isEmpty());
        QVERIFY(tree->lastDropRejectReason().isEmpty());
        delete mime;
    }


    // ---- D4 实体/属性面板 ---------------------------------------------------
    void dataops_d4_entityRenameOverrideFlow()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *ep = page->findChild<EntityPanel *>();
        QVERIFY(lp && ep);
        // D4.1：改名命令（override store）→ 树显示新名 + 面板头新名。
        paleo::dataops::EntityOverride next;
        next.name = QStringLiteral("C3-新名");
        lp->pushCommand(new paleo::dataops::EntityEditCmd(
            lp->opsContext(), fx.wellA, next, paleo::dataops::EntityOverride(), false));
        page->selectAssetsForEntities({fx.wellA});
        auto *header = page->findChild<QLabel *>(QStringLiteral("entityViewHeader"));
        QVERIFY(header->text().contains(QStringLiteral("C3-新名")));
        // 重名校验语义在对话框层（beginRenameEntity），这里验证 undo 回旧名。
        lp->undoOp();
        page->selectAssetsForEntities({fx.wellA});
        QVERIFY(header->text().contains(QStringLiteral("A1")));
        QVERIFY(!header->text().contains(QStringLiteral("C3-新名")));
        // sidecar 持久：重载后仍是旧名（undo 已写回）。
        paleo::dataops::EntityOverrideStore reloaded;
        reloaded.load(svc.catalog());
        QVERIFY(reloaded.overrideFor(fx.wellA).isEmpty());
    }

    void dataops_d4_entityCreateAndSoftDeleteUndo()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        // D4.6：新建实体命令——catalog 落实体 + 树可见。
        CatalogEntity e;
        e.id = QStringLiteral("well-C3");
        e.entityType = QStringLiteral("well");
        e.name = QStringLiteral("C3");
        lp->pushCommand(new paleo::dataops::EntityCreateCmd(lp->opsContext(), e));
        QVERIFY(svc.catalog()->hasEntity(QStringLiteral("well-C3")));
        page->refreshAssetTable();
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("dataTree"));
        bool sawC3 = false;
        QTreeWidgetItemIterator it(tree);
        while (*it)
        {
            if ((*it)->text(0) == QLatin1String("C3"))
                sawC3 = true;
            ++it;
        }
        QVERIFY(sawC3);
        // undo = 软删（catalog 无 removeEntity API——GAPS）：树里消失。
        lp->undoOp();
        sawC3 = false;
        QTreeWidgetItemIterator it2(tree);
        while (*it2)
        {
            if ((*it2)->text(0) == QLatin1String("C3"))
                sawC3 = true;
            ++it2;
        }
        QVERIFY(!sawC3);
        // redo：恢复可见。
        lp->redoOp();
        QVERIFY(svc.catalog()->hasEntity(QStringLiteral("well-C3")));
    }

    void dataops_d4_entityCoordsEditShownInPanel()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        // D4.2：坐标/备注 override → 面板显示。
        paleo::dataops::EntityOverride ov;
        ov.name = QStringLiteral("A1");
        ov.hasCoords = true;
        ov.surfaceX = 123.5;
        ov.surfaceY = 456.25;
        ov.note = QStringLiteral("备注-试算");
        lp->pushCommand(new paleo::dataops::EntityEditCmd(
            lp->opsContext(), fx.wellA, ov, paleo::dataops::EntityOverride(), false));
        page->selectAssetsForEntities({fx.wellA});
        auto *coord = page->findChild<QLabel *>(QStringLiteral("propCoord"));
        QVERIFY(coord->text().contains(QStringLiteral("123.50")));
        QVERIFY(coord->text().contains(QStringLiteral("456.25")));
    }

    void dataops_d4_roleEditDialogVocabulary()
    {
        using namespace paleo::dataops;
        RoleEditDialog dlg;
        dlg.loadRole(QStringLiteral("well_log"),
                     {QStringLiteral("well_log"), QStringLiteral("tops"),
                      QStringLiteral("well_head")});
        QCOMPARE(dlg.oldRole(), QStringLiteral("well_log"));
        QCOMPARE(dlg.newRole(), QStringLiteral("well_log")); // 预选当前
        auto *combo = dlg.findChild<QComboBox *>(QStringLiteral("roleEditCombo"));
        QCOMPARE(combo->count(), 3);
        combo->setCurrentIndex(1);
        QCOMPARE(dlg.newRole(), QStringLiteral("tops"));
        // D5.5：不可撤销说明在场。
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("roleEditWarn"))
                    ->text()
                    .contains(QStringLiteral("不可撤销")));
    }

    void dataops_d4_versionTimelineCardsAndDiffSignal()
    {
        using namespace paleo::dataops;
        qRegisterMetaType<QString>();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        DataCatalog *cat = svc.catalog();
        // 三个版本：v1 RAW、v2 DERIVED、v3 DERIVED（fixture 不带版本）。
        {
            CatalogVersion v1;
            v1.id = QStringLiteral("ver-1");
            v1.assetId = fx.astResolved;
            v1.stage = QStringLiteral("RAW");
            v1.versionNumber = 1;
            v1.fileName = QStringLiteral("A1.las");
            v1.path = QStringLiteral("RAW/ast/ver-1/A1.las");
            v1.sha256 = QStringLiteral("abc0def0000");
            QVERIFY(cat->addVersion(v1));
        }
        for (int v = 2; v <= 3; ++v)
        {
            CatalogVersion ver;
            ver.id = QStringLiteral("ver-%1").arg(v);
            ver.assetId = fx.astResolved;
            ver.stage = QStringLiteral("DERIVED");
            ver.versionNumber = v;
            ver.fileName = QStringLiteral("A1.las");
            ver.path = QStringLiteral("DERIVED/ast/ver-%1/A1.las").arg(v);
            ver.sha256 = QStringLiteral("abc%1def0000").arg(v);
            QVERIFY(cat->addVersion(ver));
        }
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        page->selectAsset(fx.astResolved);
        auto *timeline = page->findChild<paleo::dataops::VersionTimeline *>(
            QStringLiteral("versionTimeline"));
        QVERIFY(timeline);
        // 版本卡：v3→v2→v1 三张（新→旧）。
        const auto cards = timeline->findChildren<QWidget *>(QStringLiteral("versionCard"));
        QCOMPARE(int(cards.size()), 3);
        // 勾两张 → diffRequested（D4.4 入口）。
        QSignalSpy spy(timeline, &paleo::dataops::VersionTimeline::diffRequested);
        const auto picks = timeline->findChildren<QToolButton *>(QStringLiteral("versionPickButton"));
        QCOMPARE(int(picks.size()), 3);
        picks.at(0)->click(); // v3
        picks.at(1)->click(); // v2
        QCOMPARE(spy.count(), 0); // 勾选不直接发——按钮触发
        auto *diffBtn = timeline->findChild<QPushButton *>(QStringLiteral("versionDiffButton"));
        QVERIFY(diffBtn->isEnabled());
        // diff 对话框是模态 exec——测试协作器在下拍替我们关掉。
        driveModalNextTick([](QWidget *w) {
            if (auto *d = qobject_cast<QDialog *>(w))
                d->accept();
        });
        diffBtn->click();
        QCOMPARE(spy.count(), 1);
        // 超额自动取消最早一张：勾第三张 → 仍只有 2 张选中。
        picks.at(2)->click();
        QCOMPARE(timeline->checkedVersionIds().size(), 2);
    }

    void dataops_d4_versionDiffContent()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        DataCatalog *cat = svc.catalog();
        CatalogVersion a, b;
        a.id = QStringLiteral("ver-a");
        a.assetId = fx.astResolved;
        a.stage = QStringLiteral("RAW");
        a.versionNumber = 1;
        a.sha256 = QStringLiteral("aaaa");
        a.fileName = QStringLiteral("A1.las");
        b = a;
        b.id = QStringLiteral("ver-b");
        b.versionNumber = 2;
        b.stage = QStringLiteral("DERIVED");
        b.sha256 = QStringLiteral("bbbb");
        QVERIFY(cat->addVersion(a));
        QVERIFY(cat->addVersion(b));
        const VersionDiff d = diffVersions(a, b, QString(), QString());
        QCOMPARE(d.versionA, 1);
        QCOMPARE(d.versionB, 2);
        QVERIFY(!d.sameSha);
        QVERIFY(d.fieldNotes.join(QString()).contains(QStringLiteral("阶段")));
        const VersionDiff same = diffVersions(a, a, QString(), QString());
        QVERIFY(same.sameSha);
        QVERIFY(same.fieldNotes.join(QString()).contains(QStringLiteral("内容相同")));
    }

    void dataops_d4_topologyGraphNodesAndEdges()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *topo = page->findChild<paleo::dataops::TopologyGraph *>(QStringLiteral("topologyGraph"));
        QVERIFY(topo);
        // D4.5（#424 起）：侧栏拓扑收窄为所选对象的一跳邻域（≤64 项/侧），
        // 全量图移交后台资料图谱——选已决资产 astResolved 出 1 实体 + 1 资产、
        // 1 条实边。
        page->selectAsset(fx.astResolved);
        QCOMPARE(topo->nodeCount(), 2);
        QVERIFY(topo->scene()->items().size() >= 5); // 2 点 + 2 标签 + 1 边
        const QStringList shown = [topo] { QStringList ids; for (QGraphicsItem *it : topo->scene()->items())
            if (!it->data(0).toString().isEmpty()) ids << it->data(0).toString(); return ids; }();
        QVERIFY(shown.contains(fx.wellA)); QVERIFY(shown.contains(fx.astResolved));
        QVERIFY(!shown.contains(fx.wellB)); QVERIFY(!shown.contains(fx.astFree));
        // 无链接资产：一跳邻域只含自身节点、无边。
        page->selectAsset(fx.astFree);
        QCOMPARE(topo->nodeCount(), 1);
        // 未决资产（链接无实体端）：同样只落自身 1 节点。
        page->selectAsset(fx.astPending);
        QCOMPARE(topo->nodeCount(), 1);
    }

    void dataops_d4_multiSelectBatchSummary()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *ep = page->findChild<EntityPanel *>();
        QVERIFY(ep);
        // D4.9：多资产 → 批量概要（先选一次让面板有实体上下文，再多选覆盖）。
        page->selectAsset(fx.astResolved);
        ep->setMultiContext({}, {fx.astResolved, fx.astPending, fx.astFree});
        auto *header = page->findChild<QLabel *>(QStringLiteral("entityViewHeader"));
        QVERIFY(header->text().contains(QStringLiteral("3 个资产")));
        QVERIFY(header->text().contains(QStringLiteral("批量概要")));
        auto *details = page->findChild<QLabel *>(QStringLiteral("propDetailsText"));
        QVERIFY(details->text().contains(QStringLiteral("类型分布")));
        QVERIFY(details->text().contains(QStringLiteral("well_log")));
        QVERIFY(details->text().contains(QStringLiteral("共同实体"))); // A1 是 resolved 的实体
        // 单选回落常规态。
        page->selectAsset(fx.astFree);
        QVERIFY(!header->text().contains(QStringLiteral("批量概要")));
    }

    void dataops_d4_operationsHistoryAndDialog()
    {
        using namespace paleo::dataops;
        OperationsHistory h;
        for (int i = 0; i < OperationsHistory::kCapacity + 30; ++i)
            h.push(QStringLiteral("操作 %1").arg(i));
        QCOMPARE(h.entries().size(), OperationsHistory::kCapacity);
        QCOMPARE(h.entries().front(), QStringLiteral(
                     "操作 %1").arg(OperationsHistory::kCapacity + 29)); // 最新在前
        OperationsHistoryDialog dlg;
        dlg.setEntries(h.entries());
        QCOMPARE(dlg.findChild<QListWidget *>(QStringLiteral("operationsHistoryList"))
                     ->count(), OperationsHistory::kCapacity);
        h.clear();
        QVERIFY(h.entries().isEmpty());
    }

    void dataops_d4_panelSectionsAndCrudBar()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        // D4.8 分组折叠段 + D4.6 CRUD 条。
        QVERIFY(page->findChild<QWidget *>(QStringLiteral("secVersion")));
        QVERIFY(page->findChild<QWidget *>(QStringLiteral("secTopo")));
        QVERIFY(page->findChild<QWidget *>(QStringLiteral("secStats")));
        QVERIFY(page->findChild<QWidget *>(QStringLiteral("entityCrudBar")));
        QVERIFY(page->findChild<QToolButton *>(QStringLiteral("entityCreateButton")));
        QVERIFY(page->findChild<QToolButton *>(QStringLiteral("entityRenameButton")));
        QVERIFY(page->findChild<QToolButton *>(QStringLiteral("entityDeleteButton")));
        QVERIFY(page->findChild<QToolButton *>(QStringLiteral("entityHistoryButton")));
        // D6.7：CRUD 按钮可聚焦（焦点环策略）。
        QVERIFY(page->findChild<QToolButton *>(QStringLiteral("entityCreateButton"))
                    ->focusPolicy() != Qt::NoFocus);
    }

    // ---- D5 撤销/重做 -------------------------------------------------------
    void dataops_d5_stackPushUndoRedoTexts()
    {
        using namespace paleo::dataops;
        DataOpsUndoStack stack;
        QVERIFY(!stack.canUndo() && !stack.canRedo());
        class NoopCmd : public DataOpCommand
        {
        public:
            NoopCmd(QString t)
              : m_t(std::move(t))
            {
            }
            void redo() override { ++redone; }
            void undo() override { ++undone; }
            QString text() const override { return m_t; }
            QString m_t;
            int redone = 0, undone = 0;
        };
        auto *a = new NoopCmd(QStringLiteral("挂接 A1.Las→well-A1"));
        auto *b = new NoopCmd(QStringLiteral("打标签 A1「核心」"));
        stack.push(a);
        stack.push(b);
        QCOMPARE(a->redone, 1);
        QVERIFY(stack.canUndo());
        QCOMPARE(stack.undoText(), QStringLiteral("打标签 A1「核心」"));
        const QString undone = stack.undo();
        QCOMPARE(undone, QStringLiteral("打标签 A1「核心」"));
        QCOMPARE(b->undone, 1);
        QVERIFY(stack.canRedo());
        QCOMPARE(stack.redo(), QStringLiteral("打标签 A1「核心」"));
        QCOMPARE(b->redone, 2); // push 一次 + redo 一次
        // 全部撤销后：undo 空返回；redo 仍有账。
        stack.undo(); // b（redo 一次后又在栈顶）
        stack.undo(); // a
        QVERIFY(stack.undo().isEmpty());
        QCOMPARE(stack.redoDepth(), 2);
        stack.redo(); // a
        stack.redo(); // b
        QVERIFY(stack.redo().isEmpty());
    }

    void dataops_d5_stackDepthCap()
    {
        using namespace paleo::dataops;
        DataOpsUndoStack stack;
        class Noop : public DataOpCommand
        {
        public:
            void redo() override {}
            void undo() override {}
            QString text() const override { return QStringLiteral("noop"); }
        };
        for (int i = 0; i < 60; ++i)
            stack.push(new Noop);
        QCOMPARE(stack.depth(), DataOpsUndoStack::kMaxDepth); // D5.3 弹底
        QVERIFY(stack.undoTexts().size() == DataOpsUndoStack::kMaxDepth);
    }

    void dataops_d5_mergeAdjacentSameKind()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        DataOpsContext ctx;
        ctx.cat = svc.catalog();
        TagStore tags;
        AssetOverrideStore ovs;
        EntityOverrideStore evs;
        RecycleBin bin;
        tags.load(ctx.cat);
        ovs.load(ctx.cat);
        evs.load(ctx.cat);
        bin.load(ctx.cat);
        ctx.tags = &tags;
        ctx.assetOverrides = &ovs;
        ctx.entityOverrides = &evs;
        ctx.recycle = &bin;
        DataOpsUndoStack stack;
        // 同资产同标签同向连续 push → 合并为一条。
        stack.push(new TagCmd(ctx, fx.astFree, QStringLiteral("核心"), true));
        stack.push(new TagCmd(ctx, fx.astFree, QStringLiteral("核心"), true));
        QCOMPARE(stack.depth(), 1); // mergeWith 吞并
        // 不同标签不合并。
        stack.push(new TagCmd(ctx, fx.astFree, QStringLiteral("参考"), true));
        QCOMPARE(stack.depth(), 2);
        // 反向不合并。
        stack.push(new TagCmd(ctx, fx.astFree, QStringLiteral("参考"), false));
        QCOMPARE(stack.depth(), 3);
    }

    void dataops_d5_attachTransferUndoCycle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        DataCatalog *cat = svc.catalog();
        // 挂接（未决→B2）→ 撤销 → 未决恢复。
        lp->pushCommand(new paleo::dataops::AttachLinkCmd(
            lp->opsContext(), fx.astPending, QStringLiteral("tops"), fx.wellB,
            QStringLiteral("well")));
        QVERIFY(!cat->linksForAsset(fx.astPending).at(0).unresolved);
        QCOMPARE(cat->linksForAsset(fx.astPending).at(0).entityId, fx.wellB);
        lp->undoOp();
        QVERIFY(cat->linksForAsset(fx.astPending).at(0).unresolved);
        // 重做 → 再转移（B2→A1）→ 撤销回 B2。
        lp->redoOp();
        lp->pushCommand(new paleo::dataops::TransferLinkCmd(
            lp->opsContext(), fx.astPending, QStringLiteral("tops"), fx.wellB, fx.wellA));
        QCOMPARE(cat->linksForAsset(fx.astPending).at(0).entityId, fx.wellA);
        lp->undoOp();
        QCOMPARE(cat->linksForAsset(fx.astPending).at(0).entityId, fx.wellB);
        // 解挂 → 撤销 → 回 B2。
        lp->pushCommand(new paleo::dataops::DetachLinkCmd(
            lp->opsContext(), fx.astPending, QStringLiteral("tops"), fx.wellB));
        QVERIFY(cat->linksForAsset(fx.astPending).at(0).unresolved);
        lp->undoOp();
        QCOMPARE(cat->linksForAsset(fx.astPending).at(0).entityId, fx.wellB);
        // 设主周期：同 (wellA, well_log) 加非主成员 free.sgy → 提升为主
        //（A1.las 降级）→ 撤销恢复 A1.las。
        {
            EntityAssetLink member;
            member.entityType = QStringLiteral("well");
            member.entityId = fx.wellA;
            member.assetId = fx.astFree;
            member.role = QStringLiteral("well_log");
            member.isPrimary = false;
            QVERIFY(cat->addLink(member));
        }
        bool a1Primary = false, freePrimary = false;
        for (const EntityAssetLink &l : cat->linksForEntity(fx.wellA))
            if (l.assetId == fx.astResolved)
                a1Primary = l.isPrimary;
            else if (l.assetId == fx.astFree)
                freePrimary = l.isPrimary;
        QVERIFY(a1Primary);
        QVERIFY(!freePrimary);
        lp->pushCommand(new paleo::dataops::SetPrimaryCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("well_log"), fx.wellA,
            fx.astResolved));
        a1Primary = freePrimary = false;
        for (const EntityAssetLink &l : cat->linksForEntity(fx.wellA))
            if (l.assetId == fx.astResolved)
                a1Primary = l.isPrimary;
            else if (l.assetId == fx.astFree)
                freePrimary = l.isPrimary;
        QVERIFY(freePrimary); // 提升成主
        QVERIFY(!a1Primary);  // 旧主降级
        lp->undoOp();
        a1Primary = freePrimary = false;
        for (const EntityAssetLink &l : cat->linksForEntity(fx.wellA))
            if (l.assetId == fx.astResolved)
                a1Primary = l.isPrimary;
            else if (l.assetId == fx.astFree)
                freePrimary = l.isPrimary;
        QVERIFY(a1Primary);  // 撤销恢复 A1.las 为主
        QVERIFY(!freePrimary);
    }

    void dataops_d5_renameAssetCmdUndoRedo()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog()); // astResolved 显示名 "A1.las"
        // 与 dataops_d5_mergeAdjacentSameKind 同一装配：valid() 要求五指针齐。
        DataOpsContext ctx;
        ctx.cat = svc.catalog();
        TagStore tags;
        AssetOverrideStore ovs;
        EntityOverrideStore evs;
        RecycleBin bin;
        tags.load(ctx.cat);
        ovs.load(ctx.cat);
        evs.load(ctx.cat);
        bin.load(ctx.cat);
        ctx.tags = &tags;
        ctx.assetOverrides = &ovs;
        ctx.entityOverrides = &evs;
        ctx.recycle = &bin;
        RenameAssetCmd cmd(ctx, fx.astResolved, QStringLiteral("A1.las"),
                           QStringLiteral("新名字"));
        cmd.redo();
        QCOMPARE(svc.catalog()->assetById(fx.astResolved).displayName,
                 QStringLiteral("新名字"));
        cmd.undo();
        QCOMPARE(svc.catalog()->assetById(fx.astResolved).displayName,
                 QStringLiteral("A1.las"));
        // 不并栈契约：id() 为空。
        QVERIFY(cmd.id().isEmpty());
    }

    void dataops_d5_undoRedoButtonsShowOperationName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *undoBtn = page->findChild<QPushButton *>(QStringLiteral("dataUndoButton"));
        auto *redoBtn = page->findChild<QPushButton *>(QStringLiteral("dataRedoButton"));
        QVERIFY(undoBtn && redoBtn);
        QVERIFY(!undoBtn->isEnabled()); // 空栈
        QCOMPARE(undoBtn->text(), QStringLiteral("撤销"));
        lp->pushCommand(new paleo::dataops::TagCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("核心"), true));
        // D5.2：按钮文案带操作名。
        QVERIFY(undoBtn->isEnabled());
        QVERIFY2(undoBtn->text().contains(QStringLiteral("核心")),
                 qPrintable(undoBtn->text()));
        undoBtn->click();
        QVERIFY(!undoBtn->isEnabled());
        QVERIFY(redoBtn->isEnabled());
        QVERIFY(redoBtn->text().contains(QStringLiteral("核心")));
    }

    void dataops_d5_stackClearsOnCatalogSessionChange()
    {
        QTemporaryDir dir1, dir2;
        QVERIFY(dir1.isValid() && dir2.isValid());
        DataImportService svc1(nullptr, nullptr), svc2(nullptr, nullptr);
        svc1.setProjectDir(dir1.path());
        svc2.setProjectDir(dir2.path());
        DataOpsFixture fx;
        fx.build(svc1.catalog());
        fx.build(svc2.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc1));
        auto *lp = page->findChild<DataListPanel *>();
        lp->pushCommand(new paleo::dataops::TagCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("核心"), true));
        QVERIFY(lp->opStack()->canUndo());
        // 换工程（新门面/新 catalog）→ D5.6 栈清空。
        auto *doc2 = new PreviewDocService(&svc2, page.get());
        page->setProperty("paleo.page.importsvc",
                          QVariant::fromValue<QObject *>(doc2));
        page->refreshAssetTable();
        QVERIFY(!lp->opStack()->canUndo());
        QVERIFY(!lp->opStack()->canRedo());
    }

    // ---- D6 键盘与命令面板 ---------------------------------------------------
    void dataops_d6_fuzzyScoreBasics()
    {
        using namespace paleo::dataops;
        QVERIFY(fuzzyScore(QStringLiteral("A1.las"), QString()) > 0); // 空查询全命中
        QVERIFY(fuzzyScore(QStringLiteral("A1.las"), QStringLiteral("a1")) > 0);
        QVERIFY(fuzzyScore(QStringLiteral("A1.las"), QStringLiteral("A1")) >
                fuzzyScore(QStringLiteral("xxA1"), QStringLiteral("A1"))); // 词首加权
        QVERIFY(fuzzyScore(QStringLiteral("well_log"), QStringLiteral("wl")) > 0);
        QVERIFY(fuzzyScore(QStringLiteral("tops"), QStringLiteral("wl")) == 0); // 不匹配
        QVERIFY(fuzzyScore(QStringLiteral("ab"), QStringLiteral("abc")) == 0); // 候选短于查询
        // 排序：分数高在前，同分稳定。
        const QStringList cands = {QStringLiteral("b1.las"), QStringLiteral("a1.las"),
                                   QStringLiteral("zz.las")};
        const auto ranked = fuzzyRank(cands, QStringLiteral("a1"));
        QCOMPARE(ranked.size(), 1);
        QCOMPARE(ranked.front().first, 1);
    }

    void dataops_d6_commandRegistrySearchAndConflicts()
    {
        using namespace paleo::dataops;
        CommandRegistry reg;
        CommandEntry e1;
        e1.id = QStringLiteral("a");
        e1.title = QStringLiteral("全选可见项");
        e1.category = QStringLiteral("选择");
        e1.shortcut = QStringLiteral("Ctrl+A");
        e1.keywords << QStringLiteral("select all");
        reg.registerCommand(e1);
        CommandEntry e2;
        e2.id = QStringLiteral("b");
        e2.title = QStringLiteral("反选");
        e2.category = QStringLiteral("选择");
        e2.shortcut = QStringLiteral("Ctrl+Shift+A");
        reg.registerCommand(e2);
        CommandEntry e3;
        e3.id = QStringLiteral("c");
        e3.title = QStringLiteral("打开可回收清单");
        e3.category = QStringLiteral("批量");
        e3.shortcut = QStringLiteral("Ctrl+A"); // 冲突！
        reg.registerCommand(e3);
        QVERIFY(reg.has(QStringLiteral("a")));
        QCOMPARE(reg.all().size(), 3);
        // 检索：标题/关键词/分类都参与。
        const auto hits1 = reg.search(QStringLiteral("全选"));
        QCOMPARE(hits1.size(), 1);
        QCOMPARE(hits1.front().entry.id, QStringLiteral("a"));
        const auto hits2 = reg.search(QStringLiteral("select"));
        QCOMPARE(hits2.size(), 1); // 关键词命中
        QVERIFY(reg.search(QStringLiteral("批量")).size() >= 1); // 分类命中
        QVERIFY(reg.search(QStringLiteral("不存在的命令xyz")).isEmpty());
        // D6.6 冲突检测。
        const auto conflicts = reg.shortcutConflicts();
        QCOMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts.front().shortcut, QStringLiteral("Ctrl+A"));
        QCOMPARE(conflicts.front().commandIds.size(), 2);
        reg.unregister(QStringLiteral("c"));
        QVERIFY(reg.shortcutConflicts().isEmpty());
    }

    void dataops_d6_paletteSearchAndExecute()
    {
        using namespace paleo::dataops;
        DataCommandPalette palette;
        bool executed = false;
        QVector<DataCommandPalette::Source> sources;
        DataCommandPalette::Source cmd;
        cmd.kind = QStringLiteral("command");
        cmd.id = QStringLiteral("dataops.selectAllVisible");
        cmd.title = QStringLiteral("全选可见项");
        cmd.subtitle = QStringLiteral("选择");
        cmd.trigger = [&executed] { executed = true; };
        sources.append(cmd);
        DataCommandPalette::Source asset;
        asset.kind = QStringLiteral("asset");
        asset.id = QStringLiteral("ast-9");
        asset.title = QStringLiteral("Z9.las");
        asset.subtitle = QStringLiteral("资产 · well_log");
        sources.append(asset);
        palette.setSources(std::move(sources));
        auto *edit = palette.findChild<QLineEdit *>(QStringLiteral("paletteInput"));
        auto *list = palette.findChild<QListWidget *>(QStringLiteral("paletteResults"));
        QVERIFY(edit && list);
        // 空查询 = 全命中。
        QCOMPARE(list->count(), 2);
        // 输入「全选」→ 只剩命令条目。
        edit->setText(QStringLiteral("全选"));
        QCOMPARE(list->count(), 1);
        QSignalSpy chosen(&palette, &DataCommandPalette::commandChosen);
        palette.activateCurrent(); // 回车执行
        QVERIFY(executed);
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.at(0).at(0).toString(), QStringLiteral("dataops.selectAllVisible"));
        // 资产条目 → assetChosen。
        edit->setText(QStringLiteral("Z9"));
        QCOMPARE(list->count(), 1);
        QSignalSpy assetChosen(&palette, &DataCommandPalette::assetChosen);
        palette.activateCurrent();
        QCOMPARE(assetChosen.count(), 1);
        QCOMPARE(assetChosen.at(0).at(0).toString(), QStringLiteral("ast-9"));
        // 无匹配提示（D2.9 同款文案）。
        edit->setText(QStringLiteral("qqqq"));
        QCOMPARE(list->count(), 0);
        QVERIFY(palette.findChild<QLabel *>(QStringLiteral("paletteStatus"))
                    ->text()
                    .contains(QStringLiteral("无匹配")));
    }

    void dataops_d6_shortcutsDialogLoadAndConflicts()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        // 注册表已由 buildDataOpsUi 装填（D6.2 全量动作）。
        QVERIFY(lp->commandRegistry().all().size() >= 15);
        ShortcutsDialog dlg;
        dlg.loadRegistry(lp->commandRegistry());
        auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("shortcutsTable"));
        QVERIFY(table);
        QCOMPARE(table->rowCount(), int(lp->commandRegistry().all().size()));
        bool sawCtrlZ = false;
        for (int r = 0; r < table->rowCount(); ++r)
            if (table->item(r, 1)->text() == QLatin1String("Ctrl+Z"))
                sawCtrlZ = true;
        QVERIFY(sawCtrlZ);
        // 无冲突 → 冲突标签隐藏。
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("shortcutConflictsLabel"))->isHidden());
        // 注入冲突 → 标签出现。
        CommandRegistry reg = lp->commandRegistry();
        CommandEntry dup;
        dup.id = QStringLiteral("dup");
        dup.title = QStringLiteral("冲突演示");
        dup.category = QStringLiteral("测试");
        dup.shortcut = QStringLiteral("Ctrl+Z");
        reg.registerCommand(dup);
        ShortcutsDialog dlg2;
        dlg2.loadRegistry(reg);
        QVERIFY(!dlg2.findChild<QLabel *>(QStringLiteral("shortcutConflictsLabel"))->isHidden());
    }

    void dataops_d6_vimTreeNavigation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *tree = page->findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree"));
        QVERIFY(tree);
        tree->setFocus();
        tree->resize(400, 600);
        tree->expandAll();
        tree->show();
        QTest::qWait(20);
        // 默认关：j/k 走 Qt 内建（不动 currentIndex——j 无内建语义）。
        QSignalSpy slashSpy(tree, &paleo::dataops::DataNavTree::searchFocusRequested);
        QTest::keyClick(tree, Qt::Key_Slash);
        QCOMPARE(slashSpy.count(), 0);
        tree->setVimMode(true);
        QTest::keyClick(tree, Qt::Key_Slash);
        QCOMPARE(slashSpy.count(), 1); // / 聚焦搜索（D6.5）
        // g 跳顶（首个顶层项）。
        QTest::keyClick(tree, Qt::Key_G);
        QCOMPARE(tree->currentItem(), tree->topLevelItem(0));
        // j/k 在展平序里移动。
        QTest::keyClick(tree, Qt::Key_J);
        QVERIFY(tree->currentItem() != tree->topLevelItem(0));
        QTreeWidgetItem *afterJ = tree->currentItem();
        QTest::keyClick(tree, Qt::Key_K);
        QCOMPARE(tree->currentItem(), tree->topLevelItem(0));
        QVERIFY(afterJ != nullptr);
    }

    void dataops_d6_vimSettingRoundtrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
        s.remove(QStringLiteral("dataops/vimMode"));
        QVERIFY(!page->vimModeEnabled()); // 默认关（D6.5）
        page->setVimModeEnabled(true);
        QVERIFY(page->vimModeEnabled());
        QVERIFY(page->findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree"))
                    ->vimMode());
        page->setVimModeEnabled(false);
        QVERIFY(!page->vimModeEnabled());
        s.remove(QStringLiteral("dataops/vimMode"));
    }

    // ---- D7 列表形态与性能 ---------------------------------------------------
    void dataops_d7_viewModeSwitch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *stack = page->findChild<QStackedWidget *>(QStringLiteral("dataViewStack"));
        auto *icon = page->findChild<QListWidget *>(QStringLiteral("assetIconView"));
        auto *group = page->findChild<QTreeWidget *>(QStringLiteral("assetGroupTree"));
        auto *virt = page->findChild<QTableView *>(QStringLiteral("assetVirtualTable"));
        QVERIFY(stack && icon && group && virt);
        QCOMPARE(stack->count(), 5); // 树/表/图标/虚拟/分组
        // 图标视图按类型过滤后装载（D7.1 图标态）。
        lp->setViewMode(2);
        QCOMPARE(stack->currentIndex(), 2);
        QCOMPARE(icon->count(), 3);
        // 分组视图（D7.6）：默认按类型分组。
        lp->setViewMode(4);
        QCOMPARE(stack->currentIndex(), 4);
        QVERIFY(group->topLevelItemCount() >= 2); // well_log/tops/seismic 三组
        // 虚拟视图（D7.3 验收载体）。
        lp->setViewMode(3);
        QCOMPARE(stack->currentIndex(), 3);
        QCOMPARE(virt->model()->rowCount(), 3);
        // 模式按钮互斥（QButtonGroup）。
        auto *iconBtn = page->findChild<QToolButton *>(QStringLiteral("iconViewButton"));
        QVERIFY(iconBtn->isCheckable());
        iconBtn->click();
        QCOMPARE(stack->currentIndex(), 2);
    }

    void dataops_d7_flatModelBatchFetch()
    {
        using namespace paleo::dataops;
        FlatAssetModel model;
        QVector<AssetRowInfo> rows;
        for (int i = 0; i < 1000; ++i)
        {
            AssetRowInfo r;
            r.assetId = QStringLiteral("ast-%1").arg(i);
            r.displayName = QStringLiteral("f%1.las").arg(i);
            r.effectiveType = QStringLiteral("well_log");
            rows.append(r);
        }
        model.setRows(rows);
        QCOMPARE(model.rowCount(), FlatAssetModel::kBatchSize); // 首屏一批
        QVERIFY(model.canFetchMore(QModelIndex()));
        model.fetchMore(QModelIndex());
        QCOMPARE(model.rowCount(), FlatAssetModel::kBatchSize * 2);
        // 连续 fetch 到顶。
        while (model.canFetchMore(QModelIndex()))
            model.fetchMore(QModelIndex());
        QCOMPARE(model.rowCount(), 1000);
        QVERIFY(!model.canFetchMore(QModelIndex()));
        QCOMPARE(model.assetIdAt(999), QStringLiteral("ast-999"));
        QCOMPARE(model.assetIdAt(-1), QString());
        QCOMPARE(model.assetIdAt(1000), QString());
        // 数据面：UserRole = assetId。
        QCOMPARE(model.data(model.index(5, 0), Qt::UserRole).toString(),
                 QStringLiteral("ast-5"));
        QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(),
                 QStringLiteral("名称"));
    }

    void dataops_d7_flatModelFetchTiming()
    {
        using namespace paleo::dataops;
        // D7.3：单批 fetchMore（256 行增量）< 16ms 滚动帧预算。
        FlatAssetModel model;
        QVector<AssetRowInfo> rows;
        rows.reserve(10000);
        for (int i = 0; i < 10000; ++i)
        {
            AssetRowInfo r;
            r.assetId = QStringLiteral("ast-%1").arg(i);
            r.displayName = QStringLiteral("f%1.las").arg(i);
            rows.append(r);
        }
        model.setRows(rows);
        QElapsedTimer t;
        t.start();
        model.fetchMore(QModelIndex());
        const qint64 one = t.elapsed();
        QVERIFY2(one < paleo::perfbudget::relaxedBudgetMs(16),
                 qPrintable(QStringLiteral("one batch %1ms").arg(one)));
        t.restart();
        while (model.canFetchMore(QModelIndex()))
            model.fetchMore(QModelIndex());
        const qint64 all = t.elapsed();
        QVERIFY2(all < paleo::perfbudget::relaxedBudgetMs(1000),
                 qPrintable(QStringLiteral("all %1ms").arg(all)));
        QCOMPARE(model.rowCount(), 10000);
    }

    void dataops_d7_columnStateRoundtrip()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        auto *header = table->horizontalHeader();
        // D7.2：列序/宽持久化（QSettings roundtrip）。
        QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
        s.remove(QStringLiteral("dataops/cols/assetTable"));
        header->moveSection(2, 0); // 关联列挪到最前
        const int moved = header->visualIndex(2);
        QCOMPARE(moved, 0);
        table->setColumnHidden(1, true);
        saveColumnState(QStringLiteral("assetTable"), header);
        // 复原后重载。
        header->moveSection(0, 2);
        table->setColumnHidden(1, false);
        QVERIFY(restoreColumnState(QStringLiteral("assetTable"), header));
        QCOMPARE(header->visualIndex(2), 0);
        QVERIFY(table->isColumnHidden(1));
        s.remove(QStringLiteral("dataops/cols/assetTable"));
    }

    void dataops_d7_stableMultiKeySort()
    {
        using namespace paleo::dataops;
        // D7.4：主键相同 → 名称序（稳定，不乱跳）。
        QVector<AssetRowInfo> rows(4);
        rows[0].displayName = QStringLiteral("b.las");
        rows[0].effectiveType = QStringLiteral("tops");
        rows[1].displayName = QStringLiteral("a.las");
        rows[1].effectiveType = QStringLiteral("tops");
        rows[2].displayName = QStringLiteral("z.las");
        rows[2].effectiveType = QStringLiteral("well_log");
        rows[3].displayName = QStringLiteral("c.las");
        rows[3].effectiveType = QStringLiteral("well_log");
        sortAssetRows(&rows, QStringLiteral("type"), true);
        QCOMPARE(rows.at(0).displayName, QStringLiteral("a.las")); // tops 组内名称序
        QCOMPARE(rows.at(1).displayName, QStringLiteral("b.las"));
        QCOMPARE(rows.at(2).displayName, QStringLiteral("c.las")); // well_log 组内名称序
        QCOMPARE(rows.at(3).displayName, QStringLiteral("z.las"));
        // 降序：组序翻转，组内仍名称序。
        sortAssetRows(&rows, QStringLiteral("type"), false);
        QCOMPARE(rows.at(0).displayName, QStringLiteral("c.las"));
        QCOMPARE(rows.at(3).displayName, QStringLiteral("b.las"));
    }

    void dataops_d7_groupTreeModes()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *group = page->findChild<QTreeWidget *>(QStringLiteral("assetGroupTree"));
        auto *lp = page->findChild<DataListPanel *>();
        QVERIFY(group && lp);
        // 按类型：well_log/tops/seismic → 3 组。
        lp->setViewMode(4);
        QCOMPARE(group->topLevelItemCount(), 3);
        // 按实体：A1（resolved 的实体）+ 未分组（Z9/free）。
        static_cast<paleo::dataops::AssetGroupTree *>(group)
            ->setGroupBy(paleo::dataops::AssetGroupTree::GroupBy::Entity);
        lp->applyListFilter(); // 触发重灌
        QCOMPARE(group->topLevelItemCount(), 2);
        bool sawA1 = false;
        for (int i = 0; i < group->topLevelItemCount(); ++i)
            if (group->topLevelItem(i)->text(0) == QLatin1String("A1"))
                sawA1 = true;
        QVERIFY(sawA1);
        // 按版本：全部 v1 → 1 组。
        static_cast<paleo::dataops::AssetGroupTree *>(group)
            ->setGroupBy(paleo::dataops::AssetGroupTree::GroupBy::Version);
        lp->applyListFilter();
        QCOMPARE(group->topLevelItemCount(), 1);
    }

    void dataops_d7_columnFunnelApply()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        // D7.8：列头漏斗的应用面（保留集之外的行隐藏）。
        QCOMPARE(table->rowCount(), 3);
        ColumnFunnel::applyColumnFilter(
            table, 1, QSet<QString>{QStringLiteral("well_log")});
        int visible = 0;
        for (int r = 0; r < table->rowCount(); ++r)
            if (!table->isRowHidden(r) &&
                !table->item(r, 0)->data(Qt::UserRole).toString().isEmpty())
                ++visible;
        QCOMPARE(visible, 1); // 只有 A1.las 是 well_log
        // 头部右键策略在场。
        QCOMPARE(table->horizontalHeader()->contextMenuPolicy(), Qt::CustomContextMenu);
    }

    // ---- D8 导入流增强 -------------------------------------------------------
    void dataops_d8_importPresetRoundtrip()
    {
        using namespace paleo::dataops;
        QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
        s.remove(QStringLiteral("dataops/importPresets"));
        QVERIFY(importPresets().isEmpty());
        ImportPreset p;
        p.name = QStringLiteral("井场常用");
        p.dirs = {QStringLiteral("/data/wells"), QStringLiteral("/data/seis")};
        p.typeMap = {{QStringLiteral("las"), QStringLiteral("well_log")},
                     {QStringLiteral("sgy"), QStringLiteral("seismic")}};
        QVERIFY(saveImportPreset(p));
        const QList<ImportPreset> loaded = importPresets();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.front().name, QStringLiteral("井场常用"));
        QCOMPARE(loaded.front().dirs.size(), 2);
        QCOMPARE(loaded.front().typeMap.value(QStringLiteral("las")),
                 QStringLiteral("well_log"));
        QVERIFY(deleteImportPreset(QStringLiteral("井场常用")));
        QVERIFY(importPresets().isEmpty());
    }

    void dataops_d8_duplicateDetection()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        DataCatalog *cat = svc.catalog();
        // 同名命中：A1.las 已在库。
        const QString dupName = QDir(dir.path()).filePath(QStringLiteral("A1.las"));
        QVERIFY(writeFile(dupName, QByteArray("same")));
        // fixture 不带版本：SHA 口径在此不可造（需真受管版本）——同名口径
        // 即本用例的验证面；SHA 路径经 versionBySha256 的空返回自然放行。
        const QVector<DuplicateHit> hits = detectDuplicates(cat, {dupName});
        QCOMPARE(hits.size(), 1); // 同名命中（两个路径同一个文件，去重语义）
        QCOMPARE(hits.front().reason, QStringLiteral("name"));
        QCOMPARE(hits.front().existingAssetId, fx.astResolved);
        // 无重复文件 → 空。
        const QString fresh = QDir(dir.path()).filePath(QStringLiteral("new-file.xyz"));
        QVERIFY(writeFile(fresh, QByteArray("fresh-bytes")));
        QVERIFY(detectDuplicates(cat, {fresh}).isEmpty());
    }

    void dataops_d8_estimateDirectory()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir d(dir.path());
        QVERIFY(d.mkpath(QStringLiteral("sub/deep")));
        for (int i = 0; i < 5; ++i)
            QVERIFY(writeFile(d.filePath(QStringLiteral("f%1.las").arg(i)),
                              QByteArray(100, 'x')));
        QVERIFY(writeFile(d.filePath(QStringLiteral("sub/deep/g.sgy")), QByteArray(50, 'y')));
        const ImportEstimate est = estimateDirectory(dir.path());
        QCOMPARE(est.fileCount, 6); // 递归
        QCOMPARE(est.totalBytes, qint64(550));
        QCOMPARE(est.largeFileCount, 0);
        QVERIFY(est.byExtension.join(QLatin1Char(',')).contains(QStringLiteral("las:5")));
        QVERIFY(est.byExtension.join(QLatin1Char(',')).contains(QStringLiteral("sgy:1")));
    }

    void dataops_d8_retryQueueStateMachine()
    {
        using namespace paleo::dataops;
        ImportRetryQueue q;
        ImportQueueItem it;
        it.path = QStringLiteral("/tmp/x.las");
        q.enqueue(it);
        q.markRunning(0);
        QCOMPARE(q.items().at(0).state, ImportItemState::Running);
        // 失败 → 自动重试额度内 → RetryWait（D8.2）。
        q.markFailed(0, QStringLiteral("io 错误"));
        QCOMPARE(q.items().at(0).state, ImportItemState::RetryWait);
        QCOMPARE(q.items().at(0).autoRetriesLeft, 1);
        QCOMPARE(q.items().at(0).error, QStringLiteral("io 错误"));
        // 驱动一拍 → Queued。
        QCOMPARE(q.promoteRetryWaiters(), 1);
        QCOMPARE(q.items().at(0).state, ImportItemState::Queued);
        // 耗尽额度 → Failed。
        q.markRunning(0);
        q.markFailed(0, QStringLiteral("还是错"));
        q.markRunning(0);
        q.markFailed(0, QStringLiteral("彻底错"));
        QCOMPARE(q.items().at(0).state, ImportItemState::Failed);
        QCOMPARE(q.promoteRetryWaiters(), 0);
        // 手动重试 → Queued。
        QVERIFY(q.retryItem(0));
        QCOMPARE(q.items().at(0).state, ImportItemState::Queued);
        // 完成 + 进度（D8.1）。
        q.markRunning(0);
        q.markProgress(0, 60);
        QCOMPARE(q.items().at(0).progressPercent, 60);
        q.markDone(0, QStringLiteral("ast-new"));
        QCOMPARE(q.items().at(0).state, ImportItemState::Done);
        QCOMPARE(q.items().at(0).resultingAssetId, QStringLiteral("ast-new"));
        QCOMPARE(q.items().at(0).progressPercent, 100);
        // 单项取消。
        ImportQueueItem it2;
        it2.path = QStringLiteral("/tmp/y.las");
        q.enqueue(it2);
        QVERIFY(q.cancelItem(1));
        QCOMPARE(q.items().at(1).state, ImportItemState::Canceled);
        QVERIFY(!q.cancelItem(0)); // Done 不可取消
        // 摘要（D8.5）——取消态计入。
        const QString summary = q.summaryText();
        QVERIFY(summary.contains(QStringLiteral("成功 1")));
        QVERIFY(summary.contains(QStringLiteral("取消 1")));
        // 手动重试取消项 → 回 Queued。
        QVERIFY(q.retryItem(1));
        QCOMPARE(q.items().at(1).state, ImportItemState::Queued);
        // 清已完成 → 只剩 Queued 的 y.las。
        q.clearFinished();
        QCOMPARE(q.items().size(), 1);
    }

    void dataops_d8_queuePanelWithRunner()
    {
        using namespace paleo::dataops;
        ImportQueuePanel panel;
        int runCount = 0;
        panel.setRunner([&runCount](int idx, ImportQueueItem &item, ImportRetryQueue *q) {
            Q_UNUSED(item)
            ++runCount;
            q->markRunning(idx);
            q->markProgress(idx, 50);
            q->markProgress(idx, 100);
            q->markDone(idx, QStringLiteral("ast-%1").arg(idx));
        });
        QStringList paths;
        for (int i = 0; i < 3; ++i)
            paths << QStringLiteral("/tmp/import-%1.las").arg(i);
        panel.enqueuePaths(paths, {});
        QCOMPARE(runCount, 3);
        QCOMPARE(panel.queue().items().size(), 3);
        for (const ImportQueueItem &it : panel.queue().items())
            QCOMPARE(it.state, ImportItemState::Done);
        // 队列表渲染：进度条 + 状态文案（D8.1 逐文件进度）。
        auto *table = panel.findChild<QTableWidget *>(QStringLiteral("importQueueTable"));
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 3);
        QVERIFY(table->item(0, 1)->text().contains(QStringLiteral("完成")));
        auto *bar = qobject_cast<QProgressBar *>(table->cellWidget(0, 2));
        QVERIFY2(bar, "progress bar is the cell widget itself");
        QCOMPARE(bar->value(), 100);
        // 失败路径：runner 抛失败 → 状态「失败」+ 重试钮（D8.2 手动重试）。
        ImportQueuePanel failPanel;
        failPanel.setRunner([](int idx, ImportQueueItem &item, ImportRetryQueue *q) {
            q->markRunning(idx);
            if (item.path.endsWith(QLatin1String("bad.las")))
                q->markFailed(idx, QStringLiteral("坏文件"));
            else
                q->markDone(idx, QString());
        });
        // 预置失败：先耗尽自动额度再进面板（直接 enqueue 走满失败态）。
        // 简化：三条 bad → RetryWait（自动重试在 timer 里，不走 exec 路径）。
        failPanel.enqueuePaths({QStringLiteral("/tmp/bad.las")}, {});
        // Runner 里 markFailed 一次 → RetryWait（自动额度 2 未耗尽）。
        QCOMPARE(failPanel.queue().items().at(0).state, ImportItemState::RetryWait);
        failPanel.refresh();
        QVERIFY(failPanel.findChild<QPushButton *>(QStringLiteral("importItemCancel")));
    }

    void dataops_d8_importReportDialog()
    {
        using namespace paleo::dataops;
        ImportRetryQueue q;
        ImportQueueItem ok, bad;
        ok.path = QStringLiteral("/tmp/ok.las");
        bad.path = QStringLiteral("/tmp/bad.las");
        q.enqueue(ok);
        q.enqueue(bad);
        q.markRunning(0);
        q.markDone(0, QString());
        q.markRunning(1);
        q.markFailed(1, QStringLiteral("读取失败"));
        q.markRunning(1);
        q.markFailed(1, QStringLiteral("读取失败"));
        q.markRunning(1);
        q.markFailed(1, QStringLiteral("读取失败"));
        const QString text = q.summaryText();
        QVERIFY(text.contains(QStringLiteral("成功 1")));
        QVERIFY(text.contains(QStringLiteral("失败 1")));
        QVERIFY(text.contains(QStringLiteral("读取失败")));
        ImportReportDialog dlg(text);
        QVERIFY(dlg.findChild<QPlainTextEdit *>(QStringLiteral("importReportText"))
                    ->toPlainText()
                    .contains(QStringLiteral("导入摘要")));
    }

    // ---- D10 杂项（状态反馈/导入入口/静态护栏） -------------------------------
    void dataops_d10_statusMessageOnUndoRedo()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        QSignalSpy spy(page.get(), &DataPage::statusMessage);
        lp->pushCommand(new paleo::dataops::TagCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("核心"), true));
        spy.clear();
        lp->undoOp();
        QCOMPARE(spy.count(), 1); // D5.4：撤销后状态反馈
        QVERIFY(spy.at(0).at(0).toString().contains(QStringLiteral("已撤销")));
        QVERIFY(spy.at(0).at(0).toString().contains(QStringLiteral("核心")));
        spy.clear();
        lp->redoOp();
        QVERIFY(spy.at(0).at(0).toString().contains(QStringLiteral("已重做")));
    }

    void dataops_d10_externalFilesEnqueueAndSignal()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        const QString f1 = QDir(dir.path()).filePath(QStringLiteral("a.las"));
        const QString f2 = QDir(dir.path()).filePath(QStringLiteral("b.sgy"));
        QVERIFY(writeFile(f1, QByteArray("x")));
        QVERIFY(writeFile(f2, QByteArray("y")));
        QSignalSpy spy(page.get(), &DataPage::externalImportRequested);
        lp->handleExternalFiles({f1, f2}); // 2 个文件 < 20 → 不弹预估
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList().size(), 2);
        auto *queue = lp->findChild<paleo::dataops::ImportQueuePanel *>(
            QStringLiteral("importQueuePanel"));
        QVERIFY(queue);
        QCOMPARE(queue->queue().items().size(), 2);
        QCOMPARE(queue->queue().items().at(0).path, f1);
    }

    void dataops_d10_layerMarkersAndNoProgrammaticSplitterResize()
    {
        // D9.1 静态护栏：pages/ 全部源文件带层标记；且不出现对分栏的
        // setSizes 调用（宽度只能由用户拖动/最大化钮/窗口 resize 改变）。
        QDir repoRoot = QFileInfo(QStringLiteral(__FILE__)).dir();
        QVERIFY2(repoRoot.cdUp(), "repo root");
        QDir pagesDir(repoRoot.filePath(QStringLiteral("src/ui/pages")));
        QVERIFY(pagesDir.exists());
        int checked = 0;
        const auto scan = [&checked](const QDir &d) {
            for (const QFileInfo &fi :
                 d.entryInfoList({QStringLiteral("*.h"), QStringLiteral("*.cpp")},
                                 QDir::Files))
            {
                QFile f(fi.absoluteFilePath());
                QVERIFY2(f.open(QIODevice::ReadOnly),
                         qPrintable(fi.absoluteFilePath()));
                const QString src = QString::fromUtf8(f.readAll());
                QVERIFY2(src.left(200).contains(QStringLiteral("// 层：")),
                         qPrintable(fi.fileName() + QStringLiteral(" 缺层标记")));
                QVERIFY2(!src.contains(QStringLiteral("setSizes(")),
                         qPrintable(fi.fileName() +
                                    QStringLiteral(" 含程序化 setSizes（D9.1 禁）")));
                ++checked;
            }
        };
        scan(pagesDir);
        scan(QDir(pagesDir.filePath(QStringLiteral("dataops"))));
        QVERIFY2(checked >= 20, qPrintable(QStringLiteral("only %1").arg(checked)));
    }


    // =====================================================================
    // P3 补充测试（第三块）：模态对话框真实路径驱动 + 边界覆盖。
    // =====================================================================
    void dataops_d1_batchAddTagViaInputDialog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        table->selectRow(0);
        table->selectionModel()->select(
            table->model()->index(1, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        // 两拍：输入标签名 → 确认。
        driveModalNextTick([](QWidget *w) {
            if (auto *dlg = qobject_cast<QInputDialog *>(w))
            {
                dlg->setTextValue(QStringLiteral("核心资产"));
                // 直接捕指针 accept：Windows 下嵌套拍里 activeModalWidget()
                // 可能尚未就绪（曾致 accept 落空、3s 兜底 close 被当取消）。
                QPointer<QInputDialog> guard(dlg);
                QTimer::singleShot(0, [guard] {
                    if (guard)
                        guard->accept();
                });
            }
        });
        lp->batchAddTag();
        // 两个选中资产都带上标签（sidecar + 标签云出现）。
        QCOMPARE(lp->opsContext().tags->tagsFor(fx.astResolved),
                 QStringList{QStringLiteral("核心资产")});
        QCOMPARE(lp->opsContext().tags->tagsFor(fx.astPending),
                 QStringList{QStringLiteral("核心资产")});
        QVERIFY(!lp->opsContext().tags->tagsFor(fx.astFree).isEmpty() == false);
        auto *cloud = page->findChild<QWidget *>(QStringLiteral("tagCloud"));
        QTest::qWait(10);
        QVERIFY(cloud->findChildren<QPushButton *>(QStringLiteral("tagCloudChip"))
                    .size() >= 1);
    }

    void dataops_d1_batchRemoveSoftViaMessageBox()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        table->selectRow(2); // free.sgy
        QCOMPARE(table->rowCount(), 3);
        // 确认框点 Yes。
        driveModalNextTick([](QWidget *w) {
            if (auto *mb = qobject_cast<QMessageBox *>(w))
                mb->button(QMessageBox::Yes)->click();
        });
        lp->batchRemoveSoft();
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(lp->opsContext().recycle->entries().size(), 1);
        // 取消路径：确认框点 No → 不动。
        table->selectRow(1);
        const int rows = table->rowCount();
        driveModalNextTick([](QWidget *w) {
            if (auto *mb = qobject_cast<QMessageBox *>(w))
                mb->button(QMessageBox::No)->click();
        });
        lp->batchRemoveSoft();
        QCOMPARE(table->rowCount(), rows);
    }

    void dataops_d1_batchAttachViaPickerDialog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        // 选中未决的 Z9.las（row 1）。
        const int pendRow = [table, &fx]() {
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 0)->data(Qt::UserRole).toString() == fx.astPending)
                    return r;
            return -1;
        }();
        QVERIFY(pendRow >= 0);
        table->selectRow(pendRow);
        // 两拍：选择器选 B2 行 → accept。
        driveModalNextTick([](QWidget *w) {
            if (auto *dlg =
                    qobject_cast<paleo::dataops::EntityPickerDialog *>(w))
            {
                auto *list = dlg->findChild<QTableWidget *>(
                    QStringLiteral("entityPickerList"));
                for (int r = 0; r < list->rowCount(); ++r)
                    if (list->item(r, 0)->text() == QLatin1String("B2"))
                        list->selectRow(r);
                dlg->accept();
            }
        });
        lp->batchAttachToEntity();
        bool resolved = false;
        for (const EntityAssetLink &l : svc.catalog()->linksForAsset(fx.astPending))
            if (l.entityId == fx.wellB && !l.unresolved)
                resolved = true;
        QVERIFY(resolved);
    }

    void dataops_d4_entityCreateViaDialog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *ep = page->findChild<EntityPanel *>();
        QVERIFY(ep);
        // 两拍：名字填 C9 → accept。
        driveModalNextTick([](QWidget *w) {
            if (auto *dlg =
                    qobject_cast<paleo::dataops::EntityCreateDialog *>(w))
            {
                dlg->findChild<QLineEdit *>(QStringLiteral("entityNameEdit"))
                    ->setText(QStringLiteral("C9"));
                dlg->accept();
            }
        });
        ep->beginCreateEntity();
        // 新井实体落库 + 树里出现。
        bool saw = false;
        for (const CatalogEntity &e : svc.catalog()->entities(QStringLiteral("well")))
            if (e.name == QLatin1String("C9"))
                saw = true;
        QVERIFY(saw);
        // 重名拒绝：同名再建 → 警告框（点掉）→ 不新增。
        const int wells = int(svc.catalog()->entities(QStringLiteral("well")).size());
        driveModalNextTick([](QWidget *w) {
            if (auto *mb = qobject_cast<QMessageBox *>(w))
                mb->button(QMessageBox::Ok)->click();
        });
        // 直接调对话框校验面：空名拒绝（不弹创建）。
        paleo::dataops::EntityCreateDialog dlg;
        QVERIFY(dlg.chosenName().isEmpty());
        QCOMPARE(dlg.chosenType(), QStringLiteral("well"));
        QCOMPARE(dlg.chosenX(), 0.0);
        QVERIFY(svc.catalog()->entities(QStringLiteral("well")).size() == wells);
    }

    void dataops_d4_entityRenameViaDialog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *ep = page->findChild<EntityPanel *>();
        driveModalNextTick([](QWidget *w) {
            if (auto *dlg = qobject_cast<paleo::dataops::EntityEditDialog *>(w))
            {
                dlg->findChild<QLineEdit *>(QStringLiteral("entityEditName"))
                    ->setText(QStringLiteral("A1-改"));
                dlg->accept();
            }
        });
        ep->beginRenameEntity(fx.wellA);
        paleo::dataops::EntityOverrideStore reloaded;
        reloaded.load(svc.catalog());
        QCOMPARE(reloaded.displayName(svc.catalog()->entityById(fx.wellA)),
                 QStringLiteral("A1-改"));
        // 面板头同步显示新名（D4.1 内联编辑的落点）。
        page->selectAssetsForEntities({fx.wellA});
        QVERIFY(page->findChild<QLabel *>(QStringLiteral("entityViewHeader"))
                    ->text()
                    .contains(QStringLiteral("A1-改")));
    }

    void dataops_d4_editRoleViaDialogFlow()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        // 选中已决的 A1.las。
        const int row = [table, &fx]() {
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 0)->data(Qt::UserRole).toString() == fx.astResolved)
                    return r;
            return -1;
        }();
        QVERIFY(row >= 0);
        table->selectRow(row);
        // 统一关联编辑器（非模态）：以「添加补充资料」给 A1 增加 tops 用途。
        lp->editRoleForSelection();
        auto *editor =
            lp->findChild<QDialog *>(QStringLiteral("assetAssociationEditor"));
        QVERIFY(editor);
        auto *target =
            editor->findChild<QComboBox *>(QStringLiteral("associationTarget"));
        auto *role =
            editor->findChild<QComboBox *>(QStringLiteral("associationRole"));
        auto *operation = editor->findChild<QComboBox *>(
            QStringLiteral("associationOperation"));
        QVERIFY(target && role && operation);
        operation->setCurrentIndex(0); // 添加补充资料
        const int targetIdx = target->findData(fx.wellA);
        QVERIFY(targetIdx >= 0);
        target->setCurrentIndex(targetIdx);
        const int roleIdx = role->findData(QStringLiteral("tops"));
        QVERIFY(roleIdx >= 0);
        role->setCurrentIndex(roleIdx);
        auto *buttons = editor->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
        buttons->button(QDialogButtonBox::Save)->click();
        // 新角色链接在场（旧角色保留——catalog 无删除 API，GAPS）。
        bool sawTops = false, sawLog = false;
        for (const EntityAssetLink &l :
             svc.catalog()->linksForAsset(fx.astResolved))
        {
            if (l.role == QLatin1String("tops") && l.entityId == fx.wellA)
                sawTops = true;
            if (l.role == QLatin1String("well_log"))
                sawLog = true;
        }
        QVERIFY(sawTops);
        QVERIFY(sawLog);
    }

    void dataops_d4_entityDeleteDialogDispositions()
    {
        // D4.6：删除实体的资产处置二选一（对话框纯面）。
        paleo::dataops::EntityDeleteDialog keep(3);
        QVERIFY(!keep.assetsToRecycle()); // 默认保留资产
        paleo::dataops::EntityDeleteDialog recycle(0);
        recycle.findChildren<QRadioButton *>().at(1)->click();
        QVERIFY(recycle.assetsToRecycle());
    }

    void dataops_d2_tagCloudClickFilters()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        lp->pushCommand(new paleo::dataops::TagCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("地震"), true));
        lp->pushCommand(new paleo::dataops::TagCmd(
            lp->opsContext(), fx.astResolved, QStringLiteral("测井"), true));
        page->refreshAssetTable();
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        QTest::qWait(10); // 收 deleteLater 尸（重建的旧 chip）
        const QList<QPushButton *> chips =
            page->findChildren<QPushButton *>(QStringLiteral("tagCloudChip"));
        QCOMPARE(int(chips.size()), 2);
        // 点「地震」→ 只剩 free.sgy。
        QPushButton *seis = nullptr;
        for (QPushButton *c : chips)
            if (c->text().contains(QStringLiteral("地震")))
                seis = c;
        QVERIFY(seis);
        seis->click();
        int visible = 0;
        QString name;
        for (int r = 0; r < table->rowCount(); ++r)
            if (!table->isRowHidden(r) &&
                !table->item(r, 0)->data(Qt::UserRole).toString().isEmpty())
            {
                ++visible;
                name = table->item(r, 0)->text();
            }
        QCOMPARE(visible, 1);
        QCOMPARE(name, QStringLiteral("free.sgy"));
        QCOMPARE(lp->activeTagFilter(), QStringLiteral("地震"));
        // 再点取消 → 全回来（重新取 chip——重建后旧 chip 在 deleteLater 队列）。
        QTest::qWait(10);
        QPushButton *seis2 = nullptr;
        for (QPushButton *c : page->findChildren<QPushButton *>(QStringLiteral("tagCloudChip")))
            if (c->text().contains(QStringLiteral("地震")))
                seis2 = c;
        QVERIFY(seis2);
        seis2->click();
        QCOMPARE(lp->activeTagFilter(), QString());
        visible = 0;
        for (int r = 0; r < table->rowCount(); ++r)
            if (!table->isRowHidden(r) &&
                !table->item(r, 0)->data(Qt::UserRole).toString().isEmpty())
                ++visible;
        QCOMPARE(visible, 3);
    }

    void dataops_d2_filterBarAddConditionViaUi()
    {
        using namespace paleo::dataops;
        qRegisterMetaType<FilterCondition>();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *bar = page->findChild<QWidget *>(QStringLiteral("multiDimFilterBar"));
        QVERIFY(bar);
        auto *dim = bar->findChild<QComboBox *>(QStringLiteral("filterDimCombo"));
        auto *value = bar->findChild<QComboBox *>(QStringLiteral("filterValueCombo"));
        auto *add = bar->findChild<QPushButton *>(QStringLiteral("filterAddButton"));
        auto *typeFilter =
            page->findChild<QComboBox *>(QStringLiteral("assetTypeFilter"));
        QVERIFY(dim && value && add && typeFilter);
        // 类型维度 + 值下拉词表已装填（refreshChipBar 填充）。
        const int typeIdx = dim->findText(QStringLiteral("类型"));
        QVERIFY(typeIdx >= 0);
        dim->setCurrentIndex(typeIdx);
        const int valIdx = value->findText(QStringLiteral("seismic"));
        QVERIFY2(valIdx >= 0, "type vocabulary populated");
        value->setCurrentIndex(valIdx);
        QSignalSpy spy(lp, &DataListPanel::selectionCountChanged); // 任意信号证明事件环通
        add->click();
        // 条件落进 FilterGroup + 写回旧类型下拉（reconcile 镜像）+ 表只剩 seismic 行。
        QCOMPARE(lp->filterGroup().conditions.size(), 1);
        QCOMPARE(lp->filterGroup().conditions.at(0).value, QStringLiteral("seismic"));
        QCOMPARE(typeFilter->currentData().toString(), QStringLiteral("seismic"));
        auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
        int visible = 0;
        for (int r = 0; r < table->rowCount(); ++r)
            if (!table->isRowHidden(r) &&
                !table->item(r, 0)->data(Qt::UserRole).toString().isEmpty())
                ++visible;
        QCOMPARE(visible, 1);
        // chip 行出现（D2.2）。
        QCOMPARE(page->findChildren<QPushButton *>(QStringLiteral("filterChip")).size(),
                 1);
        // AND/OR 切换信号面。
        auto *mode = bar->findChild<QToolButton *>(QStringLiteral("filterModeButton"));
        mode->click();
        QVERIFY(lp->filterGroup().orMode);
        mode->click();
        QVERIFY(!lp->filterGroup().orMode);
    }

    void dataops_d2_chipClearAllViaUi()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        FilterGroup g;
        g.add({FilterDim::Type, QStringLiteral("well_log"), false});
        g.add({FilterDim::Status, QStringLiteral("RAW"), false});
        lp->applyFilterGroup(g);
        QTest::qWait(10);
        QCOMPARE(page->findChildren<QPushButton *>(QStringLiteral("filterChip")).size(), 2);
        auto *clear = page->findChild<QPushButton *>(QStringLiteral("filterClearAllButton"));
        QVERIFY(clear);
        clear->click();
        QVERIFY(lp->filterGroup().isEmpty());
        QTest::qWait(10);
        QCOMPARE(page->findChildren<QPushButton *>(QStringLiteral("filterChip")).size(), 0);
    }

    void dataops_d6_tabFocusTraversalReachesNewControls()
    {
        // D6.4 键盘审计：Tab 环路覆盖新部件（过滤条/搜索/工具按钮可达）。
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *search = page->findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
        QVERIFY(search);
        // 选项披露（dataListAdvancedOptions）默认收起——默认面只有搜索行和
        // 数据视图；键盘审计先展开披露再走 Tab 环路（折叠态控件不在焦点链，
        // wave/deepen-perf 修：原测试在默认收起态断言过滤条可达，过期红）。
        auto *options = page->findChild<QToolButton *>(QStringLiteral("dataListOptionsButton"));
        QVERIFY(options);
        page->show();
        QTest::qWait(20);
        options->click();
        QTest::qWait(20);
        search->setFocus();
        // TEST-07/ARCH-08：offscreen QPA 不派发平台焦点事件——keyClick(Tab)
        // 遍历在无窗口管理器环境不可复现地红（审计证据 LastTest.log:151-153；
        // 本机 Qt 6.11 offscreen 偶绿，CI 红）。改为 nextInFocusChain 链步进
        // + 真实 Tab 语义过滤（焦点策略含 TabFocus 位且对页面可见）——
        // 平台无关、确定性，与 Tab 键的实际停驻面同构。
        auto nextTabStop = [&page](QWidget *w) -> QWidget * {
            QWidget *next = w->nextInFocusChain();
            while (next && next != w &&
                   !((next->focusPolicy() & Qt::TabFocus) && next->isVisibleTo(page.get())))
                next = next->nextInFocusChain();
            return next == w ? nullptr : next; // 环路闭合 = nullptr
        };
        QSet<QString> visited;
        QWidget *w = search;
        for (int i = 0; i < 80 && w; ++i)
        {
            visited.insert(w->objectName());
            w = nextTabStop(w);
        }
        // 至少到达过搜索框 + 过滤维度下拉 + 表（或其视口属主）。
        QVERIFY(visited.contains(QStringLiteral("assetSearchEdit")));
        QVERIFY(visited.contains(QStringLiteral("filterDimCombo")) ||
                visited.contains(QStringLiteral("filterValueCombo")));
    }

    void dataops_d7_columnConfigDialogResult()
    {
        using namespace paleo::dataops;
        ColumnConfigDialog dlg({QStringLiteral("名称"), QStringLiteral("类型"),
                                QStringLiteral("关联")},
                               {true, false, true});
        auto *list = dlg.findChild<QListWidget *>(QStringLiteral("columnConfigList"));
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->item(1)->checkState(), Qt::Unchecked);
        // 下移第 0 行 → 顺序变 类型前（result 反映）。
        list->setCurrentRow(0);
        dlg.findChild<QPushButton *>(QStringLiteral("columnDownButton"))->click();
        const QList<QPair<int, bool>> r = dlg.result();
        QCOMPARE(r.size(), 3);
        QCOMPARE(r.at(0).first, 1); // 类型列排最前
        QCOMPARE(r.at(0).second, false); // 且隐藏
        QCOMPARE(r.at(1).first, 0); // 名称列退居第二
        // 上移回去。
        list->setCurrentRow(0);
        dlg.findChild<QPushButton *>(QStringLiteral("columnUpButton"))->click();
        QCOMPARE(dlg.result().at(0).first, 1);
    }

    void dataops_d7_iconViewLoadAndActivation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        auto *icon = page->findChild<QListWidget *>(QStringLiteral("assetIconView"));
        QVERIFY(icon);
        lp->setViewMode(2); // 只在进入图标视图时装载，隐藏视图不再随筛选重建。
        QCOMPARE(icon->count(), 3);
        QCOMPARE(icon->viewMode(), QListView::IconMode);
        // 激活语义 = 打开预览（D7.1 图标态与表一致）。
        QSignalSpy spy(page.get(), &DataPage::assetActivated);
        emit icon->itemActivated(icon->item(0));
        QCOMPARE(spy.count(), 1);
        // 图标态多选（D1.1 跨视图）。
        icon->item(0)->setSelected(true);
        icon->item(1)->setSelected(true);
        QCOMPARE(lp->currentAssetSelection().size(), 2);
    }

    void dataops_d7_virtualViewColumnRestore()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        lp->setViewMode(3);
        auto *virt = page->findChild<QTableView *>(QStringLiteral("assetVirtualTable"));
        QVERIFY(virt && virt->model());
        QCOMPARE(virt->model()->columnCount(), FlatAssetModel::ColCount);
        // 模型数据面：DisplayRole 名称列 + 右对齐列。
        const QModelIndex first = virt->model()->index(0, FlatAssetModel::ColName);
        QVERIFY(first.isValid());
        QVERIFY(!virt->model()->data(first, Qt::DisplayRole).toString().isEmpty());
    }

    void dataops_d8_duplicateDialogResolutions()
    {
        using namespace paleo::dataops;
        QVector<DuplicateHit> hits(2);
        hits[0].incomingPath = QStringLiteral("/in/a.las");
        hits[0].reason = QStringLiteral("sha");
        hits[1].incomingPath = QStringLiteral("/in/b.sgy");
        hits[1].reason = QStringLiteral("name");
        DuplicateDialog dlg;
        dlg.loadHits(hits);
        auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("duplicateTable"));
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("SHA 相同"));
        QCOMPARE(table->item(1, 1)->text(), QStringLiteral("同名"));
        // 逐条处置 + 一键全量。
        dlg.setRowResolution(0, DuplicateResolution::Skip);
        QCOMPARE(dlg.resolutions().value(QStringLiteral("/in/a.las")),
                 DuplicateResolution::Skip);
        auto *allRename = [&]() {
            const auto btns = dlg.findChildren<QPushButton *>();
            for (QPushButton *b : btns)
                if (b->text().contains(QStringLiteral("全部重命名")))
                    return b;
            return static_cast<QPushButton *>(nullptr);
        }();
        QVERIFY(allRename);
        allRename->click();
        QCOMPARE(dlg.resolutions().value(QStringLiteral("/in/b.sgy")),
                 DuplicateResolution::Rename);
    }

    void dataops_d8_estimateDialogBatches()
    {
        using namespace paleo::dataops;
        ImportEstimateDialog dlg;
        ImportEstimate est;
        est.fileCount = 1200;
        est.totalBytes = qint64(1200) * 1024 * 1024;
        est.largeFileCount = 3;
        est.byExtension = {QStringLiteral("las:900"), QStringLiteral("sgy:300")};
        dlg.setEstimate(est);
        QVERIFY(dlg.findChild<QLabel *>()->text().contains(QStringLiteral("1200")));
        QVERIFY(!dlg.batchChosen()); // 默认一次全部
        dlg.findChildren<QRadioButton *>().at(1)->click();
        QVERIFY(dlg.batchChosen());
        // batchPaths 切分（D8.6）。
        QStringList paths;
        for (int i = 0; i < 450; ++i)
            paths << QStringLiteral("f%1").arg(i);
        QCOMPARE(batchPaths(paths, 200).size(), 200);
        QCOMPARE(batchPaths(paths, 500).size(), 450);
    }

    void dataops_d10_exportFileWriteRoundtrip()
    {
        using namespace paleo::dataops;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVector<AssetRowInfo> rows(1);
        rows[0].displayName = QStringLiteral("A1.las");
        rows[0].fileName = QStringLiteral("A1.las");
        rows[0].effectiveType = QStringLiteral("well_log");
        const QString path = dir.filePath(QStringLiteral("out.json"));
        QVERIFY(writeExportFile(path, exportJson(rows, ExportFields())));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value(QStringLiteral("count")).toInt(), 1);
        QVERIFY(!writeExportFile(QStringLiteral("/nonexistent-dir/x/y.json"),
                                 QByteArray("x")));
    }

    void dataops_d10_recycleBinEdgeCases()
    {
        using namespace paleo::dataops;
        RecycleBin bin;
        bin.remove(QStringLiteral("a"), QStringLiteral("t"), QStringLiteral("n1"),
                   QStringLiteral("r"));
        bin.remove(QStringLiteral("a"), QStringLiteral("t"), QStringLiteral("n1"),
                   QStringLiteral("r")); // 幂等
        bin.remove(QStringLiteral("b"), QStringLiteral("t2"), QStringLiteral("n2"), QString());
        QCOMPARE(bin.entries().size(), 2);
        QVERIFY(bin.isRemoved(QStringLiteral("a")));
        QVERIFY(!bin.restore(QStringLiteral("zzz"))); // 不存在
        QVERIFY(bin.restore(QStringLiteral("b")));
        QVERIFY(!bin.isRemoved(QStringLiteral("b")));
        bin.clearAll();
        QVERIFY(bin.entries().isEmpty());
    }

    void dataops_d10_tagStoreLimits()
    {
        using namespace paleo::dataops;
        TagStore tags;
        const QString long40(40, QChar(QLatin1Char('x')));
        const QString long41(41, QChar(QLatin1Char('x')));
        QVERIFY(tags.addTag(QStringLiteral("a"), long40));  // 40 上限内
        QVERIFY(!tags.addTag(QStringLiteral("a"), long41)); // 超限拒
        QVERIFY(!tags.addTag(QStringLiteral("a"), QStringLiteral("带\t制表符"))); // 控制字符拒
        QCOMPARE(tags.tagsFor(QStringLiteral("a")).size(), 1);
        // 大小写不敏感去重。
        QVERIFY(tags.addTag(QStringLiteral("b"), QStringLiteral("Core")));
        QVERIFY(!tags.addTag(QStringLiteral("b"), QStringLiteral("CORE")));
        QCOMPARE(tags.tagsFor(QStringLiteral("b")), QStringList{QStringLiteral("Core")});
    }

    void dataops_d10_selectionMixSemantics()
    {
        using namespace paleo::dataops;
        SelectionMix mix;
        QVERIFY(mix.isEmpty());
        QVERIFY(!mix.mixed());
        mix.assetIds = {QStringLiteral("a1"), QStringLiteral("a2")};
        QVERIFY(!mix.mixed());
        mix.entityIds = {QStringLiteral("e1")};
        QVERIFY(mix.mixed());
        QVERIFY(!mix.isEmpty());
        // 公共子集 = 资产集（mixed 时实体只作上下文）。
        const QSet<QString> expected{QStringLiteral("a1"), QStringLiteral("a2")};
        QCOMPARE(mix.commonAssetIds(), expected);
    }

    void dataops_d10_undoStackClearSignal()
    {
        using namespace paleo::dataops;
        DataOpsUndoStack stack;
        class Noop : public DataOpCommand
        {
        public:
            void redo() override {}
            void undo() override {}
            QString text() const override { return QStringLiteral("x"); }
        };
        QSignalSpy spy(&stack, &DataOpsUndoStack::stackChanged);
        stack.push(new Noop);
        QCOMPARE(spy.count(), 1);
        stack.undo();
        QCOMPARE(spy.count(), 2);
        stack.redo();
        QCOMPARE(spy.count(), 3);
        stack.clear();
        QCOMPARE(spy.count(), 4);
        stack.clear(); // 空清不发
        QCOMPARE(spy.count(), 4);
    }

    void dataops_d10_highlightDelegatePaintSmoke()
    {
        // D2.7：委托自绘冒烟（不崩 + needle 设置往返）。
        QImage img(200, 24, QImage::Format_ARGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        paleo::dataops::HighlightDelegate delegate;
        delegate.setNeedle(QStringLiteral("A1"));
        QCOMPARE(delegate.needle(), QStringLiteral("A1"));
        QStyleOptionViewItem opt;
        opt.rect = QRect(0, 0, 200, 24);
        opt.font = p.font();
        opt.textElideMode = Qt::ElideRight;
        QModelIndex idx; // 空模型索引——paint 走基类空文本路径
        delegate.paint(&p, opt, idx);
        p.end();
        delegate.setNeedle(QString());
        QVERIFY(delegate.needle().isEmpty());
    }

    void dataops_d10_operationsHistoryInPanelFlow()
    {
        // D4.10：批量操作落历史（会话内，DataPage 面）。
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        lp->pushCommand(new paleo::dataops::TagCmd(
            lp->opsContext(), fx.astFree, QStringLiteral("核心"), true));
        // applyEntityDrop 记历史。
        lp->applyEntityDrop({fx.astPending}, fx.wellB);
        const QStringList entries = lp->operationsHistory()->entries();
        QVERIFY2(entries.size() >= 1, "history records batch ops");
        QVERIFY(entries.join(QLatin1Char(';')).contains(QStringLiteral("挂接")));
    }


    void dataops_d2_stateStringCjkAndSpecialEscaping()
    {
        using namespace paleo::dataops;
        // D2.10：中文/空格/特殊字符经 URL 编码往返不丢。
        FilterGroup g;
        g.add({FilterDim::Search, QStringLiteral("测井 曲线 #1"), false});
        g.add({FilterDim::Tag, QStringLiteral("核心/资料"), true});
        g.add({FilterDim::Regex, QStringLiteral("A\\d+\\.las"), false});
        g.orMode = true;
        const QString s1 = g.toStateString();
        QVERIFY(s1.contains(QLatin1String("%")));
        const FilterGroup back = FilterGroup::fromStateString(s1);
        QCOMPARE(back.conditions.size(), 3);
        QCOMPARE(back.conditions.at(0).value, QStringLiteral("测井 曲线 #1"));
        QCOMPARE(back.conditions.at(1).value, QStringLiteral("核心/资料"));
        QVERIFY(back.conditions.at(1).negate);
        QCOMPARE(back.conditions.at(2).value, QStringLiteral("A\\d+\\.las"));
        QVERIFY(back.orMode);
        // 空组的状态串仍可解析回空组。
        QVERIFY(FilterGroup::fromStateString(FilterGroup().toStateString()).isEmpty());
    }

    void dataops_d5_transferToSameEntityIsNoop()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        // 已挂到 well-A1 的资产再「转移」到 well-A1 → 跳过（不增命令不乱栈）。
        const int depthBefore = lp->opStack()->depth();
        lp->applyEntityDrop({fx.astResolved}, fx.wellA);
        QCOMPARE(lp->opStack()->depth(), depthBefore);
        QCOMPARE(svc.catalog()->linksForAsset(fx.astResolved).at(0).entityId, fx.wellA);
        // 空资产列表 → 无操作。
        lp->applyEntityDrop({}, fx.wellB);
        QCOMPARE(lp->opStack()->depth(), depthBefore);
    }

    void dataops_d3_multiAssetDropOnEntityAttachesAll()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataOpsFixture fx;
        fx.build(svc.catalog());
        std::unique_ptr<DataPage> page(fx.makePage(&svc));
        auto *lp = page->findChild<DataListPanel *>();
        // D3.7：一条 mime 带两个未决/自由资产 → 目标实体一次全收。
        // （fixture 只有一条未决——补一条未决链接后多 id 拖。）
        EntityAssetLink un2;
        un2.entityType = QStringLiteral("well");
        un2.assetId = fx.astFree;
        un2.role = QStringLiteral("seismic_volume");
        un2.unresolved = true;
        QVERIFY(svc.catalog()->addLink(un2));
        lp->applyEntityDrop({fx.astPending, fx.astFree}, fx.wellA);
        for (const QString &id : {fx.astPending, fx.astFree})
        {
            bool ok = false;
            for (const EntityAssetLink &l : svc.catalog()->linksForAsset(id))
                if (l.entityId == fx.wellA && !l.unresolved)
                    ok = true;
            QVERIFY2(ok, qPrintable(id));
        }
        // 两条都可撤销回未决。
        lp->undoOp();
        lp->undoOp();
        for (const QString &id : {fx.astPending, fx.astFree})
            QVERIFY(svc.catalog()->linksForAsset(id).at(0).unresolved);
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestPanelsDataOps tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_panels_dataops.moc"
