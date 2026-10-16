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
#include <QLabel>
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
  QgsApplication::initQgis(); TestEvidenceSelection test;
  const int result = QTest::qExec(&test, argc, argv); QgsApplication::exitQgis(); return result;
}
#include "tst_evidenceselection.moc"
