// 图斑级综合编图归因（G03）：冻结输入版本重放确定性决策 + compose/polygonize
// 回执（G10a）。验证归因来自真实计算参数与冻结版本，而不是复制输入清单。
#include "../src/catalog/datacatalog.h"
#include "../src/domain/singlefactorrequest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/composeattribution.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/mappingworkbench.h"
#include "helpers/simulatemissingfile.h"
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsrasterblock.h>
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
  // 登记一个相栅格版本，返回版本 id（调用方须 QTRY 等 product.* 声明同步）。
  static QString stageRasterInput(Fixture *f, const QString &name,
                                  const QVector<int> &cells, int cols,
                                  int rows, const QRectF &extent,
                                  QString *error) {
    DerivedAssetRegistrar registrar(&f->catalog, f->dir.path());
    const auto staged =
        registrar.stage("facies_raster", name, name + ".tif", error);
    if (!staged.isValid())
      return {};
    if (!MappingArtifactWriter::raster(staged.absolutePath, cells, cols, rows,
                                       extent, error))
      return {};
    if (!registrar.commit(staged, {}, "fixture",
                          {{"mapping_product", true},
                           {"horizon", "D61"},
                           {"layer_type", "raster"},
                           {"title", name}},
                          error))
      return {};
    return staged.versionId;
  }
  // 与工作台解析器同契约的测试侧冻结输入展开：独立打开图层，验证
  // replay/replayGrid 不依赖 MappingWorkbench 的内部缓存。
  static QList<paleo::attribution::ResolvedInput>
  frozenInputs(DataCatalog *catalog, const QString &dir,
               const QString &composedId) {
    const auto composed = catalog->versionById(composedId);
    QList<paleo::attribution::ResolvedInput> out;
    for (const auto &entryValue :
         composed.extra.value("ordered_inputs").toList()) {
      const auto entry = entryValue.toMap();
      paleo::attribution::ResolvedInput in;
      in.layerId = entry.value("layer_id").toString();
      in.versionId = entry.value("version_id").toString();
      in.priority = entry.value("priority").toInt();
      const auto v = catalog->versionById(in.versionId);
      const auto path = DataCatalog::resolvedVersionPath(dir, v) +
                        v.extra.value("source_suffix").toString();
      const auto file = path.section('|', 0, 0);
      in.raster = v.extra.value("layer_type").toString() ==
                      QLatin1String("raster") ||
                  file.endsWith(QLatin1String(".tif"), Qt::CaseInsensitive) ||
                  file.endsWith(QLatin1String(".tiff"), Qt::CaseInsensitive);
      const auto kind = v.extra.value("kind").toString();
      in.continuous =
          in.layerId.startsWith(QLatin1String("factor.")) ||
          v.extra.contains("factor_id") ||
          paleo::singlefactor::isAnalysisFactorRaster(
              kind, v.extra.value("value_source").toString());
      std::unique_ptr<QgsMapLayer> layer =
          in.raster
              ? std::unique_ptr<QgsMapLayer>(new QgsRasterLayer(
                    path, QStringLiteral("frozen"), QStringLiteral("gdal")))
              : std::unique_ptr<QgsMapLayer>(new QgsVectorLayer(
                    path, QStringLiteral("frozen"), QStringLiteral("ogr")));
      if (layer->isValid()) {
        in.layer = layer.get();
        in.hold = std::shared_ptr<QgsMapLayer>(std::move(layer));
      } else {
        in.missing = true;
        in.missingReason = QStringLiteral("无法打开冻结输入");
      }
      out << in;
    }
    return out;
  }
  // 逐像元比对重放网格与成果栅格（nodata ↔ -9999）。
  static void expectGridEqualsRaster(
      const paleo::attribution::GridReplay &replay, QgsRasterLayer *raster,
      const char *context) {
    std::unique_ptr<QgsRasterBlock> block(raster->dataProvider()->block(
        1, raster->extent(), raster->width(), raster->height()));
    QVERIFY2(block && block->isValid(), context);
    QCOMPARE(replay.cols, raster->width());
    QCOMPARE(replay.rows, raster->height());
    QCOMPARE(int(replay.codes.size()),
             int(raster->width()) * raster->height());
    for (int i = 0; i < replay.codes.size(); ++i) {
      const int expected = block->isNoData(i)
                               ? -9999
                               : int(std::round(block->value(i)));
      QVERIFY2(replay.codes[i] == expected,
               qPrintable(QStringLiteral("%1: cell %2 replay=%3 raster=%4")
                              .arg(context)
                              .arg(i)
                              .arg(replay.codes[i])
                              .arg(expected)));
    }
  }
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
    // z: 0/20/40/60 → 阈值 {20,25,30,35,40,45} 下四角分别落入相 1/2/6/7（默认七微相分类库）。
    QVariantList points{QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                        QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                        QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                        QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}};
    const auto polygons = f.composedPolygons(points, QVariantList{20., 25., 30., 35., 40., 45.}, &error);
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
    const QVariantList expectedThresholds{20., 25., 30., 35., 40., 45.};
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
    // 新输入重算出全部相 7 的新成果，归因反映新版本。
    const auto newFactor = f.latest("single_factor_raster");
    const auto recomposed =
        f.work.compose("D61", {newFactor}, {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
                       &error);
    QVERIFY2(!recomposed.isEmpty(), qPrintable(error));
    int newCode = 0;
    const auto newFid = firstFeature(&f.layers, recomposed, &newCode, nullptr);
    QVERIFY(newFid >= 0);
    QCOMPARE(newCode, 7);
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
    // Windows 上打开中的 GPKG 句柄会令 rename 失败（Linux 可行），该
    // 平台跳过本段（主开发平台为 Linux，回归覆盖在 Linux CI）。
#ifndef Q_OS_WIN
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
#endif
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
  // T1/T2 共用的纯相点 compose：返回相面图层 id；T1 验证冻结网格逐像元
  // 等值重放，T2 验证无冻结记录时重建口径与 composeRaster 一致。
  void frozenGridLocksComposeReplay() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("facies-points-gf.geojson");
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
    const auto polygonVersion = f.work.versionForLayer(composed);
    QVERIFY(!polygonVersion.parentVersionIds.isEmpty());
    const auto composedV =
        f.catalog.versionById(polygonVersion.parentVersionIds.first());
    QCOMPARE(composedV.extra.value("kind").toString(),
             QStringLiteral("composed_facies"));
    // X2：成果版本冻结了保存时网格。
    const auto gridExtra = composedV.extra.value("compose_grid").toMap();
    QVERIFY(!gridExtra.isEmpty());
    const auto rasterPath =
        DataCatalog::resolvedVersionPath(f.dir.path(), composedV) +
        composedV.extra.value("source_suffix").toString();
    QgsRasterLayer raster(rasterPath, QStringLiteral("composed"),
                          QStringLiteral("gdal"));
    QVERIFY(raster.isValid());
    QCOMPARE(gridExtra.value("cols").toInt(), raster.width());
    QCOMPARE(gridExtra.value("rows").toInt(), raster.height());
    const auto ext = gridExtra.value("extent").toList();
    QCOMPARE(ext.size(), 4);
    QVERIFY(qAbs(ext[0].toDouble() - raster.extent().xMinimum()) < 1e-9);
    QVERIFY(qAbs(ext[1].toDouble() - raster.extent().yMinimum()) < 1e-9);
    QVERIFY(qAbs(ext[2].toDouble() - raster.extent().xMaximum()) < 1e-9);
    QVERIFY(qAbs(ext[3].toDouble() - raster.extent().yMaximum()) < 1e-9);
    const auto inputs = frozenInputs(&f.catalog, f.dir.path(), composedV.id);
    QCOMPARE(inputs.size(), 1);
    QVERIFY(!inputs.first().missing);
    const auto frozen =
        paleo::attribution::frozenGridFromExtra(composedV.extra);
    QVERIFY(frozen.valid());
    const QList<double> thresholds;
    // T1：冻结网格 → 逐像元与成果栅格同值。
    const auto replay = paleo::attribution::replayGrid(
        inputs, composedV.extra.value("facies").toList(), thresholds,
        4 * 1024 * 1024, frozen);
    QVERIFY2(replay.valid, "frozen grid replay must be valid");
    QCOMPARE(replay.gridSource, QStringLiteral("frozen"));
    expectGridEqualsRaster(replay, &raster, "frozen replay");
    // T2：无冻结记录 → 按 composeRaster 规则重建同一网格（旧 gridOf 无
    // 条件 grow(100) 会得到 [-100,200]²，本断言在旧实现下必败）。
    const auto rebuilt = paleo::attribution::replayGrid(
        inputs, composedV.extra.value("facies").toList(), thresholds,
        4 * 1024 * 1024, paleo::attribution::FrozenGrid{});
    QVERIFY2(rebuilt.valid, "reconstructed grid replay must be valid");
    QCOMPARE(rebuilt.gridSource, QStringLiteral("reconstructed"));
    QCOMPARE(rebuilt.cols, raster.width());
    QCOMPARE(rebuilt.rows, raster.height());
    QCOMPARE(rebuilt.extent, raster.extent());
    expectGridEqualsRaster(rebuilt, &raster, "reconstructed replay");
    // 图斑归因上报 frozen/exact。
    const auto fid = firstFeature(&f.layers, composed, nullptr, nullptr);
    QVERIFY(fid >= 0);
    const auto attribution =
        f.work.featureEvidence(composed, {fid}).value("attribution").toMap();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(attribution.value("grid_source").toString(),
             QStringLiteral("frozen"));
    QCOMPARE(attribution.value("fidelity").toString(),
             QStringLiteral("exact"));
  }
  // T3：网格不对齐的第二栅格输入如实标 resampled / approximate。
  void resampledInputHonestFidelity() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    // A：10×10 定网格，左上角 3×3 像元 nodata；B：5 米像元、满幅 code2，
    // 与 A 网格不对齐 → block 重采样，赢得 A 的 nodata 区。
    QVector<int> a(100, 1);
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x)
        a[y * 10 + x] = -9999;
    const auto vA = stageRasterInput(&f, QStringLiteral("rasterA"), a, 10, 10,
                                     QRectF(0, 0, 100, 100), &error);
    QVERIFY2(!vA.isEmpty(), qPrintable(error));
    const auto vB = stageRasterInput(&f, QStringLiteral("rasterB"),
                                     QVector<int>(400, 2), 20, 20,
                                     QRectF(0, 0, 100, 100), &error);
    QVERIFY2(!vB.isEmpty(), qPrintable(error));
    const auto idA = QStringLiteral("product.%1").arg(vA);
    const auto idB = QStringLiteral("product.%1").arg(vB);
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.declaration(idA).layerId.isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.declaration(idB).layerId.isEmpty(), 5000);
    const auto composed =
        f.work.compose("D61", {idA, idB}, QVariantMap{}, &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    const auto polygonVersion = f.work.versionForLayer(composed);
    const auto composedV =
        f.catalog.versionById(polygonVersion.parentVersionIds.first());
    const auto inputs = frozenInputs(&f.catalog, f.dir.path(), composedV.id);
    const auto frozen =
        paleo::attribution::frozenGridFromExtra(composedV.extra);
    QVERIFY(frozen.valid());
    const QList<double> thresholds;
    const auto replay = paleo::attribution::replayGrid(
        inputs, composedV.extra.value("facies").toList(), thresholds,
        4 * 1024 * 1024, frozen);
    QVERIFY(replay.valid);
    const auto rasterPath =
        DataCatalog::resolvedVersionPath(f.dir.path(), composedV) +
        composedV.extra.value("source_suffix").toString();
    QgsRasterLayer raster(rasterPath, QStringLiteral("composed"),
                          QStringLiteral("gdal"));
    QVERIFY(raster.isValid());
    expectGridEqualsRaster(replay, &raster, "resampled replay");
    QCOMPARE(replay.alignments.value(0), QStringLiteral("grid_defining"));
    QCOMPARE(replay.alignments.value(1), QStringLiteral("resampled"));
    QCOMPARE(replay.fidelity, QStringLiteral("approximate"));
    QVERIFY(!replay.fidelityReasons.isEmpty());
    // B 在 nodata 区内赢过像元：找覆盖 (10,90) 的图斑验证输入口径行。
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(composed));
    QVERIFY(layer);
    QgsFeature feature;
    auto it = layer->getFeatures();
    bool found = false;
    while (it.nextFeature(feature)) {
      if (feature.geometry().contains(10, 90)) {
        found = true;
        break;
      }
    }
    QVERIFY(found);
    const auto attribution =
        f.work.featureEvidence(composed, {feature.id()})
            .value("attribution")
            .toMap();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(attribution.value("fidelity").toString(),
             QStringLiteral("approximate"));
    QVERIFY(!attribution.value("fidelity_reasons").toList().isEmpty());
    QStringList alignments;
    for (const auto &v : attribution.value("inputs").toList())
      alignments << v.toMap().value("alignment").toString();
    QVERIFY(alignments.contains(QStringLiteral("grid_defining")));
    QVERIFY(alignments.contains(QStringLiteral("resampled")));
  }
  // T4：单次归因解析 9 个冻结输入（LRU 上限 8），驱逐不得悬垂在用图层；
  // 预先塞满缓存后 compareVersions 仍正常。
  void attributionCacheEvictionKeepsInputsAlive() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QStringList ids;
    for (int i = 0; i < 9; ++i) {
      const auto vid = stageRasterInput(
          &f, QStringLiteral("raster%1").arg(i), QVector<int>{1, 1, 1, 1}, 2, 2,
          QRectF(0, 0, 100, 100), &error);
      QVERIFY2(!vid.isEmpty(), qPrintable(error));
      ids << QStringLiteral("product.%1").arg(vid);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.declaration(ids.last()).layerId.isEmpty(),
                             5000);
    const auto composed = f.work.compose("D61", ids, QVariantMap{}, &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    const auto fid = firstFeature(&f.layers, composed, nullptr, nullptr);
    QVERIFY(fid >= 0);
    // 一次归因解析 9 个输入 → 必然触发 LRU 驱逐；修复前被驱逐的是任意
    // QHash 序条目的裸指针 delete，仍在 inputs 里的指针悬垂。
    const auto attribution =
        f.work.featureEvidence(composed, {fid}).value("attribution").toMap();
    QCOMPARE(attribution.value("status").toString(), QStringLiteral("ok"));
    QVERIFY(attribution.value("unresolved").toList().isEmpty());
    QCOMPARE(attribution.value("inputs").toList().size(), 9);
    // 缓存已被 9 个版本挤占：再对其中两个输入版本做 compareVersions。
    const auto polygonVersion = f.work.versionForLayer(composed);
    const auto composedV =
        f.catalog.versionById(polygonVersion.parentVersionIds.first());
    const auto ordered = composedV.extra.value("ordered_inputs").toList();
    const auto v0 = ordered.first().toMap().value("version_id").toString();
    const auto v1 = ordered.last().toMap().value("version_id").toString();
    const auto diff = f.work.compareVersions(v0, v1, &error);
    QVERIFY2(diff.contains(QStringLiteral("cells_changed")),
             qPrintable(error));
  }
  // T5：单点重放按像元中心决策并如实报告 cell/query_point。
  void pointReplaySnapsToCellCenter() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto vA = stageRasterInput(&f, QStringLiteral("gridRaster"),
                                     QVector<int>{1, 1, 2, 2}, 2, 2,
                                     QRectF(0, 0, 100, 100), &error);
    QVERIFY2(!vA.isEmpty(), qPrintable(error));
    const auto idA = QStringLiteral("product.%1").arg(vA);
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.declaration(idA).layerId.isEmpty(), 5000);
    const auto composed = f.work.compose("D61", {idA}, QVariantMap{}, &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(composed));
    QVERIFY(layer);
    QgsFeature feature;
    auto it = layer->getFeatures();
    QVERIFY(it.nextFeature(feature));
    // (49.9, 50.1) 落在像元 (0,0)（中心 25,75），cell 原点取像元中心。
    const auto at =
        f.work.composeAttributionAt(composed, feature, QPointF(49.9, 50.1));
    QCOMPARE(at.value("status").toString(), QStringLiteral("ok"));
    const auto cell = at.value("cell").toMap();
    QCOMPARE(cell.value("col").toInt(), 0);
    QCOMPARE(cell.value("row").toInt(), 0);
    QVERIFY(qAbs(cell.value("x").toDouble() - 25.0) < 1e-9);
    QVERIFY(qAbs(cell.value("y").toDouble() - 75.0) < 1e-9);
    const auto query = at.value("query_point").toMap();
    QVERIFY(qAbs(query.value("x").toDouble() - 49.9) < 1e-9);
    QVERIFY(qAbs(query.value("y").toDouble() - 50.1) < 1e-9);
    // 与成果栅格该像元同值。
    const auto polygonVersion = f.work.versionForLayer(composed);
    const auto composedV =
        f.catalog.versionById(polygonVersion.parentVersionIds.first());
    const auto rasterPath =
        DataCatalog::resolvedVersionPath(f.dir.path(), composedV) +
        composedV.extra.value("source_suffix").toString();
    QgsRasterLayer raster(rasterPath, QStringLiteral("composed"),
                          QStringLiteral("gdal"));
    QVERIFY(raster.isValid());
    std::unique_ptr<QgsRasterBlock> block(raster.dataProvider()->block(
        1, raster.extent(), raster.width(), raster.height()));
    QVERIFY(block && block->isValid());
    QCOMPARE(at.value("code").toInt(), int(std::round(block->value(0))));
    // 网格外：如实 unresolved。
    const auto outside = f.work.composeAttributionAt(composed, feature,
                                                     QPointF(500, 500));
    QCOMPARE(outside.value("status").toString(), QStringLiteral("unresolved"));
    QVERIFY(outside.value("message").toString().contains(
        QStringLiteral("网格之外")));
  }
  // T6：同一缺失输入在三处（图斑归因 / 单点归因 / 影响分析）报同一原因。
  void missingFrozenInputReportsIdenticalReason() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto vA = stageRasterInput(&f, QStringLiteral("goneRaster"),
                                     QVector<int>{1, 1, 1, 1}, 2, 2,
                                     QRectF(0, 0, 100, 100), &error);
    QVERIFY2(!vA.isEmpty(), qPrintable(error));
    const auto idA = QStringLiteral("product.%1").arg(vA);
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.declaration(idA).layerId.isEmpty(), 5000);
    const auto composed = f.work.compose("D61", {idA}, QVariantMap{}, &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    const auto fid = firstFeature(&f.layers, composed, nullptr, nullptr);
    QVERIFY(fid >= 0);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(composed));
    QVERIFY(layer);
    const auto feature = layer->getFeature(fid);
    const auto polygonVersion = f.work.versionForLayer(composed);
    const auto composedV =
        f.catalog.versionById(polygonVersion.parentVersionIds.first());
    const auto ordered = composedV.extra.value("ordered_inputs").toList();
    const auto inputV = ordered.first().toMap().value("version_id").toString();
    const auto inputLayerId =
        ordered.first().toMap().value("layer_id").toString();
    const auto inputVersion = f.catalog.versionById(inputV);
    const auto inputPath =
        DataCatalog::resolvedVersionPath(f.dir.path(), inputVersion)
            .section('|', 0, 0);
    QVERIFY(QFile::exists(inputPath));
    PALEO_REQUIRE_FILE_GONE(SimulateMissingFile::rename(
        f.project.project(), inputPath, inputPath + QStringLiteral(".gone")));
    const auto expected =
        QStringLiteral("冻结输入文件缺失（版本 %1）").arg(inputV);
    const auto reasonFor = [](const QVariantList &unresolved,
                              const QString &layerId) -> QString {
      for (const auto &value : unresolved) {
        const auto row = value.toMap();
        if (row.value("layer_id").toString() == layerId)
          return row.value("reason").toString();
      }
      return {};
    };
    const auto attribution =
        f.work.featureEvidence(composed, {fid}).value("attribution").toMap();
    QCOMPARE(reasonFor(attribution.value("unresolved").toList(), inputLayerId),
             expected);
    const auto at =
        f.work.composeAttributionAt(composed, feature, QPointF(50, 50));
    QCOMPARE(reasonFor(at.value("unresolved").toList(), inputLayerId),
             expected);
    const auto impact = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", inputV},
         {"after_version_id", inputV}},
        &error);
    QString impactReason;
    for (const auto &value : impact.value("unresolved").toList()) {
      const auto row = value.toMap();
      for (const auto &nested : row.value("reason").toList()) {
        const auto inner = nested.toMap();
        if (inner.value("layer_id").toString() == inputLayerId)
          impactReason = inner.value("reason").toString();
      }
    }
    QCOMPARE(impactReason, expected);
    QVERIFY(QFile::rename(inputPath + QStringLiteral(".gone"), inputPath));
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
