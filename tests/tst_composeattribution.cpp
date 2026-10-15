// 图斑级综合编图归因（G03）：冻结输入版本重放确定性决策 + compose/polygonize
// 回执（G10a）。验证归因来自真实计算参数与冻结版本，而不是复制输入清单。
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/mappingworkbench.h"
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

class TestComposeAttribution : public QObject {
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
    // 样点 → 单因素 → 阈值融合 → 转面：返回相面图层 id。
    QString composedPolygons(const QVariantList &points,
                             const QVariantList &thresholds, QString *error) {
      const auto source = dir.filePath(
          QStringLiteral("samples-%1.geojson").arg(QDateTime::currentMSecsSinceEpoch()));
      if (!MappingArtifactWriter::points(source, points, error))
        return {};
      LayerDeclaration d;
      d.layerId = "wells.D61";
      d.horizon = "D61";
      d.type = "vector";
      d.source = source;
      d.title = QStringLiteral("样点");
      if (!layers.declare(d, error))
        return {};
      QVariantMap params{{"pointsLayerId", d.layerId},
                         {"field", "z"},
                         {"cellSize", 10}};
      if (!work.generateFactor("D61", "sandthick", params, error))
        return {};
      const auto factorId = latest("single_factor_raster");
      if (factorId.isEmpty())
        return {};
      return work.compose("D61", {factorId},
                          {{"thresholds", thresholds}}, error);
    }
  };
  static qint64 firstFeature(QgisLayerService *layers, const QString &id,
                             int *code, QString *uid) {
    auto *layer = qobject_cast<QgsVectorLayer *>(layers->instantiate(id));
    if (!layer)
      return -1;
    QgsFeature f;
    auto it = layer->getFeatures();
    if (it.nextFeature(f)) {
      if (code)
        *code = f.attribute("facies_code").toInt();
      if (uid) {
        const int index = f.fields().indexOf("paleo_feature_uid");
        *uid = index < 0 ? QString() : f.attribute(index).toString();
      }
      return f.id();
    }
    return -1;
  }
