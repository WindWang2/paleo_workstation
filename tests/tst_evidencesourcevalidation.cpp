#include "../src/catalog/datacatalog.h"
#include "../src/domain/interpretationevidence.h"
#include "../src/domain/predictionevidence.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>

class TestEvidenceSourceValidation : public QObject {
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
    QString draft;
    QgsFeatureId fid = FID_NULL;
    QString error;
    QVariantList schema{QVariantMap{{"code", 1}, {"name", "river"}, {"facies", "river"}, {"subfacies", "channel"}, {"microfacies", "river"}, {"color", "#97B4CE"}}};
    bool init() {
      if (!catalog.open(dir.path()) || !project.createProject(dir.filePath("project.qgz")) || !manifest.open()) return false;
      store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"), dir.filePath("project.sqlite"));
      constraints.setStore(&store); constraints.setCatalog(&catalog, dir.path());
      project.setDeclarationProvider([this](QVector<LayerDeclaration> *out, QString *err) { return layers.tryDeclared(out, err); });
      processing.setProject(project.project()); work.bindCatalog(&catalog, dir.path());
      if (!work.saveFacies("D61", schema, &error)) return false;
      QgsVectorLayer grid("Polygon?field=facies_code:integer", "target", "memory");
      grid.setCrs(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
      QgsFeature f(grid.fields()); f.setAttribute("facies_code", 1); f.setGeometry(QgsGeometry::fromRect(QgsRectangle(0, 0, 10, 10)));
      QgsFeatureList features{f}; if (!grid.dataProvider()->addFeatures(features)) return false;
      const auto path = dir.filePath("target.gpkg"); if (!MappingArtifactWriter::vectorSnapshot(&grid, path, &error)) return false;
      LayerDeclaration d; d.layerId = "input.target"; d.horizon = "D61"; d.type = "vector"; d.title = "target";
      d.group = "05_Composition"; d.source = path + "|layername=features";
      if (!layers.declare(d, &error)) return false;
      draft = work.copyForEditing(d.layerId, {}, &error);
      if (draft.isEmpty() || !layers.instantiate(draft, &error)) return false;
      auto *v = layer(); if (!v) return false;
      QgsFeature f2; auto it = v->getFeatures(); if (!it.nextFeature(f2)) return false; fid = f2.id(); return true;
    }
    QgsVectorLayer *layer() { return qobject_cast<QgsVectorLayer *>(layers.layer(draft)); }
    QString source(const QString &name, const QString &horizon, bool duplicate = false) {
      QgsVectorLayer src("Polygon?field=facies_code:integer&field=horizon:string&field=paleo_feature_uid:string", name, "memory");
      src.setCrs(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
      QgsFeature f(src.fields()); f.setAttribute("facies_code", 1); f.setAttribute("horizon", horizon);
      f.setAttribute(MappingFeatureIdentity::UidField, "source-uid"); f.setGeometry(QgsGeometry::fromRect(QgsRectangle(0, 0, 10, 10)));
      QgsFeatureList features{f}; if (duplicate) features << f;
      if (!src.dataProvider()->addFeatures(features)) return {};
      const auto path = dir.filePath(name + ".gpkg"); if (!MappingArtifactWriter::vectorSnapshot(&src, path, &error)) return {};
      CatalogAsset a; a.id = "asset-" + name; a.type = "facies_vector"; a.displayName = name;
      CatalogVersion v; v.id = "version-" + name; v.assetId = a.id; v.path = path; v.managed = false; v.stage = "DERIVED";
      v.extra = {{"mapping_product", true}, {"layer_type", "vector"}, {"source_suffix", "|layername=features"},
                 {"kind", "composition"}, {"horizon", horizon}, {"facies", work.versionForLayer(draft).extra.value("facies")}};
      if (!catalog.addAsset(a, &error) || !catalog.addVersion(v, &error)) return {}; return v.id;
    }
    bool link(const QString &version, const QString &text = "source", bool pinned = true, QVariantMap extra = {}) {
      extra.insert("source_version", version); extra.insert("text", text); extra.insert("role", "REFERENCES");
      if (pinned) extra.insert("source_feature_uid", "source-uid");
      return work.addEvidence(draft, {fid}, extra, &error);
    }
    bool confirm() { return work.changeEvidence(draft, {fid}, "review", {{"status", "Confirmed"}, {"actor_id", "local:test"}, {"reason", "checked sources"}}, &error); }
    QVariantMap detail() { return work.featureEvidence(draft, {fid}); }
    QString raw() { return layer()->getFeature(fid).attribute("paleo_interpretation").toString(); }
    QgsVectorLayer *sourceLayer(const QString &version) {
      const auto id = work.layerForVersion(version, &error);
      return qobject_cast<QgsVectorLayer *>(layers.instantiate(id, &error));
    }
  };
  static bool hasCode(const QVariantMap &detail, const QString &code) {
    for (const auto &v : detail.value("quality").toList()) if (v.toMap().value("code").toString() == code) return true; return false;
  }
private slots:
  void directVersionCannotForgeHorizonOrHealth() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("wrong", "D62"); QVERIFY(!source.isEmpty());
    const auto before = f.raw();
    QVERIFY(!f.link(source, "forged", false, {{"source_horizon", "D61"}, {"source_missing", false}, {"source_stale", false}}));
    QVERIFY(f.error.contains("层位")); QCOMPARE(f.raw(), before);
  }
  void deletedPinnedFeatureBlocksConfirmationButCanUnlink() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("deleted", "D61"); QVERIFY(!source.isEmpty());
    QVERIFY2(f.link(source), qPrintable(f.error)); auto *v = f.sourceLayer(source); QVERIFY(v);
    v->setReadOnly(false); QVERIFY(v->startEditing()); QgsFeature sourceFeature; auto it = v->getFeatures(); QVERIFY(it.nextFeature(sourceFeature));
    QVERIFY(v->deleteFeature(sourceFeature.id())); QVERIFY(v->commitChanges()); v->setReadOnly(true);
    const auto detail = f.detail(); QVERIFY(hasCode(detail, "missing_source_feature"));
    const auto before = f.raw(); QVERIFY(!f.confirm()); QCOMPARE(f.raw(), before);
    const auto id = detail.value("records").toList().first().toMap().value("id").toString();
    QVERIFY2(f.work.removeEvidence(f.draft, {f.fid}, id, &f.error), qPrintable(f.error));
    QVERIFY2(f.confirm(), qPrintable(f.error));
  }
  void duplicatePinnedIdentityCannotBeAccepted() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("duplicate", "D61"); QVERIFY(!source.isEmpty());
    QVERIFY2(f.link(source), qPrintable(f.error)); auto *v = f.sourceLayer(source); QVERIFY(v);
    v->setReadOnly(false); QVERIFY(v->startEditing()); QgsFeature existing; auto it = v->getFeatures(); QVERIFY(it.nextFeature(existing));
    QgsFeature duplicate(v->fields()); duplicate.setAttributes(existing.attributes()); duplicate.setGeometry(existing.geometry());
    const int providerKey = duplicate.fields().indexOf("fid");
    if (providerKey >= 0) duplicate.setAttribute(providerKey, QVariant());
    QVERIFY(v->addFeature(duplicate)); QVERIFY(v->commitChanges()); v->setReadOnly(true);
    QVERIFY(hasCode(f.detail(), "missing_source_feature")); const auto before = f.raw(); QVERIFY(!f.confirm()); QCOMPARE(f.raw(), before);
  }
  void whitespacePinnedIdentityCannotBeAccepted_data() {
    QTest::addColumn<QString>("suffix");
    QTest::newRow("space") << QStringLiteral(" ");
    QTest::newRow("tab") << QStringLiteral("\t");
    QTest::newRow("unicode-nbsp") << QString(QChar(0x00a0));
  }
  void whitespacePinnedIdentityCannotBeAccepted() {
    QFETCH(QString, suffix);
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("whitespace", "D61"); QVERIFY(!source.isEmpty());
    QVERIFY2(f.link(source), qPrintable(f.error)); auto *v = f.sourceLayer(source); QVERIFY(v);
    v->setReadOnly(false); QVERIFY(v->startEditing()); QgsFeature existing; auto it = v->getFeatures(); QVERIFY(it.nextFeature(existing));
    QgsFeature duplicate(v->fields()); duplicate.setAttributes(existing.attributes()); duplicate.setGeometry(existing.geometry());
    const int providerKey = duplicate.fields().indexOf("fid");
    if (providerKey >= 0) duplicate.setAttribute(providerKey, QVariant());
    duplicate.setAttribute(MappingFeatureIdentity::UidField, QString("source-uid") + suffix);
    QVERIFY(v->addFeature(duplicate)); QVERIFY(v->commitChanges()); v->setReadOnly(true);
    QVERIFY(hasCode(f.detail(), "missing_source_feature"));
    const auto before = f.raw(); QVERIFY(!f.confirm()); QCOMPARE(f.raw(), before);
  }
  void unknownPinnedGeometryIsNotReportedAsDeleted() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("invalid", "D61"); QVERIFY(!source.isEmpty());
    QVERIFY2(f.link(source), qPrintable(f.error)); auto *v = f.sourceLayer(source); QVERIFY(v);
    v->setCrs(QgsCoordinateReferenceSystem());
    const auto detail = f.detail(); const auto record = detail.value("records").toList().first().toMap();
    QCOMPARE(record.value("spatial_state").toString(), QString("unknown")); QVERIFY(!record.value("source_feature_missing").toBool());
    const auto before = f.raw(); QVERIFY(!f.confirm()); QVERIFY(f.error.contains("核查尚未完成")); QCOMPARE(f.raw(), before);
  }
  void oneSourceManyRelationsReusesBoundedSourceBudget() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("many", "D61"); QVERIFY(!source.isEmpty());
    for (int i = 0; i < 9; ++i) QVERIFY2(f.link(source, QString("observation %1").arg(i)), qPrintable(f.error));
    QVERIFY2(f.confirm(), qPrintable(f.error)); QCOMPARE(f.detail().value("interpretation").toMap().value("status").toString(), QString("Confirmed"));
  }
  void manyDistinctPinnedSourcesRemainConfirmable() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error));
    for (int i = 0; i < 9; ++i) {
      const auto source = f.source(QString("distinct-%1").arg(i), "D61"); QVERIFY(!source.isEmpty());
      QVERIFY2(f.link(source, QString("observation %1").arg(i)), qPrintable(f.error));
    }
    const auto before = f.detail();
    QCOMPARE(before.value("records").toList().size(), 9);
    QCOMPARE(before.value("records").toList().last().toMap().value("spatial_state").toString(), QString("unknown"));
    QVERIFY2(f.confirm(), qPrintable(f.error)); // bounded detail truncation cannot block valid business confirmation.
    QCOMPARE(f.detail().value("interpretation").toMap().value("status").toString(), QString("Confirmed"));
  }
  void sourceSchemaAndCurrentVersionWarningsAreAuthoritative() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("classification", "D61"); QVERIFY(!source.isEmpty());
    QVERIFY2(f.link(source, "source", true, {{"source_current_version", source}, {"source_stale", false}, {"source_facies_schema_hash", "forged"}}), qPrintable(f.error));
    QVERIFY(!hasCode(f.detail(), "source_classification_mismatch"));
    auto changedSchema = f.work.versionForLayer(f.draft).extra.value("facies").toList();
    auto category = changedSchema.first().toMap(); category.insert("name", "revised river"); changedSchema[0] = category;
    QVERIFY(f.catalog.updateVersionExtra(source, "facies", changedSchema, &f.error));
    auto next = f.catalog.versionById(source); next.id += "-next"; next.versionNumber = 2;
    QVERIFY(f.catalog.addVersion(next, &f.error));
    const auto detail = f.detail(); QVERIFY(hasCode(detail, "source_classification_mismatch")); QVERIFY(hasCode(detail, "newer_source_version"));
    const auto record = detail.value("records").toList().first().toMap();
    QCOMPARE(record.value("source_version").toString(), source);
    QCOMPARE(record.value("source_current_version").toString(), next.id);
    QCOMPARE(record.value("source_facies_schema_hash").toString(), paleo::evidence::fingerprint(changedSchema));
    QVERIFY2(f.confirm(), qPrintable(f.error)); // reviewed warnings do not rewrite the pinned version.
    QCOMPARE(f.detail().value("records").toList().first().toMap().value("source_version").toString(), source);
  }
  void danglingAssetOnlyReferenceCannotForgeHealth() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto before = f.raw();
    QVERIFY(!f.work.addEvidence(f.draft, {f.fid}, {{"source_asset", "not-an-asset"}, {"source_missing", false}, {"text", "forged asset"}}, &f.error));
    QCOMPARE(f.raw(), before);
    CatalogAsset asset; asset.id = "asset-only"; asset.type = "reference"; asset.displayName = "historical asset";
    QVERIFY(f.catalog.addAsset(asset, &f.error));
    QVERIFY2(f.work.addEvidence(f.draft, {f.fid}, {{"source_asset", asset.id}, {"text", "historical asset reference"}}, &f.error), qPrintable(f.error));
    QVERIFY2(f.confirm(), qPrintable(f.error));
    const auto record = f.detail().value("records").toList().first().toMap();
    QCOMPARE(record.value("source_asset").toString(), asset.id); QVERIFY(record.value("source_version").toString().isEmpty());
    QVERIFY(f.catalog.removeAssets({asset.id}, &f.error));
    QVERIFY(hasCode(f.detail(), "missing_source"));
    const auto healthyRevision = f.raw(); QVERIFY(!f.confirm()); QCOMPARE(f.raw(), healthyRevision);
    QVERIFY2(f.work.removeEvidence(f.draft, {f.fid}, record.value("id").toString(), &f.error), qPrintable(f.error));
    QVERIFY2(f.confirm(), qPrintable(f.error));
  }
  void currentCatalogStateOverridesPersistedHealth() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); const auto source = f.source("catalog", "D61"); QVERIFY(!source.isEmpty());
    QVERIFY2(f.link(source), qPrintable(f.error)); QVERIFY(f.catalog.updateVersionExtra(source, "horizon", "D62", &f.error));
    QVERIFY(hasCode(f.detail(), "source_horizon_mismatch")); const auto before = f.raw(); QVERIFY(!f.confirm()); QCOMPARE(f.raw(), before);
    QVERIFY(f.catalog.updateVersionExtra(source, "horizon", "D61", &f.error));
    QVERIFY(f.catalog.updateVersionExtra(source, "stale", true, &f.error));
    QVERIFY(hasCode(f.detail(), "stale_source")); QVERIFY2(f.confirm(), qPrintable(f.error)); // warnings remain reviewable, not arbitrary blocking.
  }
  void optionalUnloadedReferenceAndLegacyRecordRemainCompatible() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error)); QVERIFY2(f.confirm(), qPrintable(f.error)); // legacy empty evidence.
    CatalogAsset a; a.id = "raw"; a.type = "reference"; a.displayName = "reference";
    CatalogVersion v; v.id = "raw-version"; v.assetId = a.id; v.managed = false; v.path = f.dir.filePath("reference.dat"); v.stage = "RAW";
    QFile file(v.path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("reference"); file.close();
    QVERIFY(f.catalog.addAsset(a)); QVERIFY(f.catalog.addVersion(v));
    QVERIFY2(f.link(v.id, "optional reference", false), qPrintable(f.error));
    QVERIFY2(f.confirm(), qPrintable(f.error));
    QVERIFY(QFile::remove(v.path)); QVERIFY(hasCode(f.detail(), "missing_source"));
    const auto before = f.raw(); QVERIFY(!f.confirm()); QCOMPARE(f.raw(), before);
    const auto evidenceId = f.detail().value("records").toList().first().toMap().value("id").toString();
    QVERIFY2(f.work.removeEvidence(f.draft, {f.fid}, evidenceId, &f.error), qPrintable(f.error));
  }
};
int main(int argc, char **argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false);
  const auto prefix = qEnvironmentVariable("QGIS_PREFIX_PATH");
  if (!prefix.isEmpty()) app.setPrefixPath(prefix, true);
  app.initQgis(); GDALAllRegister();
  TestEvidenceSourceValidation test; const int result = QTest::qExec(&test, argc, argv); QgsApplication::exitQgis(); return result;
}
#include "tst_evidencesourcevalidation.moc"
