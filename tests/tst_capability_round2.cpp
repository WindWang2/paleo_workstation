// 层：测试壳
#include <QtTest/QtTest>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <qgsapplication.h>

#include "algorithms/velocitymodel.h"
#include "catalog/datacatalog.h"
#include "workflow/capabilitygateway.h"

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
  void idwComputesReadableRaster();
  void idwThenContourPassesTheRaster();
  void runningCancelStopsBeforePublish();
  void contourCycleIsRejected();
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

void TestCapabilityRound2::idwComputesReadableRaster() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest request = hostRequest(dir.path(), QStringLiteral("idw"));
  request.capabilityId = QStringLiteral("constraint.idw");
  request.parameters.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  request.parameters.insert(QStringLiteral("CELL_SIZE"), 25.0);
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  request.inputs.insert(QStringLiteral("points"), squarePoints());
  const ExecuteResult computed = paleo::capability::execute(request);
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
  PlanNode idw{QStringLiteral("idw"), QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT"),
               QStringLiteral("compute-only"),
               {{QStringLiteral("FIELD"), QStringLiteral("z")}, {QStringLiteral("CELL_SIZE"), 25.0}},
               {{QStringLiteral("points_ref"), QStringLiteral("ver-points")},
                {QStringLiteral("subinterval_id"), QStringLiteral("sub-1")},
                {QStringLiteral("points"), squarePoints()}}};
  PlanNode contour{QStringLiteral("contours"), QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES"),
                   QStringLiteral("compute-only"), {{QStringLiteral("CONTOUR_LEVEL"), 5.0}}, {}};
  const QVector<PlanEdge> edges{{QStringLiteral("idw"), QStringLiteral("OUTPUT"), QStringLiteral("contours"),
                                 QStringLiteral("RASTER")}};
  const ExecuteResult result = paleo::capability::executePlan(host, {idw, contour}, edges);
  QVERIFY2(result.ok, qPrintable(result.code));
  const QString lines = result.summary.value(QStringLiteral("result_path")).toString();
  QVERIFY(lines.endsWith(QStringLiteral(".gpkg")));
  QVERIFY(QFileInfo(lines).size() > 0);
  QCOMPARE(result.summary.value(QStringLiteral("plan_order")).toString(), QStringLiteral("idw,contours"));
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
  PlanNode a{QStringLiteral("a"), QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES")};
  PlanNode b{QStringLiteral("b"), QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES")};
  const QVector<PlanEdge> edges{{QStringLiteral("a"), QStringLiteral("LINES"), QStringLiteral("b"), QStringLiteral("LINES")},
                                {QStringLiteral("b"), QStringLiteral("LINES"), QStringLiteral("a"), QStringLiteral("LINES")}};
  QCOMPARE(paleo::capability::checkPlan({a, b}, edges).code, QStringLiteral("cycle"));
  QCOMPARE(paleo::capability::checkPlan({a}, {{QStringLiteral("a"), QStringLiteral("LINES"), QStringLiteral("a"),
                                               QStringLiteral("NO_SUCH")}})
               .code,
           QStringLiteral("port_type"));
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
