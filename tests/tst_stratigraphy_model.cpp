#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <paleo_data/service.h>
#include "../src/io/areamodel.h"
#include "../src/services/stratigraphymodel.h"
#include "../src/workflow/stratigraphyworkflow.h"
#include "../src/domain/arearules.h"
#include "../src/domain/mappinghorizons.h"
class StratigraphyModelTest : public QObject {
 Q_OBJECT
private slots:
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
};
QTEST_GUILESS_MAIN(StratigraphyModelTest)
#include "tst_stratigraphy_model.moc"
