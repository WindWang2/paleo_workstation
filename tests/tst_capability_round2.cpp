// 层：测试壳
#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <cstring>

#include <gdal.h>

#include <qgsapplication.h>

#include "algorithms/velocitymodel.h"
#include "catalog/datacatalog.h"
#include "fixtures/documentsourcefixture.h"
#include "services/capabilityseismicinput.h"
#include "workflow/capabilitygateway.h"
#include "workflow/capabilitytaskrunner.h"
#include <paleo_data/object_descriptor.h>

namespace {

using paleo::capability::ExecuteRequest;
using paleo::capability::ExecuteResult;
using paleo::capability::PlanEdge;
using paleo::capability::PlanNode;

ExecuteRequest hostRequest(const QString &dir, const QString &client) {
  ExecuteRequest request;
  request.documentId = QStringLiteral("doc-r2");
  request.callerScope = QStringLiteral("host-test");
  request.clientRequestId = client;
  request.bindingMode = QStringLiteral("pinned-value");
  request.frozenHead = QStringLiteral("h1");
  request.currentHead = QStringLiteral("h1");
  request.operationDir = dir;
  request.publishIntent = QStringLiteral("compute-only");
  return request;
}

QVariantList squarePoints() {
  QVariantList points;
  const QList<QPair<double, double>> xy{{0, 0}, {100, 0}, {0, 100}, {100, 100}};
  const QList<double> z{0, 0, 10, 10};
  for (int i = 0; i < xy.size(); ++i) {
    QVariantMap row;
    row.insert(QStringLiteral("x"), xy[i].first);
    row.insert(QStringLiteral("y"), xy[i].second);
    row.insert(QStringLiteral("z"), z[i]);
    row.insert(QStringLiteral("well_id"), QStringLiteral("W%1").arg(i));
    points.append(row);
  }
  return points;
}

} // namespace

class TestCapabilityRound2 : public QObject {
  Q_OBJECT
private slots:
  void forgedHeadDoesNotAuthorizePublish();
  void idwComputesReadableRaster_data();
  void idwComputesReadableRaster();
  void idwThenContourPassesTheRaster();
  void runningCancelStopsBeforePublish();
  void contourCycleIsRejected();
  void invalidPlanStartsNoChildOperation();
  void badPointValueIsRejected();
  void rasterUsesLocalGrid();
  void issuedRasterDoesNotCrossDocuments();
  void frozenPointsAndLinesRejectPartialRows();
  void documentTraceWindowStagesPortReceipt_data();
  void documentTraceWindowStagesPortReceipt();
};

void TestCapabilityRound2::forgedHeadDoesNotAuthorizePublish() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QJsonObject arguments;
  arguments.insert(QStringLiteral("client_request_id"), QStringLiteral("forge"));
  arguments.insert(QStringLiteral("binding_mode"), QStringLiteral("pinned-value"));
  arguments.insert(QStringLiteral("frozen_head"), QStringLiteral("h1"));
  arguments.insert(QStringLiteral("current_head"), QStringLiteral("h1"));
  arguments.insert(QStringLiteral("publish_intent"), QStringLiteral("publish"));
  QVariantMap parameters;
  parameters.insert(QStringLiteral("TIME_SEMANTIC"), QStringLiteral("TWT"));
  parameters.insert(QStringLiteral("DEPTH_SEMANTIC"), QStringLiteral("TVD"));
  parameters.insert(QStringLiteral("EXTRAPOLATION"), QStringLiteral("reject_outside"));
  parameters.insert(QStringLiteral("MODEL_REVISION"), QStringLiteral("rev-1"));
  arguments.insert(QStringLiteral("parameters"), QJsonObject::fromVariantMap(parameters));
  QVariantMap sample;
  sample.insert(QStringLiteral("sampleId"), QStringLiteral("s1"));
  sample.insert(QStringLiteral("wellId"), QStringLiteral("W1"));
  sample.insert(QStringLiteral("time"), 150.0);
  QVariantMap inputs;
  inputs.insert(QStringLiteral("samples"), QVariantList{sample});
  paleo::velmodel::VelocityWellControl well;
  well.wellId = QStringLiteral("W1");
  well.x = 0;
  well.y = 0;
  well.knots = {{100, 200}, {200, 500}};
  QString fitError;
  inputs.insert(QStringLiteral("model"),
                paleo::velmodel::VelocityModel::fit({well}, paleo::velmodel::ModelType::IntervalAverage, &fitError)
                    .toJson()
                    .toVariantMap());
  arguments.insert(QStringLiteral("inputs"), QJsonObject::fromVariantMap(inputs));
  const QJsonObject missing = paleo::capability::runChatTool(
      QStringLiteral("paleo.depth_convert_batch"), arguments, QStringLiteral("host-test"), QStringLiteral("doc-r2"),
      dir.path());
  QCOMPARE(missing.value(QStringLiteral("code")).toString(), QStringLiteral("host_head_required"));
  const QJsonObject forged = paleo::capability::runChatTool(
      QStringLiteral("paleo.depth_convert_batch"), arguments, QStringLiteral("host-test"), QStringLiteral("doc-r2"),
      dir.path() + QStringLiteral("/b"), QStringLiteral("h-real"));
  QCOMPARE(forged.value(QStringLiteral("code")).toString(), QStringLiteral("stale_revision"));
}

