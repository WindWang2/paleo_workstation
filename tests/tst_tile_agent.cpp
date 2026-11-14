// 层：测试壳
#include <QtTest>
#include "helpers/tileagentfixture.h"
#include "qgis/qgisruntime.h"
#include <QApplication>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>

using namespace tile_agent_fixture;
class TestTileAgent:public QObject {
  Q_OBJECT
private slots:
  void missingComputeDependenciesRejectBeforeScheduling() {
    Fixture f;QVERIFY(f.init());const auto prepared=f.phase("dependency-prepare","prepare");
    QVERIFY2(prepared.value("toolOk").toBool(),qPrintable(QJsonDocument(prepared).toJson()));
    const auto source=prepared.value("operationId").toString();const auto tasks=f.data.tasks.tasks().size();
    const auto versions=f.data.catalog.versions().size();f.rebind(false);
    const auto noProvider=f.phase("dependency-provider","compute-only",source);
    QCOMPARE(noProvider.value("code").toString(),QString("ort_provider_unavailable"));QVERIFY(!noProvider.value("toolOk").toBool());
    QCOMPARE(f.data.tasks.tasks().size(),tasks);f.rebind(true);
    ModelRegistryScan scan;scan.manifestFound=true;ModelRegistryEntry other;other.name="other";
    other.file="model.onnx";other.absolutePath=f.data.temp.filePath(other.file);scan.entries.append(other);f.onnx.setModelRegistry(scan);
    QCOMPARE(f.onnx.availableModels(),QStringList{"other"});
    const auto noFrozenModel=f.phase("dependency-model","compute-only",source);
    QCOMPARE(noFrozenModel.value("code").toString(),QString("model_unavailable"));QVERIFY(!noFrozenModel.value("toolOk").toBool());
    QCOMPARE(f.data.tasks.tasks().size(),tasks);QCOMPARE(f.data.catalog.versions().size(),versions);
  }
  void synchronousNativeEditsAtInstantiationCannotBecomeOwnBaseline() {
    Fixture f;QVERIFY(f.init());QVERIFY(f.previousLayer());const auto c=f.compute("native-signal");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    const auto hook=connect(&f.layers,&QgisLayerService::layerInstantiated,&f.chat,[&](const QString &id) {
      if(id!="aifacies.T1.model")return;
      auto *native=qobject_cast<QgsRasterLayer *>(f.layers.layer(id));QVERIFY(native && native->renderer());
      native->setName("Changed in native signal");native->renderer()->setOpacity(0.31);
    });
    const auto publication=f.phase("native-signal-publish","publish",c);disconnect(hook);
    const auto facts=f.get(publication.value("operationId").toString());
    QCOMPARE(facts.value("commitState").toString(),QString("committed"));QCOMPARE(facts.value("presentation").toString(),QString("superseded"));
    auto *native=qobject_cast<QgsRasterLayer *>(f.layers.layer("aifacies.T1.model"));QVERIFY(native && native->renderer());
    QCOMPARE(native->name(),QString("Changed in native signal"));QCOMPARE(native->renderer()->opacity(),0.31);
  }
  void nativeRenameAndRendererChangesSupersedeFrozenPresentation() {
    Fixture f;QVERIFY(f.init());QPointer<QgsMapLayer> previous=f.previousLayer();QVERIFY2(previous,qPrintable(f.error));
    const auto computed=f.compute("native-edit");QVERIFY2(!computed.isEmpty(),qPrintable(f.error));
    auto *raster=qobject_cast<QgsRasterLayer *>(previous.data());QVERIFY(raster && raster->renderer());
    raster->setName("Human native name");raster->renderer()->setOpacity(0.23);
    const auto versions=f.data.catalog.versions().size();const auto publication=f.phase("native-publish","publish",computed);
    const auto facts=f.get(publication.value("operationId").toString());
    QCOMPARE(facts.value("commitState").toString(),QString("committed"));QCOMPARE(facts.value("presentation").toString(),QString("superseded"));
    QVERIFY(previous);QCOMPARE(f.layers.layer("aifacies.T1.model"),previous.data());
    QCOMPARE(previous->name(),QString("Human native name"));QCOMPARE(raster->renderer()->opacity(),0.23);
    QCOMPARE(f.data.catalog.versions().size(),versions+3);
  }
  void userNativeChangesAfterOwnPartialPresentationAreNeverReapplied() {
    Fixture f;QVERIFY(f.init());QVERIFY(f.previousLayer());const auto c=f.compute("native-partial");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    int declared=0;const auto connection=connect(&f.layers,&QgisLayerService::layerDeclared,&f.chat,[&](const QString &){
      if(++declared==1)f.manifest.setReadOnly(true);
    });
    const auto pub=f.phase("native-partial-publish","publish",c);const auto id=pub.value("operationId").toString();QVERIFY(!id.isEmpty());
    QCOMPARE(f.get(id).value("presentation").toString(),QString("pending"));disconnect(connection);f.manifest.setReadOnly(false);
    QPointer<QgsMapLayer> changed=f.layers.layer("aifacies.T1.model");QVERIFY(changed);
    auto *raster=qobject_cast<QgsRasterLayer *>(changed.data());QVERIFY(raster && raster->renderer());
    changed->setName("Edited after partial");raster->renderer()->setOpacity(0.17);const auto versions=f.data.catalog.versions().size();
    f.call("paleo.operation_resume",{{"operation_id",id}});
    QCOMPARE(f.get(id).value("presentation").toString(),QString("superseded"));
    QVERIFY(changed);QCOMPARE(f.layers.layer("aifacies.T1.model"),changed.data());
    QCOMPARE(changed->name(),QString("Edited after partial"));QCOMPARE(raster->renderer()->opacity(),0.17);
    QCOMPARE(f.data.catalog.versions().size(),versions);
  }
  void unchangedNativePartialPresentationSurvivesProjectReopen() {
    Fixture f;QVERIFY(f.init());
    f.project.setOpenGate([&](const QString &directory,bool,QString *,bool *,bool *readOnly) {
      *readOnly=false;const auto claim=f.lock.claim();return claim && claim->isHeldFor(directory);
    });
    QVERIFY2(f.project.createProject(f.data.project+"/native.qgz"),qPrintable(f.project.lastErrors().join(';')));
    QVERIFY(f.previousLayer());const auto c=f.compute("native-reopen");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    int declared=0;const auto connection=connect(&f.layers,&QgisLayerService::layerDeclared,&f.chat,[&](const QString &){
      if(++declared==1)f.manifest.setReadOnly(true);
    });
    const auto pub=f.phase("native-reopen-publish","publish",c);const auto id=pub.value("operationId").toString();QVERIFY(!id.isEmpty());
    QCOMPARE(f.get(id).value("presentation").toString(),QString("pending"));disconnect(connection);f.manifest.setReadOnly(false);
    const auto versions=f.data.catalog.versions().size();QVERIFY(f.project.writeProject());
    const auto path=f.project.projectPath();f.project.closeProject();QVERIFY(f.project.openProject(path));
    const auto resumed=f.call("paleo.operation_resume",{{"operation_id",id}});
    QVERIFY2(resumed.value("toolOk").toBool(),qPrintable(QJsonDocument(resumed).toJson()));
    QCOMPARE(f.get(id).value("presentation").toString(),QString("presented"));
    QCOMPARE(f.data.catalog.versions().size(),versions);QCOMPARE(f.layers.declared().size(),3);
  }
  void resumedPartialPresentationKeepsReleasedHorizonsLazy() {
    Fixture f;QVERIFY(f.init());QVERIFY(f.previousLayer());const auto c=f.compute("native-lazy");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    int declared=0;const auto connection=connect(&f.layers,&QgisLayerService::layerDeclared,&f.chat,[&](const QString &){
      if(++declared==1)f.manifest.setReadOnly(true);
    });
    const auto pub=f.phase("native-lazy-publish","publish",c);const auto id=pub.value("operationId").toString();QVERIFY(!id.isEmpty());
    QCOMPARE(f.get(id).value("presentation").toString(),QString("pending"));disconnect(connection);f.manifest.setReadOnly(false);
    QVERIFY(f.layers.layer("aifacies.T1.model"));const auto versions=f.data.catalog.versions().size();
    f.layers.releaseHorizon("T1");QVERIFY(!f.layers.layer("aifacies.T1.model"));
    const auto resumed=f.call("paleo.operation_resume",{{"operation_id",id}});
    QVERIFY2(resumed.value("toolOk").toBool(),qPrintable(QJsonDocument(resumed).toJson()));
    QCOMPARE(f.get(id).value("presentation").toString(),QString("presented"));
    QVERIFY(!f.layers.layer("aifacies.T1.model"));QCOMPARE(f.data.catalog.versions().size(),versions);
  }
  void productionExternalGrantFollowsTheLiveConfiguredRunner() {
    Fixture f;QVERIFY(f.init());QVERIFY(!(f.host.context().allowedEffects&paleo::host::ExternalTransfer));
    QCOMPARE(f.chat.toolRunner()->readiness("paleo.well_facies_prediction",{},true).code,QString("well_endpoint_unavailable"));
    WellFaciesConfig config;config.baseUrl=QUrl("http://127.0.0.1:1");config.apiKey="in-memory-only";config.allowInsecureHttp=true;
    f.chat.toolRunner()->setFaciesConfig(config);QVERIFY(f.host.context().allowedEffects&paleo::host::ExternalTransfer);
    f.data.catalog.setLockedReadOnly(true);const auto live=f.host.context();QVERIFY(live.readOnly);
    QVERIFY(paleo::host::checkContext(live,live,paleo::host::Read|paleo::host::ExternalTransfer).isEmpty());
    config.apiKey.clear();f.chat.toolRunner()->setFaciesConfig(config);
    QVERIFY(!(f.host.context().allowedEffects&paleo::host::ExternalTransfer));
    config.apiKey="invalid\r\nkey";f.chat.toolRunner()->setFaciesConfig(config);
    QVERIFY(!(f.host.context().allowedEffects&paleo::host::ExternalTransfer));
    config.apiKey="in-memory-only";config.baseUrl=QUrl("file:///tmp/invalid-endpoint");f.chat.toolRunner()->setFaciesConfig(config);
    QVERIFY(!(f.host.context().allowedEffects&paleo::host::ExternalTransfer));QVERIFY(f.data.tasks.tasks().isEmpty());
  }
  void prepareAndComputeArePrivatePublishCommitsThreeOnce() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error));const auto before=f.data.catalog.versions().size();
    const auto assets=f.data.catalog.assets().size();const auto mutation=f.data.catalog.mutationSeq();
    auto prepared=f.phase("p","prepare");QVERIFY2(prepared.value("toolOk").toBool(),qPrintable(QJsonDocument(prepared).toJson()));
    const auto p=prepared.value("operationId").toString();QVERIFY(!p.isEmpty());
    QCOMPARE(f.data.catalog.versions().size(),before);QVERIFY(f.layers.declared().isEmpty());
    QCOMPARE(f.data.catalog.assets().size(),assets);QCOMPARE(f.data.catalog.mutationSeq(),mutation);
    auto computed=f.phase("c","compute-only",p);QVERIFY2(computed.value("toolOk").toBool(),qPrintable(QJsonDocument(computed).toJson()));
    const auto c=computed.value("operationId").toString();QCOMPARE(f.get(c).value("computeCount").toInt(),1);
    QCOMPARE(f.data.catalog.versions().size(),before);QVERIFY(f.layers.declared().isEmpty());
    QCOMPARE(f.data.catalog.assets().size(),assets);QCOMPARE(f.data.catalog.mutationSeq(),mutation);
    const auto ports=f.call("paleo.operation_results",{{"operation_id",c}});QCOMPARE(ports.value("ports").toArray().size(),3);
    f.rebind(false);QStringList advertised;for(const auto &tool:f.chat.tools())advertised.append(tool.name);
    QVERIFY(advertised.contains("paleo.tile_classification_v2"));
    const auto published=f.phase("pub","publish",c);QVERIFY2(published.value("toolOk").toBool(),qPrintable(QJsonDocument(published).toJson()));
    const auto id=published.value("operationId").toString();const auto facts=f.get(id);
    QCOMPARE(facts.value("commitState").toString(),QString("committed"));QCOMPARE(facts.value("presentation").toString(),QString("presented"));
    QCOMPARE(facts.value("computeCount").toInt(),0);QCOMPARE(f.data.catalog.versions().size(),before+3);QCOMPARE(f.layers.declared().size(),3);
    const auto replay=f.phase("pub","publish",c);QVERIFY(replay.value("toolOk").toBool());QCOMPARE(replay.value("operationId"),published.value("operationId"));
    QCOMPARE(f.data.catalog.versions().size(),before+3);QCOMPARE(f.layers.declared().size(),3);
    const auto conflict=f.phase("pub","compute-only",p);QCOMPARE(conflict.value("code").toString(),QString("idempotency_conflict"));
    const auto cancelled=f.call("paleo.operation_cancel",{{"operation_id",id}});
    QCOMPARE(cancelled.value("code").toString(),QString("cancel_after_commit"));QCOMPARE(f.data.catalog.versions().size(),before+3);
    const auto bytes=f.call("paleo.operation_read",{{"operation_id",id},{"port_id","OUTPUT"},{"max_bytes",16}});
    QVERIFY2(bytes.value("toolOk").toBool(),qPrintable(QJsonDocument(bytes).toJson()));QVERIFY(!bytes.value("data").toString().isEmpty());
    f.chat.newSession();const auto denied=f.get(id);QVERIFY(!denied.value("toolOk").toBool());
  }
  void partialPresentationResumesWithoutAddingVersions() {
    Fixture f;QVERIFY(f.init());const auto c=f.compute("partial");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    const auto before=f.data.catalog.versions().size();int declarations=0;
    const auto connection=connect(&f.layers,&QgisLayerService::layerDeclared,&f.chat,[&](const QString &){
      if(++declarations==1)f.manifest.setReadOnly(true);
    });
    const auto pub=f.phase("publish-partial","publish",c);const auto id=pub.value("operationId").toString();QVERIFY(!id.isEmpty());
    auto facts=f.get(id);QCOMPARE(facts.value("commitState").toString(),QString("committed"));
    QCOMPARE(facts.value("presentation").toString(),QString("pending"));QCOMPARE(f.layers.declared().size(),1);
    QCOMPARE(f.data.catalog.versions().size(),before+3);disconnect(connection);f.manifest.setReadOnly(false);
    const auto resumed=f.call("paleo.operation_resume",{{"operation_id",id}});QVERIFY2(resumed.value("toolOk").toBool(),qPrintable(QJsonDocument(resumed).toJson()));
    facts=f.get(id);QCOMPARE(facts.value("presentation").toString(),QString("presented"));
    QCOMPARE(f.layers.declared().size(),3);QCOMPARE(f.data.catalog.versions().size(),before+3);QCOMPARE(facts.value("computeCount").toInt(),0);
  }
  void pendingPresentationNeverOverwritesAUserChange() {
    Fixture f;QVERIFY(f.init());const auto c=f.compute("superseded");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    f.manifest.setReadOnly(true);const auto pub=f.phase("publish-pending","publish",c);
    const auto id=pub.value("operationId").toString();QVERIFY(!id.isEmpty());
    QCOMPARE(f.get(id).value("commitState").toString(),QString("committed"));f.manifest.setReadOnly(false);
    LayerDeclaration user;user.layerId="aifacies.T1.model";user.horizon="T1";user.type="raster";
    user.source=f.data.temp.filePath("input.tif");user.title="User change";QVERIFY(f.layers.declare(user,&f.error));
    const auto count=f.data.catalog.versions().size();f.call("paleo.operation_resume",{{"operation_id",id}});
    QCOMPARE(f.get(id).value("presentation").toString(),QString("superseded"));
    QCOMPARE(f.layers.declaration(user.layerId).title,QString("User change"));QCOMPARE(f.data.catalog.versions().size(),count);
  }
  void changedSourceCannotPublishComputedArtifacts() {
    Fixture f;QVERIFY(f.init());const auto c=f.compute("changed");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    const auto before=f.data.catalog.versions().size();QFile input(f.data.temp.filePath("input.tif"));
    QVERIFY(input.open(QIODevice::Append));QCOMPARE(input.write("changed"),qint64(7));input.close();
    const auto published=f.phase("changed-publish","publish",c);QVERIFY(!published.value("toolOk").toBool());
    QCOMPARE(f.data.catalog.versions().size(),before);QVERIFY(f.layers.declared().isEmpty());
  }
  void freshHostPublishesFrozenResultsAfterDocumentReopenWithoutModel() {
    Fixture f;QVERIFY(f.init());const auto c=f.compute("restart");QVERIFY2(!c.isEmpty(),qPrintable(f.error));
    const auto before=f.data.catalog.versions().size();const auto epoch=f.data.session.epoch();
    f.data.session.close();QVERIFY(f.data.bind());QVERIFY(f.data.session.epoch()>epoch);
    paleo::capability::CapabilityHost restarted;configureCapabilityCallers(&restarted);restarted.setTaskService(&f.data.tasks);
    restarted.setBindingProvider([&]{return captureCapabilityBinding(&f.data.session,&f.data.catalog,&f.lock,&f.selection);});
    f.assist.setOnnxService(nullptr);
    bindChatToolRunner(&f.chat,&f.assist,&f.layers,&f.data.catalog,f.data.project,f.data.session.documentId(),&restarted,&f.data.session);
    const auto pub=f.phase("restart-publish","publish",c);QVERIFY2(pub.value("toolOk").toBool(),qPrintable(QJsonDocument(pub).toJson()));
    const auto facts=f.get(pub.value("operationId").toString());QCOMPARE(facts.value("commitState").toString(),QString("committed"));
    QCOMPARE(facts.value("presentation").toString(),QString("presented"));QCOMPARE(facts.value("computeCount").toInt(),0);
    QCOMPARE(f.data.catalog.versions().size(),before+3);QCOMPARE(f.layers.declared().size(),3);
  }
};
int main(int argc,char **argv) {
  if(!QgisRuntime::initialize())return 2;
  TestTileAgent test;const int result=QTest::qExec(&test,argc,argv);QgisRuntime::shutdown();return result;
}
#include "tst_tile_agent.moc"
