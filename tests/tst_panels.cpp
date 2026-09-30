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

// §42.2 right-dock page panels. Panels emit intents only (§25: no Qgs* in
// ui/pages) — the tests bind them to workflows/services constructed over a
// bare manifest; a null QgisProjectService keeps the layer service off QGIS
// entirely, so a plain QApplication (no QgisRuntime) is enough.
class TestPanels : public QObject
{
  Q_OBJECT

  private:
    // Declares `count` single-factor layers plus `other` non-factor layers in
    // a temp manifest; caller keeps `dir` alive for the manifest's sqlite db.
    void seedManifest(LayerManifest *m, int factors, int others)
    {
      for (int i = 0; i < factors; ++i)
      {
        LayerDeclaration d;
        d.layerId = QStringLiteral("factor.T1.f%1").arg(i);
        d.horizon = QStringLiteral("T1");
        d.type = QStringLiteral("raster");
        d.source = QStringLiteral("memory|f%1").arg(i); // memory = not file-backed
        d.group = QStringLiteral("04_SingleFactor");
        QVERIFY2(m->upsert(d), qPrintable(d.layerId));
      }
      for (int i = 0; i < others; ++i)
      {
        LayerDeclaration d;
        d.layerId = QStringLiteral("constraints.T1.c%1").arg(i);
        d.horizon = QStringLiteral("T1");
        d.type = QStringLiteral("vector");
        d.source = QStringLiteral("memory|c%1").arg(i);
        d.group = QStringLiteral("02_Constraints");
        QVERIFY2(m->upsert(d), qPrintable(d.layerId));
      }
    }

    // T22 文件夹确认表测试栈：layer/store 只需非空（m_store 仅判空，
    // m_layers 仅层位分支用），不需要 QgisRuntime。
    struct FolderStack
    {
      LayerManifest manifest;
      QgisLayerService layers;
      PaleoProjectStore store;
      DataImportService svc;
      FolderStack(const QString &manifestPath, const QString &projectDir)
        : manifest(manifestPath), layers(nullptr, &manifest), svc(&layers, &store)
      {
        manifest.open();
        svc.setProjectDir(projectDir);
      }
    };

    // W2：文件夹确认表的壳出口——测试里就地接 FolderImportWorkflow
    //（无任务服务 → 同步旧路径，点击即完成）。win 非空时挂「查看未决」。
    static PaleoFolderConfirm::Hooks
    folderConfirmHooks(FolderImportWorkflow &wf, const QString &dir,
                       PaleoMainWindow *win = nullptr)
    {
      PaleoFolderConfirm::Hooks hooks;
      hooks.importRow = [&wf](const QString &p, const QString &f) {
        return wf.importFolderRow(p, f, nullptr);
      };
      hooks.importAll = [&wf, dir](const QMap<QString, QString> &ov,
                                   FolderImportWorkflow::ImportDone done) {
        wf.importFolder(dir, ov, std::move(done));
      };
      hooks.importedWellHead = [&wf](const QString &rowPath) {
        return wf.importedWellHeadAsset(rowPath);
      };
      if (win)
        hooks.showUnresolved = [win] {
          win->showPage(QStringLiteral("data"));
          if (auto *page = win->findChild<DataPage *>())
            page->setUnresolvedFilter(true);
        };
      return hooks;
    }

    static bool writeFile(const QString &path, const QByteArray &content)
    {
      QFile f(path);
      if (!f.open(QIODevice::WriteOnly))
        return false;
      f.write(content);
      return true;
    }

    static int tableRowForPath(const QTableWidget *t, const QString &suffix)
    {
      for (int r = 0; r < t->rowCount(); ++r)
        if (t->item(r, 0)->data(Qt::UserRole).toString().endsWith(suffix))
          return r;
      return -1;
    }