void TestCapabilityRound2::idwComputesReadableRaster_data() {
  QTest::addColumn<bool>("background");
  QTest::newRow("direct") << false;
  QTest::newRow("background") << true;
}

void TestCapabilityRound2::idwComputesReadableRaster() {
  QFETCH(bool, background);
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest request = hostRequest(dir.path(), QStringLiteral("idw"));
  request.capabilityId = QStringLiteral("constraint.idw");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  request.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  request.inputs.insert(QStringLiteral("points"), squarePoints());
  ExecuteResult computed;
  if (background) {
    PaleoTaskService tasks;
    bool done = false;
    QVERIFY(paleo::capability::startCapabilityTask(&tasks, this, QStringLiteral("IDW"), request,
        [&](ExecuteResult result) { computed = std::move(result); done = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
  } else {
    computed = paleo::capability::execute(request);
  }
  QVERIFY2(computed.ok, qPrintable(computed.code));
  QCOMPARE(computed.code, QStringLiteral("computed"));
  QVERIFY(computed.recomputed);
  QCOMPARE(computed.commitState, QStringLiteral("not_requested"));
  const QString path = computed.summary.value(QStringLiteral("result_path")).toString();
  QVERIFY(QFileInfo(path).size() > 0);
  QVERIFY(computed.summary.value(QStringLiteral("width")).toInt() > 0);
  QVERIFY(computed.summary.value(QStringLiteral("height")).toInt() > 0);
  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path() + QStringLiteral("/cat")));
  QCOMPARE(catalog.versions().size(), 0);
}

void TestCapabilityRound2::idwThenContourPassesTheRaster() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest host = hostRequest(dir.path(), QStringLiteral("dag"));
  PlanNode idw{QStringLiteral("idw,|地震"), QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT"),
               QStringLiteral("compute-only"),
               {{QStringLiteral("FIELD"), QStringLiteral("z")}, {QStringLiteral("CELL_SIZE"), 25.0}},
               {{QStringLiteral("points_ref"), QStringLiteral("ver-points")},
                {QStringLiteral("subinterval_id"), QStringLiteral("sub-1")},
                {QStringLiteral("points"), squarePoints()}}};
  PlanNode contour{QStringLiteral("contours|,等值线"), QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES"),
                   QStringLiteral("compute-only"), {{QStringLiteral("CONTOUR_LEVEL"), 5.0}}, {}};
  const QVector<PlanEdge> edges{{idw.id, QStringLiteral("OUTPUT"), contour.id,
                                 QStringLiteral("RASTER")}};
  const ExecuteResult result = paleo::capability::executePlan(host, {idw, contour}, edges);
  QVERIFY2(result.ok, qPrintable(result.code));
  const QString lines = result.summary.value(QStringLiteral("result_path")).toString();
  QVERIFY(lines.endsWith(QStringLiteral(".gpkg")));
  QVERIFY(QFileInfo(lines).size() > 0);
  QCOMPARE(result.summary.value(QStringLiteral("plan_order")).toStringList(), QStringList({idw.id, contour.id}));
  QCOMPARE(QJsonDocument::fromJson(result.responseJson).object().value(QStringLiteral("plan_order")).toArray(),
           QJsonArray({idw.id, contour.id}));
}

