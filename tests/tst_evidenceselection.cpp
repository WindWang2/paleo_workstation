#include "helpers/workflowfixture.h"
#include "../src/qgis/evidencemapadapter.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/workflow/evidenceinteraction.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include <QtTest>
#include "../src/ui/paleotheme.h"
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QElapsedTimer>
#include <QDir>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgscollapsiblegroupbox.h>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <qgsapplication.h>
#include <qgsvectorlayer.h>
#include <qgsproject.h>

namespace {
struct Fixture {
  paleo::tests::WorkflowFixture data;
  ConstraintWorkflow constraints{&data.proc, &data.layers};
  MappingWorkbench work{&data.layers, &data.proc, &data.projectSvc, &constraints};
  QString draft, error;
  QgsVectorLayer *layer = nullptr;
  QgsFeature a, b;
  bool init() {
    if (!paleo::tests::initFixture(data)) return false;
    data.store.setProjectPaths(data.projectSvc.projectPath(), data.dir.filePath("project.gpkg"), data.dir.filePath("project.sqlite"));
    constraints.setStore(&data.store); constraints.setCatalog(&data.catalog, data.dir.path());
    work.bindCatalog(&data.catalog, data.dir.path());
    const auto path = data.dir.filePath("points.geojson");
    if (!MappingArtifactWriter::points(path, {
        QVariantMap{{"id", "A"}, {"x", 0.0}, {"y", 0.0}, {"facies_code", 1}},
        QVariantMap{{"id", "B"}, {"x", 10.0}, {"y", 10.0}, {"facies_code", 1}}}, &error)) return false;
    LayerDeclaration source; source.layerId = "prediction.fixture"; source.horizon = "D61";
    source.type = "vector"; source.source = path;
    if (!data.layers.declare(source, &error)) return false;
    draft = work.copyForEditing(source.layerId, {}, &error);
    layer = qobject_cast<QgsVectorLayer *>(data.layers.instantiate(draft, &error));
    if (!layer) return false;
    auto features = layer->getFeatures();
    return features.nextFeature(a) && features.nextFeature(b);
  }
  QVariantMap graphNode(const QgsFeature &feature) {
    return {{"project_id", data.projectSvc.mapConfiguration().projectId}, {"layer_id", draft},
      {"version_id", work.versionForLayer(draft).id}, {"feature_uid", MappingFeatureIdentity::uid(feature)}, {"horizon", "D61"}};
  }
};
bool anyHighlight(EvidenceInteraction &interaction) {
  for (auto *adapter : interaction.findChildren<EvidenceMapAdapter *>())
    if (!adapter->highlightedUid().isEmpty()) return true;
  return false;
}
}

