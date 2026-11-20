// 层：测试壳
#include <QtTest>
#include <qgsapplication.h>
#include "workflow/capabilityhost.h"
#include "workflow/capabilitynativeoperation.h"
#include "workflow/chatresultstore.h"
#include "services/toolresultprotocol.h"
#include "services/storagegovernance.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include "services/subintervalfactors.h"
#include "io/areamodel.h"
#include "services/stratigraphicsubintervals.h"
#include <QCryptographicHash>
#include "workflow/analysisresultsync.h"
#include "workflow/constraintanalysisidentity.h"
#include "workflow/derivedassets.h"
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
  void chatInstanceCannotGrantAnUnboundCallerScope() {
    CapabilityHost unbound;QVERIFY(unbound.callerScope(host::Caller::Agent,"session").isEmpty());
    QCOMPARE(unbound.prepare(depth("unbound"),host::Caller::Agent,{}, {}, {},"session").code,QString("caller_unbound"));
  }
  void depthHostPublishesRealValuesAndReplaysOffline() {
    Host h;QVERIFY(h.init());auto r=depth("depth");const auto p=h.service.prepare(r,host::Caller::Agent,{}, {}, {},"chat1");QVERIFY2(p.ok,qPrintable(p.code));
    bool done=false;ExecuteResult result;QString error;
    QVERIFY2(h.service.start(p.handle,r,this,"depth",[&](ExecuteResult out){result=out;done=true;},&error),qPrintable(error));QTRY_VERIFY(done);
    QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));QCOMPARE(h.catalog.versions().size(),1);
    const auto version=h.catalog.versions().first();QFile output(h.root.path()+'/'+version.path);QVERIFY(output.open(QIODevice::ReadOnly));
    const auto rows=QJsonDocument::fromJson(output.readAll()).array();QCOMPARE(rows.size(),1);QVERIFY(rows.first().toObject().value("depthM").toDouble()>0);
    const auto roots=storage::operationRetention(h.root.path());QVERIFY2(roots.complete,qPrintable(roots.code));
    QVERIFY(roots.protectsPath(output.fileName()));
    auto restored=storage::snapshot(&h.catalog);restored.versions.clear(); // Simulate a Catalog snapshot without the committed row.
    const auto report=storage::scan(restored);QVERIFY(report.complete);QVERIFY(report.orphans.isEmpty());
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
  // P3/W4+W5：真实宿主冻结（freezeConstraintIdwRequest）的 constraint.idw
  // compute→publish 全链：单元身份/父版本进入冻结计划与发布成员 extra，
  // agent_result 版本经通用投影获得指向真实单元的 single_factor 绑定。
  void constraintIdwHostChainFreezesUnitAndPublishesEvidence()
  {
    Host h;QVERIFY(h.init());
    // DS 方案（单元身份权威）+ 宿主绑定注入真实冻结分支。
    paleo::strat::UnitSelection s1;
    {
      std::string detail;paleo::data::DataService ds;
      QVERIFY(ds.createProject(paleo::area_model::directory(h.root.path()).toStdString(),"p3-host",detail));
      const auto raw=h.root.filePath(QStringLiteral("p3host-source.txt"));
      {QFile file(raw);QVERIFY(file.open(QIODevice::WriteOnly));file.write("p3host");file.close();}
      paleo::data::ParsedFile parsed;parsed.path=raw.toStdString();parsed.kind="test";parsed.status="parsed";
      paleo::data::WellDataset well;well.wellInfo.wellName="W-1";well.wellInfo.wellheadX=0;well.wellInfo.wellheadY=0;
      well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";parsed.wells.push_back(well);
      long long sourceId=0;
      QVERIFY(ds.importPreparedFile(raw.toStdString(),raw.toStdString(),raw.toStdString(),
        QCryptographicHash::hash(QByteArray("p3host"),QCryptographicHash::Sha256).toHex().toStdString(),parsed,sourceId,detail));
      std::vector<paleo::data::WellSummary> wells;QVERIFY(ds.listWells(wells,detail));
      paleo::data::StratigraphicScheme scheme;std::vector<paleo::data::FrameworkIssue> issues;
      QVERIFY(ds.commitStratigraphicScheme(paleo::strat::draftScheme("scheme","scheme",{"TOP","BASE"},
        {{wells.front().id,{{"TOP",100.},{"BASE",200.}}}}),{},scheme,issues,detail));
      s1=paleo::strat::makeSelection(scheme,QString::fromStdString(scheme.intervals.front().id.value),1);
      ds.closeProject();
    }
    QVERIFY(s1.isValid());
    h.service.setBindingProvider([this,&h,&s1] {
      HostBinding b;b.projectDir=h.root.path();b.operationDir=h.root.path()+"/.paleo/capability-ops";b.catalog=&h.catalog;b.writeClaim=h.lock.claim();
      b.context.documentId="doc-native";b.context.epoch=h.epoch;b.context.open=true;b.context.persistentIdentity=true;
      b.context.readOnly=!h.writable;b.context.ownsWrites=h.writable;b.context.allowedEffects=host::Read|host::PrivateStage|host::CatalogPublish|host::Presentation;
      b.freeze=[this,&h,&s1](ExecuteRequest &r){
        if(r.capabilityId=="constraint.idw")
          return paleo::constraint_identity::freezeConstraintIdwRequest(r,h.root.path(),&h.catalog,s1);
        r.bindingMode="pinned-value";return QString();};
      b.validateInputs=[&h](const ExecuteRequest&){return h.providerOnline?QString():QString("provider_offline");};return b;
    });
    // 受管井点版本：points_ref 解析为父版本。
    QString pointsVersion;
    {
      DerivedAssetRegistrar registrar(&h.catalog,h.root.path());QString error;
      const auto st=registrar.stage(QStringLiteral("well_factor_points"),QStringLiteral("p"),QStringLiteral("p.gpkg"),&error);
      QVERIFY(st.isValid());QFile file(st.absolutePath);QVERIFY(file.open(QIODevice::WriteOnly));file.write("pts");file.close();
      DerivedFileFingerprint fp;fp.sha256=QStringLiteral("cc");DerivedOutputSetReceipt receipt;
      QVariantMap extra;extra.insert(QStringLiteral("manifest_layer_id"),QStringLiteral("wellfactor.sample.sandthick"));
      QVERIFY(registrar.commit(st,{},QStringLiteral(""),extra,&error,&fp));pointsVersion=st.versionId;
    }
    ExecuteRequest r;r.capabilityId="constraint.idw";r.clientRequestId="idw-unit";r.publishIntent="publish";
    r.parameters={{"FIELD","z"},{"CELL_SIZE",25.0}};QVariantList points;
    for(int i=0;i<4;++i)points.append(QVariantMap{{"x",(i%2)*100.0},{"y",(i/2)*100.0},{"z",(i/2)*10.0},{"well_id",QString::number(i)}});
    r.inputs={{"points",points},{"points_ref",QStringLiteral("catalog:")+pointsVersion},{"subinterval_id",s1.subIntervalId}};
    const auto p=h.service.prepare(r,host::Caller::Agent);QVERIFY2(p.ok,qPrintable(p.code));
    bool done=false;ExecuteResult result;QString error;
    QVERIFY2(h.service.start(p.handle,r,this,"idw-unit",[&](ExecuteResult out){result=out;done=true;},&error),qPrintable(error));
    QTRY_VERIFY(done);QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));
    QCOMPARE(h.catalog.versions().size(),2);
    // 发布成员携带宿主冻结的单元身份 + 实际消费的父版本。
    CatalogVersion published;
    for(const auto &v:h.catalog.versions())if(v.id!=pointsVersion)published=v;
    QVERIFY(!published.id.isEmpty());
    QCOMPARE(published.parentVersionIds,QStringList{pointsVersion});
    QCOMPARE(published.extra.value(QStringLiteral("subIntervalId")).toString(),s1.subIntervalId);
    QCOMPARE(published.extra.value(QStringLiteral("schemeRevision")).toString(),s1.revision);
    QVERIFY(published.extra.value(QStringLiteral("subinterval_verified")).toBool());
    QCOMPARE(published.extra.value(QStringLiteral("algorithm_id")).toString(),QString("paleo:paleo_constraint_idw"));
    // source_operation_id 后续发布：沿用原冻结上下文（单元/父版本不变）。
    ExecuteRequest reuse;r.capabilityId="constraint.idw";reuse.clientRequestId="idw-reuse";reuse.publishIntent="publish";
    reuse.inputs={{"source_operation_id",result.operationId}};
    const auto q=h.service.prepare(reuse,host::Caller::Agent);QVERIFY2(q.ok,qPrintable(q.code));done=false;
    QVERIFY(h.service.start(q.handle,reuse,this,"idw-reuse",[&](ExecuteResult out){result=out;done=true;},&error));QTRY_VERIFY(done);
    QVERIFY2(result.ok,qPrintable(result.code));QCOMPARE(result.commitState,QString("committed"));
    QCOMPARE(h.catalog.versions().size(),2); // 复用不新建数值/版本
    // 无效请求在数值前拒绝。
    ExecuteRequest mismatch;mismatch.capabilityId="constraint.idw";mismatch.clientRequestId="idw-mismatch";mismatch.publishIntent="compute-only";
    mismatch.parameters=r.parameters;mismatch.inputs=r.inputs;mismatch.inputs.insert("subinterval_id",QStringLiteral("S9"));
    QCOMPARE(h.service.prepare(mismatch,host::Caller::Agent).code,QString("subinterval_mismatch"));
    // 通用投影把 Host 核验的 agent_result 绑定为指向真实单元的 single_factor。
    QVERIFY2(syncAnalysisResults(&h.catalog,h.root.path()).isEmpty(),"projection must succeed");
    paleo::data::DataService ds;std::string detail;
    QVERIFY(ds.openProjectReadOnly(paleo::area_model::directory(h.root.path()).toStdString(),detail));
    std::vector<paleo::data::SubIntervalResultBinding> bindings;
    QVERIFY(ds.listSubIntervalResults(s1.schemeId.toStdString(),paleo::data::SubIntervalId{s1.subIntervalId.toStdString()},bindings,detail));
    QVERIFY(std::any_of(bindings.begin(),bindings.end(),[&](const auto &b){
      return b.kind=="single_factor"&&b.id=="catalog."+published.id.toStdString()&&b.assetRef==published.id.toStdString();}));
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
    QCOMPARE(result.summary.value("child_operations").toList().size(),2);
    QCOMPARE(h.service.preparePlan(host,nodes,edges,host::Caller::Agent).code,QString("idempotency_conflict"));
    const auto state=h.service.recovery()->get("agent",result.operationId);QCOMPARE(state.value("computeCount").toInt(),1);
    nodes[0]=idw;h.providerOnline=false;const auto replay=h.service.preparePlan(host,nodes,edges,host::Caller::Agent);QVERIFY(replay.ok);
  }
  void replayTicketsStillRespectHostCapacity() {
    Host h;QVERIFY(h.init());auto r=depth("replay-capacity");const auto p=h.service.prepare(r,host::Caller::Agent);QVERIFY(p.ok);
    bool done=false;QString error;QVERIFY(h.service.start(p.handle,r,this,"depth",[&](ExecuteResult){done=true;},&error));QTRY_VERIFY(done);
    for(int i=0;i<128;++i)QVERIFY(h.service.prepare(r,host::Caller::Agent).ok);
    QCOMPARE(h.service.prepare(r,host::Caller::Agent).code,QString("prepared_capacity_exceeded"));
    ++h.epoch;QVERIFY(h.service.prepare(r,host::Caller::Agent).ok);
  }
  void hostCommitFactsSurviveChatBoundAndModelCacheFailure() {
    Host h;QVERIFY(h.init());auto request=depth("model-facts");request.documentId="doc-native";
    request.callerScope=h.service.callerScope(host::Caller::Agent,"chat");request.projectDir=h.root.path();request.bindingMode="pinned-value";
    QString code;const auto cap=CapabilityRegistry::spec(request.capabilityId,1);
    request.parameters=effectiveScalarParams(cap.algorithmId,cap.algorithmContractVersion,request.parameters,&code);QCOMPARE(code,QString("ok"));code.clear();
    auto frozen=freezeNativeOperation(request,nativeIntent(request),&code);QVERIFY(code.isEmpty());
    frozen.presentationIntent={{"target","layer"},{"expectedState","initial"}};
    h.service.setPresentationHandlers([](const QJsonObject&){return QString("initial");},
      [](const QJsonObject&,const PublicationReceipt&){return QString("pending");});
    const auto accepted=h.service.recovery()->accept(frozen);QVERIFY2(accepted.ok,qPrintable(accepted.code));
    bool done=false;QString error;ExecuteResult result;
    QVERIFY(h.service.recovery()->start(request.callerScope,accepted.record.operationId,this,"native publication",[&](ExecuteResult r){result=r;done=true;},false,&error));QTRY_VERIFY(done);
    QCOMPARE(result.commitState,QString("committed"));QCOMPARE(result.presentation,QString("pending"));QVERIFY(result.ok);
    const auto check=[&](const ExecuteResult &native,bool oversized) {
      auto supplied=native;
      if(oversized) {
        auto receipt=supplied.summary.value("receipt").toMap();receipt.insert("longDiagnostic",QString(70*1024,'x'));supplied.summary.insert("receipt",receipt);
        supplied.summary.insert("result_path","C:/private/never-send");
      }
      const auto raw=chatToolResponse(supplied);const auto bytes=QJsonDocument(raw).toJson(QJsonDocument::Compact);
      QVERIFY(bytes.size()<=64*1024);QCOMPARE(raw.value("contract").toString(),QString("paleo.operation.v2"));
      QCOMPARE(raw.value("code").toString(),native.code);QCOMPARE(raw.value("execution").toString(),QString("succeeded"));
      QCOMPARE(raw.value("commitState").toString(),QString("committed"));QCOMPARE(raw.value("presentation").toString(),QString("pending"));
      QCOMPARE(raw.value("requestHash").toString(),native.requestHash);QCOMPARE(raw.value("resultRef").toObject().value("operationId").toString(),native.operationId);
      QVERIFY(!raw.value("revision").toString().isEmpty());QCOMPARE(raw.value("computeCount").toInt(),1);QVERIFY(!bytes.contains("private/never-send"));
      if(oversized) {
        QCOMPARE(raw.value("transport").toObject().value("code").toString(),QString("response_budget_exceeded"));
        QVERIFY(!raw.value("commitReceiptRef").toString().isEmpty());
      }
      chat::ChatResultStore::Limits limits;limits.handles=0;chat::ChatResultStore store(limits);store.setScope("chat","doc-native",1);
      const auto model=store.encode(QString::fromUtf8(bytes),true,768);
      QVERIFY(chat::compactJson(model).size()<=768);QCOMPARE(model.value("status").toString(),QString("committed"));
      QCOMPARE(model.value("code").toString(),native.code);QCOMPARE(model.value("commitState").toString(),QString("committed"));
      QCOMPARE(model.value("presentation").toString(),QString("pending"));
      QCOMPARE(model.value("payloadContract").toString(),QString("paleo.operation.v2"));
      QCOMPARE(model.value("resultRef").toObject().value("operationId").toString(),native.operationId);
      QVERIFY(!model.value("commitReceiptRef").toString().isEmpty());
    };
    check(result,false);check(result,true);h.providerOnline=false;done=false;
    QVERIFY(h.service.recovery()->start(request.callerScope,result.operationId,this,"replay",[&](ExecuteResult r){result=r;done=true;},false,&error));QTRY_VERIFY(done);
    check(result,true);QCOMPARE(h.catalog.versions().size(),1);
  }
};
int main(int argc,char **argv) {
  QgsApplication app(argc,argv,false);app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"),true);app.initQgis();
  NativeRecoveryTests test;const auto code=QTest::qExec(&test,argc,argv);QgsApplication::exitQgis();return code;
}
#include "tst_capability_recovery_native.moc"
