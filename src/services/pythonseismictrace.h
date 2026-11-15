// 层：数据
#pragma once
#include "domain/documentsource.h"
#include "authenticatedartifactreader.h"
#include "pythoninputbundle.h"
#include "pythonprocess.h"
#include <paleo_data/seismic_window.h>
namespace paleo::python {
inline constexpr char SeismicTraceCapability[]="python.seismic_trace_compute";
inline constexpr char SeismicTraceTool[]="paleo.python_compute_trace";
inline constexpr char SeismicTraceInputContract[]="paleo.python-seismic-trace-input.v1";
struct SeismicTraceSelection {
  document::SourceRef source;
  qlonglong volumeId=0,traceIndex=-1;
  QString volumeRevision,amplitudeUnit,timeUnit,timeType,timeReference;
  int firstSample=0,sampleCount=0;
};
struct SeismicTraceReadPlan {
  SeismicTraceSelection selection;
  document::SourceRecord source;
  QString projectDir,modelDirectory,documentId,callerScope;
  std::shared_ptr<artifacts::AuthenticatedArtifactReader> reader;
  artifacts::AuthenticatedArtifactReader::Identity authenticatedSource;
  qint64 readByteBudget=1024*1024;
};
struct SeismicTraceMaterialization {
  QString code;InputBundle bundle;QJsonObject metadata;
  data::SeismicWindowReadStats io;
  artifacts::AuthenticatedArtifactReader::Statistics authentication;
  bool ok()const{return code==QLatin1String("ok");}
};
bool seismicTraceSelectionFromJson(const QJsonObject &,SeismicTraceSelection &,QString *error=nullptr);
QJsonObject seismicTraceSelectionJson(const SeismicTraceSelection &);
QJsonObject seismicTraceToolRequestSchema();
QString validateSeismicTraceToolArguments(const QJsonObject &);
SeismicTraceMaterialization materializeSeismicTrace(const SeismicTraceReadPlan &,const QByteArray &,
  const ExecutionPolicy &,const std::shared_ptr<ExecutionPermit> &,const std::function<bool()> &cancelled={});
QJsonObject seismicTraceReadCutJson(const data::SeismicWindowReadCut &);
bool validateSeismicTraceInputMetadata(const QJsonObject &);
}
