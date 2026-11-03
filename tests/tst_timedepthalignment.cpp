// Synthetic SEG-Y fixture; no private well data.
#define main seismicFixtureMain
#include "../vendor/Data_service/tests/seismic_service_tests.cpp"
#undef main
#include "catalog/datacatalog.h"
#include "catalog/areawellprojection.h"
#include "io/areamodel.h"
#include "services/timedepthalignment.h"
#include "workflow/sectionworkbench.h"
#include "ui/seismicsection/timedepthalignmentdialog.h"
#include <QtTest>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

class TestTimeDepthAlignment:public QObject{
  Q_OBJECT
private slots:
  void sourcesAndVolumeCommitTogether(){
    QTemporaryDir temp;const auto project=temp.path()+"/project";DataService ds;std::string error;
    QVERIFY(ds.createProject(utf8(paleo::area_model::directory(project)),"Alignment",error));
    const auto path=temp.filePath("checkshots");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("# Petrel checkshots format\n# Unit in depth: m\nVERSION 1\nBEGIN HEADER\nX\nY\nZ\nTWT picked\nMD\nWell\nEND HEADER\n"
               "1 2 -100 -50 110 A\n1 2 -200 -100 210 A\n1 2 -100 -60 120 B\n1 2 -200 -110 220 B\n");file.close();
    ImportReport report;QVERIFY2(ds.importFile(utf8(path),{},report,error),error.c_str());
    const auto other=temp.filePath("checkshots2");QVERIFY(file.copy(other));
    QFile second(other);QVERIFY(second.open(QIODevice::ReadWrite));auto text=second.readAll();text.replace(" A\n"," C\n").replace(" B\n"," D\n");second.resize(0);second.write(text);second.close();
    QVERIFY2(ds.importFile(utf8(other),{},report,error),error.c_str());
    const auto seismic=temp.filePath("volume.sgy");fixture(seismic);const auto volumeId=importVolume(ds,seismic,error);
    std::vector<DataEntry> entries;QVERIFY(ds.listData("time_depth",0,entries,error));QCOMPARE(entries.size(),size_t(4));
    const auto id=entries.front().id;
    QVERIFY(!ds.confirmTimeDepthAlignment({id},"ms","TWT","zero",true,volumeId+999,error));
    TimeDepthTable table;QVERIFY(ds.readTimeDepth(id,table,error));QVERIFY(table.timeUnit.empty());QCOMPARE(table.depthTimePairs[0].timeValue,-50.);
    QVERIFY(!ds.confirmTimeDepthAlignment({id,999999},"ms","TWT","zero",true,volumeId,error));
    QVERIFY(ds.readTimeDepth(id,table,error));QVERIFY(table.timeUnit.empty());
    SeismicVolume before;QVERIFY(ds.readVolume(volumeId,before,error));auto owt=before;owt.sampling.timeType="OWT";
    QVERIFY(ds.saveVolume(volumeId,owt,error));QVERIFY(!ds.confirmTimeDepthAlignment({id},"ms","TWT","zero",true,volumeId,error));
    QVERIFY(ds.readTimeDepth(id,table,error));QVERIFY(table.timeReference.empty());QVERIFY(ds.saveVolume(volumeId,before,error));
    DataCatalog catalog;QString detail;QVERIFY2(catalog.open(project,&detail),qPrintable(detail));QVERIFY2(registerAreaWells(catalog,&detail),qPrintable(detail));
    const auto a=catalog.wellsMatchingName("A");QCOMPARE(a.size(),1);SectionWorkbench workbench(&catalog);
    const auto options=workbench.timeDepthAlignmentOptions(a,&detail);QVERIFY2(detail.isEmpty(),qPrintable(detail));
    QCOMPARE(options.value("sources").toList().size(),1);QCOMPARE(options.value("sources").toList().first().toMap().value("relations").toInt(),2);
    TimeDepthAlignmentDialog dialog(options);QSignalSpy requested(&dialog,&TimeDepthAlignmentDialog::saveRequested);
    auto *save=dialog.findChild<QPushButton*>("saveTimeDepthSemantics");QVERIFY(!save->isEnabled());
    dialog.findChild<QComboBox*>("timeDepthUnit")->setCurrentIndex(1);
    dialog.findChild<QComboBox*>("timeDepthSign")->setCurrentIndex(2);
    dialog.findChild<QLineEdit*>("timeDepthReference")->setText("shared zero");
    dialog.findChild<QComboBox*>("timeDepthVolume")->setCurrentIndex(1);QVERIFY(save->isEnabled());save->click();QCOMPARE(requested.size(),1);
    QSignalSpy changed(&workbench,&SectionWorkbench::alignmentsChanged);
    QVERIFY2(workbench.saveTimeDepthAlignment(requested.first()[0].toMap(),&detail),qPrintable(detail));QCOMPARE(changed.size(),1);
    for(const auto &entry:entries){QVERIFY(ds.readTimeDepth(entry.id,table,error));
      if(entry.sourceId==entries.front().sourceId){QCOMPARE(table.timeReference,std::string("shared zero"));QVERIFY(table.depthTimePairs[0].timeValue>0);}
      else QVERIFY(table.timeReference.empty());
    }
    QVERIFY(ds.confirmTimeDepthAlignment({id,entries.back().id},"ms","TWT","shared zero",true,volumeId,error));
    for(const auto &entry:entries){QVERIFY(ds.readTimeDepth(entry.id,table,error));QCOMPARE(table.timeReference,std::string("shared zero"));QVERIFY(table.depthTimePairs[0].timeValue>0);}
    SeismicVolume after;QVERIFY(ds.readVolume(volumeId,after,error));QCOMPARE(after.sampling.timeType,std::string("TWT"));QCOMPARE(after.sampling.timeReference,std::string("shared zero"));
    QCOMPARE(after.sampling.firstTime,before.sampling.firstTime);QCOMPARE(after.sampling.sampleInterval,before.sampling.sampleInterval);QCOMPARE(after.sampling.sampleCount,before.sampling.sampleCount);
    QVERIFY(ds.confirmTimeDepthAlignment({id},"ms","TWT","shared zero",true,volumeId,error));QVERIFY(ds.readTimeDepth(id,table,error));QCOMPARE(table.depthTimePairs[0].timeValue,50.);
    QVERIFY(ds.confirmTimeDepthSemantics(id,"ms","TWT","shared zero",false,error));QVERIFY(ds.readTimeDepth(id,table,error));QCOMPARE(table.depthTimePairs[0].timeValue,-50.);
    catalog.setLockedReadOnly(true);QVERIFY(!workbench.saveTimeDepthAlignment(requested.first()[0].toMap(),&detail));
    const auto readOnly=workbench.timeDepthAlignmentOptions(a,&detail);TimeDepthAlignmentDialog readonlyDialog(readOnly);
    QVERIFY(!readonlyDialog.findChild<QPushButton*>("saveTimeDepthSemantics")->isEnabled());
  }
};
QTEST_MAIN(TestTimeDepthAlignment)
#include "tst_timedepthalignment.moc"