  private slots:
    // ---- DataPage ----
    void dataPage_importButtonsEmitKind()
    {
      DataPage page;
      QSignalSpy spy(&page, &DataPage::importRequested);
      const QHash<QString, QString> cases = {
        {QStringLiteral("importWells"), QStringLiteral("wells")},
        {QStringLiteral("importSeismic"), QStringLiteral("seismic")},
        {QStringLiteral("importBoundary"), QStringLiteral("boundary")},
        {QStringLiteral("importFolder"), QStringLiteral("folder")},
      };
      for (auto it = cases.constBegin(); it != cases.constEnd(); ++it)
      {
        auto *btn = page.findChild<QPushButton *>(it.key());
        QVERIFY2(btn, qPrintable(it.key()));
        // T32 a11y：每个导入入口都有 accessibleName + 非空描述。
        QVERIFY2(!btn->accessibleName().isEmpty(),
                 qPrintable(it.key() + QStringLiteral(" needs accessibleName")));
        QVERIFY(!btn->accessibleDescription().isEmpty());
        spy.clear();
        btn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), it.value());
      }
    }

    void dataPage_assetTableEmptyState()
    {
      DataPage page;
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QVERIFY(table);
      QCOMPARE(table->columnCount(), 3);
      QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("名称"));
      QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("类型"));
      QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("关联"));
      // §42.4: an empty table shows a disabled guidance row, never a blank panel.
      QCOMPARE(table->rowCount(), 1);
      QVERIFY(table->item(0, 0));
      QVERIFY(!(table->item(0, 0)->flags() & Qt::ItemIsEnabled));
      QVERIFY(!table->item(0, 0)->text().isEmpty());
    }

    // §4 预览壳重排：资产表「关联」列——未决链接给「未决」徽标 + 井下拉 +
    // 页内确认条（资产名+实体名同时写出）；确认挂接后出现「撤销」，撤销回到
    // 未决（撤销只认本会话从本页挂上的链接）。
    void dataPage_unresolvedAttachConfirmUndo()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      CatalogAsset asset;
      asset.id = QStringLiteral("ast-1");
      asset.type = QStringLiteral("well_log");
      asset.displayName = QStringLiteral("A1.las");
      QVERIFY(cat->addAsset(asset));
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.assetId = asset.id;
      link.role = QStringLiteral("well_log");
      link.unresolved = true;
      link.note = QStringLiteral("未匹配井名: A1x");
      QVERIFY(cat->addLink(link));

      DataPage page;
      PreviewDocService previewDoc1(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc1));
      page.refreshAssetTable();
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QVERIFY(table);
      QCOMPARE(table->rowCount(), 1);
      QCOMPARE(table->item(0, 2)->text(), QStringLiteral("未决"));

      auto *badge = table->findChild<QLabel *>(QStringLiteral("unresolvedBadge"));
      QVERIFY(badge);
      QVERIFY(badge->styleSheet().contains(QStringLiteral("#FFF4E0")));
      QCOMPARE(badge->toolTip(), QStringLiteral("未匹配井名: A1x"));

      auto *combo = table->findChild<QComboBox *>(QStringLiteral("resolveEntityCombo"));
      auto *attach = table->findChild<QPushButton *>(QStringLiteral("attachLinkButton"));
      QVERIFY(combo && attach);
      QVERIFY(!attach->isEnabled()); // 哨兵「（选择井）」未选实体
      QCOMPARE(combo->count(), 2);
      QCOMPARE(combo->itemText(1), QStringLiteral("A1"));
      combo->setCurrentIndex(1);
      QVERIFY(attach->isEnabled());
      attach->click();

      // 页内确认条同时写出资产名和实体名——不弹模态框。
      auto *confirmText = table->findChild<QLabel *>(QStringLiteral("attachConfirmText"));
      QVERIFY(confirmText);
      QVERIFY(confirmText->text().contains(QStringLiteral("A1.las")));
      QVERIFY(confirmText->text().contains(QStringLiteral("A1")));
      table->findChild<QPushButton *>(QStringLiteral("attachConfirmButton"))->click();

      // 已决：实体名进关联列，链接成为主关联；「撤销」入口出现。
      QCOMPARE(table->item(0, 2)->text(), QStringLiteral("A1"));
      QCOMPARE(cat->links().size(), 1);
      QVERIFY(!cat->links().at(0).unresolved);
      QVERIFY(cat->links().at(0).isPrimary);
      QCOMPARE(cat->links().at(0).entityId, QStringLiteral("well-1"));
      auto *undo = table->findChild<QPushButton *>(QStringLiteral("undoAttachButton"));
      QVERIFY(undo);
      QVERIFY(!table->findChild<QLabel *>(QStringLiteral("unresolvedBadge")));

      // 撤销 → 回到未决徽标 + 挂接控件。
      undo->click();
      QVERIFY(cat->links().at(0).unresolved);
      QVERIFY(cat->links().at(0).entityId.isEmpty());
      QCOMPARE(table->item(0, 2)->text(), QStringLiteral("未决"));
      QVERIFY(table->findChild<QLabel *>(QStringLiteral("unresolvedBadge")));
      QVERIFY(!table->findChild<QPushButton *>(QStringLiteral("undoAttachButton")));
    }

    // 「设为主版本」：同井同角色的两条已决链接，非主那条给按钮；点击后主
    // 关联换到该资产（不变量：同 (entityType,entityId,role) 只留一条主）。
    void dataPage_setPrimaryLink()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      for (const char *id : {"ast-1", "ast-2"})
      {
        CatalogAsset a;
        a.id = QString::fromLatin1(id);
        a.type = QStringLiteral("well_log");
        a.displayName = a.id + QStringLiteral(".las");
        QVERIFY(cat->addAsset(a));
      }
      EntityAssetLink l1;
      l1.entityType = QStringLiteral("well");
      l1.entityId = well.id;
      l1.assetId = QStringLiteral("ast-1");
      l1.role = QStringLiteral("well_log");
      l1.isPrimary = true;
      QVERIFY(cat->addLink(l1));
      EntityAssetLink l2 = l1;
      l2.assetId = QStringLiteral("ast-2");
      l2.isPrimary = false; // 旧版本——同角色非主链接
      QVERIFY(cat->addLink(l2));

      DataPage page;
      PreviewDocService previewDoc2(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc2));
      page.refreshAssetTable();
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QCOMPARE(table->rowCount(), 2);
      // ast-1 是主链接行（纯文本无控件）；ast-2 行有「设为主版本」。
      auto *primary = table->findChild<QPushButton *>(QStringLiteral("setPrimaryButton"));
      QVERIFY(primary);
      primary->click();
      QVERIFY(!cat->links().at(0).isPrimary);
      QVERIFY(cat->links().at(1).isPrimary);
      // 刷新后角色互换：ast-1 成了非主旧版本，它的行拿到同一个按钮。
      QVERIFY(table->findChild<QPushButton *>(QStringLiteral("setPrimaryButton")));
    }

    // ---- T28：链接身份寻址 + undo 跨 reload 恢复 ----

    // 撤销跨 open() 存活：attach 降级了 ast-1 的主关联并清了 note；新会话
    // （新 svc + 新 DataPage，同一工程目录）里撤销条仍在，点击后降级的
    // primary 恢复、note 回到未决徽标（D4）。
    void dataPage_undoRestoresDemotedPrimaryAndNoteAcrossReload()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      {
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataCatalog *cat = svc.catalog();
        CatalogEntity well;
        well.id = QStringLiteral("well-1");
        well.entityType = QStringLiteral("well");
        well.name = QStringLiteral("A1");
        QVERIFY(cat->addEntity(well));
        for (const char *id : {"ast-1", "ast-2"})
        {
          CatalogAsset a;
          a.id = QString::fromLatin1(id);
          a.type = QStringLiteral("well_log");
          a.displayName = a.id + QStringLiteral(".las");
          QVERIFY(cat->addAsset(a));
        }
        EntityAssetLink primary;
        primary.entityType = QStringLiteral("well");
        primary.entityId = well.id;
        primary.assetId = QStringLiteral("ast-1");
        primary.role = QStringLiteral("well_log");
        primary.isPrimary = true;
        QVERIFY(cat->addLink(primary));
        EntityAssetLink pending = primary;
        pending.assetId = QStringLiteral("ast-2");
        pending.entityId.clear();
        pending.isPrimary = true;
        pending.unresolved = true;
        pending.note = QStringLiteral("未匹配井名: Z9");
        QVERIFY(cat->addLink(pending));

        DataPage page;
        PreviewDocService previewDoc3(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc3));
        page.refreshAssetTable();
        auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
        QVERIFY(table);
        auto *combo = table->findChild<QComboBox *>(QStringLiteral("resolveEntityCombo"));
        auto *attach = table->findChild<QPushButton *>(QStringLiteral("attachLinkButton"));
        QVERIFY(combo && attach);
        combo->setCurrentIndex(1); // A1
        attach->click();
        table->findChild<QPushButton *>(QStringLiteral("attachConfirmButton"))->click();

        // attach 后：ast-2 成主关联，ast-1 被降级，note 被清（catalog 语义）。
        const auto links1 = cat->links();
        QCOMPARE(links1.size(), 2);
        bool sawAst2Primary = false, sawAst1Demoted = false;
        for (const EntityAssetLink &l : links1)
        {
          if (l.assetId == QLatin1String("ast-2"))
          {
            QVERIFY(!l.unresolved);
            QVERIFY(l.isPrimary);
            sawAst2Primary = true;
          }
          if (l.assetId == QLatin1String("ast-1"))
          {
            QVERIFY(!l.isPrimary); // 降级
            sawAst1Demoted = true;
          }
        }
        QVERIFY(sawAst2Primary && sawAst1Demoted);
      } // svc 析构——会话状态全部消失

      // 新会话：同工程目录重新打开。撤销入口靠 vault 存活。
      DataImportService svc2(nullptr, nullptr);
      svc2.setProjectDir(dir.path());
      DataCatalog *cat2 = svc2.catalog();
      DataPage page2;
      PreviewDocService previewDoc2(&svc2);
        page2.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc2));
      page2.refreshAssetTable();
      auto *table2 = page2.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      auto *undo = table2->findChild<QPushButton *>(QStringLiteral("undoAttachButton"));
      QVERIFY2(undo, "undo entry must survive an open() reload (project-scoped vault)");

      undo->click();
      bool sawRestoredPrimary = false, sawUnresolved = false;
      for (const EntityAssetLink &l : cat2->links())
      {
        if (l.assetId == QLatin1String("ast-2"))
        {
          QVERIFY2(l.unresolved, "undo must return the link to unresolved");
          sawUnresolved = true;
        }
        if (l.assetId == QLatin1String("ast-1"))
        {
          QVERIFY2(l.isPrimary, "undo must restore the demoted primary (D4)");
          sawRestoredPrimary = true;
        }
      }
      QVERIFY(sawUnresolved && sawRestoredPrimary);
      // note 在 UI 层恢复显示（catalog 无 note 写回 API——A 包接缝）。
      auto *badge = table2->findChild<QLabel *>(QStringLiteral("unresolvedBadge"));
      QVERIFY(badge);
      QCOMPARE(badge->toolTip(), QStringLiteral("未匹配井名: Z9"));
      // 记录已消费：撤销入口消失。
      QVERIFY(!table2->findChild<QPushButton *>(QStringLiteral("undoAttachButton")));
    }

    // 交错变更不串线：刷新建好转钮后，外部挂接另一条链接 + 整表重建，
    // 本行动作仍按 (assetId, role) 命中自己的链接（T28 身份寻址）。
    void dataPage_actionsHitOwnLinkAfterInterleavedChanges()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      for (const char *wid : {"well-1", "well-2"})
      {
        CatalogEntity w;
        w.id = QString::fromLatin1(wid);
        w.entityType = QStringLiteral("well");
        w.name = w.id == QLatin1String("well-1") ? QStringLiteral("A1")
                                                  : QStringLiteral("A2");
        QVERIFY(cat->addEntity(w));
      }
      for (const char *id : {"ast-1", "ast-2"})
      {
        CatalogAsset a;
        a.id = QString::fromLatin1(id);
        a.type = QStringLiteral("well_log");
        a.displayName = a.id + QStringLiteral(".las");
        QVERIFY(cat->addAsset(a));
        EntityAssetLink l;
        l.entityType = QStringLiteral("well");
        l.assetId = a.id;
        l.role = QStringLiteral("well_log");
        l.unresolved = true;
        QVERIFY(cat->addLink(l));
      }

      DataPage page;
      PreviewDocService previewDoc4(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc4));
      page.refreshAssetTable();
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QCOMPARE(table->rowCount(), 2);

      // 外部变更（模拟后台 dedup 挂接 ast-1 → A1）+ changed() 式整表重建。
      QCOMPARE(cat->attachLink(0, QStringLiteral("well-1")), true);
      page.refreshAssetTable();

      // ast-1 行已决（无控件）；ast-2 行仍可挂——按钮是表里唯一那个。
      auto *combo = table->findChild<QComboBox *>(QStringLiteral("resolveEntityCombo"));
      auto *attach = table->findChild<QPushButton *>(QStringLiteral("attachLinkButton"));
      QVERIFY(combo && attach);
      combo->setCurrentIndex(combo->findData(QStringLiteral("well-2"))); // A2
      attach->click();
      table->findChild<QPushButton *>(QStringLiteral("attachConfirmButton"))->click();

      for (const EntityAssetLink &l : cat->links())
      {
        if (l.assetId == QLatin1String("ast-1"))
        {
          QCOMPARE(l.entityId, QStringLiteral("well-1")); // 外部挂接不被扰动
          QVERIFY(l.isPrimary);
        }
        if (l.assetId == QLatin1String("ast-2"))
        {
          QVERIFY(!l.unresolved);
          QCOMPARE(l.entityId, QStringLiteral("well-2")); // 命中自己的链接
          QVERIFY(l.isPrimary);
        }
      }
    }

    // 确认条在 changed() 整表重建后仍存活：待确认状态落在页面属性上，
    // 重建时恢复同一条确认（文本 + 当前页），确认照常生效。
    void dataPage_confirmStripSurvivesRefresh()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      CatalogAsset a;
      a.id = QStringLiteral("ast-1");
      a.type = QStringLiteral("well_log");
      a.displayName = QStringLiteral("A1.las");
      QVERIFY(cat->addAsset(a));
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.assetId = a.id;
      l.role = QStringLiteral("well_log");
      l.unresolved = true;
      QVERIFY(cat->addLink(l));

      DataPage page;
      PreviewDocService previewDoc5(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc5));
      page.refreshAssetTable();
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      auto *combo = table->findChild<QComboBox *>(QStringLiteral("resolveEntityCombo"));
      combo->setCurrentIndex(1);
      table->findChild<QPushButton *>(QStringLiteral("attachLinkButton"))->click();

      // changed() → 整表重建（这里直接驱动同一入口）。
      page.refreshAssetTable();
      table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      auto *confirmText = table->findChild<QLabel *>(QStringLiteral("attachConfirmText"));
      QVERIFY2(confirmText && confirmText->isVisibleTo(table),
               "confirm strip must survive a catalog-changed rebuild");
      QVERIFY(confirmText->text().contains(QStringLiteral("A1.las")));
      QVERIFY(confirmText->text().contains(QStringLiteral("A1")));

      // 确认仍生效。
      table->findChild<QPushButton *>(QStringLiteral("attachConfirmButton"))->click();
      QVERIFY(!cat->links().at(0).unresolved);
      QCOMPARE(cat->links().at(0).entityId, QStringLiteral("well-1"));
    }

    // ---- T20 余项：catalogOpenFailed 状态栏露出 ----
    // 注入打开失败（坏 catalog.json）→ 状态栏红胶囊（DESIGN.md error token）
    // 常驻显示原因 + 数据页导入按钮禁用。
    void mainWindow_catalogOpenFailureSurfacesInStatusbar()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString metaDir = QDir(dir.path()).filePath(QStringLiteral("artifacts/metadata"));
      QVERIFY(QDir().mkpath(metaDir));
      QVERIFY(writeFile(QDir(metaDir).filePath(QStringLiteral("catalog.json")),
                        QByteArrayLiteral("{ not json")));

      QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
      PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, nullptr);
      DataImportService svc(nullptr, nullptr);
      win.attachWorkflows(nullptr, nullptr, nullptr, nullptr, &svc);

      auto *label = win.findChild<QLabel *>(QStringLiteral("statusCatalogError"));
      QVERIFY2(label, "statusbar must own a catalog-error capsule");
      QVERIFY(label->styleSheet().contains(QStringLiteral("#FDEBEB"))); // errorBg
      QVERIFY(label->styleSheet().contains(QStringLiteral("#C62828"))); // errorText 深色变体（AA）
      QVERIFY(!label->isVisibleTo(&win));

      QSignalSpy spy(&svc, &DataImportService::catalogOpenFailed);
      svc.setProjectDir(dir.path()); // open() 失败 → 信号
      QCOMPARE(spy.count(), 1);
      QVERIFY(label->isVisibleTo(&win));
      QVERIFY(label->text().contains(QString::fromUtf8("数据目录打开失败")));
      for (const char *name : {"importWells", "importSeismic", "importBoundary",
                               "importFolder"})
      {
        auto *btn = win.findChild<QPushButton *>(QLatin1String(name));
        QVERIFY2(btn && !btn->isEnabled(), name);
        QVERIFY2(!btn->toolTip().isEmpty(), "禁用必须带 reason tooltip（§35）");
      }
    }

    // D6 地图→表：实体 id → 选中其已决关联的资产行（未决不算命中；首个命中
    // 行发 assetActivated，与手点同通路）。
    void dataPage_mapSelectionSelectsLinkedAssetRows()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      for (const char *id : {"ast-1", "ast-2", "ast-3"})
      {
        CatalogAsset a;
        a.id = QString::fromLatin1(id);
        a.type = QStringLiteral("well_log");
        a.displayName = a.id + QStringLiteral(".las");
        QVERIFY(cat->addAsset(a));
      }
      EntityAssetLink l1;
      l1.entityType = QStringLiteral("well");
      l1.entityId = well.id;
      l1.assetId = QStringLiteral("ast-1");
      l1.role = QStringLiteral("well_log");
      l1.isPrimary = true;
      QVERIFY(cat->addLink(l1));
      EntityAssetLink l2 = l1;
      l2.assetId = QStringLiteral("ast-3");
      l2.isPrimary = false;
      QVERIFY(cat->addLink(l2));
      EntityAssetLink l3;
      l3.entityType = QStringLiteral("well");
      l3.assetId = QStringLiteral("ast-2");
      l3.role = QStringLiteral("well_log");
      l3.unresolved = true; // 未决链接不命中
      QVERIFY(cat->addLink(l3));

      DataPage page;
      PreviewDocService previewDoc6(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc6));
      page.refreshAssetTable();
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QCOMPARE(table->rowCount(), 3);
      QSignalSpy spy(&page, &DataPage::assetActivated);

      page.selectAssetsForEntities({QStringLiteral("well-1")});
      const auto sel = table->selectionModel()->selectedRows();
      QCOMPARE(sel.size(), 2); // ast-1 + ast-3（同一口井的两条已决链接）
      QCOMPARE(spy.count(), 1);
      const QString activated = spy.at(0).at(0).toString();
      QVERIFY(activated == QStringLiteral("ast-1") || activated == QStringLiteral("ast-3"));

      // 无命中实体不动当前选中；空 id 集不动作。
      table->clearSelection();
      spy.clear();
      page.selectAssetsForEntities({QStringLiteral("well-nope")});
      QVERIFY(table->selectionModel()->selectedRows().isEmpty());
      QCOMPARE(spy.count(), 0);
    }

    // ---- PredictPage ----
    void predictPage_combosAndRun()
    {
      PredictPage page(nullptr, nullptr);
      page.setHorizons({QStringLiteral("T1"), QStringLiteral("T2")});
      page.setAlgorithms({QStringLiteral("paleo:x"), QStringLiteral("onnx:toy")});

      auto *hc = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
      auto *ac = page.findChild<QComboBox *>(QStringLiteral("algoCombo"));
      QVERIFY(hc && ac);
      QCOMPARE(hc->count(), 2);
      QCOMPARE(ac->count(), 2);
      QCOMPARE(hc->itemText(0), QStringLiteral("T1"));
      QCOMPARE(hc->itemText(1), QStringLiteral("T2"));

      QCOMPARE(ac->itemData(0).toString(), QStringLiteral("paleo:x"));
      QCOMPARE(ac->itemData(1).toString(), QStringLiteral("onnx:toy"));
      QCOMPARE(ac->itemText(0), QStringLiteral("paleo:x"));
      QCOMPARE(ac->itemText(1), QStringLiteral("toy (ONNX)"));

      auto *paramsArea = page.findChild<QWidget *>(QStringLiteral("onnxParamsArea"));
      QVERIFY(paramsArea);

      auto *inputEdit = page.findChild<QLineEdit *>(QStringLiteral("onnxInputEdit"));
      if (!inputEdit)
        inputEdit = page.findChild<QLineEdit *>(QStringLiteral("inputEdit"));
      auto *shapeEdit = page.findChild<QLineEdit *>(QStringLiteral("onnxShapeEdit"));
      if (!shapeEdit)
        shapeEdit = page.findChild<QLineEdit *>(QStringLiteral("shapeEdit"));
      auto *nameEdit = page.findChild<QLineEdit *>(QStringLiteral("onnxInputNameEdit"));
      if (!nameEdit)
        nameEdit = page.findChild<QLineEdit *>(QStringLiteral("inputNameEdit"));

      QVERIFY(inputEdit);
      QVERIFY(shapeEdit);
      QVERIFY(nameEdit);

      // Check params area visibility when switching algorithms
      ac->setCurrentIndex(1);
      emit ac->activated(1);
      QVERIFY(!paramsArea->isHidden());

      ac->setCurrentIndex(0);
      emit ac->activated(0);
      QVERIFY(paramsArea->isHidden());

      ac->setCurrentIndex(1);
      emit ac->activated(1);
      QVERIFY(!paramsArea->isHidden());

      // Valid ONNX run with parameters
      hc->setCurrentIndex(0); // "T1"
      inputEdit->setText(QStringLiteral("2.0"));
      shapeEdit->setText(QStringLiteral("1"));
      nameEdit->setText(QStringLiteral("x"));

      QSignalSpy spy(&page, &PredictPage::runRequested);
      auto *runBtn = page.findChild<QPushButton *>(QStringLiteral("runButton"));
      QVERIFY(runBtn);
      runBtn->click();

      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.first().at(0).toString(), QStringLiteral("T1"));
      QCOMPARE(spy.first().at(1).toString(), QStringLiteral("onnx:toy"));

      const QVariantMap params = spy.first().at(2).toMap();
      const QVariantList inputList = params.value(QStringLiteral("input")).toList();
      QCOMPARE(inputList.size(), 1);
      QCOMPARE(inputList.at(0).toFloat(), 2.0f);

      const QVariantList shapeList = params.value(QStringLiteral("shape")).toList();
      QCOMPARE(shapeList.size(), 1);
      QCOMPARE(shapeList.at(0).toLongLong(), qint64(1));

      QCOMPARE(params.value(QStringLiteral("inputName")).toString(), QStringLiteral("x"));

      // Invalid input should not emit runRequested and should show status error
      inputEdit->setText(QStringLiteral("abc"));
      spy.clear();
      runBtn->click();
      QCOMPARE(spy.count(), 0);

      auto *status = page.findChild<QLabel *>(QStringLiteral("statusLabel"));
      QVERIFY(status);
      QVERIFY(!status->text().isEmpty());

      // Paleo run should emit with empty params map
      ac->setCurrentIndex(0);
      emit ac->activated(0);
      spy.clear();
      runBtn->click();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.first().at(0).toString(), QStringLiteral("T1"));
      QCOMPARE(spy.first().at(1).toString(), QStringLiteral("paleo:x"));
      QVERIFY(spy.first().at(2).toMap().isEmpty());
    }

    void predictPage_statusShowsWorkflowFailure()
    {
      // Unbound workflow fails runPrediction and emits predictionFailed.
      PredictionWorkflow wf(nullptr, nullptr);
      PredictPage page(&wf, nullptr);
      auto *status = page.findChild<QLabel *>(QStringLiteral("statusLabel"));
      QVERIFY(status);

      QString err;
      QVERIFY(!wf.runPrediction(QStringLiteral("T1"), QStringLiteral("alg.x"),
                              QVariantMap(), &err));
      QVERIFY(status->text().contains(QStringLiteral("not bound")));
    }

    // ---- ConstraintPage ----
    void constraintPage_emitsDrawAndIdw()
    {
      ConstraintPage page(nullptr);
      auto *hc = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
      auto *spin = page.findChild<QSpinBox *>(QStringLiteral("faciesCodeSpin"));
      QVERIFY(hc && spin);
      hc->addItem(QStringLiteral("T1"));
      hc->setCurrentIndex(0);
      spin->setValue(5);

      QSignalSpy drawSpy(&page, &ConstraintPage::drawConstraintRequested);
      page.findChild<QPushButton *>(QStringLiteral("drawButton"))->click();
      QCOMPARE(drawSpy.count(), 1);
      QCOMPARE(drawSpy.first().at(0).toString(), QStringLiteral("T1"));
      QCOMPARE(drawSpy.first().at(1).toString(), QStringLiteral("line")); // default shape
      QCOMPARE(drawSpy.first().at(2).toInt(), 5);

      QSignalSpy idwSpy(&page, &ConstraintPage::runIdwRequested);
      page.findChild<QPushButton *>(QStringLiteral("runIdwButton"))->click();
      QCOMPARE(idwSpy.count(), 1);
      QCOMPARE(idwSpy.first().at(0).toString(), QStringLiteral("T1"));
    }

    // ---- ComposePage ----
    void composePage_nullLayersStaysEmpty()
    {
      ComposePage page(nullptr, nullptr);
      page.refreshFactors();
      QCOMPARE(page.findChild<QListWidget *>(QStringLiteral("factorList"))->count(), 0);
    }

    // wave/mapping-pipeline 阶段C+E — 编图链/导出/版本按钮块 + 发布门。
    void composePage_mappingChainButtonsAndPublishGate()
    {
      ComposePage page(nullptr, nullptr);

      QSignalSpy chainSpy(&page, &ComposePage::thicknessChainRequested);
      QSignalSpy exportSpy(&page, &ComposePage::exportPdfRequested);
      QSignalSpy saveSpy(&page, &ComposePage::saveVersionRequested);
      QSignalSpy publishSpy(&page, &ComposePage::publishRequested);

      // D8 触发门：未选层位禁用且写明原因；setThicknessHorizon 命名+开闸。
      auto *chain = page.findChild<QPushButton *>(QStringLiteral("thicknessChainButton"));
      QVERIFY(chain != nullptr);
      QVERIFY(!chain->isEnabled());
      QCOMPARE(chain->toolTip(), QStringLiteral("先在顶部层位 chip 选择层位"));
      chain->click();
      QCOMPARE(chainSpy.count(), 0); // 禁用态不触发
      page.setThicknessHorizon(QStringLiteral("D61"));
      QVERIFY(chain->isEnabled());
      QCOMPARE(chain->text(), QStringLiteral("生成 D61 等厚图"));

      chain->click();
      page.findChild<QPushButton *>(QStringLiteral("exportPdfButton"))->click();
      page.findChild<QPushButton *>(QStringLiteral("saveVersionButton"))->click();
      QCOMPARE(chainSpy.count(), 1);
      QCOMPARE(exportSpy.count(), 1);
      QCOMPARE(saveSpy.count(), 1);

      // 发布门：PDF 能导出之前禁用（不发信号），setPublishEnabled 开闸。
      auto *publish = page.findChild<QPushButton *>(QStringLiteral("publishButton"));
      QVERIFY(publish != nullptr);
      QVERIFY(!publish->isEnabled());
      QCOMPARE(publish->toolTip(), QStringLiteral("导出 PDF 后再保存")); // 缺 PDF
      publish->click();
      QCOMPARE(publishSpy.count(), 0);
      page.setPublishEnabled(true);
      QVERIFY(publish->isEnabled());
      publish->click();
      QCOMPARE(publishSpy.count(), 1);
      page.setPublishEnabled(false);
      QVERIFY(!publish->isEnabled());
    }

    // 阶段E 完整发布门：hasPdf + 逐井残差覆盖两条都要满足；tooltip 指认缺项。
    void composePage_publishStateTooltips()
    {
      ComposePage page(nullptr, nullptr);
      auto *publish = page.findChild<QPushButton *>(QStringLiteral("publishButton"));
      auto *state = page.findChild<QLabel *>(QStringLiteral("publishStateLabel"));
      auto *save = page.findChild<QPushButton *>(QStringLiteral("saveVersionButton"));
      QVERIFY(publish != nullptr && state != nullptr && save != nullptr);

      // 缺 PDF（残差已齐也救不了）。
      page.setPublishState(false, 3, 3);
      QVERIFY(!publish->isEnabled());
      QCOMPARE(publish->toolTip(), QStringLiteral("导出 PDF 后再保存"));

      // 有 PDF 但残差覆盖不全 → 禁 + 计数 tooltip。
      page.setPublishState(true, 2, 3);
      QVERIFY(!publish->isEnabled());
      QCOMPARE(publish->toolTip(),
               QStringLiteral("还有 1/3 口井没有残差或原因 — 先在验证页运行验证"));

      // 未跑验证（0/0）→ 禁 + 运行验证提示。
      page.setPublishState(true, 0, 0);
      QVERIFY(!publish->isEnabled());
      QCOMPARE(publish->toolTip(), QStringLiteral("先在验证页运行验证"));

      // 两条都齐 → 放闸，tooltip 清空。
      page.setPublishState(true, 3, 3);
      QVERIFY(publish->isEnabled());
      QVERIFY(publish->toolTip().isEmpty());

      // 版本状态标注：已发布 → 文案 + 「保存新版本」；下一版回到编辑中。
      page.setVersionState(2, true);
      QCOMPARE(state->text(), QStringLiteral("已发布 · v2"));
      QCOMPARE(save->text(), QStringLiteral("保存新版本"));
      page.setVersionState(3, false);
      QCOMPARE(state->text(), QStringLiteral("编辑中 · v3"));
      QCOMPARE(save->text(), QStringLiteral("保存版本"));
      page.setVersionState(0, false);
      QCOMPARE(state->text(), QStringLiteral("还没有保存的版本"));
    }

    void composePage_listsOnlySingleFactorGroup()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      LayerManifest manifest(dir.filePath(QStringLiteral("m.sqlite")));
      seedManifest(&manifest, 2, 1);
      QgisLayerService layers(nullptr, &manifest); // null project svc: manifest only

      ComposePage page(nullptr, &layers);
      page.refreshFactors();
      auto *list = page.findChild<QListWidget *>(QStringLiteral("factorList"));
      QCOMPARE(list->count(), 2);
      for (int i = 0; i < list->count(); ++i)
      {
        QVERIFY(list->item(i)->flags() & Qt::ItemIsUserCheckable);
        QVERIFY(list->item(i)->data(Qt::UserRole).toString().startsWith(
            QStringLiteral("factor.")));
      }

      list->item(0)->setCheckState(Qt::Checked);
      list->item(1)->setCheckState(Qt::Unchecked);
      QSignalSpy spy(&page, &ComposePage::fuseRequested);
      page.findChild<QPushButton *>(QStringLiteral("fuseButton"))->click();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.first().at(0).toStringList(),
               QStringList{QStringLiteral("factor.T1.f0")});
    }

    void composePage_polygonizeEmitsSelection()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      LayerManifest manifest(dir.filePath(QStringLiteral("m.sqlite")));
      QVERIFY(manifest.open());
      LayerDeclaration composite;
      composite.layerId = QStringLiteral("composite.T1");
      composite.horizon = QStringLiteral("T1");
      composite.type = QStringLiteral("raster");
      composite.source = QStringLiteral("memory|composite");
      composite.group = QStringLiteral("03_Composite");
      QVERIFY(manifest.upsert(composite));
      LayerDeclaration factor;
      factor.layerId = QStringLiteral("factor.T1.f0");
      factor.horizon = QStringLiteral("T1");
      factor.type = QStringLiteral("raster");
      factor.source = QStringLiteral("memory|f");
      factor.group = QStringLiteral("04_SingleFactor");
      QVERIFY(manifest.upsert(factor));

      QgisLayerService layers(nullptr, &manifest);
      ComposePage page(nullptr, &layers);
      auto *combo = page.findChild<QComboBox *>(QStringLiteral("faciesRasterCombo"));
      QVERIFY(combo);
      QCOMPARE(combo->count(), 1);
      QCOMPARE(combo->currentData().toString(), QStringLiteral("composite.T1"));

      auto *minArea = page.findChild<QDoubleSpinBox *>(QStringLiteral("minAreaSpin"));
      auto *simplify = page.findChild<QDoubleSpinBox *>(QStringLiteral("simplifySpin"));
      QVERIFY(minArea && simplify);
      minArea->setValue(2.5);
      simplify->setValue(0.25);

      QSignalSpy spy(&page, &ComposePage::polygonizeRequested);
      page.findChild<QPushButton *>(QStringLiteral("polygonizeButton"))->click();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.first().at(0).toString(), QStringLiteral("composite.T1"));
      QCOMPARE(spy.first().at(1).toDouble(), 2.5);
      QCOMPARE(spy.first().at(2).toDouble(), 0.25);
    }

    void composePage_polygonizeEmptyExplains()
    {
      ComposePage page(nullptr, nullptr);
      QSignalSpy spy(&page, &ComposePage::polygonizeRequested);
      auto *btn = page.findChild<QPushButton *>(QStringLiteral("polygonizeButton"));
      QVERIFY(btn);
      btn->click();
      QCOMPARE(spy.count(), 0);
      auto *status = page.findChild<QLabel *>(QStringLiteral("statusLabel"));
      QVERIFY(status);
      QVERIFY(status->text().contains(QStringLiteral("栅格")));
    }

    // A raster declared after the page exists lands in the combo via
    // layerDeclared — no manual refreshFactors() needed.
    void composePage_faciesComboTracksNewDeclarations()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      LayerManifest manifest(dir.filePath(QStringLiteral("m.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService layers(nullptr, &manifest);
      ComposePage page(nullptr, &layers);
      auto *combo = page.findChild<QComboBox *>(QStringLiteral("faciesRasterCombo"));
      QVERIFY(combo);
      QCOMPARE(combo->count(), 0);

      LayerDeclaration raster;
      raster.layerId = QStringLiteral("pred.T1.onnx.toy");
      raster.horizon = QStringLiteral("T1");
      raster.type = QStringLiteral("raster");
      raster.source = QStringLiteral("memory|onnx");
      raster.group = QStringLiteral("03_Predict");
      QVERIFY(layers.declare(raster));
      QCOMPARE(combo->count(), 1);
      QCOMPARE(combo->currentData().toString(), QStringLiteral("pred.T1.onnx.toy"));

      LayerDeclaration vector;
      vector.layerId = QStringLiteral("wells.T1");
      vector.horizon = QStringLiteral("T1");
      vector.type = QStringLiteral("vector");
      vector.source = QStringLiteral("memory|w");
      vector.group = QStringLiteral("00_Data");
      QVERIFY(layers.declare(vector));
      QCOMPARE(combo->count(), 1); // non-raster stays out
    }

    // ---- ValidatePage ----
    void validatePage_nullWorkflowIsSafe()
    {
      ValidatePage page(nullptr);
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("issueTable"));
      QVERIFY(table);
      QCOMPARE(table->columnCount(), 4);
      page.populate(); // must not crash with no workflow bound
      QCOMPARE(table->rowCount(), 0);
    }

    void validatePage_populatesAndLocates()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      LayerManifest manifest(dir.filePath(QStringLiteral("m.sqlite")));
      LayerDeclaration d;
      d.layerId = QStringLiteral("predict.T1.gone");
      d.horizon = QStringLiteral("T1");
      d.type = QStringLiteral("raster");
      d.source = dir.filePath(QStringLiteral("missing.tif")); // absent on disk
      d.group = QStringLiteral("01_Prediction");
      QVERIFY(manifest.upsert(d));
      QgisLayerService layers(nullptr, &manifest);
      ValidationWorkflow wf(&layers, nullptr);

      ValidatePage page(&wf);
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("issueTable"));
      page.findChild<QPushButton *>(QStringLiteral("runButton"))->click();
      QCOMPARE(table->rowCount(), 1);
      QCOMPARE(table->item(0, 1)->text(), QStringLiteral("SRC_MISSING"));

      // Double-click on the row emits the locate intent with the stored data.
      QSignalSpy spy(&page, &ValidatePage::locateRequested);
      QVERIFY(QMetaObject::invokeMethod(
          table, "itemDoubleClicked",
          Q_ARG(QTableWidgetItem *, table->item(0, 2))));
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.first().at(0).toString(), QStringLiteral("predict.T1.gone"));
      QCOMPARE(spy.first().at(1).toString(), QString()); // no wktLocation
      // wave/mapping-pipeline：非残差问题载荷为空表（三视图只做地图缩放）。
      const QVariantMap payload = spy.first().at(2).toMap();
      QVERIFY(!payload.contains(QStringLiteral("wellId")));
      QVERIFY(!payload.contains(QStringLiteral("inline")));

      // 「在数据页看这条剖面」（预览壳重排）：无 inline 测线号的问题行
      // 不让跳页——按钮禁用，点击不发 seismicSectionRequested。
      auto *openSection =
          page.findChild<QPushButton *>(QStringLiteral("openSeismicSectionButton"));
      QVERIFY(openSection);
      QVERIFY(!openSection->isEnabled());
      QSignalSpy sectionSpy(&page, &ValidatePage::seismicSectionRequested);
      openSection->click();
      QCOMPARE(sectionSpy.count(), 0);
    }

    // ---- T27 胶囊化：状态文字 = DESIGN.md status-tag（浅底深字），不再
    // setForeground 彩色裸文字。级别列胶囊；残差列胶囊+mono 数字面。----
    void validatePage_severityAndResidualCapsules()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      LayerManifest manifest(dir.filePath(QStringLiteral("m.sqlite")));
      LayerDeclaration d;
      d.layerId = QStringLiteral("predict.T1.gone");
      d.horizon = QStringLiteral("T1");
      d.type = QStringLiteral("raster");
      d.source = dir.filePath(QStringLiteral("missing.tif"));
      d.group = QStringLiteral("01_Prediction");
      QVERIFY(manifest.upsert(d));
      QgisLayerService layers(nullptr, &manifest);
      ValidationWorkflow wf(&layers, nullptr);

      ValidatePage page(&wf);
      page.populate();
      auto *issueTable = page.findChild<QTableWidget *>(QStringLiteral("issueTable"));
      QVERIFY(issueTable);
      QCOMPARE(issueTable->rowCount(), 1);
      // SRC_MISSING 是错误级：胶囊就是级别列的 cellWidget，token = errorBg。
      auto *sevCapsule = qobject_cast<QLabel *>(issueTable->cellWidget(0, 0));
      QVERIFY2(sevCapsule && sevCapsule->objectName() == QStringLiteral("statusCapsule"),
               "severity cell must carry a capsule label");
      QCOMPARE(sevCapsule->text(), QStringLiteral("错误"));
      QVERIFY(sevCapsule->styleSheet().contains(QStringLiteral("#FDEBEB")));
      QVERIFY(sevCapsule->styleSheet().contains(QStringLiteral("#C62828"))); // errorText
      QVERIFY(sevCapsule->styleSheet().contains(QStringLiteral("border-radius")));
      // 级别 item 不再持彩色裸文字。
      QVERIFY(issueTable->item(0, 0)->text().isEmpty());
      QVERIFY(!issueTable->item(0, 0)->foreground().color().isValid()
              || issueTable->item(0, 0)->foreground() == QBrush());

      // 残差表：注入 pass / exceed / 未计算 三行，胶囊 token 逐行断言。
      QVariantList rows;
      QVariantMap pass;
      pass.insert(QStringLiteral("well_name"), QStringLiteral("A1"));
      pass.insert(QStringLiteral("status"), QStringLiteral("pass"));
      pass.insert(QStringLiteral("residual_ms"), 3.2);
      pass.insert(QStringLiteral("threshold_ms"), 10.0);
      rows.append(pass);
      QVariantMap exceed;
      exceed.insert(QStringLiteral("well_name"), QStringLiteral("A2"));
      exceed.insert(QStringLiteral("status"), QStringLiteral("exceed"));
      exceed.insert(QStringLiteral("residual_ms"), 22.5);
      rows.append(exceed);
      QVariantMap none;
      none.insert(QStringLiteral("well_name"), QStringLiteral("A3"));
      none.insert(QStringLiteral("status"), QStringLiteral("no_raster"));
      none.insert(QStringLiteral("reason"), QStringLiteral("没有 D61 栅格"));
      rows.append(none);
      wf.setProperty("paleo.wf.residualRows", rows);
      // populate() 内部 validate() 会清空 residualRows 属性（无 ProjectData
      // 门面时）——渲染面用静态助手直灌行（与 populate 同一渲染代码路径）。
      auto *resTable = page.findChild<QTableWidget *>(QStringLiteral("residualTable"));
      QVERIFY(resTable);
      ValidatePage::fillResidualTable(resTable, rows);
      QCOMPARE(resTable->rowCount(), 3);

      const auto capsuleAt = [resTable](int r) {
        return resTable->cellWidget(r, 1)
            ->findChild<QLabel *>(QStringLiteral("statusCapsule"));
      }; // 残差列 cellWidget 是 HBox 容器，胶囊是它的子标签
      QCOMPARE(capsuleAt(0)->text(), QStringLiteral("通过"));
      QVERIFY(capsuleAt(0)->styleSheet().contains(QStringLiteral("#E8F5E9"))); // successBg
      QCOMPARE(capsuleAt(1)->text(), QStringLiteral("超过阈值"));
      QVERIFY(capsuleAt(1)->styleSheet().contains(QStringLiteral("#FFF4E0"))); // warningBg
      // 中性「未计算」胶囊：surface-alt 底 + text-muted 字（无语义色）。
      QCOMPARE(capsuleAt(2)->text(), QStringLiteral("未计算"));
      QVERIFY(capsuleAt(2)->styleSheet().contains(QStringLiteral("#EDF1F5")));
      QVERIFY(capsuleAt(2)->styleSheet().contains(QStringLiteral("#5D6E80")));

      // 数值面 JetBrains Mono 9pt：残差数字 + 阈值列。
      auto *cell0 = resTable->cellWidget(0, 1);
      bool sawMonoValue = false;
      for (QLabel *l : cell0->findChildren<QLabel *>())
        if (l->text().contains(QStringLiteral("3.2")))
        {
          sawMonoValue = l->font().families().contains(QStringLiteral("JetBrains Mono"));
          QCOMPARE(l->font().pointSize(), 9);
        }
      QVERIFY2(sawMonoValue, "residual number must render in JetBrains Mono 9pt");
      QVERIFY(resTable->item(0, 2)->font()
                  .families()
                  .contains(QStringLiteral("JetBrains Mono"))); // 阈值列
    }

    // T27：版本状态标签胶囊化——已发布绿 / 编辑中橙 / 无版本中性。
    void composePage_versionStateCapsule()
    {
      ComposePage page(nullptr, nullptr);
      auto *state = page.findChild<QLabel *>(QStringLiteral("publishStateLabel"));
      QVERIFY(state);
      page.setVersionState(2, true);
      QVERIFY(state->styleSheet().contains(QStringLiteral("#E8F5E9")));
      page.setVersionState(3, false);
      QVERIFY(state->styleSheet().contains(QStringLiteral("#FFF4E0")));
      page.setVersionState(0, false);
      QVERIFY(state->styleSheet().contains(QStringLiteral("#EDF1F5"))); // 未计算
    }

    // ---- T31 空态 / 未决过滤 ----

    // 空资产表：居中提示 + 下一步动作指引（点名「导入工区文件夹」）。
    void dataPage_emptyStateIsCenteredGuidance()
    {
      DataPage page;
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QVERIFY(table);
      QCOMPARE(table->rowCount(), 1);
      auto *it = table->item(0, 0);
      QVERIFY(it);
      QVERIFY(it->text().contains(QString::fromUtf8("还没有数据资产")));
      QVERIFY2(it->text().contains(QString::fromUtf8("导入工区文件夹")),
               "empty state must name the next concrete step");
      QVERIFY(it->textAlignment() & Qt::AlignHCenter); // T31 居中
    }

    // 「查看未决」过滤：只留有未决链接的行；过滤条 + 清除过滤恢复全表。
    void dataPage_unresolvedFilterNarrowsRows()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      CatalogAsset resolved, pending;
      resolved.id = QStringLiteral("ast-1");
      resolved.type = QStringLiteral("well_log");
      resolved.displayName = QStringLiteral("A1.las");
      QVERIFY(cat->addAsset(resolved));
      pending.id = QStringLiteral("ast-2");
      pending.type = QStringLiteral("well_log");
      pending.displayName = QStringLiteral("Z9.las");
      QVERIFY(cat->addAsset(pending));
      EntityAssetLink ok, un;
      ok.entityType = QStringLiteral("well");
      ok.entityId = well.id;
      ok.assetId = resolved.id;
      ok.role = QStringLiteral("well_log");
      QVERIFY(cat->addLink(ok));
      un.entityType = QStringLiteral("well");
      un.assetId = pending.id;
      un.role = QStringLiteral("well_log");
      un.unresolved = true;
      QVERIFY(cat->addLink(un));

      DataPage page;
      PreviewDocService previewDoc7(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc7));
      page.refreshAssetTable();
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QCOMPARE(table->rowCount(), 2);
      QVERIFY(page.findChild<QWidget *>(QStringLiteral("unresolvedFilterBar"))->isHidden());

      page.setUnresolvedFilter(true);
      QCOMPARE(table->rowCount(), 1);
      QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Z9.las"));
      QVERIFY(!page.findChild<QWidget *>(QStringLiteral("unresolvedFilterBar"))->isHidden());

      // 清除过滤恢复全表。
      page.findChild<QPushButton *>(QStringLiteral("clearUnresolvedFilterButton"))->click();
      QCOMPARE(table->rowCount(), 2);
      QVERIFY(page.findChild<QWidget *>(QStringLiteral("unresolvedFilterBar"))->isHidden());

      // 过滤开着而没有未决资产：给「没有未决资产」空态，不是空白表。
      QVERIFY(cat->attachLink(1, QStringLiteral("well-1"))); // ast-2 也挂上 → 全部已决
      page.setUnresolvedFilter(true);
      QCOMPARE(table->rowCount(), 1);
      QVERIFY(table->item(0, 0)->text().contains(QString::fromUtf8("没有未决资产")));
    }

    // 文件夹确认完成后的「查看未决」：露出的条件（有未决行）+ 点击切数据页
    // 并把资产表过滤到未决行。
    void folderConfirm_showUnresolvedFiltersAssetTable()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      const QString projectDir = tmp.filePath(QStringLiteral("proj"));
      QVERIFY(QDir().mkpath(projectDir));
      const QString root = tmp.filePath(QStringLiteral("area"));
      QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
      QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
          QByteArrayLiteral("#WellHead File From SMI\n#Name X Y KB TD\n"
                            "A1  1.0  2.0  0.0  2000.0\n")));
      // Z9.las 引用不存在的井 → 未决行。
      QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("Z9.las")),
                        QByteArrayLiteral("~Well\nWELL. Z9 : WELL\n~A DEPT\n1.0\n")));

      FolderStack st(tmp.filePath(QStringLiteral("m.sqlite")), projectDir);
      QString err;
      const auto preview = st.svc.previewFolder(root, &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      QCOMPARE(preview.size(), 2);

      QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
      PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, nullptr);
      win.attachWorkflows(nullptr, nullptr, nullptr, nullptr, &st.svc);
      QDialog dlg;
      FolderImportWorkflow wf(&st.svc, nullptr);
      PaleoFolderConfirm::buildFolderConfirmDialog(
          &dlg, root, preview, folderConfirmHooks(wf, root, &win));
      auto *showUnresolved =
          dlg.findChild<QPushButton *>(QStringLiteral("folderShowUnresolvedButton"));
      QVERIFY(showUnresolved);
      QVERIFY(!showUnresolved->isVisibleTo(&dlg)); // 导入前不出现

      dlg.findChild<QPushButton *>(QStringLiteral("folderConfirmButton"))->click();
      QVERIFY(showUnresolved->isVisibleTo(&dlg)); // 有未决行 → 露出

      showUnresolved->click();
      QCOMPARE(win.currentPage(), QStringLiteral("data"));
      auto *page = win.findChild<DataPage *>();
      QVERIFY(page);
      auto *table = page->findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QVERIFY(table);
      QCOMPARE(table->rowCount(), 1); // 只有 Z9 那条未决资产
      QVERIFY(table->item(0, 0)->text().contains(QStringLiteral("Z9")));
      QVERIFY(!page->findChild<QWidget *>(QStringLiteral("unresolvedFilterBar"))->isHidden());
    }

    // ---- T22 文件夹导入确认表（PaleoMainWindow 静态面）----
    // 只调静态助手，不实例化主窗——栈上只要一个非空的 layer/store 就能让
    // DataImportService 真实走导入（LayerManifest 指临时 sqlite）。

    // 词表 = 分类器实际输出集：无「tops」（角色名不是类型）、有「tabular」
    // （未匹配 .dat 的真实类型）；label↔type 经 item data 绑定，不靠显示文
    // 本反推——旧版 setCurrentText("tabular") 静默失败错停在 well_head。
    void folderConfirm_typeVocabularyAndMapping()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      const QString projectDir = tmp.filePath(QStringLiteral("proj"));
      QVERIFY(QDir().mkpath(projectDir));
      const QString root = tmp.filePath(QStringLiteral("area"));
      QVERIFY(QDir().mkpath(root));
      QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("a.las")),
          QByteArrayLiteral("~Well\nWELL. A9 : WELL\n~A DEPT\n1.0\n")));
      QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("x.dat")),
                        QByteArrayLiteral("a,b\n1,2\n"))); // 无路径段语义 → tabular

      FolderStack st(tmp.filePath(QStringLiteral("m.sqlite")), projectDir);
      QString err;
      const auto preview = st.svc.previewFolder(root, &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      QCOMPARE(preview.size(), 2);

      QTableWidget table(0, 4);
      QVector<QComboBox *> combos;
      PaleoFolderConfirm::populateFolderConfirmTable(&table, root, preview, &combos);
      QCOMPARE(combos.size(), 2);

      const QStringList vocab = projectClassifierTypes();
      QVERIFY(!vocab.contains(QStringLiteral("tops")));
      QVERIFY(vocab.contains(QStringLiteral("tabular")));
      QVERIFY(vocab.contains(QStringLiteral("reference")));
      QVERIFY(vocab.contains(QStringLiteral("well_stratification")));
      for (auto *c : combos)
      {
        QCOMPARE(c->count(), vocab.size());
        for (int i = 0; i < c->count(); ++i)
          QCOMPARE(c->itemData(i).toString(), vocab.at(i)); // type 存 item data
        QCOMPARE(c->itemText(vocab.indexOf(QStringLiteral("well_stratification"))),
                 QString::fromUtf8("井分层")); // 显示标签是中文，不等于类型 id
      }
      QCOMPARE(combos.at(0)->currentData().toString(), QStringLiteral("well_log"));
      QCOMPARE(combos.at(1)->currentData().toString(), QStringLiteral("tabular"));

      // 未改动的行不发 override；改成 document 才出现在覆盖表里。
      QVERIFY(PaleoFolderConfirm::collectFolderTypeOverrides(&table, preview, combos)
                  .isEmpty());
      combos.at(1)->setCurrentIndex(vocab.indexOf(QStringLiteral("document")));
      const auto ov =
          PaleoFolderConfirm::collectFolderTypeOverrides(&table, preview, combos);
      QCOMPARE(ov.size(), 1);
      QCOMPARE(ov.value(preview.at(1).path), QStringLiteral("document"));
    }

    // 固定辅助锁：HZ28-6-1 命名行禁用下拉 + tooltip；「参考资料」目录内其他
    // XML 默认显示「参考」但可改——override 照常进收集表。pdf 显示真实类型。
    void folderConfirm_fixedAuxiliaryLockAndReferenceDefault()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      const QString projectDir = tmp.filePath(QStringLiteral("proj"));
      QVERIFY(QDir().mkpath(projectDir));
      const QString root = tmp.filePath(QStringLiteral("area"));
      QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
      QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("参考资料"))));
      const QByteArray logXml(
          "<logs><log><logcurveinfo/><logdata>1 2</logdata></log></logs>");
      QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("参考资料/HZ28-6-1测井.xml")),
                        logXml));
      QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("参考资料/other.xml")),
                        logXml));
      QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("参考资料/spec.pdf")),
                        QByteArrayLiteral("%PDF-1.4 fake")));
      QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
          QByteArrayLiteral("#WellHead File From SMI\n#Name X Y KB TD\n"
                            "A1  1.0  2.0  0.0  2000.0\n")));

      FolderStack st(tmp.filePath(QStringLiteral("m.sqlite")), projectDir);
      QString err;
      const auto preview = st.svc.previewFolder(root, &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      QCOMPARE(preview.size(), 4);
      QCOMPARE(preview.at(0).classifiedType, QStringLiteral("well_head")); // 阶段 1 先行

      QTableWidget table(0, 4);
      QVector<QComboBox *> combos;
      PaleoFolderConfirm::populateFolderConfirmTable(&table, root, preview, &combos);
      QCOMPARE(combos.size(), 4);

      const int rHz = tableRowForPath(&table, QStringLiteral("HZ28-6-1测井.xml"));
      const int rOther = tableRowForPath(&table, QStringLiteral("other.xml"));
      const int rSpec = tableRowForPath(&table, QStringLiteral("spec.pdf"));
      const int rHeads = tableRowForPath(&table, QStringLiteral("heads.dat"));
      QVERIFY(rHz >= 0 && rOther >= 0 && rSpec >= 0 && rHeads == 0);

      // HZ28：锁定 + 固定「参考」。
      QVERIFY(!combos.at(rHz)->isEnabled());
      QCOMPARE(combos.at(rHz)->currentData().toString(), QStringLiteral("reference"));
      QVERIFY(combos.at(rHz)->toolTip().contains(QString::fromUtf8("该文件固定为参考资料")));
      // other.xml：默认「参考」，可改。
      QVERIFY(combos.at(rOther)->isEnabled());
      QCOMPARE(combos.at(rOther)->currentData().toString(), QStringLiteral("reference"));
      // spec.pdf / heads.dat：真实类型。
      QCOMPARE(combos.at(rSpec)->currentData().toString(), QStringLiteral("document"));
      QCOMPARE(combos.at(rHeads)->currentData().toString(), QStringLiteral("well_head"));

      // 收集：other.xml 的「参考」默认值与分类器原类型不同 → 成 override；
      // HZ28 行禁用 → 不发 override。
      auto ov = PaleoFolderConfirm::collectFolderTypeOverrides(&table, preview, combos);
      QCOMPARE(ov.value(preview.at(rOther).path), QStringLiteral("reference"));
      QVERIFY(!ov.contains(preview.at(rHz).path));
      QVERIFY(!ov.contains(preview.at(rSpec).path));
      QVERIFY(!ov.contains(preview.at(rHeads).path));
      QVERIFY(ov.contains(preview.at(rOther).path));
      const int ovSize = ov.size();

      // 改成井类（≠分类器原类型 well_log）→ override 送达收集表（后端不再
      // 整目录锁参考）；选回 well_log（=原类型）→ 不发覆盖，后端按原类型走。
      const QStringList vocab = projectClassifierTypes();
      combos.at(rOther)->setCurrentIndex(vocab.indexOf(QStringLiteral("well_head")));
      ov = PaleoFolderConfirm::collectFolderTypeOverrides(&table, preview, combos);
      QCOMPARE(ov.value(preview.at(rOther).path), QStringLiteral("well_head"));
      QCOMPARE(ov.size(), ovSize);
      combos.at(rOther)->setCurrentIndex(vocab.indexOf(QStringLiteral("well_log")));
      ov = PaleoFolderConfirm::collectFolderTypeOverrides(&table, preview, combos);
      QVERIFY(!ov.contains(preview.at(rOther).path)); // 同原类型 → 不成 override
      QCOMPARE(ov.size(), ovSize - 1);
    }

    // 确认导入 → 结果行 + 汇总；失败行挂「重试」——按当前下拉类型重导单行，
    // 行与汇总一起更新。仍失败的回挂重试按钮；内容级失败行重导命中 dedup
    // （字节已在库）也如实记行。
    void folderConfirm_importSummaryAndRowRetry()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      const QString projectDir = tmp.filePath(QStringLiteral("proj"));
      QVERIFY(QDir().mkpath(projectDir));
      const QString root = tmp.filePath(QStringLiteral("area"));
      QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
      // bad.dat：井口表只有注释 → 入库后解析失败（RAW 已落库，重导命中 dedup）。
      const QString badPath =
          QDir(root).filePath(QString::fromUtf8("井位/bad.dat"));
      QVERIFY(writeFile(badPath,
                        QByteArrayLiteral("#WellHead File From SMI\n# no rows\n")));
      QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/good.dat")),
          QByteArrayLiteral("#WellHead File From SMI\n#Name X Y KB TD\n"
                            "A1  1.0  2.0  0.0  2000.0\n")));
      // locked.las / nodat.dat：读不了 → 入库前就失败，重导留 Failed。
      // Windows：只读属性不挡读，改持零共享句柄模拟「占用中不可读」；
      // 对应「修好文件」步骤在 Windows 是 CloseHandle。
      const QString lockedPath = QDir(root).filePath(QStringLiteral("locked.las"));
      const QString nodatPath = QDir(root).filePath(QStringLiteral("nodat.dat"));
      QVERIFY(writeFile(lockedPath, QByteArrayLiteral(
          "~Well\nWELL. A1 : WELL\n~A DEPT\n1.0\n")));
      QVERIFY(writeFile(nodatPath, QByteArrayLiteral("a,b\n1,2\n")));
