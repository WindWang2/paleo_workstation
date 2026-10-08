#include "../src/qgis/wellattributestore.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/wellcomposite/wellcompositepanel.h"
#include "../src/workflow/wellfaciesworkflow.h"
#include "../src/workflow/workflows.h"
#include "helpers/workflowfixture.h"
#include <QSignalSpy>
#include <QToolButton>
#include <QtTest>
#include <limits>
#include <qgsapplication.h>
#include <qgsfeatureiterator.h>
#include <qgsfield.h>
#include <qgsvectorlayer.h>
using namespace paleo::tests;
using namespace WellComposite;
namespace {
bool addWell(WorkflowFixture &f, const QString &id) {
  CatalogEntity well;
  well.entityType = "well";
  well.id = id;
  well.name = id;
  well.hasSurface = true;
  well.surfaceX = 100;
  well.surfaceY = 200;
  well.coordinateStatus = "untransformed";
  return f.catalog.addEntity(well);
}
QVariantMap interval(const QString &id, double top, double base,
                     const QString &litho) {
  return {{"well_id", id},
          {"top_md", top},
          {"base_md", base},
          {"lithology", litho}};
}
ComprehensiveWellData data() {
  ComprehensiveWellData d;
  d.wellName = "W1";
  d.minDepth = 0;
  d.maxDepth = 20;
  LithologyInterval a;
  a.topDepth = 0;
  a.bottomDepth = 10;
  a.lithoName = "砂岩";
  a.patternType = "sandstone";
  LithologyInterval b = a;
  b.topDepth = 10;
  b.bottomDepth = 20;
  b.lithoName = "泥岩";
  b.patternType = "mudstone";
  d.lithologyIntervals = {a, b};
  return d;
}
} // namespace
class TestWellAttributes : public QObject {
  Q_OBJECT
private slots:
  void sourceFaciesSeedIsIdempotentAndKeepsManualClearing() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    auto source = data();
    FaciesInterval facies;
    facies.topDepth = 0;
    facies.bottomDepth = 20;
    facies.majorFacies = "三角洲";
    facies.subFacies = "三角洲前缘";
    facies.microFacies = "水下分流河道";
    facies.patternType = "sandstone";
    source.faciesIntervals = {facies};
    QString error;
    QVERIFY2(WellAttributeStore::seed(&f.catalog, f.dir.path(), &f.layers, "W1",
                                      source, &error),
             qPrintable(error));
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           false, false);
    QVERIFY(layer);
    QSignalSpy commits(layer, &QgsVectorLayer::afterCommitChanges);
    QVERIFY(WellAttributeStore::seed(&f.catalog, f.dir.path(), &f.layers, "W1",
                                     source, &error));
    QCOMPARE(commits.size(), 0);
    ComprehensiveWellData assembled = data();
    WellAttributeStore::apply(
        WellAttributeStore::rows(f.dir.path(), nullptr, false, "W1"),
        &assembled);
    QCOMPARE(assembled.faciesIntervals.size(), 2);
    QCOMPARE(assembled.faciesIntervals.first().subFacies, facies.subFacies);
    QCOMPARE(assembled.faciesIntervals.first().microFacies, facies.microFacies);
    QVERIFY(layer->startEditing());
    auto it = layer->getFeatures();
    QgsFeature row;
    while (it.nextFeature(row))
      QVERIFY(layer->changeAttributeValue(
          row.id(), layer->fields().indexOf("facies"), QStringLiteral("")));
    QVERIFY(layer->commitChanges());
    commits.clear();
    QVERIFY(WellAttributeStore::seed(&f.catalog, f.dir.path(), &f.layers, "W1",
                                     source, &error));
    QCOMPARE(commits.size(), 0);
    WellAttributeStore::apply(
        WellAttributeStore::rows(f.dir.path(), nullptr, false, "W1"),
        &assembled);
    QVERIFY(assembled.faciesIntervals.isEmpty());
  }
  void predictionSplitsPreservesAndReopens() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    QVERIFY(addWell(f, "W2"));
    QString error;
    QVERIFY2(WellAttributeStore::seed(&f.catalog, f.dir.path(), &f.layers, "W1",
                                      data(), &error),
             qPrintable(error));
    QVERIFY(WellAttributeStore::mergeIntervals(
        &f.catalog, f.dir.path(), &f.layers, {interval("W2", 0, 20, "灰岩")},
        &error));
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           false, false);
    QVERIFY(layer);
    QVERIFY(layer->startEditing());
    QVERIFY(layer->addAttribute(QgsField("expert_note", QMetaType::QString)));
    auto features = layer->getFeatures();
    QgsFeature row;
    while (features.nextFeature(row))
      if (row.attribute("well_id") == "W1")
        QVERIFY(layer->changeAttributeValue(
            row.id(), layer->fields().indexOf("expert_note"), "保留解释"));
    QVERIFY(layer->commitChanges());
    WellFaciesWorkflow wf;
    wf.setCatalog(&f.catalog);
    wf.setLayerService(&f.layers);
    wf.setData(data());
    WellFaciesResult prediction;
    prediction.modelName = "fixture";
    prediction.jobId = "job1";
    TextInterval a;
    a.topDepth = 0;
    a.bottomDepth = 7;
    a.text = "河道";
    TextInterval b = a;
    b.topDepth = 7;
    b.bottomDepth = 20;
    b.text = "湖泊";
    prediction.intervals = {a, b};
    QVERIFY2(wf.publishLithoAsset(prediction).isEmpty(), qPrintable(error));
    const auto rows =
        WellAttributeStore::rows(f.dir.path(), &f.layers, false, "W1");
    QCOMPARE(rows.size(), 3);
    for (const auto &v : rows)
      QCOMPARE(v.toMap().value("expert_note").toString(), QString("保留解释"));
    QCOMPARE(WellAttributeStore::rows(f.dir.path(), &f.layers, false, "W2")
                 .first()
                 .toMap()
                 .value("lithology")
                 .toString(),
             QString("灰岩"));
    auto persisted =
        WellAttributeStore::rows(f.dir.path(), nullptr, false, "W1");
    QCOMPARE(persisted.size(), 3);
    ComprehensiveWellData assembled = data();
    WellAttributeStore::apply(persisted, &assembled);
    QCOMPARE(assembled.lithologyIntervals.size(), 3);
    QCOMPARE(assembled.faciesIntervals.size(), 3);
    QVERIFY(layer->startEditing());
    features = layer->getFeatures();
    while (features.nextFeature(row))
      if (row.attribute("well_id") == "W1" &&
          row.attribute("top_md").toDouble() == 0) {
        QVERIFY(layer->changeAttributeValue(
            row.id(), layer->fields().indexOf("facies"), "人工修订相"));
        QVERIFY(layer->changeAttributeValue(
            row.id(), layer->fields().indexOf("lithology"), "泥岩"));
      }
    QVERIFY(layer->commitChanges());
    QVERIFY(WellAttributeStore::seed(&f.catalog, f.dir.path(), &f.layers, "W1",
                                     data(), &error));
    persisted = WellAttributeStore::rows(f.dir.path(), nullptr, false, "W1");
    bool found = false;
    for (const auto &v : persisted)
      if (v.toMap().value("top_md").toDouble() == 0) {
        QCOMPARE(v.toMap().value("facies").toString(), QString("人工修订相"));
        QCOMPARE(v.toMap().value("predicted_facies").toString(),
                 QString("河道"));
        QCOMPARE(v.toMap().value("lithology").toString(), QString("泥岩"));
        found = true;
      }
    QVERIFY(found);
  }
  void invalidAndBusyBatchDoesNotWrite() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    QString error;
    QVERIFY(WellAttributeStore::mergeIntervals(
        &f.catalog, f.dir.path(), &f.layers, {interval("W1", 0, 20, "砂岩")},
        &error));
    const auto before = WellAttributeStore::rows(f.dir.path(), nullptr, false);
    auto fractionalCode = interval("W1", 0, 20, "泥岩");
    fractionalCode.insert("facies_code", 1.5);
    for (const QVariantList &invalid :
         {QVariantList{interval("W1", 0, 10, "泥岩"),
                       interval("ghost", 10, 20, "泥岩")},
          QVariantList{interval("W1", 0, 12, "泥岩"),
                       interval("W1", 10, 20, "泥岩")},
          QVariantList{interval("W1", std::numeric_limits<double>::quiet_NaN(),
                                20, "泥岩")},
          QVariantList{fractionalCode}}) {
      QVERIFY(!WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(),
                                                  &f.layers, invalid, &error));
      QCOMPARE(WellAttributeStore::rows(f.dir.path(), nullptr, false), before);
    }
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           false, false);
    QVERIFY(layer->startEditing());
    QVERIFY(!WellAttributeStore::mergeIntervals(
        &f.catalog, f.dir.path(), &f.layers, {interval("W1", 0, 20, "泥岩")},
        &error));
    QVERIFY(layer->isEditable());
    QVERIFY(layer->rollBack());
    f.catalog.setLockedReadOnly(true);
    QVERIFY(!WellAttributeStore::mergeIntervals(
        &f.catalog, f.dir.path(), &f.layers, {interval("W1", 0, 20, "泥岩")},
        &error));
    QVERIFY(WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers, false,
                                     false)
                ->readOnly());
    QCOMPARE(WellAttributeStore::rows(f.dir.path(), nullptr, false), before);
  }
  void maintainedFactorsTakePriorityAndStayInHorizon() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    QString error;
    QVariantList values{QVariantMap{{"well_id", "W1"},
                                    {"horizon", "D61"},
                                    {"log_sand_thickness_md", 8.0},
                                    {"log_layer_thickness_md", 20.0},
                                    {"sand_ratio", 0.4}},
                        QVariantMap{{"well_id", "W1"},
                                    {"horizon", "D53"},
                                    {"log_sand_thickness_md", 0.0},
                                    {"sand_ratio", 0.0}}};
    QVERIFY(WellAttributeStore::seedFactors(&f.catalog, f.dir.path(), &f.layers,
                                            values, &error));
    ConstraintWorkflow factors(&f.proc, &f.layers);
    factors.setCatalog(&f.catalog, f.dir.path());
    QVariantMap params{{"factorMode", "direct"},
                       {"valueField", "log_sand_thickness_md"}};
    QVERIFY2(factors.extractWellFactors("D61", "sandthick", params, &error),
             qPrintable(error));
    QCOMPARE(factors.wellFactorRows().first().toMap().value("value").toDouble(),
             8.0);
    QVERIFY(factors.extractWellFactors("D53", "sandthick", params, &error));
    QCOMPARE(factors.wellFactorRows().first().toMap().value("value").toDouble(),
             0.0);
    params.insert("valueField", "sand_ratio");
    QVERIFY(factors.extractWellFactors("D61", "sandratio", params, &error));
    QCOMPARE(factors.wellFactorRows().first().toMap().value("value").toDouble(),
             0.4);
    values[0] = QVariantMap{
        {"well_id", "W1"}, {"horizon", "D61"}, {"log_sand_thickness_md", 99.0}};
    QVERIFY(WellAttributeStore::seedFactors(&f.catalog, f.dir.path(), &f.layers,
                                            values, &error));
    QCOMPARE(WellAttributeStore::rows(f.dir.path(), nullptr, true, "W1", "D61")
                 .first()
                 .toMap()
                 .value("log_sand_thickness_md")
                 .toDouble(),
             8.0);
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           true, false);
    QVERIFY(layer->startEditing());
    auto it = layer->getFeatures();
    QgsFeature row;
    while (it.nextFeature(row))
      if (row.attribute("horizon") == "D61")
        QVERIFY(layer->changeAttributeValue(
            row.id(), layer->fields().indexOf("sand_ratio"), 1.5));
    QVERIFY(!layer->commitChanges());
    QVERIFY(layer->rollBack());
  }
  void panelReflectsSavedAttributes() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    QString error;
    WellCompositePanel panel;
    auto *wf = new WellFaciesWorkflow(&panel);
    wf->setCatalog(&f.catalog);
    wf->setLayerService(&f.layers);
    panel.bindFaciesWorkflow(wf);
    QVERIFY(panel.loadWellData(data()));
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           false, false);
    QVERIFY(layer);
    QVERIFY(layer->startEditing());
    auto it = layer->getFeatures();
    QgsFeature row;
    while (it.nextFeature(row))
      if (row.attribute("top_md").toDouble() == 0) {
        QVERIFY(layer->changeAttributeValue(
            row.id(), layer->fields().indexOf("lithology"), "砾岩"));
        QVERIFY(layer->changeAttributeValue(
            row.id(), layer->fields().indexOf("facies"), "冲积扇"));
      }
    QVERIFY(layer->commitChanges());
    QTRY_COMPARE(panel.currentData().lithologyIntervals.first().lithoName,
                 QString("砾岩"));
    QCOMPARE(panel.currentData().faciesIntervals.first().majorFacies,
             QString("冲积扇"));
    QSignalSpy table(wf, &WellFaciesWorkflow::attributeTableRequested);
    auto *button = panel.findChild<QToolButton *>("btnWellAttributes");
    QVERIFY(button);
    button->click();
    QCOMPARE(table.size(), 1);
    QCOMPARE(table.first().first().toString(),
             WellAttributeStore::intervalLayerId());
    const QString screenshot =
        qEnvironmentVariable("PALEO_WELL_ATTRIBUTES_SCREENSHOT");
    if (!screenshot.isEmpty()) {
      panel.resize(1200, 720);
      panel.show();
      QTest::qWait(100);
      QVERIFY(panel.grab().save(screenshot));
    }
  }
};
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.initQgis();
  PaleoTheme::applyLightTheme();
  TestWellAttributes test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_wellattributes.moc"