private slots:
  void thresholdAttributionUsesFrozenVersionsAndRealParameters() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    // z: 0/20/40/60 → 阈值 {20,40} 下四角分别落入相 1/2/2/3。
    QVariantList points{QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                        QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                        QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                        QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}};
    const auto polygons = f.composedPolygons(points, QVariantList{20., 40.}, &error);
    QVERIFY2(!polygons.isEmpty(), qPrintable(error));
    int code = 0;
    QString uid;
    const auto fid = firstFeature(&f.layers, polygons, &code, &uid);
    QVERIFY(fid >= 0);
    QVERIFY(!uid.isEmpty()); // 图斑稳定身份在场
    const auto snapshot = f.work.featureEvidence(polygons, {fid});
    const auto attribution = snapshot.value("attribution").toMap();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(attribution.value("origin").toString(), QStringLiteral("product"));
    QVERIFY(!attribution.value("calculation_version").toString().isEmpty());
    const auto params = attribution.value("calculation_parameters").toMap();
    const QVariantList expectedThresholds{20., 40.};
    QCOMPARE(params.value("thresholds").toList(),
             expectedThresholds); // 真实执行参数，不是输入清单复制
    const auto inputs = attribution.value("inputs").toList();
    QCOMPARE(inputs.size(), 1);
    const auto input = inputs.first().toMap();
    QCOMPARE(input.value("decision").toString(),
             QStringLiteral("threshold_classification"));
    QVERIFY(input.value("cells").toLongLong() > 0);
    QVERIFY(input.value("fraction").toDouble() > 0.99);
    QVERIFY(input.value("value_min").toDouble() >= 0.0);
    QVERIFY(input.value("value_max").toDouble() <= 60.0);
    // 图斑相编码必须与重放分布一致（平滑聚合可能吸收少数像元）。
    const auto distribution = attribution.value("code_distribution").toList();
    QVERIFY(!distribution.isEmpty());
    QCOMPARE(distribution.first().toMap().value("code").toInt(), code);
    QVERIFY(attribution.value("consistent_fraction").toDouble() > 0.5);
    // 采样有界。
    QVERIFY(attribution.value("sampled_cells").toLongLong() > 0);
    QVERIFY(attribution.value("sampled_cells").toLongLong() <= 4096);
    // compose 回执：融合版本记录每输入胜出像元（G10a）。
    const auto composed = f.catalog.versionById(
        attribution.value("calculation_version").toString());
    const auto receipts = composed.extra.value("compose_receipts").toMap();
    QVERIFY(receipts.value("total_cells").toLongLong() > 0);
    QCOMPARE(receipts.value("decided_cells").toLongLong(),
             receipts.value("total_cells").toLongLong());
    QCOMPARE(receipts.value("inputs").toList().size(), 1);
    QVERIFY(receipts.value("inputs").toList().first().toMap()
                .value("won_cells").toLongLong() > 0);
    // 转面回执：PROVENANCE 随版本落库。
    const auto polygonVersion = f.work.versionForLayer(polygons);
    QVERIFY(!polygonVersion.extra.value("polygonize_provenance").toString().isEmpty());

    // 输入版本更新后：旧成果归因必须继续使用保存时冻结的版本。
    QVariantList shifted{QVariantMap{{"x", 0}, {"y", 0}, {"z", 100}},
                         QVariantMap{{"x", 100}, {"y", 0}, {"z", 100}},
                         QVariantMap{{"x", 0}, {"y", 100}, {"z", 100}},
                         QVariantMap{{"x", 100}, {"y", 100}, {"z", 100}}};
    const auto source2 = f.dir.filePath("samples-shifted.geojson");
    QVERIFY(MappingArtifactWriter::points(source2, shifted, &error));
    LayerDeclaration d2;
    d2.layerId = "wells.D61";
    d2.horizon = "D61";
    d2.type = "vector";
    d2.source = source2;
    d2.title = QStringLiteral("样点-新版");
    QVERIFY(f.layers.declare(d2, &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", d2.layerId}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    // 同资产出现新版本 → 旧冻结输入成为非当前版本。
    const auto frozenInputVersion =
        inputs.first().toMap().value("version_id").toString();
    QVERIFY(f.catalog.currentVersion(
                f.catalog.versionById(frozenInputVersion).assetId).id !=
            frozenInputVersion);
    const auto again = f.work.featureEvidence(polygons, {fid});
    const auto frozen = again.value("attribution").toMap();
    QCOMPARE(frozen.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(frozen.value("inputs").toList().first().toMap()
                 .value("value_max").toDouble(),
             input.value("value_max").toDouble()); // 仍是旧值域
    QVERIFY(frozen.value("inputs").toList().first().toMap()
                .value("value_max").toDouble() < 100.0);
    // 新输入重算出全部相 3 的新成果，归因反映新版本。
    const auto newFactor = f.latest("single_factor_raster");
    const auto recomposed =
        f.work.compose("D61", {newFactor}, {{"thresholds", QVariantList{20., 40.}}},
                       &error);
    QVERIFY2(!recomposed.isEmpty(), qPrintable(error));
    int newCode = 0;
    const auto newFid = firstFeature(&f.layers, recomposed, &newCode, nullptr);
    QVERIFY(newFid >= 0);
    QCOMPARE(newCode, 3);
    const auto newAttribution =
        f.work.featureEvidence(recomposed, {newFid}).value("attribution").toMap();
    QVERIFY(newAttribution.value("inputs").toList().first().toMap()
                .value("value_min").toDouble() >= 100.0);
  }
  void nearestPointAttributionAndUnresolvedInputs() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    // 纯相点输入（无阈值）：整幅网格按最近邻归属。
    const auto source = f.dir.filePath("facies-points.geojson");
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
    const auto composed =
        f.work.compose("D61", {d.layerId}, QVariantMap{}, &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    int code = 0;
    const auto fid = firstFeature(&f.layers, composed, &code, nullptr);
    QVERIFY(fid >= 0);
    const auto attribution =
        f.work.featureEvidence(composed, {fid}).value("attribution").toMap();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("ok"));
    const auto inputs = attribution.value("inputs").toList();
    QCOMPARE(inputs.size(), 1);
    QCOMPARE(inputs.first().toMap().value("decision").toString(),
             QStringLiteral("categorical_nearest"));
    QVERIFY(inputs.first().toMap().value("nearest_distance_max").toDouble() >= 0);
    // 局部位置级（Level 3）：图斑内一点的单点决策。
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(composed));
    QVERIFY(layer);
    const auto feature = layer->getFeature(fid);
    const auto centroid = feature.geometry().centroid().asPoint();
    const auto at = f.work.composeAttributionAt(composed, feature,
                                                QPointF(centroid.x(), centroid.y()));
    QCOMPARE(at.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(at.value("code").toInt(), code);
    QVERIFY(at.value("winning_input").toMap().contains(QStringLiteral("layer_id")));
    QVERIFY(at.value("nearest_distance").toDouble() >= 0);
    // 缺失输入版本：从目录移除冻结输入的资产后，归因如实给出 unresolved
    // 与原因，不静默换用当前版本（P1#8 回归）。
    const auto polygonVersion = f.work.versionForLayer(composed);
    QVERIFY(!polygonVersion.parentVersionIds.isEmpty());
    const auto composedVersion =
        f.catalog.versionById(polygonVersion.parentVersionIds.first());
    const auto ordered = composedVersion.extra.value("ordered_inputs").toList();
    QVERIFY(!ordered.isEmpty());
    const auto inputVersionId = ordered.first().toMap().value("version_id").toString();
    const auto inputVersion = f.catalog.versionById(inputVersionId);
    QVERIFY(!inputVersion.assetId.isEmpty());
    // 目录的血缘保护拒绝删除被引用的父版本（正确行为）——用文件缺失
    // 模拟：missing 检查先于图层缓存，改名后归因必须如实报缺。
    const auto inputPath =
        DataCatalog::resolvedVersionPath(f.dir.path(), inputVersion).section('|', 0, 0);
    QVERIFY(QFile::exists(inputPath));
    QVERIFY(QFile::rename(inputPath, inputPath + QStringLiteral(".gone")));
    const auto broken = f.work.featureEvidence(composed, {fid}).value("attribution").toMap();
    QVERIFY(QFile::rename(inputPath + QStringLiteral(".gone"), inputPath));
    QCOMPARE(broken.value("status").toString(), QStringLiteral("unresolved"));
    const auto unresolved = broken.value("unresolved").toList();
    QVERIFY(!unresolved.isEmpty());
    bool namedReason = false;
    for (const auto &value : unresolved)
      namedReason |= value.toMap().value("reason").toString().contains(
          QStringLiteral("缺失"));
    QVERIFY(namedReason);
    // 恢复后归因回到 ok（缓存按版本重开）。
    const auto healed = f.work.featureEvidence(composed, {fid}).value("attribution").toMap();
    QCOMPARE(healed.value("status").toString(), QStringLiteral("ok"));
  }
  void manualDraftHasNoFabricatedAlgorithmAttribution() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QgsVectorLayer grid("Polygon?field=facies_code:integer", "grid", "memory");
    grid.setCrs(
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QgsFeatureList features;
    for (int i = 0; i < 3; ++i) {
      QgsFeature feature(grid.fields());
      feature.setAttribute("facies_code", i + 1);
      feature.setGeometry(
          QgsGeometry::fromRect(QgsRectangle(i * 100, 0, (i + 1) * 100, 100)));
      features << feature;
    }
    QVERIFY(grid.dataProvider()->addFeatures(features));
    const auto path = f.dir.filePath("manual-grid.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(&grid, path, &error));
    LayerDeclaration d;
    d.layerId = "input.grid";
    d.horizon = "D61";
    d.type = "vector";
    d.group = "05_Composition";
    d.title = "manual grid";
    d.source = path + "|layername=features";
    QVERIFY(f.layers.declare(d, &error));
    const auto draft = f.work.copyForEditing(d.layerId, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    int code = 0;
    const auto fid = firstFeature(&f.layers, draft, &code, nullptr);
    QVERIFY(fid >= 0);
    const auto attribution =
        f.work.featureEvidence(draft, {fid}).value("attribution").toMap();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("manual"));
    QVERIFY(!attribution.contains(QStringLiteral("inputs")));
  }
};
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath("/usr", true);
  app.initQgis();
  GDALAllRegister();
  TestComposeAttribution test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_composeattribution.moc"
