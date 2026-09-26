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

#include "../src/ui/pages/pagepanels.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/workflow/workflows.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/projectclassifier.h"
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
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&svc));
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
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&svc));
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
      page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&svc));
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

      page.findChild<QPushButton *>(QStringLiteral("thicknessChainButton"))->click();
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
      PaleoMainWindow::populateFolderConfirmTable(&table, root, preview, &combos);
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
      QVERIFY(PaleoMainWindow::collectFolderTypeOverrides(&table, preview, combos)
                  .isEmpty());
      combos.at(1)->setCurrentIndex(vocab.indexOf(QStringLiteral("document")));
      const auto ov =
          PaleoMainWindow::collectFolderTypeOverrides(&table, preview, combos);
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
      PaleoMainWindow::populateFolderConfirmTable(&table, root, preview, &combos);
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
      auto ov = PaleoMainWindow::collectFolderTypeOverrides(&table, preview, combos);
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
      ov = PaleoMainWindow::collectFolderTypeOverrides(&table, preview, combos);
      QCOMPARE(ov.value(preview.at(rOther).path), QStringLiteral("well_head"));
      QCOMPARE(ov.size(), ovSize);
      combos.at(rOther)->setCurrentIndex(vocab.indexOf(QStringLiteral("well_log")));
      ov = PaleoMainWindow::collectFolderTypeOverrides(&table, preview, combos);
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
      PaleoMainWindow::buildFolderConfirmDialog(&dlg, &st.svc, root, preview,
                                                nullptr);
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
      using R = DataImportService::FolderRowResult;
      using Outcome = DataImportService::FolderRowResult::Outcome;
      QVector<R> rows(4);
      rows[0].outcome = Outcome::Imported;
      rows[1].outcome = Outcome::Unresolved;
      rows[2].outcome = Outcome::Failed;
      rows[3].outcome = Outcome::Skipped;
      QCOMPARE(PaleoMainWindow::folderImportSummaryText(rows),
               QString::fromUtf8("入库 1，未决 1，失败 1，跳过 1"));
      rows.removeLast();
      QCOMPARE(PaleoMainWindow::folderImportSummaryText(rows),
               QString::fromUtf8("入库 1，未决 1，失败 1"));
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