void TestCapabilityRound2::runningCancelStopsBeforePublish() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path() + QStringLiteral("/cat")));
  ExecuteRequest request = hostRequest(dir.path(), QStringLiteral("stop"));
  request.capabilityId = QStringLiteral("constraint.idw");
  request.publishIntent = QStringLiteral("publish");
  request.catalog = &catalog;
  request.projectDir = dir.path();
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  request.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  request.inputs.insert(QStringLiteral("points"), squarePoints());
  request.onRunning = [&](const QString &operationId) {
    paleo::capability::OperationLog log(request.operationDir);
    QCOMPARE(log.cancel(operationId), QStringLiteral("cancelled"));
  };
  const int before = catalog.versions().size();
  const ExecuteResult stopped = paleo::capability::execute(request);
  QCOMPARE(stopped.execution, QStringLiteral("cancelled"));
  QCOMPARE(catalog.versions().size(), before);
}

void TestCapabilityRound2::contourCycleIsRejected() {
  PlanNode a{QStringLiteral("a"), QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT")};
  PlanNode b{QStringLiteral("b"), QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT")};
  const QVector<PlanEdge> edges{{QStringLiteral("a"), QStringLiteral("OUTPUT"), QStringLiteral("b"), QStringLiteral("INPUT")},
                                {QStringLiteral("b"), QStringLiteral("OUTPUT"), QStringLiteral("a"), QStringLiteral("INPUT")}};
  QCOMPARE(paleo::capability::checkPlan({a, b}, edges).code, QStringLiteral("cycle"));
  QCOMPARE(paleo::capability::checkPlan({a}, {{QStringLiteral("a"), QStringLiteral("OUTPUT"), QStringLiteral("a"),
                                               QStringLiteral("NO_SUCH")}})
               .code,
           QStringLiteral("port_type"));
}

void TestCapabilityRound2::invalidPlanStartsNoChildOperation() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  auto host = hostRequest(dir.path(), QStringLiteral("preflight-all"));
  PlanNode first{QStringLiteral("first"), QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT"),
                 {}, {{QStringLiteral("CELL_SIZE"), 25.0}},
                 {{QStringLiteral("points"), squarePoints()},
                  {QStringLiteral("subinterval_id"), QStringLiteral("S1")}}};
  // The first node is valid; a missing downstream scalar must stop the entire
  // plan before even accepting the first child into operations.json.
  PlanNode last{QStringLiteral("last"), QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES")};
  const QVector<PlanEdge> edges{{first.id, QStringLiteral("OUTPUT"), last.id, QStringLiteral("RASTER")}};
  const auto result = paleo::capability::executePlan(host, {first, last}, edges);
  QCOMPARE(result.code, QStringLiteral("missing_required"));
  QVERIFY(result.operationId.isEmpty());
  QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);

  last.parameters.insert(QStringLiteral("CONTOUR_LEVEL"), 5.0);
  auto badEdges = edges;
  badEdges[0].fromPort = QStringLiteral("ANY_NONEMPTY_NAME");
  QCOMPARE(paleo::capability::executePlan(host, {first, last}, badEdges).code, QStringLiteral("port_type"));
  QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
}

void TestCapabilityRound2::badPointValueIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest request = hostRequest(dir.path(), QStringLiteral("bad-z"));
  request.capabilityId = QStringLiteral("constraint.idw");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  request.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  QVariantList points = squarePoints();
  QVariantMap bad = points.first().toMap();
  bad.insert(QStringLiteral("z"), QStringLiteral("not-a-number"));
  points[0] = bad;
  request.inputs.insert(QStringLiteral("points"), points);
  QCOMPARE(paleo::capability::execute(request).code, QStringLiteral("invalid_point_value"));
}

void TestCapabilityRound2::rasterUsesLocalGrid() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest request = hostRequest(dir.path(), QStringLiteral("crs"));
  request.capabilityId = QStringLiteral("constraint.idw");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  request.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  request.inputs.insert(QStringLiteral("points"), squarePoints());
  const ExecuteResult computed = paleo::capability::execute(request);
  QVERIFY2(computed.ok, qPrintable(computed.code));
  GDALAllRegister();
  GDALDatasetH dataset = GDALOpen(computed.summary.value(QStringLiteral("result_path")).toString().toUtf8().constData(),
                                  GA_ReadOnly);
  QVERIFY(dataset);
  const char *wkt = GDALGetMetadataItem(dataset, "PALEO_CRS_WKT", nullptr);
  QVERIFY(wkt);
  QCOMPARE(QString::fromUtf8(wkt), DataCatalog::localGridCrsWkt());
  GDALClose(dataset);
}

