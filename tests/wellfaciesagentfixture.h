// 层：测试壳
#include "services/wellfaciesinputplan.h"
#include "services/wellfaciesinputstore.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDir>
#include <algorithm>
#include <cmath>

namespace {
namespace data=paleo::data;
namespace wf=paleo::wellfacies;
struct Fixture {
  QTemporaryDir temp;
  explicit Fixture(const QString &parent={})
    :temp((parent.isEmpty()?QDir::tempPath():parent)+"/paleo-well-XXXXXX"){}
  wf::InputJob job;
  data::DataService service;
  std::string error;
  bool init(QString curveDepthType="MD",QString curveUnit="m",bool holes=false,bool duplicate=false,QString featureUnit="API",QString reference="KB") {
    job.projectDir=temp.filePath("project");
    if(!service.createProject(paleo::area_model::directory(job.projectDir).toStdString(),"synthetic",error))return false;
    const auto sourcePath=temp.filePath("fixture");QFile file(sourcePath);
    if(!file.open(QIODevice::WriteOnly))return false;file.write("synthetic");file.close();
    data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    data::WellDataset well;well.wellInfo.wellName="SYNTH-1";
    well.defaultDepthInfo={"MD","m",reference.toStdString()};
    data::LogSet logs;logs.logSetName="Acquisition";
    logs.depthInfo={curveDepthType.toStdString(),curveUnit.toStdString(),reference.toStdString()};
    data::LogCurve curve;curve.curveName="GR";curve.curveUnit=featureUnit.toStdString();
    const double scale=curveUnit=="ft"?.3048:1.;
    for(int range=0;range<2;++range)for(int i=0;i<=10;++i) {
      logs.samplingAxis.depthValues.push_back((100.+range*100+i)/scale);
      curve.sampleValues.push_back(holes&&range==0&&i==4?data::Missing:20.+i);
    }
    logs.logCurves={curve};well.wellbore.logSets.push_back(logs);
    if(duplicate){logs.logSetName="Other acquisition";well.wellbore.logSets.push_back(logs);}
    data::IntervalTrack lithology;lithology.trackName="岩性";lithology.trackType="岩性";
    lithology.depthInfo=well.defaultDepthInfo;
    for(int i=0;i<2;++i){data::GeologicalInterval interval;interval.intervalName="细砂岩";
      interval.topDepth=100.+100*i;interval.bottomDepth=111.+100*i;lithology.intervals.push_back(interval);}
    well.wellbore.intervalTracks.push_back(lithology);parsed.wells.push_back(well);
    long long source=0;
    if(!service.importPreparedFile(sourcePath.toStdString(),sourcePath.toStdString(),sourcePath.toStdString(),
      QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString(),parsed,source,error))return false;
    std::vector<long long>wells;if(!service.listSourceWells(source,wells,error)||wells.size()!=1)return false;
    data::StratigraphicScheme scheme,saved;scheme.id="scheme";scheme.name="Synthetic";
    for(int i=0;i<2;++i){data::StratigraphicBoundary boundary;boundary.name=i?"Base":"Top";boundary.order=i;
      scheme.boundaries.push_back(boundary);}
    data::framework::deriveUnits(scheme);
    for(int i=0;i<2;++i){data::WellIntervalOccurrence occurrence;occurrence.wellId=wells.front();
      occurrence.interval=scheme.intervals.front().id;occurrence.top=100.+i*100;occurrence.bottom=110.+i*100;
      scheme.wellOccurrences.push_back(occurrence);}
    std::vector<data::FrameworkIssue>issues;
    if(!service.commitStratigraphicScheme(scheme,{},saved,issues,error))return false;
    data::ModelChangeHead head;if(!service.readModelChangeHead(head,error)||!head.available)return false;
    job.request.well={"doc","well","area-model",QString::fromStdString(head.storeId),QString::fromStdString(head.generation),
      wells.front(),head.sequence,true};
    auto &unit=job.request.scope.unit;unit.schemeId=QString::fromStdString(saved.id);
    unit.revision=QString::fromStdString(saved.revision.value);unit.intervalId=QString::fromStdString(saved.intervals.front().id.value);
    unit.aggregate=true;
    job.request.context={"doc","area-model",paleo::document::RelationPurpose::FaciesAnalysis,
      unit.schemeId,unit.revision,unit.intervalId,{},true,{}};
    job.request.clientRequestId="request";job.request.modelId="model";job.request.modelVersion="v1";
    job.request.endpointId=QString(64,'a');
    job.model={"model","Synthetic model","v1",{}, {"GR"},{QStringLiteral("河口坝")},2,{{"GR","API"}}};
    return true;
  }
};
}
