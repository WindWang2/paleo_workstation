// G14 端到端场景 + G11 性能基线：
// 场景 A/C/D 合流——图斑归因→人工证据→冲突→反查→保存版本→工程重开仍可追溯；
// 约束编辑保存的精确变更信号；大规模点击路径的有界性与 BASELINE 计时
// （沿用 tst_mapsnapshot_scale 口径：qInfo 记录实测，不设墙钟断言——
//  不声称没有实测的数据通过性能验收，也不伪造硬门）。
#include "../src/catalog/datacatalog.h"
#include "../src/io/constraintstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/qgisconstrainteditsession.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/wellattributestore.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/mappingworkbench.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>

class TestEvidenceEndToEnd : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService project;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&project, &manifest};
    QgisProcessingService processing{&store};
    ConstraintWorkflow constraints{&processing, &layers};
    MappingWorkbench work{&layers, &processing, &project, &constraints};
    bool init() {
      if (!catalog.open(dir.path()) ||
          !project.createProject(dir.filePath("project.qgz")) ||
          !manifest.open())
        return false;
      store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"),
                            dir.filePath("project.sqlite"));
      constraints.setStore(&store);
      constraints.setCatalog(&catalog, dir.path());
      project.setDeclarationProvider(
          [this](QVector<LayerDeclaration> *out, QString *error) {
            return layers.tryDeclared(out, error);
          });
      processing.setProject(project.project());
      work.bindCatalog(&catalog, dir.path());
      return true;
    }
    QString latest(const QString &kind) {
      QString found;
      int ver = 0;
      for (const auto &row : work.products("D61")) {
        auto r = row.toMap();
        if (r.value("kind") == kind && r.value("version").toInt() > ver) {
          found = r.value("id").toString();
          ver = r.value("version").toInt();
        }
      }
      return found;
    }
  };
  // 约束会话装置（对齐 tst_constraintediting 的最小口径）。
  struct ConstraintFixture {
    QTemporaryDir dir;
    PaleoProjectStore project;
    ConstraintStore store{dir.filePath("project.gpkg"), &project};
    std::unique_ptr<QgsVectorLayer> layer;
    QString error;
    bool open() {
      project.setProjectPaths(dir.filePath("p.qgz"), store.gpkgPath(),
                              dir.filePath("meta.sqlite"));
      if (!store.appendExtended("T1", "c-1", "LINESTRING(0 0, 10 0, 20 0)",
                                "direction_line", 7,
                                "{\"semantic\":\"direction_guide\"}", 1, &error))
        return false;
      layer = std::make_unique<QgsVectorLayer>(
          store.gpkgPath() + "|layername=constraints", "constraints", "ogr");
      layer->setSubsetString("horizon='T1'");
      layer->setCustomProperty("paleoLayerId", "constraints.T1");
      return layer->isValid();
    }
    bool move(double y) {
      const auto f =
          layer->getFeature(store.load("T1").first().value("fid").toLongLong());
      auto geometry = f.geometry();
      if (!geometry.moveVertex(10, y, 1))
        return false;
      layer->beginEditCommand("move");
      const bool ok = layer->changeGeometry(f.id(), geometry);
      layer->endEditCommand();
      return ok;
    }
  };
