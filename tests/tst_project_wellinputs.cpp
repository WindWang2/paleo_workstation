#include "catalog/datacatalog.h"
#include "domain/mappinghorizons.h"
#include "domain/welllogfacies.h"
#include "io/cuttingsdoc.h"
#include "io/welllogread.h"
#include "metadata/layermanifest.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/qgislayerservice.h"
#include "qgis/facieshierarchyrenderer.h"
#include "qgis/qgisprocessingservice.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/wellattributestore.h"
#include "services/projectdata.h"
#include "services/welllogset.h"
#include "workflow/mappingworkbench.h"
#include "workflow/wellpredictionservice.h"
#include "workflow/wellsectionworkflow.h"
#include "workflow/workflows.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QtTest>
#include <algorithm>
#include <limits>
#include <qgsapplication.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

using namespace WellComposite;
class TestProjectWellInputs : public QObject {
  Q_OBJECT
private slots:
  void actualProjectInputs() {
    const QString root = qEnvironmentVariable("PALEO_WELL_INPUT_PROJECT");
    if (root.isEmpty()) QSKIP("Set PALEO_WELL_INPUT_PROJECT for dataset acceptance");
    AreaRules::setProjectDir(root);
    QFile probe(root + "/artifacts/metadata/catalog.sqlite");
    QVERIFY2(probe.open(QIODevice::ReadOnly), qPrintable(probe.errorString()));
    QCOMPARE(probe.read(16), QByteArray("SQLite format 3\0", 16));
    probe.close();
    DataCatalog catalog;
    catalog.setLockedReadOnly(true);
    QString error;
    QVERIFY2(catalog.open(root, &error), qPrintable(error));
    ProjectDataFacade project;
    project.setCatalog(&catalog, root);
    const auto wells = project.wells();
    QVERIFY(!wells.isEmpty());
    QStringList ids;
    for (const auto &w : wells) ids << w.id;
    QStringList warnings;
    WellSectionWorkflow section(&catalog);
    const auto lithologies = section.lithologiesFor(ids, &warnings);
    const auto allIntervals = WellAttributeStore::rows(root, nullptr, false);
    const auto allFormations = WellAttributeStore::rows(root, nullptr, WellAttributeStore::Table::Formations);
    QHash<QString, QVariantList> intervals, formations;
    for (const auto &row : allIntervals) intervals[row.toMap().value("well_id").toString()] << row;
    for (const auto &row : allFormations) formations[row.toMap().value("well_id").toString()] << row;
    WellFaciesModel model;
    model.id = "input-contract";
    model.curves = {"GR"};
    model.window = 128;
    QVariantList summary;
    int total = 0;
    for (const auto &well : wells) {
      const auto refs = WellLogSet::wellCurveIndex(&catalog, root, well.id);
      const auto found = std::find_if(refs.cbegin(), refs.cend(), [](const auto &r) {
        return r.mnemonic.compare("GR", Qt::CaseInsensitive) == 0;
      });
      QVERIFY2(found != refs.cend(), qPrintable(well.name + ": no GR"));
      const auto doc = WellLogRead::parseDoc(found->path);
      QVERIFY(found->column > 0 && found->column < doc.curves.size());
      ComprehensiveWellData data;
      data.wellName = well.name;
      CurveData gr;
      gr.name = "GR"; gr.unit = doc.curves[found->column].unit;
      for (double d : doc.curves.first().values) gr.depths << float(d);
      for (double v : doc.curves[found->column].values) gr.values << float(v);
      data.continuousCurves = {gr};
      for (const auto &row : formations.value(well.id)) {
        const auto r = row.toMap();
        FormationInterval f;
        f.name = r.value("formation").toString(); f.unitType = r.value("unit_type").toString();
        f.topDepth = r.value("top_md").toDouble(); f.bottomDepth = r.value("base_md").toDouble();
        data.formationIntervals << f;
      }
      for (const auto &row : lithologies.value(well.id)) {
        LithologyInterval l;
        l.topDepth = row.topMd; l.bottomDepth = row.baseMd; l.lithoName = row.litho;
        data.lithologyIntervals << l;
      }
      WellAttributeStore::apply(intervals.value(well.id), &data);
      ComprehensiveWellData rawData = data;
      rawData.lithologyIntervals.clear();
      for (const auto &link : catalog.linksForEntity(well.id)) {
        if (link.role != "cuttings" || link.unresolved) continue;
        const auto version = catalog.currentVersion(link.assetId);
        const auto table = paleo::io::readCuttingsFile(DataCatalog::resolvedVersionPath(root, version));
        QVERIFY2(table.ok, qPrintable(well.name + ": " + table.error));
        for (const auto &row : table.intervals) {
          LithologyInterval l;
          l.topDepth = row.topMd; l.bottomDepth = row.baseMd; l.lithoName = row.litho;
          rawData.lithologyIntervals << l;
        }
      }
      QVERIFY(!rawData.lithologyIntervals.isEmpty());
      const auto tops = project.topsForMapping(well.id);
      for (const auto &h : mappingHorizons()) {
        QVERIFY2(std::any_of(tops.cbegin(), tops.cend(), [&h](const auto &t) { return t.horizon == h; }),
                 qPrintable(well.name + ": missing mapping top " + h));
      }
      int least = std::numeric_limits<int>::max();
      for (const auto &formation : data.formationIntervals) {
        const auto plan = planWellFaciesFormationRun(data, {model}, formation.name);
        QVERIFY2(plan.ready(), qPrintable(well.name + " / " + formation.name + ": " + plan.reason));
        const auto rawPlan = planWellFaciesFormationRun(rawData, {model}, formation.name);
        QVERIFY2(rawPlan.ready(), qPrintable(well.name + " / raw " + formation.name + ": " + rawPlan.reason));
        const int count = plan.runs.first().input.rows.size();
        least = std::min(least, count);
        ++total;
      }
      qInfo().noquote() << well.name << "formations=" << data.formationIntervals.size() << "minimum points=" << least;
      summary << QVariantMap{{"well", well.name}, {"formations", data.formationIntervals.size()}, {"minimum_points", least}};
    }
    qInfo() << "INPUT_ACCEPTANCE wells=" << wells.size() << "well_formation_pairs=" << total;
    const QString output = qEnvironmentVariable("PALEO_WELL_INPUT_REPORT");
    if (!output.isEmpty()) {
      QFile report(output);
      QVERIFY(report.open(QIODevice::WriteOnly));
      report.write(QJsonDocument::fromVariant(QVariantMap{{"wells", wells.size()}, {"well_formation_pairs", total},
          {"model_window", 128}, {"summary", summary}, {"warnings", warnings}}).toJson());
    }
    AreaRules::reset();
  }
  void rerunSelectedMappingHorizons() {
    const QString root = qEnvironmentVariable("PALEO_WELL_INPUT_PROJECT");
    const QStringList horizons = qEnvironmentVariable("PALEO_WELL_INPUT_RERUN").split(',', Qt::SkipEmptyParts);
    if (root.isEmpty() || horizons.isEmpty()) QSKIP("Explicit PALEO_WELL_INPUT_RERUN required for live prediction");
    AreaRules::setProjectDir(root);
    DataCatalog catalog;
    QString error;
    QVERIFY2(catalog.open(root, &error), qPrintable(error));
    QgisProjectService project;
    PaleoProjectStore store;
    LayerManifest manifest(root + "/paleo_data.qgz.project.sqlite");
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    QVERIFY2(project.openProject(root + "/paleo_data.qgz"), qPrintable(project.lastErrors().join(';')));
    store.setProjectPaths(project.projectPath(), root + "/paleo_data.gpkg", root + "/paleo_data.qgz.project.sqlite");
    QgisProcessingService processing(&store);
    processing.setProject(project.project());
    ConstraintWorkflow constraints(&processing, &layers);
    constraints.setStore(&store); constraints.setCatalog(&catalog, root);
    MappingWorkbench work(&layers, &processing, &project, &constraints);
    WellPredictionService remote;
    work.setWellPredictionService(&remote);
    work.bindCatalog(&catalog, root);
    project.setDeclarationProvider([&layers](QVector<LayerDeclaration> *out, QString *e) { return layers.tryDeclared(out, e); });
    QStringList ids;
    for (const auto &row : work.inputs("wells")) ids << row.toMap().value("id").toString();
    QCOMPARE(ids.size(), 20);
    connect(&work, &MappingWorkbench::predictionDetailChanged, this, [](const QString &s) { qInfo().noquote() << s; });
    for (const auto &h : horizons) {
      QSignalSpy errors(&work, &MappingWorkbench::errorOccurred), ready(&work, &MappingWorkbench::productReady);
      QVERIFY2(work.predict(h, "wells", ids, &error), qPrintable(error));
      QElapsedTimer timer; timer.start();
      while (work.busy() && timer.elapsed() < 900000) QTest::qWait(100);
      QVERIFY2(!work.busy(), "live prediction timeout");
      QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
      QVERIFY(!ready.isEmpty());
      CatalogVersion newest;
      for (const auto &v : catalog.versions())
        if (v.extra.value("kind") == "wells_prediction" && v.extra.value("horizon") == h) newest = v;
      QCOMPARE(newest.extra.value("predicted_well_count").toInt(), 20);
      QCOMPARE(newest.extra.value("skipped_well_count").toInt(), 0);
      QVERIFY(catalog.updateVersionExtra(newest.id, "synthetic_input", true, &error));
      QVERIFY(catalog.updateVersionExtra(newest.id, "input_repair_report", QStringLiteral("输入补齐记录.json"), &error));
      qInfo().noquote() << "LIVE_PREDICTION" << h << newest.id << "20/20";
      QVERIFY(layers.instantiate(newest.extra.value("layer_id").toString(), &error));
      QVERIFY(project.writeProject());
    }
    QVERIFY2(catalog.exportCatalogJson(root + "/artifacts/metadata/catalog.json", &error), qPrintable(error));
    AreaRules::reset();
  }
  void materializeLatestResults() {
    const QString root = qEnvironmentVariable("PALEO_WELL_INPUT_PROJECT");
    if (root.isEmpty() || qEnvironmentVariable("PALEO_WELL_INPUT_SAVE") != "1")
      QSKIP("Explicit PALEO_WELL_INPUT_SAVE required for project writing");
    DataCatalog catalog;
    QString error;
    QVERIFY2(catalog.open(root, &error), qPrintable(error));
    QgisProjectService project;
    LayerManifest manifest(root + "/paleo_data.qgz.project.sqlite");
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    QVERIFY(project.openProject(root + "/paleo_data.qgz"));
    PaleoProjectStore store;
    store.setProjectPaths(project.projectPath(), root + "/paleo_data.gpkg", root + "/paleo_data.qgz.project.sqlite");
    QgisProcessingService processing(&store);
    ConstraintWorkflow constraints(&processing, &layers);
    constraints.setStore(&store); constraints.setCatalog(&catalog, root);
    MappingWorkbench work(&layers, &processing, &project, &constraints);
    work.bindCatalog(&catalog, root);
    QHash<QString, CatalogVersion> newest;
    for (const auto &v : catalog.versions())
      if (v.extra.value("kind") == "wells_prediction") newest[v.extra.value("horizon").toString()] = v;
    for (const auto &h : mappingHorizons()) {
      const auto v = newest.value(h);
      QCOMPARE(v.extra.value("predicted_well_count").toInt(), 20);
      const QString id = v.extra.value("layer_id").toString();
      auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(id, &error));
      QVERIFY2(layer && layer->isValid(), qPrintable(id + ": " + error));
      QCOMPARE(layer->featureCount(), 20);
    }
    project.setDeclarationProvider([&layers](QVector<LayerDeclaration> *out, QString *e) { return layers.tryDeclared(out, e); });
    QVERIFY(project.writeProject());
    QVERIFY(catalog.exportCatalogJson(root + "/artifacts/metadata/catalog.json", &error));
  }
  void savedProjectReopens() {
    const QString root = qEnvironmentVariable("PALEO_WELL_INPUT_PROJECT");
    if (root.isEmpty()) QSKIP("Set PALEO_WELL_INPUT_PROJECT for dataset acceptance");
    QgsProject project;
    QVERIFY(project.read(root + "/paleo_data.qgz"));
    int count = 0;
    for (auto *layer : project.mapLayers()) {
      QVERIFY2(layer->isValid(), qPrintable(layer->name() + ": " + layer->source()));
      ++count;
    }
    QVERIFY(count >= 2);
    qInfo() << "SAVED_PROJECT_VALID_LAYERS" << count;
  }
  void savePredictionMicrofacies() {
    const QString root = qEnvironmentVariable("PALEO_MICROFACIES_PROJECT_SAVE");
    if (root.isEmpty()) QSKIP("Explicit project path required to save prediction styling");
    DataCatalog catalog;
    catalog.setLockedReadOnly(true);
    QString error;
    QVERIFY2(catalog.open(qEnvironmentVariable("PALEO_MICROFACIES_CATALOG"), &error), qPrintable(error));
    QVERIFY(!catalog.recoveredFromBackup());
    QHash<QString, CatalogVersion> products;
    for (const auto &version : catalog.versions()) {
      const auto kind = version.extra.value("kind").toString();
      if (kind == "wells_prediction" || kind == "seismic_prediction")
        products[version.extra.value("layer_id").toString()] = version;
    }
    QgsProject project;
    QVERIFY(project.read(root + "/paleo_data.qgz"));
    int styled = 0;
    for (auto *layer : project.mapLayers()) {
      const auto version = products.value(layer->customProperty("paleoLayerId").toString());
      if (version.id.isEmpty()) continue;
      layer->setCustomProperty("paleo/faciesDisplayMode", "micro_facies");
      const auto entries = FaciesHierarchyRenderer::apply(layer, version.extra.value("facies").toList(), "micro_facies");
      QVERIFY(!entries.isEmpty());
      for (const auto &entry : entries)
        QCOMPARE(entry.toMap().value("level").toString(), QString("micro_facies"));
      ++styled;
    }
    QVERIFY(styled > 0); // Unopened horizons remain declared, not materialized.
    QString output = qEnvironmentVariable("PALEO_MICROFACIES_OUTPUT");
    if (output.isEmpty()) output = root + "/paleo_data.qgz";
    QVERIFY2(project.write(output), qPrintable(project.error()));
    QgsProject reopened;
    QVERIFY(reopened.read(output));
    QCOMPARE(reopened.mapLayers().size(), project.mapLayers().size());
    for (auto *layer : reopened.mapLayers()) {
      QVERIFY(layer->isValid());
      if (products.contains(layer->customProperty("paleoLayerId").toString())) {
        QCOMPARE(layer->customProperty("paleo/faciesDisplayMode").toString(), QString("micro_facies"));
        QCOMPARE(layer->customProperty("paleo/faciesResolvedLevel").toString(), QString("micro_facies"));
      }
    }
    qInfo() << "PROJECT_PREDICTIONS_SAVED_AS_MICROFACIES" << styled;
  }
};
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.initQgis();
  const QUrl proxyUrl(qEnvironmentVariable("PALEO_WELL_INPUT_HTTP_PROXY"));
  if (!proxyUrl.host().isEmpty())
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
        proxyUrl.host(), proxyUrl.port(8080), proxyUrl.userName(), proxyUrl.password()));
  TestProjectWellInputs test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_project_wellinputs.moc"