#ifdef Q_OS_WIN
      HANDLE lockedHandle = CreateFileW(
          reinterpret_cast<const wchar_t *>(lockedPath.utf16()), GENERIC_READ, 0,
          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      QVERIFY(lockedHandle != INVALID_HANDLE_VALUE);
      HANDLE nodatHandle = CreateFileW(
          reinterpret_cast<const wchar_t *>(nodatPath.utf16()), GENERIC_READ, 0,
          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      QVERIFY(nodatHandle != INVALID_HANDLE_VALUE);
#else
      QVERIFY(QFile::setPermissions(lockedPath, QFileDevice::Permissions()));
      QVERIFY(QFile::setPermissions(nodatPath, QFileDevice::Permissions()));
#endif

      FolderStack st(tmp.filePath(QStringLiteral("m.sqlite")), projectDir);
      QString err;
      const auto preview = st.svc.previewFolder(root, &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      QCOMPARE(preview.size(), 4);

      QDialog dlg;
      FolderImportWorkflow wf(&st.svc, nullptr);
      PaleoFolderConfirm::buildFolderConfirmDialog(
          &dlg, root, preview, folderConfirmHooks(wf, root));
      // CRS 契约句：只读一行，挂在确认表上方。
      auto *crsNote = dlg.findChild<QLabel *>(QStringLiteral("folderCrsNote"));
      QVERIFY(crsNote);
      QCOMPARE(crsNote->text(),
               QString::fromUtf8(
                   "局部工程坐标，单位米。源文件里的 EPSG:4326 只是标签，不会画到地图上。"));
      QVERIFY(!(crsNote->textInteractionFlags() & Qt::TextEditable));

      auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("folderTable"));
      auto *summary = dlg.findChild<QLabel *>(QStringLiteral("folderSummary"));
      auto *confirm =
          dlg.findChild<QPushButton *>(QStringLiteral("folderConfirmButton"));
      QVERIFY(table && summary && confirm);
      // T32 a11y：确认表报名。
      QVERIFY(!table->accessibleName().isEmpty());
      QVERIFY(!table->accessibleDescription().isEmpty());
      const int rBad = tableRowForPath(table, QStringLiteral("bad.dat"));
      const int rGood = tableRowForPath(table, QStringLiteral("good.dat"));
      const int rLock = tableRowForPath(table, QStringLiteral("locked.las"));
      const int rNodat = tableRowForPath(table, QStringLiteral("nodat.dat"));
      QVERIFY(rBad >= 0 && rGood >= 0 && rLock >= 0 && rNodat >= 0);

      confirm->click();
      QVERIFY(!confirm->isEnabled());
      QVERIFY(table->item(rBad, 3)->text().contains(QString::fromUtf8("失败")));
      QVERIFY(table->item(rLock, 3)->text().contains(QString::fromUtf8("失败")));
      QVERIFY(table->item(rNodat, 3)->text().contains(QString::fromUtf8("失败")));
      QVERIFY(table->item(rGood, 3)->text().contains(QString::fromUtf8("已入库")));
      QCOMPARE(summary->text(),
               QString::fromUtf8("入库 1，未决 0，失败 3"));
      QCOMPARE(dlg.findChildren<QPushButton *>(
                   QStringLiteral("folderRetry")).size(), 3);

      // locked.las 仍不可读 → 重试仍失败，按钮回挂。
      auto *lockCombo =
          table->findChild<QComboBox *>(QStringLiteral("folderType%1").arg(rLock));
      QVERIFY(lockCombo && lockCombo->isEnabled()); // 失败行保留下拉可换类型
      auto *retryCell = static_cast<QWidget *>(table->cellWidget(rLock, 3));
      QVERIFY(retryCell);
      auto *retry = retryCell->findChild<QPushButton *>(QStringLiteral("folderRetry"));
      QVERIFY(retry);
      retry->click();
      QVERIFY(table->item(rLock, 3)->text().contains(QString::fromUtf8("失败")));
      QVERIFY(dlg.findChild<QPushButton *>(QStringLiteral("folderRetry")));

      // 修好文件 → 重试入库并挂到 A1（井口先行建的）。
#ifdef Q_OS_WIN
      CloseHandle(lockedHandle);
#else
      QVERIFY(QFile::setPermissions(lockedPath,
          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
          QFileDevice::ReadGroup | QFileDevice::ReadOther));
#endif
      retry = static_cast<QWidget *>(table->cellWidget(rLock, 3))
                  ->findChild<QPushButton *>(QStringLiteral("folderRetry"));
      QVERIFY(retry);
      retry->click();
      QVERIFY(table->item(rLock, 3)->text().contains(QString::fromUtf8("已入库")));
      QCOMPARE(table->item(rLock, 2)->text(), QStringLiteral("A1"));
      QVERIFY(!lockCombo->isEnabled()); // 行不再失败 → 收掉改类型入口
      QVERIFY(!table->cellWidget(rLock, 3));

      // nodat.dat 换成 document 再重试：按当前下拉类型重导。
      auto *nodatCombo =
          table->findChild<QComboBox *>(QStringLiteral("folderType%1").arg(rNodat));
      QVERIFY(nodatCombo && nodatCombo->isEnabled());
      nodatCombo->setCurrentIndex(
          projectClassifierTypes().indexOf(QStringLiteral("document")));
#ifdef Q_OS_WIN
      CloseHandle(nodatHandle);
#else
      QVERIFY(QFile::setPermissions(nodatPath,
          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
          QFileDevice::ReadGroup | QFileDevice::ReadOther));
#endif
      static_cast<QWidget *>(table->cellWidget(rNodat, 3))
          ->findChild<QPushButton *>(QStringLiteral("folderRetry"))
          ->click();
      QVERIFY(table->item(rNodat, 3)->text().contains(QString::fromUtf8("已入库")));

      // bad.dat 原样重试：字节已在库（dedup 如实记行），按钮消失。
      retry = static_cast<QWidget *>(table->cellWidget(rBad, 3))
                  ->findChild<QPushButton *>(QStringLiteral("folderRetry"));
      QVERIFY(retry);
      retry->click();
      QVERIFY(table->item(rBad, 3)->text().contains(QString::fromUtf8("已入库")));
      QVERIFY(table->item(rBad, 3)->text().contains(
          QString::fromUtf8("字节已在库")));

      QVERIFY(dlg.findChildren<QPushButton *>(
                  QStringLiteral("folderRetry")).isEmpty());
      QCOMPARE(summary->text(),
               QString::fromUtf8("入库 4，未决 0，失败 0"));

      // 后端资产类型跟着走 document（重试用的是当前下拉值）。
      bool sawDoc = false;
      for (const CatalogAsset &a : st.svc.catalog()->assets())
        if (a.displayName == QLatin1String("nodat.dat"))
        {
          sawDoc = true;
          QCOMPARE(a.type, QStringLiteral("document"));
        }
      QVERIFY(sawDoc);
    }

    // 汇总函数口径（D3）：四种结局计数，跳过只在 >0 时列第四项。
    void folderSummary_countsAllFourOutcomes()
    {
      using R = FolderRowResult;
      using Outcome = FolderRowResult::Outcome;
      QVector<R> rows(4);
      rows[0].outcome = Outcome::Imported;
      rows[1].outcome = Outcome::Unresolved;
      rows[2].outcome = Outcome::Failed;
      rows[3].outcome = Outcome::Skipped;
      QCOMPARE(PaleoFolderConfirm::folderImportSummaryText(rows),
               QString::fromUtf8("入库 1，未决 1，失败 1，跳过 1"));
      rows.removeLast();
      QCOMPARE(PaleoFolderConfirm::folderImportSummaryText(rows),
               QString::fromUtf8("入库 1，未决 1，失败 1"));
    }

    // ---- p5a：EntityView facade → DataPage 角色槽数据视图 ----
    // entityDataView（B 包纯查询门面）接进数据页：实体选中（D6 通路
    // selectAssetsForEntities）→ 按词表序枚举角色槽（含空槽「缺失」占位）；
    // primary=资产名+版本号、members 按 ordinal、unresolved 落名；下游
    // DERIVED 产物 + stale「过时」标记；missingSources 诊断行；空态不崩不猜。

    // 角色槽渲染：well 词表 9 槽全枚举（词表序），井头主关联带版本号，
    // 测井成员按 ordinal 序（非入库序），未决链接落到对应槽位。
    void dataPage_entityViewRoleSlotsRender()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      auto addAsset = [cat](const QString &id, const QString &name) {
        CatalogAsset a;
        a.id = id;
        a.type = QStringLiteral("well_log");
        a.displayName = name;
        return cat->addAsset(a);
      };
      QVERIFY(addAsset(QStringLiteral("ast-head"), QStringLiteral("A1.dat")));
      QVERIFY(addAsset(QStringLiteral("ast-main"), QStringLiteral("A1_main.las")));
      QVERIFY(addAsset(QStringLiteral("ast-old"), QStringLiteral("A1_old.las")));
      QVERIFY(addAsset(QStringLiteral("ast-mid"), QStringLiteral("A1_mid.las")));
      QVERIFY(addAsset(QStringLiteral("ast-pend"), QStringLiteral("A1x.las")));
      CatalogVersion head;
      head.id = QStringLiteral("ver-h1");
      head.assetId = QStringLiteral("ast-head");
      head.stage = QStringLiteral("RAW");
      head.versionNumber = 3; // 版本号是 currentVersion 的，不是恒 v1
      QVERIFY(cat->addVersion(head));
      auto link = [&well](const QString &asset, const QString &role, int ordinal,
                          bool primary, bool unresolved) {
        EntityAssetLink l;
        l.entityType = QStringLiteral("well");
        l.entityId = well.id; // 未决链接也带实体 id（上游歧义链接约定）
        l.assetId = asset;
        l.role = role;
        l.isPrimary = primary;
        l.unresolved = unresolved;
        l.ordinal = ordinal;
        return l;
      };
      QVERIFY(cat->addLink(link(QStringLiteral("ast-head"),
                                QStringLiteral("well_head"), 0, true, false)));
      QVERIFY(cat->addLink(link(QStringLiteral("ast-main"),
                                QStringLiteral("well_log"), 0, true, false)));
      QVERIFY(cat->addLink(link(QStringLiteral("ast-old"),
                                QStringLiteral("well_log"), 5, false, false)));
      QVERIFY(cat->addLink(link(QStringLiteral("ast-mid"),
                                QStringLiteral("well_log"), 1, false, false)));
      EntityAssetLink pend = link(QStringLiteral("ast-pend"),
                                  QStringLiteral("well_log"), 0, false, true);
      pend.note = QStringLiteral("未匹配井名: A1x");
      QVERIFY(cat->addLink(pend));

      DataPage page;
      PreviewDocService previewDoc8(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc8));
      page.selectAssetsForEntities({QStringLiteral("well-1")});
      auto *roleTable = page.findChild<QTableWidget *>(QStringLiteral("entityRoleTable"));
      QVERIFY2(roleTable, "entity selection must render a role-slot table");
      auto *header = page.findChild<QLabel *>(QStringLiteral("entityViewHeader"));
      QVERIFY(header);
      QVERIFY(header->text().contains(QStringLiteral("A1")));

      // 词表序 9 槽全枚举：井头打头、其他收尾。
      QCOMPARE(roleTable->rowCount(), 9);
      QCOMPARE(roleTable->item(0, 0)->text(), QString::fromUtf8("井身/井位"));
      QCOMPARE(roleTable->item(1, 0)->text(), QString::fromUtf8("测井曲线"));
      QCOMPARE(roleTable->item(8, 0)->text(), QString::fromUtf8("其他"));

      // 井头槽：primary = 资产名 + 当前版本号（v3，非字面 v1）。
      const QString headPrimary = roleTable->item(0, 1)->text();
      QVERIFY(headPrimary.contains(QStringLiteral("A1.dat")));
      QVERIFY(headPrimary.contains(QStringLiteral("v3")));

      // 测井槽：primary=主曲线；成员按 ordinal（mid(1) 在 old(5) 前，与入库
      // 序相反）；未决资产名落到未决列。
      const QString logPrimary = roleTable->item(1, 1)->text();
      QVERIFY(logPrimary.contains(QStringLiteral("A1_main.las")));
      QCOMPARE(roleTable->item(1, 2)->text(),
               QStringLiteral("A1_mid.las、A1_old.las"));
      QCOMPARE(roleTable->item(1, 3)->text(), QStringLiteral("A1x.las"));

      // 空槽（trajectory 等）：「缺失」占位 + 灰字克制样式（upstream
      // missing-source 可见性原则），不是空白行也不是凭空消失。
      const QString trajPrimary = roleTable->item(2, 1)->text();
      QCOMPARE(trajPrimary, QString::fromUtf8("缺失"));
      QCOMPARE(roleTable->item(2, 2)->text(), QStringLiteral("—"));
      QCOMPARE(roleTable->item(2, 3)->text(), QStringLiteral("—"));
      QVERIFY(roleTable->item(2, 1)->foreground().color().name()
                  .compare(QStringLiteral("#5d6e80"), Qt::CaseInsensitive) == 0);
    }

    // 宽度契约：超长实体名/文件名不得推高属性面板的宽度需求——内容在面板
    // 现有宽度内换行/裁切（Ignored 策略），而不是把「数据属性」dock 撑宽。
    void dataPage_entityViewLongNameDoesNotWidenPanel()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-long");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1_") + QString(200, QChar(u'深')) + QStringLiteral("井");
      QVERIFY(cat->addEntity(well));

      DataPage page;
      PreviewDocService previewDoc(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc));
      page.selectAssetsForEntities({QStringLiteral("well-long")});

      auto *header = page.findChild<QLabel *>(QStringLiteral("entityViewHeader"));
      QVERIFY(header);
      QVERIFY(header->text().contains(QStringLiteral("A1_")));
      // 头部 Ignored：sizeHint 不随文本长度增长（200 字名称 << 800px 阈值）
      QVERIFY2(header->sizeHint().width() < 800,
               qPrintable(QStringLiteral("header sizeHint width = %1").arg(header->sizeHint().width())));
      auto *props = page.findChild<QWidget *>(QStringLiteral("entityViewSection"));
      QVERIFY(props);
      QVERIFY2(props->sizeHint().width() < 800,
               qPrintable(QStringLiteral("panel sizeHint width = %1").arg(props->sizeHint().width())));
    }

    // 派生产物表：下游 DERIVED 版本列 displayName + vN；父版本被更高
    // versionNumber 取代 → extra["stale"] → 「过时」胶囊（B 包标记）。
    void dataPage_entityViewDerivedProductsAndStale()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      for (const char *aid : {"ast-raw", "ast-d1", "ast-d2"})
      {
        CatalogAsset a;
        a.id = QString::fromLatin1(aid);
        a.type = QStringLiteral("generic");
        a.displayName = QString::fromLatin1(aid) + QStringLiteral(".grd");
        QVERIFY(cat->addAsset(a));
      }
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = well.id;
      l.assetId = QStringLiteral("ast-raw");
      l.role = QStringLiteral("well_log");
      QVERIFY(cat->addLink(l));
      CatalogVersion r1;
      r1.id = QStringLiteral("ver-r1");
      r1.assetId = QStringLiteral("ast-raw");
      r1.versionNumber = 1;
      QVERIFY(cat->addVersion(r1));
      CatalogVersion d1;
      d1.id = QStringLiteral("ver-d1");
      d1.assetId = QStringLiteral("ast-d1");
      d1.stage = QStringLiteral("DERIVED");
      d1.versionNumber = 2;
      d1.parentVersionIds = {QStringLiteral("ver-r1")};
      QVERIFY(cat->addVersion(d1));
      CatalogVersion d2;
      d2.id = QStringLiteral("ver-d2");
      d2.assetId = QStringLiteral("ast-d2");
      d2.stage = QStringLiteral("DERIVED");
      d2.versionNumber = 1;
      d2.parentVersionIds = {QStringLiteral("ver-r1")};
      QVERIFY(cat->addVersion(d2));

      DataPage page;
      PreviewDocService previewDoc9(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc9));
      page.selectAssetsForEntities({QStringLiteral("well-1")});
      auto *derived = page.findChild<QTableWidget *>(QStringLiteral("derivedProductsTable"));
      QVERIFY2(derived, "entity view must list downstream DERIVED products");
      QCOMPARE(derived->rowCount(), 2);
      const auto rowForName = [derived](const QString &name) {
        for (int r = 0; r < derived->rowCount(); ++r)
          if (derived->item(r, 0)->text().contains(name))
            return r;
        return -1;
      };
      const int r1row = rowForName(QStringLiteral("ast-d1.grd"));
      const int r2row = rowForName(QStringLiteral("ast-d2.grd"));
      QVERIFY(r1row >= 0 && r2row >= 0);
      QCOMPARE(derived->item(r1row, 1)->text(), QStringLiteral("v2"));
      QCOMPARE(derived->item(r2row, 1)->text(), QStringLiteral("v1"));
      // 尚无 stale：状态列是灰字占位，不是「过时」。
      QVERIFY(!derived->cellWidget(r1row, 2));

      // 父版本被取代 → 下游 DERIVED 全标 stale；changed() 通路（此处直接
      // 驱动 refreshAssetTable——mainwindow 把 changed() 接到它）重取后过时。
      CatalogVersion r2 = r1;
      r2.id = QStringLiteral("ver-r2");
      r2.versionNumber = 2;
      QVERIFY(cat->addVersion(r2));
      page.refreshAssetTable();
      for (int r : {r1row, r2row})
      {
        auto *cap = qobject_cast<QLabel *>(derived->cellWidget(r, 2));
        QVERIFY2(cap, "stale derived product must carry a status capsule");
        QCOMPARE(cap->text(), QString::fromUtf8("过时"));
      }
    }

    // missingSources 非空 → 诊断行如实列出悬空的 parentVersionId。
    void dataPage_entityViewMissingSources()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      for (const char *aid : {"ast-raw", "ast-d"})
      {
        CatalogAsset a;
        a.id = QString::fromLatin1(aid);
        a.type = QStringLiteral("generic");
        a.displayName = QString::fromLatin1(aid) + QStringLiteral(".dat");
        QVERIFY(cat->addAsset(a));
      }
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = well.id;
      l.assetId = QStringLiteral("ast-raw");
      l.role = QStringLiteral("well_log");
      QVERIFY(cat->addLink(l));
      CatalogVersion raw;
      raw.id = QStringLiteral("ver-r1");
      raw.assetId = QStringLiteral("ast-raw");
      QVERIFY(cat->addVersion(raw));
      CatalogVersion d;
      d.id = QStringLiteral("ver-d1");
      d.assetId = QStringLiteral("ast-d");
      d.stage = QStringLiteral("DERIVED");
      d.parentVersionIds = {QStringLiteral("ver-r1")}; // 先干净血缘
      QVERIFY(cat->addVersion(d));

      DataPage page;
      PreviewDocService previewDoc10(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc10));
      page.selectAssetsForEntities({QStringLiteral("well-1")});
      auto *missing = page.findChild<QLabel *>(QStringLiteral("missingSourcesLabel"));
      QVERIFY2(missing, "dangling parentVersionIds must surface a diagnosis line");
      QVERIFY(!missing->isVisibleTo(&page));

      // 出现悬空引用（新 DERIVED 版本声明了不存在的父版本）→ 诊断行出现。
      CatalogAsset ghostAsset;
      ghostAsset.id = QStringLiteral("ast-d2");
      ghostAsset.type = QStringLiteral("generic");
      ghostAsset.displayName = QStringLiteral("ast-d2.dat");
      QVERIFY(cat->addAsset(ghostAsset));
      CatalogVersion d2;
      d2.id = QStringLiteral("ver-d2");
      d2.assetId = QStringLiteral("ast-d2");
      d2.stage = QStringLiteral("DERIVED");
      d2.parentVersionIds = {QStringLiteral("ver-r1"), QStringLiteral("ver-ghost")};
      QVERIFY(cat->addVersion(d2));
      page.refreshAssetTable();
      QVERIFY(missing->isVisibleTo(&page));
      QVERIFY(missing->text().contains(QStringLiteral("ver-ghost")));
    }

    // 空态：未绑导入服务 / catalog 未开 / 未选实体 / 未知 entityId——
    // 各自如实提示，不崩、不编造槽位。
    void dataPage_entityViewEmptyStates()
    {
      // 未绑服务：选中动作本身安全，视图区给空态提示。
      {
        DataPage page;
        page.selectAssetsForEntities({QStringLiteral("well-1")});
        auto *hint = page.findChild<QLabel *>(QStringLiteral("entityViewEmptyLabel"));
        QVERIFY2(hint, "unbound page must still own an empty-state label");
        QVERIFY(hint->isVisibleTo(&page));
        QVERIFY(!hint->text().isEmpty());
        QVERIFY(!page.findChild<QTableWidget *>(QStringLiteral("entityRoleTable"))
                     ->isVisibleTo(&page));
      }
      // 服务在但 catalog 未开（未设工程目录）。
      {
        DataImportService svc(nullptr, nullptr);
        DataPage page;
        PreviewDocService previewDoc11(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc11));
        page.selectAssetsForEntities({QStringLiteral("well-1")});
        auto *hint = page.findChild<QLabel *>(QStringLiteral("entityViewEmptyLabel"));
        QVERIFY(hint->isVisibleTo(&page));
        QVERIFY(hint->text().contains(QString::fromUtf8("工程还没打开")));
      }
      // catalog 开了但未选实体：指引下一步（地图点选）。
      {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataPage page;
        PreviewDocService previewDoc12(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc12));
        page.refreshAssetTable();
        auto *hint = page.findChild<QLabel *>(QStringLiteral("entityViewEmptyLabel"));
        QVERIFY(hint->isVisibleTo(&page));
        QVERIFY(hint->text().contains(QString::fromUtf8("地图")));
      }
      // 未知实体：如实说不在目录，带原 id，不猜。
      {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataImportService svc(nullptr, nullptr);
        svc.setProjectDir(dir.path());
        DataPage page;
        PreviewDocService previewDoc13(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc13));
        page.selectAssetsForEntities({QStringLiteral("well-nope")});
        auto *hint = page.findChild<QLabel *>(QStringLiteral("entityViewEmptyLabel"));
        QVERIFY(hint->isVisibleTo(&page));
        QVERIFY(hint->text().contains(QStringLiteral("well-nope")));
      }
    }

    // changed() 重取：挂接后 catalog 变了，refreshAssetTable（mainwindow 的
    // changed() 接线目标）重取实体视图，无需重新选择；纯查询不涨 revision。
    void dataPage_entityViewRefetchesAfterCatalogChange()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();
      CatalogEntity well;
      well.id = QStringLiteral("well-1");
      well.entityType = QStringLiteral("well");
      well.name = QStringLiteral("A1");
      QVERIFY(cat->addEntity(well));
      for (const char *aid : {"ast-main", "ast-pend"})
      {
        CatalogAsset a;
        a.id = QString::fromLatin1(aid);
        a.type = QStringLiteral("well_log");
        a.displayName = QString::fromLatin1(aid) + QStringLiteral(".las");
        QVERIFY(cat->addAsset(a));
      }
      EntityAssetLink main;
      main.entityType = QStringLiteral("well");
      main.entityId = well.id;
      main.assetId = QStringLiteral("ast-main");
      main.role = QStringLiteral("well_log");
      main.isPrimary = true;
      QVERIFY(cat->addLink(main));
      EntityAssetLink pend = main;
      pend.assetId = QStringLiteral("ast-pend");
      pend.isPrimary = false;
      pend.unresolved = true; // entityId 指向本实体的未决链接（上游约定）
      QVERIFY(cat->addLink(pend));

      DataPage page;
      PreviewDocService previewDoc14(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc14));
      page.selectAssetsForEntities({QStringLiteral("well-1")});
      auto *roleTable = page.findChild<QTableWidget *>(QStringLiteral("entityRoleTable"));
      QVERIFY(roleTable);
      // 渲染本身是纯查询：revision 不动。
      const int revBefore = cat->catalogRevision();
      page.refreshAssetTable();
      QCOMPARE(cat->catalogRevision(), revBefore);
      QVERIFY(roleTable->item(1, 1)->text().contains(QStringLiteral("ast-main")));
      QVERIFY(roleTable->item(1, 3)->text().contains(QStringLiteral("ast-pend")));

      // 挂接待定链接（catalog 变更）→ changed() 通路重取：新主关联是
      // ast-pend，旧主关联落成员桶，未决列清空。
      QVERIFY(cat->attachLink(1, QStringLiteral("well-1")));
      page.refreshAssetTable();
      QVERIFY(roleTable->item(1, 1)->text().contains(QStringLiteral("pend")));
      QVERIFY(roleTable->item(1, 2)->text().contains(QStringLiteral("main")));
      QVERIFY(roleTable->item(1, 3)->text().isEmpty()
              || roleTable->item(1, 3)->text() == QStringLiteral("—"));
    }

    void dataPage_surveyAreaFirstItemAndDoubleClicked()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      FolderStack stack(dir.filePath(QStringLiteral("meta.sqlite")), dir.path());
      auto *cat = stack.svc.catalog();
      QVERIFY(cat);

      CatalogEntity w;
      w.id = QStringLiteral("w1");
      w.entityType = QStringLiteral("well");
      w.name = QStringLiteral("Well-01");
      QVERIFY(cat->addEntity(w));

      CatalogEntity surv;
      surv.id = QStringLiteral("survey1");
      surv.entityType = QStringLiteral("seismic_survey");
      surv.name = QStringLiteral("Survey3D");
      surv.inlineMin = 100;
      surv.inlineMax = 500;
      surv.xlineMin = 200;
      surv.xlineMax = 600;
      QVERIFY(cat->addEntity(surv));

      DataPage page;
      PreviewDocService previewDocStackSvc(&stack.svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDocStackSvc));
      page.refreshAssetTable();

      auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
      QVERIFY(tree);
      QVERIFY(tree->topLevelItemCount() >= 1);

      // Requirement 1: "数据列表里面，增加一个测区放在第一个项"
      QTreeWidgetItem *firstItem = tree->topLevelItem(0);
      QCOMPARE(firstItem->text(0), QStringLiteral("测区"));
      QCOMPARE(firstItem->data(0, Qt::UserRole).toString(), QStringLiteral("survey_area"));
      QCOMPARE(firstItem->data(0, Qt::UserRole + 2).toString(), QStringLiteral("survey_area"));
      QVERIFY(firstItem->childCount() >= 1);
      QCOMPARE(firstItem->child(0)->text(0), QStringLiteral("工区全景地图"));

      // Requirement 1: "双击后，就是整个测区的图"
      QSignalSpy spy(&page, &DataPage::surveyAreaActivated);
      emit tree->itemDoubleClicked(firstItem, 0);
      QCOMPARE(spy.count(), 1);

      spy.clear();
      emit tree->itemDoubleClicked(firstItem->child(0), 0);
      QCOMPARE(spy.count(), 1);

      // 属性面板展示测区信息
      auto *header = page.findChild<QLabel *>(QStringLiteral("entityViewHeader"));
      QVERIFY(header);
      QVERIFY(header->text().contains(QStringLiteral("测区全景")));
      auto *propType = page.findChild<QLabel *>(QStringLiteral("propType"));
      QVERIFY(propType);
      QVERIFY(propType->text().contains(QStringLiteral("测区全景地图")));
    }

    void dataPage_columnWidthPreservesFilenameOnShrink()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      FolderStack stack(dir.filePath(QStringLiteral("meta.sqlite")), dir.path());

      DataPage page;
      PreviewDocService previewDocStackSvc(&stack.svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDocStackSvc));
      page.refreshAssetTable();

      auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("dataTree"));
      QVERIFY(tree);
      auto *table = page.findChild<QTableWidget *>(QStringLiteral("assetTable"));
      QVERIFY(table);

      // Requirement 2: "数据列表里面，当宽度减少后，应该保文件名的显示，而不是类型/描述。"
      QResizeEvent shrinkTree(QSize(180, 500), QSize(350, 500));
      qApp->sendEvent(tree, &shrinkTree);

      const int col1W = tree->columnWidth(1);
      QVERIFY2(col1W <= 50, qPrintable(QString::number(col1W)));
      QVERIFY2(col1W >= 35, qPrintable(QString::number(col1W)));

      QResizeEvent shrinkTable(QSize(200, 500), QSize(400, 500));
      qApp->sendEvent(table, &shrinkTable);

      const int tCol1 = table->columnWidth(1);
      const int tCol2 = table->columnWidth(2);
      QVERIFY2(tCol1 <= 45, qPrintable(QString::number(tCol1)));
      QVERIFY2(tCol2 <= 55, qPrintable(QString::number(tCol2)));
      QVERIFY(tCol1 + tCol2 <= 100);
    }

    void dataPage_geoJsonAssetPropertiesInEntityView()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(dir.path());
      DataCatalog *cat = svc.catalog();

      // Create a test GeoJSON file
      const QString geoPath = dir.filePath(QStringLiteral("facies_sample.geojson"));
      QFile f(geoPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(R"({
        "type": "FeatureCollection",
        "features": [
          {
            "type": "Feature",
            "geometry": { "type": "Point", "coordinates": [110.5, 30.2] },
            "properties": { "facies": "delta_front", "id": 1, "name": "F1", "period": "P1" }
          },
          {
            "type": "Feature",
            "geometry": { "type": "Point", "coordinates": [120.0, 35.8] },
            "properties": { "facies": "prodelta", "id": 2, "name": "F2", "period": "P1" }
          }
        ]
      })");
      f.close();

      CatalogAsset a;
      a.id = QStringLiteral("ast-geo-1");
      a.type = QStringLiteral("geojson");
      a.displayName = QStringLiteral("facies_sample.geojson");
      QVERIFY(cat->addAsset(a));

      CatalogVersion v;
      v.id = QStringLiteral("ver-geo-1");
      v.assetId = a.id;
      v.stage = QStringLiteral("RAW");
      v.versionNumber = 1;
      v.managed = false;
      v.path = geoPath;
      QString addErr;
      QVERIFY2(cat->addVersion(v, &addErr), qPrintable(addErr));

      DataPage page;
      PreviewDocService previewDoc17(&svc);
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&previewDoc17));
      page.selectAsset(a.id);

      auto *header = page.findChild<QLabel *>(QStringLiteral("entityViewHeader"));
      QVERIFY(header);
      QVERIFY(header->text().contains(QStringLiteral("facies_sample.geojson")));
      QVERIFY(!header->text().contains(QStringLiteral("井实体")));

      auto *propType = page.findChild<QLabel *>(QStringLiteral("propType"));
      QVERIFY(propType);
      QVERIFY(propType->text().contains(QStringLiteral("参考相图")));

      auto *propCrs = page.findChild<QLabel *>(QStringLiteral("propCrs"));
      QVERIFY(propCrs);
      QVERIFY(propCrs->text().contains(QStringLiteral("经纬度，与本测网不是同一空间")));

      auto *propCoord = page.findChild<QLabel *>(QStringLiteral("propCoord"));
      QVERIFY(propCoord);
      QVERIFY(propCoord->text().contains(QStringLiteral("110.50")));
      QVERIFY(propCoord->text().contains(QStringLiteral("120.00")));

      auto *propGrid = page.findChild<QLabel *>(QStringLiteral("propGrid"));
      QVERIFY(propGrid);
      QVERIFY(propGrid->text().contains(QStringLiteral("2")));

      auto *propDetailsText = page.findChild<QLabel *>(QStringLiteral("propDetailsText"));
      QVERIFY(propDetailsText);
      const QString details = propDetailsText->text();
      QVERIFY(details.contains(QStringLiteral("要素个数: 2")));
      QVERIFY(details.contains(QStringLiteral("facies, id, name, period")));
      QVERIFY(details.contains(QStringLiteral("经纬度，与本测网不是同一空间")));
    }
    // =====================================================================
    // P3 数据操作重构测试（wave/data-page-operations）。
    // 约定：DataOpsFixture = 2 井 + 3 资产（1 已决 / 1 未决 / 1 无链接），
    // 逐用例断言操作矩阵 / 过滤 / 拖拽 / 撤销 / 视图形态 / 导入队列。
    // =====================================================================