void TestCapabilityRound2::issuedRasterDoesNotCrossDocuments() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest request = hostRequest(dir.path(), QStringLiteral("owner"));
  request.capabilityId = QStringLiteral("constraint.idw");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  request.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  request.inputs.insert(QStringLiteral("points"), squarePoints());
  const ExecuteResult computed = paleo::capability::execute(request);
  QVERIFY2(computed.ok, qPrintable(computed.code));
  ExecuteRequest other = hostRequest(dir.path() + QStringLiteral("/other"), QStringLiteral("thief"));
  other.documentId = QStringLiteral("doc-other");
  other.capabilityId = QStringLiteral("singlefactor.contours");
  other.parameters.insert(QStringLiteral("CONTOUR_LEVEL"), 5.0);
  other.inputs.insert(QStringLiteral("raster_path"), computed.summary.value(QStringLiteral("result_path")));
  QCOMPARE(paleo::capability::execute(other).code, QStringLiteral("untrusted_result_ref"));
}

void TestCapabilityRound2::frozenPointsAndLinesRejectPartialRows() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest request = hostRequest(dir.path(), QStringLiteral("missing-z"));
  request.capabilityId = QStringLiteral("constraint.idw");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  request.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  QVariantList points = squarePoints();
  QVariantMap incomplete = points.first().toMap();
  incomplete.remove(QStringLiteral("z"));
  points[0] = incomplete;
  request.inputs.insert(QStringLiteral("points"), points);
  QCOMPARE(paleo::capability::execute(request).code, QStringLiteral("invalid_point_value"));

  request.clientRequestId = QStringLiteral("named-field");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("porosity"));
  request.inputs.insert(QStringLiteral("points"), squarePoints());
  const ExecuteResult named = paleo::capability::execute(request);
  QVERIFY2(named.ok, qPrintable(named.code));

  request.clientRequestId = QStringLiteral("field-name");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("well_id"));
  QCOMPARE(paleo::capability::execute(request).code, QStringLiteral("invalid_field"));

  auto lineRequest = [&](const QString &client, const QVariantList &lines) {
    ExecuteRequest next = hostRequest(dir.path(), client);
    next.capabilityId = QStringLiteral("constraint.idw");
    next.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
    next.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
    next.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
    next.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
    next.inputs.insert(QStringLiteral("points"), squarePoints());
    next.inputs.insert(QStringLiteral("lines"), lines);
    return paleo::capability::execute(next);
  };
  QVariantMap good;
  good.insert(QStringLiteral("wkt"), QStringLiteral("LINESTRING(50 0, 50 100)"));
  QVariantMap bad = good;
  bad.insert(QStringLiteral("wkt"), QStringLiteral("not-a-line"));
  QCOMPARE(lineRequest(QStringLiteral("bad-wkt"), {good, bad}).code, QStringLiteral("constraint_unreadable"));

  const ExecuteResult untyped = lineRequest(QStringLiteral("no-type"), {good});
  QVERIFY2(untyped.ok, qPrintable(untyped.code));
  const QJsonArray untypedRows =
      QJsonDocument::fromJson(untyped.summary.value(QStringLiteral("constraint_receipts")).toString().toUtf8()).array();
  QVERIFY(!untypedRows.isEmpty());
  bool untypedBarrier = false;
  bool untypedHull = false;
  for (const QJsonValue &value : untypedRows) {
    const QString role = value.toObject().value(QStringLiteral("role")).toString();
    untypedBarrier = untypedBarrier || role == QLatin1String("hard_barrier");
    untypedHull = untypedHull || role == QLatin1String("hull_clip");
  }
  QVERIFY(!untypedBarrier);
  QVERIFY(untypedHull);

  good.insert(QStringLiteral("type"), QStringLiteral("break_line"));
  const ExecuteResult barrier = lineRequest(QStringLiteral("barrier"), {good});
  QVERIFY2(barrier.ok, qPrintable(barrier.code));
  const QJsonArray barrierRows =
      QJsonDocument::fromJson(barrier.summary.value(QStringLiteral("constraint_receipts")).toString().toUtf8()).array();
  bool barrierRole = false;
  for (const QJsonValue &value : barrierRows)
    barrierRole = barrierRole || value.toObject().value(QStringLiteral("role")).toString() == QLatin1String("hard_barrier");
  QVERIFY(barrierRole);
}

