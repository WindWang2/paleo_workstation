#include "catalog/datacatalog.h"
#include "domain/faciescatalog.h"
#include "domain/facieshierarchy.h"
#include "domain/faciestaxonomy.h"
#include "domain/mappinghorizons.h"
#include "domain/paleomapfacies.h"
#include "metadata/layermanifest.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/facieshierarchyrenderer.h"
#include "qgis/faciesmaplegend.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprocessingservice.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/qgisruntime.h"
#include "io/paleomapfiles.h"
#include "services/paleotaskservice.h"
#include "workflow/mappingworkbench.h"
#include "workflow/paleomaplibrary.h"
#include "workflow/workflows.h"
#include "ui/paleotheme.h"
#include <QEventLoop>
#include <QFile>
#include <QIcon>
#include <QJsonDocument>
#include <QPainter>
#include <QTimer>
#include <QtTest>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

class TestProjectPaleomaps : public QObject {
  Q_OBJECT
  static QImage mapImage(QgsVectorLayer *layer, bool detail = false) {
    QgsMapSettings settings;
    settings.setLayers({layer}); settings.setDestinationCrs(layer->crs());
    auto extent = layer->extent(); extent.scale(detail ? 0.35 : 1.08);
    settings.setExtent(extent); settings.setOutputSize(QSize(1000, 1200));
    settings.setOutputDpi(120); settings.setBackgroundColor(Qt::white);
    QgsMapRendererSequentialJob job(settings); job.start(); job.waitForFinished();
    return job.renderedImage();
  }
  static bool preview(QgsVectorLayer *layer, const QString &horizon, const QString &path, bool detail = false) {
    const auto map = mapImage(layer, detail);
    if (map.isNull()) return false;
    QImage paper(1440, 1280, QImage::Format_ARGB32_Premultiplied); paper.fill(Qt::white);
    QPainter painter(&paper); painter.setPen(QColor("#24303E"));
    painter.setFont(QFont("Noto Sans SC", 14));
    painter.drawText(24, 34, horizon + (detail ? QStringLiteral(" 已有古地理图 · 局部纹理") : QStringLiteral(" 已有古地理图 · 工区三级相")));
    painter.drawImage(16, 58, map);
    painter.setFont(QFont("Noto Sans SC", 10));
    const auto entries = FaciesMapLegend::symbolEntries(layer, layer->customProperty("paleo/faciesDisplayLegend").toList());
    int y = 90;
    for (const auto &entry : entries) {
      const auto row = entry.toMap();
      row.value("legend_icon").value<QIcon>().paint(&painter, QRect(1040, y, 40, 28));
      painter.drawText(1092, y + 21, row.value("name").toString());
      const auto hierarchy = row.value("hierarchy_path").toStringList();
      painter.setFont(QFont("Noto Sans SC", 8));
      painter.drawText(1040, y + 47, hierarchy.mid(0, 2).join(QStringLiteral(" / ")));
      painter.setFont(QFont("Noto Sans SC", 10)); y += 80;
    }
    painter.end(); return paper.save(path);
  }
private slots:
  void verifyUpdatedProject() {
    const auto root = qEnvironmentVariable("PALEO_PALEOMAP_VERIFY_PROJECT");
    if (root.isEmpty()) QSKIP("Set PALEO_PALEOMAP_VERIFY_PROJECT for installed project acceptance");
    DataCatalog catalog; catalog.setLockedReadOnly(true); QString error;
    QVERIFY2(catalog.open(root, &error), qPrintable(error));
    QVERIFY(!catalog.recoveredFromBackup());
    QgsProject project; QVERIFY(project.read(root + "/paleo_data.qgz"));
    for (auto *layer : project.mapLayers()) QVERIFY2(layer->isValid(), qPrintable(layer->source()));
    QHash<QString, QVariantList> schemas;
    for (const auto &asset : catalog.assets())
      if (asset.type == "facies_schema") {
        const auto version = catalog.currentVersion(asset.id);
        schemas[version.extra.value("horizon").toString()] = version.extra.value("facies").toList();
      }
    for (const auto &horizon : mappingHorizons()) QCOMPARE(schemas.value(horizon).size(), 10);
    int maps = 0, features = 0;
    for (const auto &asset : catalog.assets()) {
      const auto version = catalog.currentVersion(asset.id);
      if (asset.type != "paleogeographic_map" || !version.extra.value("polygon").toBool()) continue;
      QCOMPARE(version.stage, QString("DERIVED"));
      const auto path = DataCatalog::resolvedVersionPath(root, version);
      QCOMPARE(DataCatalog::sha256FileHex(path), version.sha256);
      const auto id = version.extra.value("layer_id").toString();
      QgsVectorLayer *vector = nullptr;
      for (auto *layer : project.mapLayers())
        if (layer->customProperty("paleoLayerId").toString() == id) vector = qobject_cast<QgsVectorLayer *>(layer);
      QVERIFY(vector);
      QCOMPARE(vector->customProperty("paleo/faciesResolvedLevel").toString(), QString("micro_facies"));
      const auto schema = schemas.value(version.extra.value("horizon").toString());
      auto iterator = vector->getFeatures(); QgsFeature feature;
      while (iterator.nextFeature(feature)) {
        const auto leaf = FaciesCatalog::find(schema, feature.attribute("facies_code"));
        QVERIFY(FaciesTaxonomy::complete(leaf));
        QCOMPARE(feature.attribute("microfacies").toString(), leaf.value("microfacies").toString());
        QCOMPARE(feature.attribute("texture").toString(), leaf.value("texture").toString());
        ++features;
      }
      const auto image = mapImage(vector);
      int ink = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) ink += image.pixelColor(x, y).lightness() < 240;
      QVERIFY(ink > 10000);
      ++maps;
    }
    QCOMPARE(maps, 3); QCOMPARE(features, 32);
    qInfo() << "VERIFIED_PROJECT_HISTORICAL_MAPS" << maps << "FEATURES" << features << "LAYERS" << project.mapLayers().size();
  }
  void normalizeProjectMaps() {
    const auto root = qEnvironmentVariable("PALEO_PALEOMAP_REPAIR_PROJECT");
    if (root.isEmpty()) QSKIP("Explicit staging project required for historical-map migration");
    DataCatalog catalog; QString error;
    QVERIFY2(catalog.open(root, &error), qPrintable(error));
    QgisProjectService project;
    QVERIFY(project.createProject(root + "/normalized.qgs"));
    LayerManifest manifest(root + "/project.sqlite"); QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    PaleoProjectStore store;
    store.setProjectPaths(project.projectPath(), root + "/project.gpkg", root + "/project.sqlite");
    QgisProcessingService processing(&store);
    ConstraintWorkflow constraints(&processing, &layers);
    constraints.setStore(&store); constraints.setCatalog(&catalog, root);
    MappingWorkbench work(&layers, &processing, &project, &constraints);
    work.bindCatalog(&catalog, root);
    QStringList assets;
    QVariantList classes;
    QHash<QString, QList<QgsGeometry>> geometry;
    QHash<QString, CatalogVersion> originals;
    for (const auto &asset : catalog.assets()) {
      const auto version = catalog.currentVersion(asset.id);
      if (asset.type != "paleogeographic_map" || !version.extra.value("polygon").toBool()) continue;
      QCOMPARE(version.stage, QString("RAW"));
      assets << asset.id; originals[asset.id] = version;
      QgsVectorLayer source(DataCatalog::resolvedVersionPath(root, version), "original", "ogr");
      QVERIFY(source.isValid());
      auto features = source.getFeatures(); QgsFeature feature;
      while (features.nextFeature(feature)) {
        classes << QVariantMap{{"facies", feature.attribute("facies")}, {"subfacies", feature.attribute("subfacies")},
                               {"microfacies", feature.attribute("microfac")}};
        geometry[asset.id] << feature.geometry();
      }
    }
    QCOMPARE(assets.size(), 3);
    const auto originalSchema = work.facies("D61");
    const auto resolution = PaleoMapFacies::resolve(originalSchema, classes, &error);
    QVERIFY2(!resolution.isEmpty(), qPrintable(error));
    const auto schema = resolution.value("facies").toList();
    QCOMPARE(schema.size(), 10);
    for (const auto &original : originalSchema) {
      const auto row = original.toMap();
      QCOMPARE(FaciesTaxonomy::resolve(schema, FaciesHierarchy::path(row).join(" / ")).value("code"), row.value("code"));
    }
    for (const auto &horizon : mappingHorizons())
      QVERIFY2(work.saveFacies(horizon, schema, &error), qPrintable(error));
    PaleoTaskService tasks;
    PaleoMapLibrary::Request request; request.assetIds = assets;
    PaleoMapLibrary::Result result; QEventLoop loop; bool completed = false;
    auto *task = PaleoMapLibrary::importMaps(&catalog, &layers, &tasks, request, &loop,
        [&](const auto &value) { result = value; completed = true; loop.quit(); }, &error);
    QVERIFY2(task, qPrintable(error));
    QTimer timeout; timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit); timeout.start(120000); loop.exec();
    QVERIFY(completed); QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join("\n")));
    QCOMPARE(result.assets.size(), 3);
    QVariantList report;
    const QString directory = root + "/historical_facies_map";
    for (const auto &assetId : assets) {
      const auto version = catalog.currentVersion(assetId);
      QCOMPARE(version.stage, QString("DERIVED"));
      QCOMPARE(version.extra.value("source_microfacies_field").toString(), QString("microfac"));
      const QString horizon = version.extra.value("horizon").toString();
      const QString layerId = version.extra.value("layer_id").toString();
      QVERIFY(work.setDisplayMode(layerId, "micro_facies", &error));
      QVERIFY(catalog.updateVersionExtra(version.id, "display_mode", QStringLiteral("micro_facies"), &error));
      auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(layerId, &error));
      QVERIFY2(layer, qPrintable(error));
      QCOMPARE(layer->featureCount(), geometry.value(assetId).size());
      auto features = layer->getFeatures(); QgsFeature feature; int index = 0;
      QVariantList mappings;
      while (features.nextFeature(feature)) {
        auto before = geometry.value(assetId).at(index++), after = feature.geometry();
        before.convertToMultiType(); after.convertToMultiType();
        QVERIFY(before.equals(after));
        const auto leaf = FaciesCatalog::find(schema, feature.attribute("facies_code"));
        QCOMPARE(feature.attribute("facies_name").toString(), leaf.value("facies").toString());
        QCOMPARE(feature.attribute("subfacies").toString(), leaf.value("subfacies").toString());
        QCOMPARE(feature.attribute("microfacies").toString(), leaf.value("microfacies").toString());
        QCOMPARE(feature.attribute("texture").toString(), leaf.value("texture").toString());
        QCOMPARE(feature.attribute("color").toString(), leaf.value("color").toString());
        QVERIFY(!FaciesCatalog::fillPath(feature.attribute("texture").toString()).isEmpty());
        mappings << QVariantMap{{"old_code", feature.attribute("source_code")},
                                {"code", feature.attribute("facies_code")},
                                {"path", FaciesHierarchy::path(leaf)}, {"texture", leaf.value("texture")}};
      }
      const auto basename = horizon + "_historical_facies_map";
      QVERIFY(QFile::copy(DataCatalog::resolvedVersionPath(root, version), directory + "/" + basename + ".gpkg"));
      bool saved = false; layer->saveNamedStyle(directory + "/" + basename + ".gpkg.qml", saved); QVERIFY(saved);
      // Preserve the SHP geometry and format; update only classification attributes.
      QgsVectorLayer original(directory + "/" + basename + ".shp", "updated-original", "ogr");
      QVERIFY(original.isValid()); QVERIFY(original.startEditing());
      QVERIFY(original.addAttribute(QgsField("src_code", QMetaType::Type::Int)));
      QVERIFY(original.addAttribute(QgsField("texture", QMetaType::Type::QString, "", 128)));
      auto oldFeatures = original.getFeatures(); QgsFeature oldFeature;
      while (oldFeatures.nextFeature(oldFeature)) {
        const auto resolved = PaleoMapFacies::resolve(schema, {QVariantMap{{"facies", oldFeature.attribute("facies")},
            {"subfacies", oldFeature.attribute("subfacies")}, {"microfacies", oldFeature.attribute("microfac")}}}, &error);
        QVERIFY2(!resolved.isEmpty(), qPrintable(error));
        const auto code = resolved.value("codes").toList().first();
        const auto leaf = FaciesCatalog::find(schema, code);
        QVariantMap values{{"src_code", oldFeature.attribute("code")}, {"code", code}, {"facies_cod", code},
            {"name", leaf.value("name")}, {"microfac", leaf.value("microfacies")}, {"subfacies", leaf.value("subfacies")},
            {"facies", leaf.value("facies")}, {"color", leaf.value("color")}, {"texture", leaf.value("texture")}};
        for (auto it = values.cbegin(); it != values.cend(); ++it)
          QVERIFY(original.changeAttributeValue(oldFeature.id(), original.fields().indexOf(it.key()), it.value()));
      }
      QVERIFY2(original.commitChanges(), qPrintable(original.commitErrors().join("\n")));
      original.addExpressionField("\"facies_cod\"", QgsField("facies_code", QMetaType::Type::Int));
      FaciesHierarchyRenderer::apply(&original, schema, "micro_facies");
      original.saveNamedStyle(directory + "/" + basename + ".qml", saved); QVERIFY(saved);
      QStringList images{basename + "_preview.png"};
      if (horizon == "D61") images << basename + "_topology_detail.png";
      for (const auto &image : images) {
        const auto path = directory + "/" + image;
        QVERIFY(preview(layer, horizon, path, image.contains("detail")));
        CatalogAsset imageAsset;
        for (const auto &asset : catalog.assets())
          if (asset.type == "paleogeographic_map" && asset.displayName == image) imageAsset = asset;
        QVERIFY(!imageAsset.id.isEmpty());
        const auto previous = catalog.currentVersion(imageAsset.id);
        CatalogVersion rendered; rendered.id = catalog.nextVersionId(); rendered.assetId = imageAsset.id;
        rendered.versionNumber = previous.versionNumber + 1; rendered.stage = "DERIVED"; rendered.managed = true;
        rendered.fileName = image; rendered.sourceUri = previous.sourceUri;
        rendered.path = "artifacts/" + DataCatalog::managedPath("derived", imageAsset.id, rendered.id, image);
        const auto destination = root + "/" + rendered.path;
        QVERIFY(QDir().mkpath(QFileInfo(destination).absolutePath())); QVERIFY(QFile::copy(path, destination));
        rendered.sha256 = DataCatalog::sha256FileHex(destination);
        rendered.parentVersionIds = {version.id, previous.id};
        rendered.extra = previous.extra;
        rendered.extra.insert("rendered_from_version_id", version.id); rendered.extra.insert("facies", schema);
        rendered.extra.insert("facies_schema", "paleo_facies_hierarchy_v1");
        rendered.extra.insert("layer_id", "paleomap." + rendered.id);
        const QVariantList members{QVariantMap{{"file", image}, {"sha256", rendered.sha256}, {"bytes", QFileInfo(path).size()}}};
        rendered.extra.insert("bundle_members", members); rendered.extra.insert("bundle_sha256", PaleoMapFiles::fingerprint(members));
        QVERIFY(catalog.addVersion(rendered, &error));
      }
      report << QVariantMap{{"asset_id", assetId}, {"version_id", version.id}, {"horizon", horizon},
                            {"features", layer->featureCount()}, {"mappings", mappings}};
      qInfo().noquote() << "NORMALIZED_HISTORICAL_MAP" << horizon << version.id << layer->featureCount();
    }
    QVERIFY(catalog.exportCatalogJson(root + "/artifacts/metadata/catalog.json", &error));
    QVERIFY(project.writeProject());
    QFile reportFile(root + "/historical-map-repair.json"); QVERIFY(reportFile.open(QIODevice::WriteOnly));
    reportFile.write(QJsonDocument::fromVariant(QVariantMap{{"facies", schema}, {"maps", report}}).toJson());
  }
};
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  if (!QgisRuntime::initialize()) return 1;
  PaleoTheme::applyLightTheme();
  TestProjectPaleomaps test; const int result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown(); return result;
}
#include "tst_project_paleomaps.moc"