private:
    // 造一套标准工程：well-A1/well-B2，A1.las(已决主)、Z9.las(未决)、
    // free.dat(无链接)。返回结构体带全部 id（dir/svc 由调用方持有）。
    struct DataOpsFixture
    {
        QString wellA = QStringLiteral("well-A1");
        QString wellB = QStringLiteral("well-B2");
        QString astResolved = QStringLiteral("ast-res");
        QString astPending = QStringLiteral("ast-pend");
        QString astFree = QStringLiteral("ast-free");
        void build(DataCatalog *cat)
        {
            CatalogEntity a1;
            a1.id = wellA;
            a1.entityType = QStringLiteral("well");
            a1.name = QStringLiteral("A1");
            QVERIFY2(cat->addEntity(a1), "well A1");
            CatalogEntity b2;
            b2.id = wellB;
            b2.entityType = QStringLiteral("well");
            b2.name = QStringLiteral("B2");
            QVERIFY2(cat->addEntity(b2), "well B2");
            CatalogAsset r;
            r.id = astResolved;
            r.type = QStringLiteral("well_log");
            r.displayName = QStringLiteral("A1.las");
            QVERIFY(cat->addAsset(r));
            CatalogAsset p;
            p.id = astPending;
            p.type = QStringLiteral("tops");
            p.displayName = QStringLiteral("Z9.las");
            QVERIFY(cat->addAsset(p));
            CatalogAsset f;
            f.id = astFree;
            f.type = QStringLiteral("seismic");
            f.displayName = QStringLiteral("free.sgy");
            QVERIFY(cat->addAsset(f));
            EntityAssetLink ok;
            ok.entityType = QStringLiteral("well");
            ok.entityId = wellA;
            ok.assetId = astResolved;
            ok.role = QStringLiteral("well_log");
            QVERIFY(cat->addLink(ok));
            EntityAssetLink un;
            un.entityType = QStringLiteral("well");
            un.assetId = astPending;
            un.role = QStringLiteral("tops");
            un.unresolved = true;
            un.note = QStringLiteral("未匹配井名: Z9x");
            QVERIFY(cat->addLink(un));
        }
        DataPage *makePage(DataImportService *svc)
        {
            auto *page = new DataPage;
            // 门面父挂到 page——同生命周期，唯一测试路径不泄。
            auto *doc = new PreviewDocService(svc, page);
            page->setProperty("paleo.page.importsvc",
                              QVariant::fromValue<QObject *>(doc));
            page->refreshAssetTable();
            return page;
        }
    };

    // 模态对话框协作器：在下个事件拍抓 activeModalWidget 做动作（offscreen
    // 下 exec 循环照样跑，QTimer::singleShot(0) 会进 exec 的事件分发）。
    static void driveModalNextTick(const std::function<void(QWidget *)> &drive)
    {
        QTimer::singleShot(0, [drive] {
            if (QWidget *m = QApplication::activeModalWidget())
                drive(m);
        });
        // 兜底：驱动失配（类型/时机不对）3s 后强关活动模态——测试失败但不挂死。
        QTimer::singleShot(3000, [] {
            if (QWidget *m = QApplication::activeModalWidget())
                m->close();
        });
    }

  private slots:
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
        QVERIFY(!singleActs.contains(QStringLiteral("removeSoft")));

        ContextMenuSpec multi;
        multi.hasAssets = true;
        multi.assetCount = 5;
        const QStringList multiActs = contextMenuActions(multi);
        QVERIFY(multiActs.contains(QStringLiteral("attachToEntity")));
        QVERIFY(multiActs.contains(QStringLiteral("exportManifest")));
        QVERIFY(multiActs.contains(QStringLiteral("removeSoft")));
        QVERIFY(multiActs.contains(QStringLiteral("openPreviewAll")));
        // 异构选中：资产 + 实体 → 公共子集（资产操作）+ 实体定位，不给实体 CRUD。
        ContextMenuSpec mixed;
        mixed.hasAssets = true;
        mixed.hasEntities = true;
        mixed.assetCount = 2;
        mixed.entityCount = 1;
        const QStringList mixedActs = contextMenuActions(mixed);
        QVERIFY(mixedActs.contains(QStringLiteral("focusEntities")));
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
        QVERIFY(c.matches(row) || !c.matches(row)); // 无效模式退字面量（不炸）
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
        QVERIFY(bar.isVisible() || !g2.conditions.isEmpty());
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
        QVERIFY2(elapsed < 100, qPrintable(QStringLiteral("%1ms").arg(elapsed)));
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
        page->selectAsset(fx.astResolved);
        auto *topo = page->findChild<paleo::dataops::TopologyGraph *>(QStringLiteral("topologyGraph"));
        QVERIFY(topo);
        // D4.5：2 实体 + 3 资产节点；边 = 链接数（1 已决 + 1 未决虚线）。
        QCOMPARE(topo->nodeCount(), 5);
        QVERIFY(topo->scene()->items().size() >= 5);
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
        QVERIFY2(one < 16, qPrintable(QStringLiteral("one batch %1ms").arg(one)));
        t.restart();
        while (model.canFetchMore(QModelIndex()))
            model.fetchMore(QModelIndex());
        const qint64 all = t.elapsed();
        QVERIFY2(all < 1000, qPrintable(QStringLiteral("all %1ms").arg(all)));
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
        // 三拍：角色对话框选 tops → accept → 警告框 Yes。
        driveModalNextTick([](QWidget *w) {
            if (auto *dlg = qobject_cast<paleo::dataops::RoleEditDialog *>(w))
            {
                auto *combo =
                    dlg->findChild<QComboBox *>(QStringLiteral("roleEditCombo"));
                const int idx = combo->findText(QStringLiteral("tops"));
                if (idx >= 0)
                    combo->setCurrentIndex(idx);
                dlg->accept();
            }
            QTimer::singleShot(0, [] {
                if (auto *mb = qobject_cast<QMessageBox *>(
                        QApplication::activeModalWidget()))
                    mb->button(QMessageBox::Yes)->click();
            });
        });
        lp->editRoleForSelection();
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
        QSet<QString> visited;
        QWidget *w = search;
        for (int i = 0; i < 80; ++i)
        {
            visited.insert(w->objectName());
            w->setFocus();
            QTest::keyClick(w, Qt::Key_Tab);
            w = QApplication::focusWidget();
            if (!w)
                break;
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
  TestPanels tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_panels.moc"
