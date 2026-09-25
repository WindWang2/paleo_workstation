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
      QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("来源"));
      // §42.4: an empty table shows a disabled guidance row, never a blank panel.
      QCOMPARE(table->rowCount(), 1);
      QVERIFY(table->item(0, 0));
      QVERIFY(!(table->item(0, 0)->flags() & Qt::ItemIsEnabled));
      QVERIFY(!table->item(0, 0)->text().isEmpty());
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
