#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>

#include "../src/ui/pages/pagepanels.h"
#include "../src/workflow/workflows.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/io/dataimportservice.h"
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