void TestCapabilityRound2::documentTraceWindowStagesPortReceipt_data() {
  QTest::addColumn<QString>("externalPath");
  QTest::newRow("synthetic") << QString();
  const auto external = qEnvironmentVariable("PALEO_CAPABILITY_SEGY_READONLY");
  if (!external.isEmpty())
    QTest::newRow("read-only-segy") << external;
}

void TestCapabilityRound2::documentTraceWindowStagesPortReceipt() {
  using namespace paleo::capability;
  QFETCH(QString, externalPath);
  document_fixture::Fixture fixture;
  QVERIFY(fixture.init());
  const QString path = externalPath.isEmpty() ? fixture.temp.filePath(QStringLiteral("traces.sgy")) : externalPath;
  if (externalPath.isEmpty())
    QVERIFY(document_fixture::write(path, document_fixture::smallSegy()));
  const QFileInfo original(path);
  QVERIFY(original.isFile());
  const auto originalSize = original.size();
  const auto originalTime = original.lastModified();
  QVERIFY2(fixture.importModel(path), qPrintable(fixture.error));
  QVERIFY(fixture.bind());
  const auto sources = fixture.session.sources();
  QCOMPARE(sources.size(), 1);
  const auto source = sources.front();
  paleo::data::DataService sdk;
  std::string error;
  QVERIFY2(sdk.openProjectReadOnly(source.modelDirectory.toUtf8().toStdString(), error), error.c_str());
  std::vector<paleo::data::DataEntry> volumes;
  QVERIFY2(sdk.listSourceData("volumes", source.ref.sourceId, volumes, error), error.c_str());
  QCOMPARE(volumes.size(), size_t(1));
  paleo::data::DataObjectDescriptor descriptor;
  QVERIFY2(sdk.describeDataObject(paleo::data::DataObjectKind::SeismicVolume, volumes.front().id, descriptor, error), error.c_str());
  const SeismicTraceSelection window{{source.ref, paleo::document::ObjectKind::SeismicVolume, descriptor.id,
                                      QString::fromStdString(descriptor.revision)}, 1, 3, 2};

  auto request = hostRequest(fixture.temp.filePath(QStringLiteral("trace-operations")), QStringLiteral("plan"));
  request.publishIntent = QStringLiteral("prepare");
  request.parameters = {{QStringLiteral("ATTRIBUTES"), QStringList{QStringLiteral("rms")}}, {QStringLiteral("WINDOW_HALF"), 1}};
  QCOMPARE(bindSeismicTrace(request, fixture.session, window), QString());
  const auto prepared = execute(request);
  QVERIFY2(prepared.ok, qPrintable(prepared.code));
  QCOMPARE(prepared.summary.value(QStringLiteral("firstReadSample")).toInt(), 2);
  QCOMPARE(prepared.summary.value(QStringLiteral("readSampleCount")).toInt(), 4);
  OperationLog log(request.operationDir);
  QCOMPARE(log.recordById(prepared.operationId).execution, QStringLiteral("prepared"));
  QVERIFY(!QDir(request.operationDir).exists(QStringLiteral("results")));

  request.clientRequestId = QStringLiteral("run");
  request.publishIntent = QStringLiteral("compute-only");
  ExecuteResult computed;
  bool done = false;
  QVERIFY(startCapabilityTask(&fixture.tasks, this, QStringLiteral("Seismic window"), request,
      [&](ExecuteResult result) { computed = std::move(result); done = true; }));
  QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
  QVERIFY2(computed.ok, qPrintable(computed.code));
  QCOMPARE(computed.summary.value(QStringLiteral("samplesRead")).toInt(), 4);
  const auto ref = computed.summary.value(QStringLiteral("resultRef")).toMap();
  QCOMPARE(ref.value(QStringLiteral("operationId")).toString(), computed.operationId);
  const auto ports = ref.value(QStringLiteral("ports")).toList();
  QCOMPARE(ports.size(), 1);
  const auto port = ports.front().toMap();
  QCOMPARE(port.value(QStringLiteral("portId")).toString(), QStringLiteral("RMS"));
  QCOMPARE(port.value(QStringLiteral("sampleCount")).toInt(), 2);
  QFile resultFile(computed.summary.value(QStringLiteral("result_path")).toString());
  QVERIFY(resultFile.open(QIODevice::ReadOnly));
  QCOMPARE(resultFile.size(), qint64(2 * sizeof(float)));
  const auto bytes = resultFile.readAll();
  resultFile.close();
  QCOMPARE(port.value(QStringLiteral("sha256")).toByteArray(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  if (externalPath.isEmpty()) {
    float first = 0;
    std::memcpy(&first, bytes.constData(), sizeof(first));
    QVERIFY(std::abs(first - std::sqrt((12.0 * 12.0 + 13.0 * 13.0 + 14.0 * 14.0) / 3.0)) < 1e-5);
  }
  const auto replayed = execute(request);
  QCOMPARE(replayed.code, QStringLiteral("replay"));
  QCOMPARE(replayed.operationId, computed.operationId);
  QCOMPARE(replayed.summary.value(QStringLiteral("resultRef")).toMap(), ref);
  QVERIFY(!chatToolResponse(computed).contains(QStringLiteral("result_path")));

  const QString relocatedDir = fixture.temp.filePath(QStringLiteral("relocated-trace-operations"));
  QVERIFY(QDir().rename(request.operationDir, relocatedDir));
  request.operationDir = relocatedDir;
  const auto rebased = execute(request);
  QVERIFY2(rebased.ok, qPrintable(rebased.code));
  QCOMPARE(rebased.summary.value(QStringLiteral("resultRef")).toMap(), ref);
  const auto newPath = rebased.summary.value(QStringLiteral("result_path")).toString();
  QCOMPARE(QFileInfo(newPath).absolutePath(), QDir(relocatedDir).filePath(QStringLiteral("results")));
  {
    QFile tampered(newPath);
    QVERIFY(tampered.open(QIODevice::ReadWrite));
    const char changed = static_cast<char>(bytes.at(0) ^ 1);
    QCOMPARE(tampered.write(&changed, 1), 1);
  }
  QCOMPARE(QFileInfo(newPath).size(), bytes.size());
  const auto wrongDigest = execute(request);
  QCOMPARE(wrongDigest.code, QStringLiteral("result_unavailable"));
  QVERIFY(!wrongDigest.ok);
  QVERIFY(!wrongDigest.summary.contains(QStringLiteral("resultRef")));
  QVERIFY(!wrongDigest.summary.contains(QStringLiteral("result_path")));
  QFile oversized(newPath);
  QVERIFY(oversized.open(QIODevice::ReadWrite));
  QVERIFY(oversized.resize(bytes.size() + 1));
  oversized.close();
  const auto wrongSize = execute(request);
  QCOMPARE(wrongSize.code, QStringLiteral("result_unavailable"));
  QVERIFY(!wrongSize.ok);
  QVERIFY(QFile::remove(newPath));
  const auto missing = execute(request);
  QCOMPARE(missing.code, QStringLiteral("result_unavailable"));
  QVERIFY(!missing.ok);
  QVERIFY(!missing.summary.contains(QStringLiteral("resultRef")));
  QVERIFY(!missing.summary.contains(QStringLiteral("result_path")));

  request.clientRequestId = QStringLiteral("receipt-failure");
  request.onRunning = [](const QString &) { OperationLog::armOperationFileWriteFault(0); };
  const auto failed = execute(request);
  QVERIFY(!failed.ok);
  QCOMPARE(failed.code, QStringLiteral("receipt_persist_failed"));
  QVERIFY(!failed.summary.contains(QStringLiteral("resultRef")));
  QCOMPARE(OperationLog(relocatedDir).recordById(failed.operationId).execution, QStringLiteral("failed"));
  QVERIFY(!QFileInfo::exists(QDir(relocatedDir).filePath(QStringLiteral("results/") + failed.operationId + QStringLiteral("-trace.f32"))));

  QCOMPARE(QFileInfo(path).size(), originalSize);
  QCOMPARE(QFileInfo(path).lastModified(), originalTime);
}

int main(int argc, char *argv[]) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestCapabilityRound2 test;
  const int code = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return code;
}

#include "tst_capability_round2.moc"
