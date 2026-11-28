#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSignalSpy>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <paleo_data/service.h>
#include "../src/io/areamodel.h"
#include "../src/services/stratigraphymodel.h"
#include "../src/workflow/stratigraphyworkflow.h"
#include "../src/domain/arearules.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/domain/wellcompositemodel.h"
class StratigraphyModelTest : public QObject {
 Q_OBJECT
private:
 // 合成工区：单井 LAS + 一条已保存解释（NEW_TOP=100 / NEW_BASE=103）。
 static bool buildSyntheticProject(const QString &path,qlonglong *wellId,QString *revision,const QStringList &depthUnits={}) {
  paleo::data::DataService service;std::string error;
  const auto directory=paleo::area_model::directory(path).toStdString();
  if(!service.createProject(directory,"Synthetic",error))return false;
  const auto source=QDir(path).filePath("SYNTH.las");QFile las(source);
  if(!las.open(QIODevice::WriteOnly))return false;
  las.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH\nNULL. -999.25\n~Curve\nDEPT.M\nGR.API\n~Ascii\n100 5\n101 6\n102 7\n103 8\n");las.close();
  paleo::data::ImportReport report;
  if(!service.importFile(source.toStdString(),{},report,error))return false;
  std::vector<paleo::data::WellSummary> wells;
  if(!service.listWells(wells,error)||wells.empty())return false;
  std::vector<paleo::data::DataEntry> curves;
  if(!service.listData("curves",wells.front().id,curves,error)||curves.empty())return false;
  paleo::data::CorrelationInputRequest request;paleo::data::CorrelationWellSelection selection;
  selection.wellId=wells.front().id;selection.curveId=curves.front().id;selection.confirmedVertical=true;request.wells.push_back(selection);
  paleo::data::CorrelationInputSnapshot snapshot;
  if(!service.captureCorrelationInputs(request,snapshot,error))return false;
  paleo::data::InterpretationDocument document;document.name="Synthetic classification";
  document.projectId=snapshot.projectId;document.snapshot=snapshot;
  paleo::data::WellInterpretation well;well.wellId=wells.front().id;well.name="SYNTH";well.input=snapshot.wells.front();
  paleo::data::InterpretationBoundary top;top.id="top";top.name="NEW_TOP";top.role="T70";top.md=100;top.originalMd=100;
  auto bottom=top;bottom.id="bottom";bottom.name="NEW_BASE";bottom.role="T72";bottom.md=103;bottom.originalMd=103;
  // 解释版本的派生曲线：基础模型读不出时页面仍会显示它（#827 的触发条件）。
  paleo::data::InterpretationSeries curve;curve.id="series-gr";curve.name="GR_DERIVED";
  curve.axisKind="MD";curve.axisUnit="m";curve.axis={100,101,102,103};curve.values={5,6,7,8};
  well.series.push_back(curve);
  for(const auto &spec:depthUnits){
   const auto parts=spec.split(QLatin1Char('='));if(parts.size()!=2)return false;
   paleo::data::InterpretationSeries extra;extra.id=parts[0].toStdString();extra.name=extra.id;
   extra.axisKind="MD";extra.axisUnit=parts[1].toStdString();extra.axis={100};extra.values={1};
   well.series.push_back(std::move(extra));
  }
  well.boundaries={top,bottom};document.wells.push_back(well);
  paleo::data::InterpretationCommitResult saved;
  if(!service.commitInterpretation(document,{"","first"},saved,error))return false;
  if(wellId)*wellId=wells.front().id;
  if(revision)*revision=QString::fromStdString(saved.revision);
  return true;
 }
 // 把最新修订的 payload 改成非法 JSON：模拟 schema 升级不完整/库损坏。
 static bool corruptRevision(const QString &path,const QString &revision) {
  const QString file=paleo::area_model::directory(path)+"/data.sqlite";bool ok=false;
  {
   auto db=QSqlDatabase::addDatabase("QSQLITE","tst_stratigraphy_corrupt");
   db.setDatabaseName(file);
   if(!db.open())return false;
   QSqlQuery query(db);
   query.prepare("UPDATE interpretation_revisions SET payload=?,digest=? WHERE revision=?");
   query.addBindValue(QByteArray("\x01",1));query.addBindValue(QStringLiteral("deadbeef"));query.addBindValue(revision);
   ok=query.exec()&&query.numRowsAffected()==1;
   db.close();
  }
  QSqlDatabase::removeDatabase("tst_stratigraphy_corrupt");
  return ok;
 }
private slots:
 void unreadableLatestRevisionFailsLoudly() {
  QTemporaryDir project;QVERIFY(project.isValid());
  qlonglong wellId=0;QString revision;
  QVERIFY(buildSyntheticProject(project.path(),&wellId,&revision));
  QVERIFY(corruptRevision(project.path(),revision));

  // #826：模型存在但读不出 → Failed，且不跳过损坏修订去借更早版本的 tops。
  const ModelTopsResult model=readStratigraphyModelTops(project.path(),wellId);
  QVERIFY(model.status==ModelTopsStatus::Failed);
  QVERIFY(!model.error.isEmpty());
  QVERIFY(model.tops.isEmpty());
  QVERIFY(stratigraphyModelTops(project.path(),wellId).isEmpty());

  // #828：打开工程时读取失败必须报错，不能当成「没有解释」。
  StratigraphyWorkflow workflow(nullptr);QSignalSpy status(&workflow,&StratigraphyWorkflow::statusChanged);
  workflow.setProject(project.path(),false);
  QVERIFY(!workflow.documentLoadError().isEmpty());
  QCOMPARE(status.count(),1);
  QVERIFY(status.first().first().toString().contains(QStringLiteral("读取失败")));
  QVERIFY(workflow.document().wells.empty());
  AreaRules::reset();
 }
 void baseModelFailureSurvivesDerivedTracks() {
  QTemporaryDir project;QVERIFY(project.isValid());
  qlonglong wellId=0;QString revision;
  QVERIFY(buildSyntheticProject(project.path(),&wellId,&revision));
  StratigraphyWorkflow workflow(nullptr);
  workflow.setProject(project.path(),false);
  QVERIFY(workflow.documentLoadError().isEmpty());
  QVERIFY(!workflow.tracks().isEmpty()); // 解释版本里有派生曲线，装载仍会「成功」
  QVERIFY(QFile::remove(paleo::area_model::directory(project.path())+"/data.sqlite"));

  WellComposite::ComprehensiveWellData data;QString error;bool baseLoaded=true;
  QVERIFY(workflow.previewWellData(wellId,&data,&error,&baseLoaded));
  QVERIFY(!baseLoaded); // #827：调用方能看出基础模型失败
  QVERIFY(!error.isEmpty());
  QVERIFY(data.dataWarnings.join(QString()).contains(QStringLiteral("基础模型读取失败")));
  AreaRules::reset();
 }
 void footDepthUnitsStayOnTracks() {
  QTemporaryDir project;QVERIFY(project.isValid());
  qlonglong wellId=0;
  QVERIFY(buildSyntheticProject(project.path(),&wellId,nullptr,{
   QStringLiteral("FOOT=FOOT"),QStringLiteral("FTDOT=ft."),
   QStringLiteral("F=f"),QStringLiteral("XYZ=XYZ")}));
  StratigraphyWorkflow workflow(nullptr);
  workflow.setProject(project.path(),true);
  QVERIFY(workflow.documentLoadError().isEmpty());
  const auto tracks=workflow.tracks();
  const auto find=[&](const QString &name)->const StratigraphyTrack *{
   for(const auto &track:tracks) if(track.mnemonic==name) return &track;
   return nullptr;
  };
  for(const auto &name:QStringList{QStringLiteral("FOOT"),QStringLiteral("FTDOT"),QStringLiteral("F")}){
   const auto *track=find(name);
   QVERIFY2(track,qPrintable(name));
   QCOMPARE(track->depths.size(),1);
   QCOMPARE(track->depths.front(),float(100.0*0.3048));
  }
  QVERIFY(!find(QStringLiteral("XYZ")));
  AreaRules::reset();
 }
 void savedClassificationFeedsAnalysis() {
  QTemporaryDir project;QVERIFY(project.isValid());
  paleo::data::DataService service;std::string error;
  const auto directory=paleo::area_model::directory(project.path()).toStdString();
  QVERIFY2(service.createProject(directory,"Synthetic",error),error.c_str());
  const auto source=project.filePath("SYNTH.las");QFile las(source);QVERIFY(las.open(QIODevice::WriteOnly));
  las.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH\nNULL. -999.25\n~Curve\nDEPT.M\nGR.API\n~Ascii\n100 5\n101 6\n102 7\n103 8\n");las.close();
  paleo::data::ImportReport report;QVERIFY2(service.importFile(source.toStdString(),{},report,error),error.c_str());
  std::vector<paleo::data::WellSummary> wells;QVERIFY(service.listWells(wells,error));QCOMPARE(wells.size(),size_t(1));
  const auto wellId=wells.front().id;
  std::vector<paleo::data::DataEntry> curves;QVERIFY(service.listData("curves",wellId,curves,error));QVERIFY(!curves.empty());
  paleo::data::CorrelationInputRequest request;paleo::data::CorrelationWellSelection selection;
  selection.wellId=wellId;selection.curveId=curves.front().id;selection.confirmedVertical=true;request.wells.push_back(selection);
  paleo::data::CorrelationInputSnapshot snapshot;QVERIFY2(service.captureCorrelationInputs(request,snapshot,error),error.c_str());
  paleo::data::InterpretationDocument document;document.id="native-test";document.name="Synthetic classification";
  document.projectId=snapshot.projectId;document.snapshot=snapshot;
  paleo::data::WellInterpretation well;well.wellId=wellId;well.name="SYNTH";well.input=snapshot.wells.front();
  paleo::data::InterpretationBoundary top;top.id="top";top.name="NEW_TOP";top.role="T70";top.md=100;top.originalMd=100;
  auto bottom=top;bottom.id="bottom";bottom.name="NEW_BASE";bottom.role="T72";bottom.md=103;bottom.originalMd=103;
  auto candidate=top;candidate.id="candidate";candidate.name="Rejected candidate";candidate.role="candidate:T70";candidate.md=101;
  well.boundaries={top,bottom,candidate};document.wells.push_back(well);
  paleo::data::InterpretationCommitResult saved;QVERIFY2(service.commitInterpretation(document,{"","first"},saved,error),error.c_str());
  auto tops=stratigraphyModelTops(project.path(),wellId);QCOMPARE(tops.size(),2);QCOMPARE(tops.front().horizon,QString("NEW_TOP"));QCOMPARE(tops.front().md,100.0);
  document.wells.front().boundaries.front().md=100.5;
  QVERIFY2(service.commitInterpretation(document,{saved.revision,"second"},saved,error),error.c_str());
  tops=stratigraphyModelTops(project.path(),wellId);QCOMPARE(tops.front().md,100.5);
  StratigraphyWorkflow workflow(nullptr);workflow.setProject(project.path(),false);QVERIFY(workflow.canEdit());
  QSignalSpy applied(&workflow,&StratigraphyWorkflow::classificationChanged);
  workflow.activateClassification(wellId);QCOMPARE(applied.count(),1);
  QCOMPARE(mappingHorizons(),QStringList({"NEW_TOP","NEW_BASE"}));
  workflow.setProject(project.path(),true);QVERIFY(!workflow.canEdit());
  AreaRules::reset();
 }
 void failedProjectAreaWriteKeepsPreviousFile() {
  QTemporaryDir project;QVERIFY(project.isValid());
  qlonglong wellId=0;
  QVERIFY(buildSyntheticProject(project.path(),&wellId,nullptr));
  StratigraphyWorkflow workflow(nullptr);
  workflow.setProject(project.path(),false);
  QVERIFY(workflow.canEdit());
  QSignalSpy status(&workflow,&StratigraphyWorkflow::statusChanged);
  QSignalSpy applied(&workflow,&StratigraphyWorkflow::classificationChanged);
  workflow.activateClassification(wellId);
  QCOMPARE(applied.count(),1);
  const QString path=AreaRules::configFilePath(project.path());
  QFile before(path);
  QVERIFY(before.open(QIODevice::ReadOnly));
  const QByteArray original=before.readAll();
  before.close();
  QVERIFY(!original.isEmpty());
  status.clear();
  applied.clear();
#ifdef Q_OS_WIN
  const HANDLE lock=CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()),
                                GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,nullptr);
  QVERIFY(lock!=INVALID_HANDLE_VALUE);
#else
  const QString dir=QFileInfo(path).absolutePath();
  const auto writable=QFile::permissions(dir);
  QVERIFY(QFile::setPermissions(dir,QFileDevice::ReadOwner|QFileDevice::ExeOwner|
                                    QFileDevice::ReadGroup|QFileDevice::ExeGroup|
                                    QFileDevice::ReadOther|QFileDevice::ExeOther));
#endif
  workflow.activateClassification(wellId);
#ifdef Q_OS_WIN
  CloseHandle(lock);
#else
  QVERIFY(QFile::setPermissions(dir,writable));
#endif
  QCOMPARE(applied.count(),0);
  QVERIFY(status.count()>=1);
  const QString message=status.last().first().toString();
  QVERIFY(!message.isEmpty());
  QVERIFY(!message.contains(QStringLiteral("已将所选井的地层界面应用到当前工程")));
  QFile after(path);
  QVERIFY(after.open(QIODevice::ReadOnly));
  QCOMPARE(after.readAll(),original);
  AreaRules::reset();
 }
};
QTEST_GUILESS_MAIN(StratigraphyModelTest)
#include "tst_stratigraphy_model.moc"
