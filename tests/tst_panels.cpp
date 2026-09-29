#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QResizeEvent>

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
      QVERIFY(label->styleSheet().contains(QStringLiteral("#E53935"))); // error 字
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
      QVERIFY(sevCapsule->styleSheet().contains(QStringLiteral("#E53935")));
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
      const QString lockedPath = QDir(root).filePath(QStringLiteral("locked.las"));
      const QString nodatPath = QDir(root).filePath(QStringLiteral("nodat.dat"));
      QVERIFY(writeFile(lockedPath, QByteArrayLiteral(
          "~Well\nWELL. A1 : WELL\n~A DEPT\n1.0\n")));
      QVERIFY(writeFile(nodatPath, QByteArrayLiteral("a,b\n1,2\n")));
      QVERIFY(QFile::setPermissions(lockedPath, QFileDevice::Permissions()));
      QVERIFY(QFile::setPermissions(nodatPath, QFileDevice::Permissions()));

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
      QVERIFY(QFile::setPermissions(lockedPath,
          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
          QFileDevice::ReadGroup | QFileDevice::ReadOther));
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
      QVERIFY(QFile::setPermissions(nodatPath,
          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
          QFileDevice::ReadGroup | QFileDevice::ReadOther));
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
