#include "helpers/workflowfixture.h"
#include "../src/domain/interpretationevidence.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include <QtTest>
#include <qgsapplication.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>

class TestNativeEvidenceGuard : public QObject {
  Q_OBJECT
private slots:
  void failedFirstFeatureIsNotClearedBySuccessfulSecondFeature_data() {
    QTest::addColumn<bool>("separateCommands");
    QTest::newRow("same-native-command") << false;
    QTest::newRow("failed-A-then-successful-B-commands") << true;
  }
  void failedFirstFeatureIsNotClearedBySuccessfulSecondFeature() {
    QFETCH(bool, separateCommands);
    paleo::tests::WorkflowFixture fixture;
    QVERIFY(paleo::tests::initFixture(fixture)); QString error;
    ConstraintWorkflow constraints(&fixture.proc, &fixture.layers);
    constraints.setCatalog(&fixture.catalog, fixture.dir.path());
    MappingWorkbench work(&fixture.layers, &fixture.proc, &fixture.projectSvc, &constraints);
    work.bindCatalog(&fixture.catalog, fixture.dir.path());
    const auto path = fixture.dir.filePath("points.geojson");
    const QVariantList points{QVariantMap{{"id", "A"}, {"name", "A"}, {"x", 0.0}, {"y", 0.0}, {"facies_code", 1}},
                              QVariantMap{{"id", "B"}, {"name", "B"}, {"x", 2.0}, {"y", 2.0}, {"facies_code", 1}}};
    QVERIFY2(MappingArtifactWriter::points(path, points, &error), qPrintable(error));
    LayerDeclaration source; source.layerId = "prediction.fixture"; source.horizon = "D61";
    source.type = "vector"; source.source = path;
    QVERIFY2(fixture.layers.declare(source, &error), qPrintable(error));
    const auto draft = work.copyForEditing(source.layerId, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(fixture.layers.instantiate(draft, &error));
    QVERIFY(layer); work.styleLayer(draft);
    auto features = layer->getFeatures(); QgsFeature first, second;
    QVERIFY(features.nextFeature(first)); QVERIFY(features.nextFeature(second)); features.close();
    const QString firstUid = MappingFeatureIdentity::uid(first);
    QVERIFY(layer->startEditing());
    if (layer->fields().indexOf("paleo_interpretation") < 0)
      QVERIFY(layer->addAttribute(QgsField("paleo_interpretation", QMetaType::Type::QString)));
    QVariantList history;
    const auto initialVersion = work.versionForLayer(draft).id;
    for (int n = 0; n < 10000; ++n)
      history << QVariantMap{{"event_id", QString("event-%1").arg(n)}, {"actor_id", "local:test"},
                             {"before_revision", n}, {"after_revision", n + 1},
                             {"reason", "recorded fixture interpretation"}, {"timestamp", "2026-10-14T01:00:00.000Z"},
                             {"version_id", initialVersion}, {"operation_id", QString("operation-%1").arg(n)},
                             {"target_object_id", "interpretation-A"}, {"target_object_type", "Interpretation"}};
    const QVariantMap interpretation{{"id", "interpretation-A"}, {"assertion_id", "assertion-A"},
      {"feature_uid", firstUid}, {"horizon", "D61"}, {"status", "NeedsReview"}, {"revision", 10000}, {"history", history}};
    QVERIFY2(paleo::evidence::validateInterpretation(interpretation, &error), qPrintable(error));
    QVERIFY(layer->changeAttributeValue(first.id(), layer->fields().indexOf("paleo_interpretation"),
                                       paleo::evidence::encode(interpretation)));
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(';')));
    QVERIFY(layer->startEditing());
    QSignalSpy errors(&work, &MappingWorkbench::errorOccurred);
    auto firstGeometry = QgsGeometry::fromPointXY(QgsPointXY(0.5, 0.5));
    auto secondGeometry = QgsGeometry::fromPointXY(QgsPointXY(2.5, 2.5));
    layer->beginEditCommand("A audit fails");
    QVERIFY(layer->changeGeometry(first.id(), firstGeometry));
    if (separateCommands) {
      layer->endEditCommand();
      QVERIFY(!layer->allowCommit());
      layer->beginEditCommand("B succeeds after A failure");
    }
    QVERIFY(layer->changeGeometry(second.id(), secondGeometry));
    layer->endEditCommand();
    QVERIFY(errors.count() > 0);
    const auto failures = layer->property("paleo.evidenceEditFailures").toMap();
    QVERIFY(failures.contains("feature:" + QString::number(first.id())));
    QVERIFY(!layer->allowCommit());
    QVERIFY(!layer->commitChanges());
    QVERIFY(layer->isEditable());
    QCOMPARE(MappingFeatureIdentity::uid(layer->getFeature(first.id())), firstUid);
    QVERIFY(layer->rollBack());
    QVERIFY(layer->property("paleo.evidenceEditFailures").toMap().isEmpty());
    QVERIFY(layer->allowCommit());
    QCOMPARE(layer->getFeature(first.id()).geometry().asPoint(), QgsPointXY(0, 0));
    QCOMPARE(layer->getFeature(second.id()).geometry().asPoint(), QgsPointXY(2, 2));
  }
};
int main(int argc, char **argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  QgsApplication::initQgis(); TestNativeEvidenceGuard test;
  const auto result = QTest::qExec(&test, argc, argv); QgsApplication::exitQgis(); return result;
}
#include "tst_nativeevidenceguard.moc"
