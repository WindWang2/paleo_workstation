// 层：测试壳
#include "fixtures/pythonseismictracefixture.h"
#include "services/pythonseismictracereport.h"
#include "services/pythonnumerictablereport.h"
#include "services/pythongenerated.h"
#include <QJsonDocument>
#include <QtTest>
#include <bit>
using namespace paleo;
using namespace seismic_trace_fixture;
class TestPythonSeismicTrace:public QObject {
  Q_OBJECT
private slots:
  void exactTraceTimeAndActualSourceCutFreezeWithoutGuessing() {
    Model f;QVERIFY2(f.open(),qPrintable(f.error));const auto permit=python::claimExecutionPermit();
    QCOMPARE(f.descriptor.cut.sourceId,f.plan.selection.source.sourceId);
    QVERIFY(f.descriptorIo.metadataBytesRead>0);QVERIFY(f.descriptorIo.metadataBytesRead<=8192);
    QCOMPARE(f.descriptorIo.requestedSourceBytes,qint64(0));QCOMPARE(f.descriptorIo.sourceReadCalls,qint64(0));
    QCOMPARE(f.descriptorIo.outputBytes,qint64(0));
    data::SeismicWindowDescriptor missing;data::SeismicWindowReadStats missingIo;QString discoveryError;
    QVERIFY(!describeImportedVolume(f.sdk,f.plan.selection.source.sourceId+100,
      f.plan.selection.source.fileRevisionId.toLatin1(),missing,missingIo,discoveryError));
    QCOMPARE(discoveryError,QString("fixture_imported_volume_identity_mismatch"));
    QVERIFY(!missing.cut.model.available);QCOMPARE(missingIo.metadataBytesRead,qint64(0));
    const auto result=python::materializeSeismicTrace(f.plan,sourceCode(),policy(),permit);QVERIFY2(result.ok(),qPrintable(result.code));
    QVERIFY(python::validateSeismicTraceInputMetadata(result.metadata));QCOMPARE(result.io.outputBytes,qint64(51));QCOMPARE(result.io.requestedSourceBytes,qint64(652));
    QCOMPARE(result.authentication.fullScans,quint64(1));QCOMPARE(result.authentication.fullHashBytes,quint64(f.raw.size()));
    const auto data=bytes(result.bundle,"data.bin");QCOMPARE(data.size(),51);
    for(int i=0;i<3;++i) {
      QCOMPARE(std::bit_cast<double>(qFromLittleEndian<quint64>(data.constData()+i*8)),double(2+i*2));
      QCOMPARE(std::bit_cast<double>(qFromLittleEndian<quint64>(data.constData()+24+i*8)),double(9+i));
    }
    QCOMPARE(data.right(3),QByteArray(3,0));const auto provenance=result.metadata.value("provenance").toObject();
    QCOMPARE(provenance.value("readCut"),QJsonValue(python::seismicTraceReadCutJson(f.descriptor.cut)));
    QCOMPARE(provenance.value("trace").toObject().value("number").toString(),QString("17"));
    const QByteArray output=R"({"contract":"paleo.python-output.v1","ports":{"REPORT":{"contract":"paleo.python-generated-report.v1","metrics":[],"tables":[],"notes":[]}}})";
    QVERIFY(python::validateSeismicTraceReport(output,result.metadata,QString(64,'a'),QString(64,'b'),policy()).ok());
    QVERIFY(!python::validateNumericTableReport(output,result.metadata,QString(64,'a'),QString(64,'b'),policy()).ok());
    QVERIFY(!python::validateGeneratedReport(output,result.metadata,QString(64,'a'),QString(64,'b'),policy()).ok());
  }
  void subsequentWindowAuthenticatesConsumedChunksWithoutRepeatingFullSha() {
    Model f;QVERIFY(f.open(40000));const auto permit=python::claimExecutionPermit();
    const auto cold=python::materializeSeismicTrace(f.plan,sourceCode(),policy(),permit);QVERIFY2(cold.ok(),qPrintable(cold.code));
    f.plan.selection.traceIndex=39999;
    const auto warm=python::materializeSeismicTrace(f.plan,sourceCode(),policy(),permit);QVERIFY2(warm.ok(),qPrintable(warm.code));
    QCOMPARE(warm.authentication.fullScans,quint64(0));QCOMPARE(warm.authentication.fullHashBytes,quint64(0));
    QVERIFY(warm.authentication.readBytes<quint64(f.raw.size()));QVERIFY(warm.authentication.chunkHashBytes>0);
    QCOMPARE(warm.authentication.readBytes,warm.authentication.chunkHashBytes);QCOMPARE(f.plan.reader->statistics().fullScans,quint64(1));
    qInfo()<<"SEISMIC_AUTH_IO sourceBytes"<<f.raw.size()<<"coldFullHashBytes"<<cold.authentication.fullHashBytes
      <<"warmFullHashBytes"<<warm.authentication.fullHashBytes<<"warmChunkReadBytes"<<warm.authentication.readBytes;
  }
  void nullMaskAndZeroTraceNumberRemainExplicit() {
    Model f;QVERIFY(f.open(2,2));const auto permit=python::claimExecutionPermit();
    auto result=python::materializeSeismicTrace(f.plan,sourceCode(),policy(),permit);QVERIFY2(result.ok(),qPrintable(result.code));
    auto data=bytes(result.bundle,"data.bin");QCOMPARE(data.right(3),QByteArray::fromHex("000100"));
    QCOMPARE(std::bit_cast<double>(qFromLittleEndian<quint64>(data.constData()+32)),0.0);
    f.plan.selection.traceIndex=0;f.plan.selection.firstSample=0;
    result=python::materializeSeismicTrace(f.plan,sourceCode(),policy(),permit);QVERIFY(result.ok());
    QCOMPARE(result.metadata.value("provenance").toObject().value("trace").toObject().value("number").toString(),QString("0"));
    data=bytes(result.bundle,"data.bin");QCOMPARE(std::bit_cast<double>(qFromLittleEndian<quint64>(data.constData()+24)),-2.0);
  }
  void staticShapeBudgetPermissionAndCancellationPrecedeSourceIo() {
    Model f;QVERIFY(f.open());const auto permit=python::claimExecutionPermit();auto denied=policy();denied.allowGenerated=false;denied.allowTrusted=true;
    auto result=python::materializeSeismicTrace(f.plan,sourceCode(),denied,permit);QCOMPARE(result.code,QString("python_execution_denied"));
    auto plan=f.plan;plan.selection.sampleCount=8193;result=python::materializeSeismicTrace(plan,sourceCode(),policy(),permit);QVERIFY(!result.ok());
    auto limits=policy();limits.inputBytes=1;result=python::materializeSeismicTrace(f.plan,sourceCode(),limits,permit);QVERIFY(!result.ok());
    limits=policy();limits.memoryBytes=1024*1024;result=python::materializeSeismicTrace(f.plan,sourceCode(),limits,permit);QCOMPARE(result.code,QString("python_input_memory_budget"));
    result=python::materializeSeismicTrace(f.plan,sourceCode(),policy(),permit,[]{return true;});QCOMPARE(result.code,QString("cancelled"));
    QCOMPARE(f.plan.reader->statistics().readBytes,quint64(0));QVERIFY(result.bundle.root.isEmpty());QVERIFY(result.metadata.isEmpty());
  }
  void wrongRevisionUnitsAndTraceNeverDeliverPartialNumbers() {
    Model f;QVERIFY(f.open());const auto permit=python::claimExecutionPermit();auto plan=f.plan;
    plan.selection.volumeRevision="seismic-window-descriptor.v1:"+QString(64,'a');
    auto result=python::materializeSeismicTrace(plan,sourceCode(),policy(),permit);QCOMPARE(result.code,QString("python_seismic_revision_or_units_changed"));
    plan=f.plan;plan.selection.timeType="OWT";result=python::materializeSeismicTrace(plan,sourceCode(),policy(),permit);QCOMPARE(result.code,QString("python_seismic_revision_or_units_changed"));
    QCOMPARE(f.plan.reader->statistics().readBytes,quint64(0));
    plan=f.plan;plan.selection.traceIndex=2;result=python::materializeSeismicTrace(plan,sourceCode(),policy(),permit);
    QCOMPARE(result.code,QString("python_seismic_trace_out_of_range"));QVERIFY(result.metadata.isEmpty());QVERIFY(result.bundle.root.isEmpty());
    QCOMPARE(result.authentication.readBytes,quint64(0));QCOMPARE(result.io.outputBytes,qint64(0));
  }
  void unsupportedLayoutAndChangedAuthenticatedBytesFailClosed() {
    Model unsupported;QVERIFY(unsupported.open(2,-1,[](auto &bytes){qToBigEndian<quint16>(1,bytes.data()+3224);}));
    const auto permit=python::claimExecutionPermit();auto result=python::materializeSeismicTrace(unsupported.plan,sourceCode(),policy(),permit);
    QCOMPARE(result.code,QString("python_seismic_layout_unavailable"));QVERIFY(result.metadata.isEmpty());QVERIFY(result.bundle.root.isEmpty());
    Model changed;QVERIFY(changed.open());result=python::materializeSeismicTrace(changed.plan,sourceCode(),policy(),permit);QVERIFY(result.ok());
    QFile file(changed.path);QVERIFY(file.open(QIODevice::ReadWrite));const auto modified=file.fileTime(QFileDevice::FileModificationTime);
    QVERIFY(file.seek(3600+264+240+4));QCOMPARE(file.write(QByteArray::fromHex("00000000")),qint64(4));QVERIFY(file.setFileTime(modified,QFileDevice::FileModificationTime));file.close();
    result=python::materializeSeismicTrace(changed.plan,sourceCode(),policy(),permit);QCOMPARE(result.code,QString("python_seismic_source_changed"));
    QVERIFY(result.metadata.isEmpty());QVERIFY(result.bundle.root.isEmpty());QCOMPARE(result.authentication.fullScans,quint64(0));
  }
  void publicReferencesAndFrozenMetadataCannotClaimOtherContracts() {
    Model f;QVERIFY(f.open());const QJsonObject arguments{{"client_request_id","seismic"},{"binding_mode","pinned-value"},
      {"inputs",python::seismicTraceSelectionJson(f.plan.selection)},{"parameters",QJsonObject{{"code",QString::fromUtf8(sourceCode())}}}};
    QVERIFY(python::validateSeismicTraceToolArguments(arguments).isEmpty());
    for(const auto &field:QStringList{"path","sql","runtime_root","caller_scope","source_version_id"}) {
      auto invalid=arguments;auto inputs=invalid.value("inputs").toObject();inputs[field]="untrusted";invalid["inputs"]=inputs;
      QVERIFY(!python::validateSeismicTraceToolArguments(invalid).isEmpty());
    }
    auto invalid=arguments;auto inputs=invalid.value("inputs").toObject();inputs["trace_index"]=9007199254740992.0;invalid["inputs"]=inputs;
    QVERIFY(!python::validateSeismicTraceToolArguments(invalid).isEmpty());
    const auto permit=python::claimExecutionPermit();const auto result=python::materializeSeismicTrace(f.plan,sourceCode(),policy(),permit);QVERIFY(result.ok());
    auto metadata=result.metadata;metadata["contract"]="paleo.python-analysis-input.v1";QVERIFY(!python::validateSeismicTraceInputMetadata(metadata));
    metadata=result.metadata;auto axis=metadata.value("axis").toObject();axis["kind"]="MD";metadata["axis"]=axis;QVERIFY(!python::validateSeismicTraceInputMetadata(metadata));
    metadata=result.metadata;auto provenance=metadata.value("provenance").toObject();auto cut=provenance.value("readCut").toObject();cut["boundary"]="history_snapshot";
    provenance["readCut"]=cut;metadata["provenance"]=provenance;QVERIFY(!python::validateSeismicTraceInputMetadata(metadata));
  }
};
QTEST_GUILESS_MAIN(TestPythonSeismicTrace)
#include "tst_pythonseismictrace.moc"