class TestEvidenceSelection : public QObject {
  Q_OBJECT
private slots:
  void detailsAreDeferredWithoutChangingNativeOrGraphTarget() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error));
    SelectionContext selection; selection.setActiveHorizon("D61");
    QgisCanvasController canvas;
    EvidenceInteraction interaction(&f.work, &selection, &f.data.layers, &f.data.projectSvc, &canvas);
    f.layer->selectByIds({f.a.id()});
    interaction.navigateGraphNode(f.graphNode(f.b));
    const auto target = selection.target();
    QVERIFY(interaction.snapshot().value("details_deferred").toBool());
    QVERIFY(!interaction.snapshot().contains("spatial_candidates"));
    interaction.setDetailsEnabled(true);
    QCOMPARE(selection.target().featureUid, target.featureUid);
    QCOMPARE(f.layer->selectedFeatureIds(), QgsFeatureIds{f.a.id()});
    QVERIFY(!interaction.snapshot().value("details_deferred").toBool());
    QVERIFY(interaction.snapshot().contains("spatial_candidates"));
    interaction.setDetailsEnabled(false);
    QVERIFY(interaction.snapshot().value("details_deferred").toBool());
    QCOMPARE(selection.target().featureUid, target.featureUid);
  }
  void compactEditorPreservesDraftAndSelectionOnRepeatedSnapshots() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error));
    MappingWorkbenchPage page("compose", &f.work); page.setHorizon("D61"); page.selectLayer(f.draft);
    f.layer->selectByIds({f.a.id()});
    auto snapshot = f.work.featureEvidenceSummary(f.draft, {f.a.id()});
    snapshot.insert("records", QVariantList{QVariantMap{{"id", "saved"}, {"text", "saved evidence"}}});
    page.setEvidenceSnapshot(snapshot);
    auto *list = page.findChild<QListWidget *>("faciesEvidenceList"); QVERIFY(list);
    list->setCurrentRow(0); auto *item = list->item(0);
    auto *text = page.findChild<QPlainTextEdit *>("faciesEvidenceText"); QVERIFY(text);
    text->setPlainText("unsaved explanation");
    page.setEvidenceSnapshot(snapshot);
    QCOMPARE(list->item(0), item); QCOMPARE(text->toPlainText(), QString("unsaved explanation"));
    auto *section = page.findChild<QWidget *>("evidenceAdvancedSection"); QVERIFY(section);
    auto *toggle = section->findChild<QPushButton *>("collapsibleSectionToggle"); QVERIFY(toggle); QVERIFY(!toggle->isChecked());
    if (const auto path = qEnvironmentVariable("PALEO_EVIDENCE_CAPTURE"); !path.isEmpty()) {
      auto *group = page.findChild<QgsCollapsibleGroupBox *>("workbenchEvidence"); QVERIFY(group);
      group->setCollapsed(false); page.resize(450, 1000); page.show();
      QCoreApplication::processEvents(); QVERIFY(group->grab().save(path));
    }
    QSignalSpy details(&page, &MappingWorkbenchPage::evidenceDetailsRequested);
    toggle->setChecked(true); QCOMPARE(details.count(), 1); QCOMPARE(details.first().first().toBool(), true);
    QCOMPARE(text->toPlainText(), QString("unsaved explanation"));
  }
  void historicalCoverageRenderBenchmark() {
    const auto before = qEnvironmentVariable("PALEO_MAP_BEFORE");
    const auto after = qEnvironmentVariable("PALEO_MAP_AFTER");
    const auto output = qEnvironmentVariable("PALEO_MAP_CAPTURE_DIR");
    if (before.isEmpty() || after.isEmpty() || output.isEmpty()) QSKIP("Optional local source-map render benchmark");
    QVERIFY(QDir().mkpath(output));
    for (const auto &horizon : {QStringLiteral("D61"), QStringLiteral("D62"), QStringLiteral("D63")}) {
      const auto filename = horizon + "_historical_facies_map.gpkg";
      QList<qint64> timings;
      for (const auto &folder : {before, after}) {
        QgsVectorLayer layer(QDir(folder).filePath(filename), horizon, "ogr");
        QVERIFY2(layer.isValid(), qPrintable(layer.error().message()));
        QCOMPARE(layer.featureCount(), horizon == "D61" ? 13 : horizon == "D62" ? 9 : 10);
        bool style = false; layer.loadNamedStyle(QDir(folder).filePath(filename + ".qml"), style); QVERIFY(style);
        QgsMapSettings settings; settings.setLayers({&layer}); settings.setDestinationCrs(layer.crs());
        settings.setExtent(layer.extent()); settings.setOutputSize(QSize(1000, 1000));
        settings.setBackgroundColor(Qt::white);
        qint64 sum = 0;
        for (int n = 0; n < 4; ++n) {
          QElapsedTimer timer; timer.start();
          QgsMapRendererSequentialJob job(settings); job.start(); job.waitForFinished();
          if (n) sum += timer.nsecsElapsed();
          const auto image = job.renderedImage(); QVERIFY(!image.isNull());
          int ink = 0;
          for (int y = 0; y < image.height(); y += 5) for (int x = 0; x < image.width(); x += 5)
            if (image.pixelColor(x,y) != QColor(Qt::white)) ++ink;
          QVERIFY(ink > 100);
          if (n == 3) QVERIFY(image.save(QDir(output).filePath(horizon + (folder == before ? "-before.png" : "-after.png"))));
        }
        timings << sum / 3;
      }
      qInfo().noquote() << horizon << "render mean ms before/after" << timings[0] / 1000000.0 << timings[1] / 1000000.0;
    }
  }
  void nativeSelectionAndClearRemovePreviousGraphHighlight() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error));
    SelectionContext selection; selection.setActiveHorizon("D61");
    QgisCanvasController canvas;
    EvidenceInteraction interaction(&f.work, &selection, &f.data.layers, &f.data.projectSvc, &canvas);
    interaction.navigateGraphNode(f.graphNode(f.a));
    QCOMPARE(selection.target().featureUid, MappingFeatureIdentity::uid(f.a)); QVERIFY(anyHighlight(interaction));
    f.layer->selectByIds({f.b.id()});
    QCOMPARE(selection.target().featureUid, MappingFeatureIdentity::uid(f.b));
    QCOMPARE(interaction.snapshot().value("feature_uid").toString(), MappingFeatureIdentity::uid(f.b));
    QVERIFY(!anyHighlight(interaction));
    interaction.navigateGraphNode(f.graphNode(f.a)); QVERIFY(anyHighlight(interaction));
    f.layer->removeSelection();
    QVERIFY(selection.target().featureUid.isEmpty()); QVERIFY(!anyHighlight(interaction));
    QVERIFY(!f.layer->isEditable());
  }
  void unloadedLayerKeepsStableTargetAndRestoresHighlightOnReload() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error));
    SelectionContext selection; selection.setActiveHorizon("D61");
    QgisCanvasController canvas;
    EvidenceInteraction interaction(&f.work, &selection, &f.data.layers, &f.data.projectSvc, &canvas);
    interaction.navigateGraphNode(f.graphNode(f.a));
    const auto uid = selection.target().featureUid;
    QVERIFY(!uid.isEmpty()); QVERIFY(anyHighlight(interaction));
    QPointer<QgsVectorLayer> old = f.layer;
    f.data.projectSvc.project()->removeMapLayer(f.layer->id());
    QVERIFY(old.isNull()); QCOMPARE(selection.target().featureUid, uid); QVERIFY(!anyHighlight(interaction));
    auto *restored = qobject_cast<QgsVectorLayer *>(f.data.layers.instantiate(f.draft, &f.error));
    QVERIFY2(restored, qPrintable(f.error));
    QCOMPARE(interaction.snapshot().value("feature_uid").toString(), uid);
    QVERIFY(anyHighlight(interaction)); QVERIFY(restored->selectedFeatureIds().isEmpty()); QVERIFY(!restored->isEditable());
    selection.setActiveHorizon("Other");
    QVERIFY(selection.target().isEmpty()); QVERIFY(interaction.snapshot().isEmpty()); QVERIFY(!anyHighlight(interaction));
  }
  void graphTargetOwnsEveryDetailWhileNativeEditingSelectionIsDifferent() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error));
    SelectionContext selection; selection.setActiveHorizon("D61");
    QgisCanvasController canvas;
    EvidenceInteraction interaction(&f.work, &selection, &f.data.layers, &f.data.projectSvc, &canvas);
    MappingWorkbenchPage page("compose", &f.work); page.setHorizon("D61"); page.selectLayer(f.draft);
    connect(&interaction, &EvidenceInteraction::detailsChanged, &page,
            [&page](const QVariantMap &snapshot, quint64) { page.setEvidenceSnapshot(snapshot); });
    f.layer->selectByIds({f.a.id()});
    interaction.navigateGraphNode(f.graphNode(f.b));
    QCOMPARE(f.layer->selectedFeatureIds(), QgsFeatureIds{f.a.id()});
    auto snapshot = interaction.snapshot();
    snapshot.insert("records", QVariantList{QVariantMap{{"id", "evidence-B"}, {"text", "source B"}, {"feature_id", f.b.id()}}});
    page.setEvidenceSnapshot(snapshot);
    auto *list = page.findChild<QListWidget *>("faciesEvidenceList"); QVERIFY(list); QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QString("evidence-B"));
    list->setCurrentRow(0);
    auto *reason = page.findChild<QLineEdit *>("faciesEvidenceReason"); QVERIFY(reason); reason->setText("review B");
    auto *actor = page.findChild<QLineEdit *>("faciesEvidenceActor"); QVERIFY(actor); actor->setText("local:test");
    auto *state = page.findChild<QLabel *>("faciesInterpretationState"); QVERIFY(state);
    QVERIFY(state->text().contains(MappingFeatureIdentity::uid(f.b)));
    QCOMPARE(list->count(), 1); QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QString("evidence-B"));
    QVERIFY(!page.commandButton("removeEvidence")->isEnabled());
    QVERIFY(!page.commandButton("updateEvidence")->isEnabled());
    QVERIFY(!page.commandButton("verifyEvidence")->isEnabled());
    QVERIFY(!page.commandButton("reviewInterpretation")->isEnabled());
    // Matching map selection restores editing eligibility without graph navigation changing it.
    f.layer->selectByIds({f.b.id()});
    QVERIFY(page.commandButton("reviewInterpretation")->isEnabled());
    QCOMPARE(selection.target().featureUid, MappingFeatureIdentity::uid(f.b));
    selection.clearTarget("user");
    QCOMPARE(list->count(), 0); QVERIFY(!page.commandButton("reviewInterpretation")->isEnabled());
    QVERIFY(!state->text().contains(MappingFeatureIdentity::uid(f.b)));
    interaction.navigateGraphNode(f.graphNode(f.b)); QVERIFY(anyHighlight(interaction));
    f.data.projectSvc.closeProject();
    QVERIFY(selection.target().isEmpty()); QCOMPARE(list->count(), 0); QVERIFY(!anyHighlight(interaction));
  }

  // opt-02（V1）：工程边界 reset 同时清 ids 与 activeHorizon——旧实现只清
  // target：换工程后残留旧井选择（撞名 id 时新工程面板凭空高亮旧井/井对
  // 话框预选旧集）与旧层位名（保存剖面把旧工程层位写进新工程），且与
  // setSelection 等值短路复合后同集重选不再广播。
  void projectResetClearsSelectionIdsAndHorizon() {
    Fixture f; QVERIFY2(f.init(), qPrintable(f.error));
    SelectionContext selection;
    QgisCanvasController canvas;
    EvidenceInteraction interaction(&f.work, &selection, &f.data.layers,
                                    &f.data.projectSvc, &canvas);
    selection.setSelection({"well-1", "well-2"}, QStringLiteral("tree"));
    selection.setActiveHorizon(QStringLiteral("D61"));
    QCOMPARE(selection.selectedIds().size(), 2);
    QCOMPARE(selection.activeHorizon(), QStringLiteral("D61"));

    interaction.reset(); // projectAboutToClose / catalogBound 工程边界
    QVERIFY(selection.selectedIds().isEmpty());
    QVERIFY(selection.activeHorizon().isEmpty());
    QVERIFY(selection.target().isEmpty());

    // 同集重选必须重新广播（旧残留态被等值短路吞掉 selectionChanged，先红）。
    int broadcasts = 0;
    QObject::connect(&selection, &SelectionContext::selectionChanged,
                     [&broadcasts] { ++broadcasts; });
    selection.setSelection({"well-1", "well-2"}, QStringLiteral("tree"));
    QCOMPARE(broadcasts, 1);
  }
};
int main(int argc, char **argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  QgsApplication::initQgis(); PaleoTheme::ensureApplicationFonts(); app.setFont(PaleoTheme::bodyFont()); TestEvidenceSelection test;
  const int result = QTest::qExec(&test, argc, argv); QgsApplication::exitQgis(); return result;
}
#include "tst_evidenceselection.moc"