private slots:
  void scenarioAttributionEvidenceConflictReverseAndReopen() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    // 单因素 → 阈值融合 → 转面（场景 A 的数据准备：真实计算链）。
    const auto source = f.dir.filePath("samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"facies_code", 1}},
         QVariantMap{{"x", 100}, {"y", 100}, {"facies_code", 2}}},
        &error));
    LayerDeclaration d;
    d.layerId = "wells.D61";
    d.horizon = "D61";
    d.type = "vector";
    d.source = source;
    d.title = QStringLiteral("相点");
    QVERIFY(f.layers.declare(d, &error));
    const auto polygons =
        f.work.compose("D61", {d.layerId}, QVariantMap{}, &error);
    QVERIFY2(!polygons.isEmpty(), qPrintable(error));

    // 场景 C：人工纠正——编辑副本 + 反驳证据 + 冲突分析 + 确认解释。
    const auto draft = f.work.copyForEditing(polygons, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(layer);
    QgsFeature feature;
    auto iterator = layer->getFeatures();
    QVERIFY(iterator.nextFeature(feature));
    iterator.close();
    const auto before = f.work.featureEvidence(draft, {feature.id()});
    const auto attribution = before.value("attribution").toMap();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("ok"));
    // P1#1 回归：井口落面 + 地层优势相（Formations 按 formation==horizon
    // 取行）真实接线——井意见进冲突分析。
    {
      CatalogEntity well;
      well.id = QStringLiteral("W-A");
      well.name = QStringLiteral("井A");
      well.entityType = QStringLiteral("well");
      well.hasSurface = true;
      well.surfaceX = 10;
      well.surfaceY = 10;
      well.coordinateStatus = QStringLiteral("untransformed");
      QVERIFY(f.catalog.addEntity(well));
      QString mergeError;
      QVERIFY2(WellAttributeStore::mergeFormations(
                   &f.catalog, f.dir.path(), &f.layers,
                   {QVariantMap{{"well_id", well.id},
                                {"formation", QStringLiteral("D61")},
                                {"top_md", 1000},
                                {"base_md", 1060},
                                {"facies_code",
                                 feature.attribute("facies_code").toInt() == 1 ? 2 : 1},
                                {"dominant_facies", QStringLiteral("对立相")}}},
                   &mergeError),
               qPrintable(mergeError));
    }
    const auto withWell = f.work.featureEvidence(draft, {feature.id()});
    const auto wells = withWell.value("wells_in_feature").toList();
    QCOMPARE(wells.size(), 1);
    QCOMPARE(wells.first().toMap().value("well_name").toString(),
             QStringLiteral("井A"));
    QVERIFY(!wells.first().toMap().value("formation_facies_code").isNull());
    QVERIFY(f.work.addEvidence(
        draft, {feature.id()},
        {{"text", QStringLiteral("井相与当前图斑存在冲突")},
         {"role", QStringLiteral("CONTRADICTED_BY")},
         {"type", QStringLiteral("well")},
         {"reason", QStringLiteral("保留冲突来源")}},
        &error));
    const auto mid = f.work.featureEvidence(draft, {feature.id()});
    const auto conflicts = mid.value("conflicts").toList();
    QVERIFY(!conflicts.isEmpty());
    // 井意见（formation 相编码与图斑相不同）必须被点名。
    bool wellOpinionFlagged = false;
    for (const auto &value : conflicts)
      wellOpinionFlagged |=
          value.toMap().value("evidence").toMap().value("label").toString() ==
          QStringLiteral("井A");
    QVERIFY(wellOpinionFlagged);
    // 既有 check 发现（contradicting_evidence）按 manual_dispute 并入。
    bool hasDispute = false;
    for (const auto &value : conflicts)
      hasDispute |= value.toMap().value("conflict_type").toString() ==
                    QStringLiteral("manual_dispute");
    QVERIFY(hasDispute);
    const auto revision =
        mid.value("interpretation").toMap().value("revision").toULongLong();
    QVERIFY(f.work.changeEvidence(
        draft, {feature.id()}, "review",
        {{"reason", QStringLiteral("人工复核：以井相为准重新解释")},
         {"status", QStringLiteral("Disputed")},
         {"expected_revision", revision}},
        &error));
    layer->commitChanges();
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto saved = f.work.versionForLayer(draft);
    QVERIFY(!saved.id.isEmpty());

    // 场景 D：反查——有反驳证据的图斑能被索引找回（Disputed 不在
    // 「待复核」口径内：那是 NeedsReview/Proposed/identity_review）。
    const auto rows = f.work.evidenceReverseQuery(
        QStringLiteral("contradicted"), QStringLiteral("D61"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    bool found = false;
    for (const auto &value : rows)
      found |= value.toMap().value("feature_uid").toString() ==
               mid.value("feature_uid").toString();
    QVERIFY(found);
    const auto searchRows = f.work.evidenceReverseQuery(
        QStringLiteral("search"), QStringLiteral("井相与当前图斑存在冲突"),
        &error);
    QVERIFY(!searchRows.isEmpty());

    // 工程重开（重绑目录）：版本、证据与解释在新会话仍可追溯。
    f.work.bindCatalog(nullptr, {});
    f.work.bindCatalog(&f.catalog, f.dir.path());
    const auto reopened = f.work.featureEvidence(draft, {feature.id()});
    QCOMPARE(reopened.value("feature_uid").toString(),
             mid.value("feature_uid").toString());
    QCOMPARE(reopened.value("interpretation").toMap().value("status").toString(),
             QStringLiteral("Disputed"));
    QVERIFY(!reopened.value("attribution").toMap().value("inputs").toList().isEmpty());
    QVERIFY(reopened.value("conflicts").toList().size() >= conflicts.size() - 1);
  }
  void constraintSaveEmitsPreciseChangeSignal() {
    ConstraintFixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QgisConstraintEditSession session(f.layer.get(), &f.project,
                                      [](const QString &) {});
    QString error;
    QVERIFY(session.initialize(&error));
    // 未改动的保存：不发信号（空通知会让下游把每次保存都当变化）。
    QSignalSpy saved(&session, &QgisConstraintEditSession::constraintsSaved);
    QVERIFY(session.finish(true, &error));
    QCOMPARE(saved.count(), 0);
    // 第二轮：几何变化 → 精确点名。
    QgisConstraintEditSession second(f.layer.get(), &f.project,
                                     [](const QString &) {});
    QVERIFY(second.initialize(&error));
    QVERIFY(f.move(5));
    QSignalSpy saved2(&second, &QgisConstraintEditSession::constraintsSaved);
    QVERIFY(second.finish(true, &error));
    QCOMPARE(saved2.count(), 1);
    QCOMPARE(saved2.first().at(0).toString(), QStringLiteral("T1"));
    QCOMPARE(saved2.first().at(1).toStringList(), QStringList{QStringLiteral("c-1")});
    QVERIFY(saved2.first().at(2).toStringList().isEmpty());
  }
  void clickPathIsBoundedAtScale() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    // 大网格单因素（500×500）：重放采样必须钳在上限内。
    const auto source = f.dir.filePath("big-samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}},
         QVariantMap{{"x", 5000}, {"y", 5000}, {"z", 60}}},
        &error));
    LayerDeclaration d;
    d.layerId = "wells.D61";
    d.horizon = "D61";
    d.type = "vector";
    d.source = source;
    d.title = QStringLiteral("相点");
    QVERIFY(f.layers.declare(d, &error));
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", d.layerId}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factor = f.latest("single_factor_raster");
    QVERIFY(!factor.isEmpty());
    const auto composed = f.work.compose(
        "D61", {factor}, {{"thresholds", QVariantList{20., 40.}}, {"cellSize", 10}},
        &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    qInfo().noquote() << "BASELINE compose+polygonize 500x500 grid(ms):"
                      << timer.elapsed();
    int code = 0;
    QString uid;
    QgsFeatureId fid = -1;
    {
      auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(composed));
      QVERIFY(layer);
      QgsFeature feature;
      auto it = layer->getFeatures();
      QVERIFY(it.nextFeature(feature));
      it.close();
      code = feature.attribute("facies_code").toInt();
      const int index = feature.fields().indexOf("paleo_feature_uid");
      uid = index < 0 ? QString() : feature.attribute(index).toString();
      fid = feature.id();
    }
    QVERIFY(fid >= 0);
    QVERIFY(!uid.isEmpty());
    timer.restart();
    const auto snapshot = f.work.featureEvidence(composed, {fid});
    const auto attribution = snapshot.value("attribution").toMap();
    qInfo().noquote() << "BASELINE featureEvidence click on 500x500 product(ms):"
                      << timer.elapsed();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("ok"));
    QVERIFY(attribution.value("sampled_cells").toLongLong() > 0);
    QVERIFY(attribution.value("sampled_cells").toLongLong() <= 4096);
    QVERIFY(attribution.value("bbox_cells").toLongLong() > 4096); // 抽样生效
    QVERIFY(attribution.value("sampling").toString().startsWith(QStringLiteral("strided")));
    // 快速重复选择：预算内完成且结果一致（缓存命中的稳定性）。
    timer.restart();
    for (int i = 0; i < 10; ++i) {
      const auto again = f.work.featureEvidence(composed, {fid});
      QCOMPARE(again.value("feature_uid").toString(), uid);
    }
    qInfo().noquote() << "BASELINE 10 repeated clicks(ms):" << timer.elapsed();
    // 万级井实体下的井点落面查询：一次读表 + 有界遍历。
    for (int i = 0; i < 10000; ++i) {
      CatalogEntity e;
      e.id = QStringLiteral("w%1").arg(i);
      e.name = e.id;
      e.entityType = QStringLiteral("well");
      e.hasSurface = true;
      e.surfaceX = (i % 100) * 50.0;
      e.surfaceY = (i / 100) * 50.0;
      e.coordinateStatus = QStringLiteral("untransformed");
      if (!f.catalog.addEntity(e)) {
        QVERIFY2(false, "addEntity failed at scale");
        break;
      }
    }
    timer.restart();
    const auto wellsSnapshot = f.work.featureEvidence(composed, {fid});
    qInfo().noquote() << "BASELINE wells_in_feature with 10k entities(ms):"
                      << timer.elapsed();
    const auto wells = wellsSnapshot.value("wells_in_feature").toList();
    QVERIFY(wells.size() <= 500); // 有界
    qInfo().noquote() << "BASELINE wells matched:" << wells.size();
  }
};
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath("/usr", true);
  app.initQgis();
  GDALAllRegister();
  TestEvidenceEndToEnd test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_evidencee2e.moc"
