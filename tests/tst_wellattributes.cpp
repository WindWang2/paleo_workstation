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
QVariantMap formationRow(const QString &id, const QString &formation,
                         double top, double base, const QString &dominant) {
  return {{"well_id", id},
          {"formation", formation},
          {"unit_type", "段"},
          {"top_md", top},
          {"base_md", base},
          {"thickness_md", base - top},
          {"dominant_facies", dominant},
          {"predicted_dominant", dominant},
          {"dominant_ratio", 0.6},
          {"coverage", 1.0}};
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
  // 地层优势相表：每井每地层一行；按 well_id+formation 更新，只覆盖给出的列，
  // 保留手工维护的优势相之外的列与自定义属性；重开工程不依赖图层服务也能读。
  void formationTableUpsertsByWellAndFormation() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    QVERIFY(addWell(f, "W2"));
    QString error;
    QVERIFY(WellAttributeStore::rows(f.dir.path(), &f.layers,
                                     WellAttributeStore::Table::Formations)
                .isEmpty());
    QVERIFY2(WellAttributeStore::mergeFormations(
                 &f.catalog, f.dir.path(), &f.layers,
                 {formationRow("W1", "D61", 1000, 1100, "分流河道"),
                  formationRow("W1", "D62", 1100, 1180, "河口坝"),
                  formationRow("W2", "D61", 900, 1000, "湖泊")},
                 &error),
             qPrintable(error));
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           WellAttributeStore::Table::Formations,
                                           false);
    QVERIFY(layer);
    QCOMPARE(layer->featureCount(), 3);
    QCOMPARE(layer->fields().indexOf("horizon"), -1); // 地层表用 formation 列
    QVERIFY(layer->fields().indexOf("dominant_facies") >= 0);
    QCOMPARE(layer->fields().at(layer->fields().indexOf("dominant_facies")).alias(),
             QString("地层相")); // 智能预测侧不出现「优势」字样
    QCOMPARE(layer->name(), QString("测井地层相属性"));
    auto it = layer->getFeatures();
    QgsFeature feature;
    while (it.nextFeature(feature)) {
      QVERIFY(feature.hasGeometry()); // 井位点：可上图、可按属性出图
      QCOMPARE(feature.attribute("well_name").toString(),
               feature.attribute("well_id").toString());
    }

    // 手工维护：改优势相、加自定义列；预测优势相保持原值。
    QVERIFY(layer->startEditing());
    QVERIFY(layer->addAttribute(QgsField("expert_note", QMetaType::QString)));
    it = layer->getFeatures();
    while (it.nextFeature(feature))
      if (feature.attribute("well_id") == "W1" &&
          feature.attribute("formation") == "D61") {
        QVERIFY(layer->changeAttributeValue(
            feature.id(), layer->fields().indexOf("dominant_facies"), "水下分流河道"));
        QVERIFY(layer->changeAttributeValue(
            feature.id(), layer->fields().indexOf("expert_note"), "复核过"));
      }
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(";")));

    // 再预测一次 D62：只更新给出的列，D61 的维护值和自定义列不动。
    QVariantMap redo = formationRow("W1", "D62", 1100, 1180, "分流河道");
    redo.remove("predicted_dominant");
    redo.insert("dominant_ratio", 0.75);
    QVERIFY2(WellAttributeStore::mergeFormations(&f.catalog, f.dir.path(),
                                                 &f.layers, {redo}, &error),
             qPrintable(error));
    const auto d61 = WellAttributeStore::rows(
        f.dir.path(), nullptr, WellAttributeStore::Table::Formations, "W1", "D61");
    QCOMPARE(d61.size(), 1);
    QCOMPARE(d61.first().toMap().value("dominant_facies").toString(), QString("水下分流河道"));
    QCOMPARE(d61.first().toMap().value("predicted_dominant").toString(), QString("分流河道"));
    QCOMPARE(d61.first().toMap().value("expert_note").toString(), QString("复核过"));
    const auto d62 = WellAttributeStore::rows(
        f.dir.path(), nullptr, WellAttributeStore::Table::Formations, "W1", "D62");
    QCOMPARE(d62.size(), 1); // 更新而不是新增
    QCOMPARE(d62.first().toMap().value("dominant_facies").toString(), QString("分流河道"));
    QCOMPARE(d62.first().toMap().value("predicted_dominant").toString(), QString("河口坝"));
    QCOMPARE(d62.first().toMap().value("dominant_ratio").toDouble(), 0.75);
    QCOMPARE(WellAttributeStore::rows(f.dir.path(), nullptr,
                                      WellAttributeStore::Table::Formations)
                 .size(),
             3);
    // 井段表、因子表互不串扰。
    QVERIFY(WellAttributeStore::rows(f.dir.path(), nullptr, false).isEmpty());
    QVERIFY(WellAttributeStore::rows(f.dir.path(), nullptr, true).isEmpty());
  }
  void invalidFormationBatchDoesNotWrite() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    QString error;
    QVERIFY(WellAttributeStore::mergeFormations(
        &f.catalog, f.dir.path(), &f.layers,
        {formationRow("W1", "D61", 1000, 1100, "分流河道")}, &error));
    const auto before = WellAttributeStore::rows(
        f.dir.path(), nullptr, WellAttributeStore::Table::Formations);
    auto ratioTooBig = formationRow("W1", "D62", 1100, 1200, "河口坝");
    ratioTooBig.insert("dominant_ratio", 1.4);
    auto negative = formationRow("W1", "D62", 1100, 1200, "河口坝");
    negative.insert("dominant_thickness_md", -3.0);
    auto fractionalCode = formationRow("W1", "D62", 1100, 1200, "河口坝");
    fractionalCode.insert("facies_code", 2.5);
    for (const QVariantList &invalid :
         {QVariantList{formationRow("W1", "D62", 1100, 1200, "河口坝"),
                       formationRow("ghost", "D62", 1100, 1200, "河口坝")},
          QVariantList{formationRow("W1", "D62", 1100, 1200, "河口坝"),
                       formationRow("W1", "D62", 1200, 1300, "湖泊")}, // 同名重复
          QVariantList{formationRow("W1", "", 1100, 1200, "河口坝")},
          QVariantList{formationRow("W1", "D62", 1200, 1100, "河口坝")}, // 顶底倒置
          QVariantList{ratioTooBig}, QVariantList{negative},
          QVariantList{fractionalCode}}) {
      QVERIFY(!WellAttributeStore::mergeFormations(&f.catalog, f.dir.path(),
                                                   &f.layers, invalid, &error));
      QVERIFY(!error.isEmpty());
      QCOMPARE(WellAttributeStore::rows(f.dir.path(), nullptr,
                                        WellAttributeStore::Table::Formations),
               before);
    }
    // 手工把占比改成 >1：属性表保存被拦下，缓冲可回滚。
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           WellAttributeStore::Table::Formations,
                                           false);
    QVERIFY(layer->startEditing());
    auto it = layer->getFeatures();
    QgsFeature feature;
    while (it.nextFeature(feature))
      QVERIFY(layer->changeAttributeValue(
          feature.id(), layer->fields().indexOf("dominant_ratio"), 1.5));
    QVERIFY(!layer->commitChanges());
    QVERIFY(layer->rollBack());
    // 表正在编辑时预测不能写入；只读工程不能写。
    QVERIFY(layer->startEditing());
    QVERIFY(!WellAttributeStore::mergeFormations(
        &f.catalog, f.dir.path(), &f.layers,
        {formationRow("W1", "D62", 1100, 1200, "河口坝")}, &error));
    QVERIFY(layer->isEditable());
    QVERIFY(layer->rollBack());
    f.catalog.setLockedReadOnly(true);
    QVERIFY(!WellAttributeStore::mergeFormations(
        &f.catalog, f.dir.path(), &f.layers,
        {formationRow("W1", "D62", 1100, 1200, "河口坝")}, &error));
    QCOMPARE(WellAttributeStore::rows(f.dir.path(), nullptr,
                                      WellAttributeStore::Table::Formations),
             before);
  }
  // 数据管理的逐组预测结果 → 井段表 + 地层优势相表（组、段各一行）；
  // 面板多出「优势相」道，按钮打开同一张表。
  void faciesPublishWritesFormationDominance() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    auto source = data();
    FormationInterval group;
    group.name = "恩平组";
    group.unitType = "组";
    group.topDepth = 0;
    group.bottomDepth = 20;
    FormationInterval upper = group;
    upper.name = "恩平一段";
    upper.unitType = "段";
    upper.bottomDepth = 10;
    FormationInterval lower = group;
    lower.name = "恩平二段";
    lower.unitType = "段";
    lower.topDepth = 10;
    source.formationIntervals = {group, upper, lower};
    WellFaciesWorkflow wf;
    wf.setCatalog(&f.catalog);
    wf.setLayerService(&f.layers);
    wf.setData(source);
    WellFaciesResult prediction;
    prediction.modelName = "fixture";
    prediction.modelVersion = "v1";
    prediction.jobId = "job1";
    TextInterval a;
    a.topDepth = 0;
    a.bottomDepth = 12;
    a.text = "分流河道";
    TextInterval b = a;
    b.topDepth = 12;
    b.bottomDepth = 20;
    b.text = "河口坝";
    prediction.intervals = {a, b};
    QVERIFY2(wf.publishLithoAsset(prediction).isEmpty(), "publish failed");
    const auto rows = WellAttributeStore::rows(
        f.dir.path(), &f.layers, WellAttributeStore::Table::Formations, "W1");
    QCOMPARE(rows.size(), 3);
    QHash<QString, QVariantMap> byName;
    for (const auto &v : rows)
      byName.insert(v.toMap().value("formation").toString(), v.toMap());
    const auto &parent = byName.value("恩平组");
    QCOMPARE(parent.value("unit_type").toString(), QString("组"));
    QCOMPARE(parent.value("dominant_facies").toString(), QString("分流河道"));
    QCOMPARE(parent.value("predicted_dominant").toString(), QString("分流河道"));
    QCOMPARE(parent.value("dominant_thickness_md").toDouble(), 12.0);
    QCOMPARE(parent.value("dominant_ratio").toDouble(), 0.6);
    QCOMPARE(parent.value("coverage").toDouble(), 1.0);
    QCOMPARE(parent.value("source").toString(), QString("welllogfacies"));
    QCOMPARE(parent.value("model").toString(), QString("fixture v1"));
    QCOMPARE(parent.value("job_id").toString(), QString("job1"));
    QVERIFY(parent.value("facies_code").isNull());
    QVERIFY(parent.value("facies_composition").toString().contains("分流河道 12.00 m"));
    QCOMPARE(byName.value("恩平一段").value("dominant_facies").toString(), QString("分流河道"));
    QCOMPARE(byName.value("恩平二段").value("dominant_facies").toString(), QString("河口坝"));

    // 面板：优势相道随结果出现、可显隐；「地层优势相属性」打开同一张表。
    WellCompositePanel panel;
    auto *panelWf = new WellFaciesWorkflow(&panel);
    panelWf->setCatalog(&f.catalog);
    panelWf->setLayerService(&f.layers);
    panel.bindFaciesWorkflow(panelWf);
    QVERIFY(panel.loadWellData(source));
    QSignalSpy table(panelWf, &WellFaciesWorkflow::attributeTableRequested);
    auto *button = panel.findChild<QToolButton *>("btnWellFormations");
    QVERIFY(button);
    QTRY_VERIFY(button->isEnabled());
    button->click();
    QCOMPARE(table.size(), 1);
    QCOMPARE(table.first().first().toString(), WellAttributeStore::formationLayerId());
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
  // 砂地比物理域门：维护值组合（砂厚 25/层厚 20——各字段单独合法，属性表
  // 校验不查组合）绕过单字段校验后，提取端必须拒收越界比值井并注明原因，
  // 不静默裁剪、不带病插值；合法比值（15/20=0.75）照常参与。
  void contradictoryMaintainedThicknessRatioIsRefused() {
    WorkflowFixture f;
    QVERIFY(initFixture(f));
    QVERIFY(addWell(f, "W1"));
    QVERIFY(addWell(f, "W2"));
    QString error;
    QVariantList values{QVariantMap{{"well_id", "W1"},
                                    {"horizon", "D61"},
                                    {"log_sand_thickness_md", 25.0},
                                    {"log_layer_thickness_md", 20.0}},
                        QVariantMap{{"well_id", "W2"},
                                    {"horizon", "D61"},
                                    {"log_sand_thickness_md", 15.0},
                                    {"log_layer_thickness_md", 20.0}}};
    QVERIFY(WellAttributeStore::seedFactors(&f.catalog, f.dir.path(), &f.layers,
                                            values, &error));
    ConstraintWorkflow factors(&f.proc, &f.layers);
    factors.setCatalog(&f.catalog, f.dir.path());
    QVariantMap params{{"factorMode", "ratio"},
                       {"numeratorField", "log_sand_thickness_md"},
                       {"denominatorField", "log_layer_thickness_md"}};
    QVERIFY2(factors.extractWellFactors("D61", "sandratio", params, &error),
             qPrintable(error));
    const auto rows = factors.wellFactorRows();
    QCOMPARE(rows.size(), 2);
    const auto rowFor = [&rows](const QString &id) {
      for (const auto &v : rows)
        if (v.toMap().value("well_id").toString() == id)
          return v.toMap();
      return QVariantMap();
    };
    const QVariantMap bad = rowFor("W1");
    QVERIFY(!bad.value("contributing").toBool());
    QVERIFY2(bad.value("reason").toString().contains(QStringLiteral("[0,1]")),
             qPrintable(bad.value("reason").toString()));
    QCOMPARE(bad.value("value").toDouble(), 1.25); // 值如实展示，仅不参与插值
    const QVariantMap good = rowFor("W2");
    QVERIFY(good.value("contributing").toBool());
    QCOMPARE(good.value("value").toDouble(), 0.75);
    QVERIFY(good.value("reason").toString().isEmpty());
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
