// 层：测试壳
#include "fixtures/pythonnumerictablefixture.h"
#include "services/pythonnumerictable_internal.h"
#include "services/pythonnumerictablereport.h"
#include "services/pythongenerated.h"
#include <QtTest>
#include <QtEndian>
#include <bit>
using namespace paleo;
using namespace numeric_table_fixture;
namespace {
QJsonObject arguments(const Model &model) {
  return {{"client_request_id","numeric-input"},{"binding_mode","pinned-value"},{"inputs",python::numericTableSelectionJson(model.plan.selection)},
    {"parameters",QJsonObject{{"code",QString::fromUtf8(sourceCode())}}}};
}
QByteArray envelope() {
  return R"({"contract":"paleo.python-output.v1","ports":{"REPORT":{"contract":"paleo.python-generated-report.v1","metrics":[],"tables":[],"notes":[]}}})";
}
}
class TestPythonNumericTable:public QObject {
  Q_OBJECT
private slots:
  void publicSelectionHasNoPathsAuthorityOrLossyIntegers() {
    Model f;QVERIFY2(f.open(),qPrintable(f.error));const auto args=arguments(f);QVERIFY(python::validateNumericTableToolArguments(args).isEmpty());
    for(const auto &field:QStringList{"actor","project_dir","runtime_root","allow_generated","sql"}) {
      auto changed=args;changed[field]="untrusted";QVERIFY(!python::validateNumericTableToolArguments(changed).isEmpty());
    }
    for(const auto &field:QStringList{"path","sql","unit","source_version_id"}) {
      auto changed=args;auto selection=changed.value("inputs").toObject();selection[field]="untrusted";changed["inputs"]=selection;
      QVERIFY(!python::validateNumericTableToolArguments(changed).isEmpty());
    }
    for(const auto &value:QJsonArray{QJsonValue(1),QJsonValue("01"),QJsonValue("-1"),QJsonValue("9223372036854775808")}) {
      auto selection=python::numericTableSelectionJson(f.plan.selection);selection["first_row"]=value;
      python::NumericTableSelection out;QVERIFY(!python::numericTableSelectionFromJson(selection,out));QVERIFY(out.columnIds.isEmpty());
    }
    auto selected=f.plan.selection;selected.firstRow=9007199254740993LL;python::NumericTableSelection parsed;
    QVERIFY(python::numericTableSelectionFromJson(python::numericTableSelectionJson(selected),parsed));QCOMPARE(parsed.firstRow,selected.firstRow);
    selected.columnIds={"source_row"};QVERIFY(!python::numericTableSelectionFromJson(python::numericTableSelectionJson(selected),parsed));
    auto publish=args;publish["publish_intent"]="publish";publish["parameters"]=QJsonObject{};
    publish["inputs"]=QJsonObject{{"source_operation_id","existing-operation"}};QVERIFY(python::validateNumericTableToolArguments(publish).isEmpty());
  }
  void floatBooleanNullAndExactReadCutFreezeAsTypedBytes() {
    Model f;QVERIFY2(f.open(),qPrintable(f.error));const auto permit=python::claimExecutionPermit();QVERIFY(permit);
    const auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);QVERIFY2(result.ok(),qPrintable(result.code));
    QVERIFY(python::validateNumericTableInputMetadata(result.metadata));QCOMPARE(result.io.cellsRead,qint64(7));QCOMPARE(result.io.outputBytes,qint64(76));
    QCOMPARE(result.metadata.value("provenance").toObject().value("readCut"),QJsonValue(python::numericTableReadCutJson(f.descriptor.cut)));
    const auto payload=bytes(result.bundle,"data.bin");QCOMPARE(payload.size(),76);
    for(int row=0;row<4;++row)QCOMPARE(qFromLittleEndian<qint64>(payload.constData()+row*8),qint64(row+1));
    QCOMPARE(std::bit_cast<double>(qFromLittleEndian<quint64>(payload.constData()+32)),-2.5);
    QCOMPARE(payload.mid(64,4),QByteArray::fromHex("00000100"));
    QCOMPARE(payload.mid(68,4),QByteArray::fromHex("01000001"));QCOMPARE(payload.mid(72,4),QByteArray::fromHex("00000100"));
    const auto report=python::validateNumericTableReport(envelope(),result.metadata,QString(64,'a'),QString(64,'b'),policy());QVERIFY2(report.ok(),qPrintable(report.code));
    QCOMPARE(QJsonDocument::fromJson(report.bytes).object().value("provenance"),result.metadata.value("provenance"));
    QVERIFY(!python::validateGeneratedReport(envelope(),result.metadata,QString(64,'a'),QString(64,'b'),policy()).ok());
  }
  void reorderedColumnsAndSubwindowPreserveDeclaredTypes() {
    Model f;QVERIFY(f.open());f.plan.selection.columnIds={"accepted","amplitude"};f.plan.selection.firstRow=1;f.plan.selection.rowCount=2;
    const auto permit=python::claimExecutionPermit();const auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);
    QVERIFY2(result.ok(),qPrintable(result.code));const auto payload=bytes(result.bundle,"data.bin");QCOMPARE(payload.size(),38);
    QCOMPARE(qFromLittleEndian<qint64>(payload.constData()),qint64(2));QCOMPARE(qFromLittleEndian<qint64>(payload.constData()+8),qint64(3));
    const auto columns=result.metadata.value("columns").toArray();QCOMPARE(columns[0].toObject().value("dtype").toString(),QString("u1"));
    QCOMPARE(columns[1].toObject().value("dtype").toString(),QString("<f8"));QCOMPARE(payload.mid(16,4),QByteArray::fromHex("00000001"));
  }
  void physicalRowEncodingNeverPassesThroughDouble() {
    data::NumericTableWindow window;window.descriptor.projection.firstSourceRow=9007199254740993LL;window.sourceRows={9007199254740993LL,9007199254740994LL};
    data::NumericTableColumnValues values;values.column.dtype=data::NumericTableDtype::Float64;values.numbers={1,2};values.nullMask={0,0};window.columns={values};
    QString code;const auto packed=python::numeric_table_detail::payload(window,code);QVERIFY(code.isEmpty());
    QCOMPARE(qFromLittleEndian<qint64>(packed.constData()),qint64(9007199254740993LL));QCOMPARE(qFromLittleEndian<qint64>(packed.constData()+8),qint64(9007199254740994LL));
  }
  void staticInputAndMemoryBudgetsPrecedeSdkPayloadIo() {
    Model f;QVERIFY(f.open());const auto permit=python::claimExecutionPermit();auto limits=policy();limits.inputBytes=1;
    auto result=python::materializeNumericTable(f.plan,sourceCode(),limits,permit);QVERIFY(!result.ok());QCOMPARE(result.sourceMetadataBytes,qint64(0));QCOMPARE(result.io.cellsRead,qint64(0));
    limits=policy();limits.memoryBytes=16*1024*1024;result=python::materializeNumericTable(f.plan,sourceCode(),limits,permit);
    QCOMPARE(result.code,QString("python_input_memory_budget"));QCOMPARE(result.sourceMetadataBytes,qint64(0));QVERIFY(result.metadata.isEmpty());
    auto invalid=f.plan;invalid.selection.rowCount=8193;result=python::materializeNumericTable(invalid,sourceCode(),policy(),permit);
    QVERIFY(!result.ok());QCOMPARE(result.io.cellsRead,qint64(0));QVERIFY(result.bundle.root.isEmpty());
    limits=policy();limits.allowGenerated=false;limits.allowTrusted=true;result=python::materializeNumericTable(f.plan,sourceCode(),limits,permit);
    QCOMPARE(result.code,QString("python_execution_denied"));QCOMPARE(result.sourceMetadataBytes,qint64(0));
  }
  void unselectedLargeCellDoesNotBecomeInput() {
    Model f;QVERIFY2(f.open({},"numeric-doc",16*1024*1024),qPrintable(f.error));const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);QVERIFY2(result.ok(),qPrintable(result.code));
    QVERIFY(result.io.cellTextBytesRead<64);QCOMPARE(bytes(result.bundle,"data.bin").size(),76);QVERIFY(result.io.metadataBytesRead<8192);
  }
  void invalidLaterColumnDeliversNoPartialBundleOrMetadata() {
    Model f;QVERIFY(f.open({},"numeric-doc",0,"-2.5","true"));const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);QCOMPARE(result.code,QString("python_table_value_invalid"));
    QVERIFY(result.io.cellsRead>4);QVERIFY(result.io.cellTextBytesRead>0);QVERIFY(result.metadata.isEmpty());QVERIFY(result.bundle.root.isEmpty());
  }
  void readonlyConsumptionDoesNotInstallProjectionSchema() {
    Model f;QVERIFY(f.open({},"numeric-doc",0,"-2.5","1",false));data::ModelChangeHead before,after;std::string detail;
    QVERIFY(f.sdk.readModelChangeHead(before,detail));const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);
    QCOMPARE(result.code,QString("python_table_schema_unavailable"));QVERIFY(result.bundle.root.isEmpty());QCOMPARE(result.io.cellsRead,qint64(0));
    QVERIFY(f.sdk.readModelChangeHead(after,detail));QCOMPARE(after.sequence,before.sequence);
    data::NumericTableDescriptor missing;QVERIFY(!f.sdk.describeNumericTableProjection(f.declaration.tableId,missing,detail));
    QCOMPARE(QString::fromStdString(detail),QString("NumericTableSchemaUnavailable"));
  }
  void parsedModelRemainsUsableWhenOriginalFileIsOffline() {
    Model f;QVERIFY(f.open());QVERIFY(QFile::remove(f.plan.source.path));const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);QVERIFY2(result.ok(),qPrintable(result.code));
    QCOMPARE(result.metadata.value("provenance").toObject().value("readCut"),QJsonValue(python::numericTableReadCutJson(f.descriptor.cut)));
  }
  void cancelledAndWrongRevisionNeverFreezeAReplacement() {
    Model f;QVERIFY(f.open());const auto permit=python::claimExecutionPermit();
    auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit,[]{return true;});QCOMPARE(result.code,QString("cancelled"));
    QCOMPARE(result.sourceMetadataBytes,qint64(0));f.plan.selection.projectionRevision=QUuid::createUuid().toString(QUuid::WithoutBraces);
    result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);QCOMPARE(result.code,QString("python_table_revision_changed"));
    QCOMPARE(result.io.cellsRead,qint64(0));QVERIFY(result.bundle.root.isEmpty());
  }
  void malformedMetadataCannotCrossContracts() {
    Model f;QVERIFY(f.open());const auto permit=python::claimExecutionPermit();const auto result=python::materializeNumericTable(f.plan,sourceCode(),policy(),permit);QVERIFY(result.ok());
    auto changed=result.metadata;changed["contract"]="paleo.python-analysis-input.v1";QVERIFY(!python::validateNumericTableInputMetadata(changed));
    changed=result.metadata;auto columns=changed.value("columns").toArray();auto field=columns[0].toObject();field["dtype"]="<f4";columns[0]=field;changed["columns"]=columns;
    QVERIFY(!python::validateNumericTableInputMetadata(changed));
    changed=result.metadata;auto provenance=changed.value("provenance").toObject();auto cut=provenance.value("readCut").toObject();
    cut["boundary"]="historical_snapshot";provenance["readCut"]=cut;changed["provenance"]=provenance;QVERIFY(!python::validateNumericTableInputMetadata(changed));
  }
};
QTEST_GUILESS_MAIN(TestPythonNumericTable)
#include "tst_pythonnumerictable.moc"
