// 层：测试壳
#include <QtTest>
#include <qgsapplication.h>
#include "workflow/capabilityhost.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include "algorithms/velocitymodel.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <cmath>
using namespace paleo;
using namespace paleo::capability;
namespace {
struct Host {
  QTemporaryDir root;DataCatalog catalog;ProjectDirLock lock{root.path()};PaleoTaskService tasks;CapabilityHost service;
  bool providerOnline=true,writable=true;quint64 epoch=1;
  bool init() {
    if(!lock.tryLock() || !catalog.open(root.path()))return false;
    service.setTaskService(&tasks);service.setCallerScope(host::Caller::Agent,"agent");
    service.setBindingProvider([this] {
      HostBinding b;b.projectDir=root.path();b.operationDir=root.path()+"/.paleo/capability-ops";b.catalog=&catalog;b.writeClaim=lock.claim();
      b.context.documentId="doc-native";b.context.epoch=epoch;b.context.open=true;b.context.persistentIdentity=true;
      b.context.readOnly=!writable;b.context.ownsWrites=writable;b.context.allowedEffects=host::Read|host::PrivateStage|host::CatalogPublish|host::Presentation;
      b.freeze=[this](ExecuteRequest &r){if(!providerOnline)return QString("provider_offline");r.bindingMode="pinned-value";return QString();};
      b.validateInputs=[this](const ExecuteRequest&){return providerOnline?QString():QString("provider_offline");};return b;
    });return true;
  }
};
ExecuteRequest depth(const QString &id) {
  velmodel::VelocityWellControl w;w.wellId="W1";w.x=0;w.y=0;w.knots={{100,200},{200,500}};QString error;
  const auto model=velmodel::VelocityModel::fit({w},velmodel::ModelType::IntervalAverage,&error);
  ExecuteRequest r;r.capabilityId="depth.convert_batch";r.clientRequestId=id;r.publishIntent="publish";
  r.parameters={{"TIME_SEMANTIC","TWT"},{"DEPTH_SEMANTIC","TVD"},{"EXTRAPOLATION","reject_outside"},{"MODEL_REVISION","v1"}};
  r.inputs={{"model",model.toJson().toVariantMap()},{"samples",QVariantList{QVariantMap{{"sampleId","s"},{"wellId","W1"},{"occurrenceId","o"},{"time",150.0}}}}};return r;
}
}
class NativeRecoveryTests:public QObject {
  Q_OBJECT
private slots:
  void depthHostPublishesRealValuesAndReplaysOffline() {
    Host h;QVERIFY(h.init());auto r=depth("depth");const auto p=h.service.prepare(r,host::Caller::Agent,{}, {}, {},"chat1");QVERIFY2(p.ok,qPrintable(p.code));
    bool done=false;ExecuteResult result;QString error;
    QVERIFY2(h.service.start(p.handle,r,this,"depth",[&](ExecuteResult out){result=out;done=true;},&error),qPrintable(error));QTRY_VERIFY(done);
    QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);
    const auto version=h.catalog.versions().first();QFile output(h.root.path()+'/'+version.path);QVERIFY(output.open(QIODevice::ReadOnly));
    const auto rows=QJsonDocument::fromJson(output.readAll()).array();QCOMPARE(rows.size(),1);QVERIFY(rows.first().toObject().value("depthM").toDouble()>0);
    h.providerOnline=false;h.writable=false;++h.epoch;
    const auto replay=h.service.prepare(r,host::Caller::Agent,{}, {}, {},"chat1");QVERIFY2(replay.ok,qPrintable(replay.code));done=false;
    QVERIFY(h.service.start(replay.handle,r,this,"replay",[&](ExecuteResult out){result=out;done=true;},&error));QTRY_VERIFY(done);
    QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);
    QCOMPARE(h.service.prepare(r,host::Caller::Agent,{}, {}, {},"chat2").code,QString("project_read_only"));
  }
  void seismicHostPublishesAllRequestedPorts() {
    Host h;QVERIFY(h.init());ExecuteRequest r;r.capabilityId="seismic.attributes.compute";r.clientRequestId="seis";r.publishIntent="publish";
    r.parameters={{"ATTRIBUTES",QStringList{"rms","envelope","phase"}},{"SAMPLE_INTERVAL_MS",4.0}};
    QVariantList trace;for(int i=0;i<16;++i)trace.append(std::sin(.25*i));r.inputs={{"traces",QVariantList{QVariant(trace)}},{"full_trace",true}};
    const auto p=h.service.prepare(r,host::Caller::Agent);QVERIFY2(p.ok,qPrintable(p.code));bool done=false;ExecuteResult result;QString error;
    QVERIFY(h.service.start(p.handle,r,this,"seis",[&](ExecuteResult out){result=out;done=true;},&error));QTRY_VERIFY(done);
    QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),3);
    for(const auto &v:h.catalog.versions()){QFile file(h.root.path()+'/'+v.path);QCOMPARE(file.size(),qint64(64));QVERIFY(!v.sha256.isEmpty());}
  }
  void idwContourRestartedReferenceAndPreparedPublish() {
    Host h;QVERIFY(h.init());ExecuteRequest r;r.capabilityId="constraint.idw";r.clientRequestId="idw";r.publishIntent="compute-only";
    r.parameters={{"FIELD","z"},{"CELL_SIZE",25.0}};QVariantList points;
    for(int i=0;i<4;++i)points.append(QVariantMap{{"x",(i%2)*100.0},{"y",(i/2)*100.0},{"z",(i/2)*10.0},{"well_id",QString::number(i)}});
    r.inputs={{"points",points},{"points_ref","v-points"},{"subinterval_id","S"}};
    const auto p=h.service.prepare(r,host::Caller::Agent);QVERIFY2(p.ok,qPrintable(p.code));bool done=false;ExecuteResult result;QString error;
    QVERIFY(h.service.start(p.handle,r,this,"idw",[&](ExecuteResult out){result=out;done=true;},&error));QTRY_VERIFY(done);QVERIFY2(result.ok,qPrintable(result.code));
    const auto sourceId=result.operationId;
    ExecuteRequest contour;contour.capabilityId="singlefactor.contours";contour.clientRequestId="contour";contour.publishIntent="publish";
    contour.parameters={{"CONTOUR_LEVEL",5.0}};contour.inputs={{"operation_result",QVariantMap{{"operationId",sourceId},{"portId","OUTPUT"}}}};
    const auto c=h.service.prepare(contour,host::Caller::Agent);QVERIFY2(c.ok,qPrintable(c.code));done=false;
    QVERIFY(h.service.start(c.handle,contour,this,"contour",[&](ExecuteResult out){result=out;done=true;},&error));QTRY_VERIFY(done);
    QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);
    auto publish=r;publish.clientRequestId="publish-prepared";publish.publishIntent="publish";publish.parameters.clear();publish.inputs={{"source_operation_id",sourceId}};
    h.providerOnline=false;const auto q=h.service.prepare(publish,host::Caller::Agent);QVERIFY2(q.ok,qPrintable(q.code));done=false;
    QVERIFY2(h.service.start(q.handle,publish,this,"publish",[&](ExecuteResult out){result=out;done=true;},&error),qPrintable(error));QTRY_VERIFY(done);
    QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),2);
  }
  void parentGraphIdentityIsCheckedBeforeAnyNewChild() {
    Host h;QVERIFY(h.init());ExecuteRequest host;host.clientRequestId="parent-dag";host.publishIntent="publish";
    QVariantList points;for(int i=0;i<4;++i)points.append(QVariantMap{{"x",(i%2)*100.0},{"y",(i/2)*100.0},{"z",(i/2)*10.0},{"well_id",QString::number(i)}});
    PlanNode idw{"idw,|节点","constraint.idw",1,"OUTPUT","compute-only",{{"FIELD","z"},{"CELL_SIZE",25.0}},
      {{"points",points},{"points_ref","v-points"},{"subinterval_id","S"}}};
    PlanNode contour{"contours|,节点","singlefactor.contours",1,"LINES","compute-only",{{"CONTOUR_LEVEL",5.0}}, {}};
    QVector<PlanNode> nodes{idw,contour};QVector<PlanEdge> edges{{idw.id,"OUTPUT",contour.id,"RASTER"}};
    const auto p=h.service.preparePlan(host,nodes,edges,host::Caller::Agent);QVERIFY2(p.ok,qPrintable(p.code));
    bool done=false;ExecuteResult result;QString error;
    QVERIFY2(h.service.start(p.handle,host,this,"DAG",[&](ExecuteResult out){result=out;done=true;},&error),qPrintable(error));QTRY_VERIFY(done);
    QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);
    nodes[0].parameters["CELL_SIZE"]=10.0;
    QCOMPARE(h.service.preparePlan(host,nodes,edges,host::Caller::Agent).code,QString("idempotency_conflict"));
    const auto state=h.service.recovery()->get("agent",result.operationId);QCOMPARE(state.value("computeCount").toInt(),1);
    nodes[0]=idw;h.providerOnline=false;const auto replay=h.service.preparePlan(host,nodes,edges,host::Caller::Agent);QVERIFY(replay.ok);
  }
};
int main(int argc,char **argv) {
  QgsApplication app(argc,argv,false);app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH","/usr"),true);app.initQgis();
  NativeRecoveryTests test;const auto code=QTest::qExec(&test,argc,argv);QgsApplication::exitQgis();return code;
}
#include "tst_capability_recovery_native.moc"
