// 层：测试壳
#pragma once
#include "pythonnumerictablenativefixture.h"
#include "pythonseismictracefixture.h"
#include "app/pythonseismictracewiring.h"
namespace seismic_trace_host_fixture {
using namespace paleo;
struct Fixture:numeric_table_native_fixture::Fixture {
  python::SeismicTraceSelection traceSelection;
  data::SeismicWindowDescriptor descriptor;
  data::SeismicWindowReadStats descriptorIo;
  document::SourceRecord source;
  QByteArray raw;
  bool openTrace() {
    if(!open())return false;const auto path=QDir(project).filePath("source.sgy");raw=seismic_trace_fixture::sourceBytes();
    QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(raw)!=raw.size())return false;file.close();
    const auto sha=QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex();std::string detail;long long sourceId=0;
    if(!table.sdk.importPreparedFile(path.toUtf8().toStdString(),path.toUtf8().toStdString(),path.toUtf8().toStdString(),sha.toStdString(),
        seismic_trace_fixture::parsed(path),sourceId,detail)){error=QString::fromStdString(detail);return false;}
    if(!seismic_trace_fixture::describeImportedVolume(table.sdk,sourceId,sha,descriptor,descriptorIo,error))return false;
    if(!registerAreaSources(catalog,&error))return false;
    traceSelection=seismic_trace_fixture::selection({session.documentId(),"area-model",sourceId,QString::fromLatin1(sha)},descriptor);
    source=session.source(traceSelection.source);if(source.versionId.isEmpty()){error="formal seismic source missing";return false;}return true;
  }
  void configureTrace() {
    host->registerExecutionAdapter(QLatin1String(python::SeismicTraceCapability),python::environmentSeismicTraceAdapter(registry,
      assemblePythonSeismicTraceProvider(&catalog,&session,host.get())));
  }
  QJsonObject traceArguments(const QString &id,const QByteArray &code=seismic_trace_fixture::sourceCode())const {
    auto out=arguments(id,code);out["inputs"]=python::seismicTraceSelectionJson(traceSelection);return out;
  }
  capability::ExecuteRequest traceRequest(const QString &client="exact-trace")const {
    auto out=request();out.capabilityId=QLatin1String(python::SeismicTraceCapability);out.clientRequestId=client;
    out.inputs=python::seismicTraceSelectionJson(traceSelection).toVariantMap();return out;
  }
};
}
