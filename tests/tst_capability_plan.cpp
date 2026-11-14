// 层：测试壳
#include <QtTest/QtTest>
#include <limits>

#include "workflow/capabilityplan.h"

using namespace paleo::capability;

namespace {

PlanNode idw(const QString &id = QStringLiteral("idw")) {
  const QVariantList points{QVariantMap{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0},
                                       {QStringLiteral("z"), 1.0}}};
  return {id, QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT"), {}, {},
          {{QStringLiteral("points"), points}, {QStringLiteral("subinterval_id"), QStringLiteral("S1")}}};
}

PlanNode contour(const QString &id = QStringLiteral("contour")) {
  return {id, QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES"), {},
          {{QStringLiteral("CONTOUR_LEVEL"), 0.5}}, {}};
}

PlanEdge rasterEdge(const QString &from = QStringLiteral("idw"), const QString &to = QStringLiteral("contour")) {
  return {from, QStringLiteral("OUTPUT"), to, QStringLiteral("RASTER")};
}

QString code(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges = {}) {
  return checkPlan(nodes, edges).code;
}

} // namespace

class TestCapabilityPlan : public QObject {
  Q_OBJECT
private slots:
  void validGraphCarriesStructuredOrderAndBindings();
  void exactPortsAndContracts_data();
  void exactPortsAndContracts();
  void graphRejectsConflictingAssignments();
  void graphRejectsCyclesWithoutAStringIdentity();
  void allNodesArePreflighted_data();
  void allNodesArePreflighted();
  void boundedDescriptorAndExpansion();
  void portSemantics_data();
  void portSemantics();
  void parametersHaveOneSourceOfDefaults();
};

void TestCapabilityPlan::validGraphCarriesStructuredOrderAndBindings() {
  const QString sourceId = QStringLiteral("source,|/测井");
  const QString targetId = QStringLiteral("target|,地震");
  const auto plan = validatePlan({contour(targetId), idw(sourceId)}, {rasterEdge(sourceId, targetId)});
  QVERIFY2(plan.ok, qPrintable(plan.issues.isEmpty() ? QString() : plan.issues.first().code));
  QCOMPARE(plan.order, QStringList({sourceId, targetId}));
  QCOMPARE(plan.edges.size(), 1);
  QCOMPARE(plan.edges.first().fromNode, 1);
  QCOMPARE(plan.edges.first().toNode, 0);
  QCOMPARE(plan.edges.first().fromPort.id, QStringLiteral("OUTPUT"));
  QCOMPARE(plan.edges.first().inputKey, QStringLiteral("raster_path"));
  QCOMPARE(plan.nodes[1].inputKeys.value(QStringLiteral("INPUT")), QStringLiteral("points"));
  QCOMPARE(plan.nodes[1].inputKeys.value(QStringLiteral("CONSTRAINTS")), QStringLiteral("lines"));
  QCOMPARE(plan.nodes[1].node.intent, QStringLiteral("compute-only"));
  QCOMPARE(checkPlan({idw(), contour()}, {rasterEdge()}).message, QString());
}

void TestCapabilityPlan::exactPortsAndContracts_data() {
  QTest::addColumn<QString>("mutation");
  QTest::addColumn<QString>("expected");
  for (const auto &name : {QStringLiteral("unknown-source"), QStringLiteral("empty-source"),
                            QStringLiteral("unknown-target"), QStringLiteral("empty-target"),
                            QStringLiteral("selected-output"), QStringLiteral("empty-output")})
    QTest::newRow(qPrintable(name)) << name << QStringLiteral("port_type");
  QTest::newRow("unselected-output") << QStringLiteral("unselected-output") << QStringLiteral("unselected_output");
  QTest::newRow("diagnostic-output") << QStringLiteral("diagnostic-output") << QStringLiteral("unsupported_output_shape");
  QTest::newRow("unknown-node") << QStringLiteral("unknown-node") << QStringLiteral("unknown_node");
  QTest::newRow("unknown-capability") << QStringLiteral("unknown-capability") << QStringLiteral("unknown_capability");
  QTest::newRow("version") << QStringLiteral("version") << QStringLiteral("unsupported_version");
  QTest::newRow("internal") << QStringLiteral("internal") << QStringLiteral("not_callable");
  QTest::newRow("external-adapter") << QStringLiteral("unavailable") << QStringLiteral("not_callable");
  QTest::newRow("multi-port-seismic") << QStringLiteral("multi-port-seismic") << QStringLiteral("unsupported_plan_adapter");
  QTest::newRow("batch-value-depth") << QStringLiteral("batch-value-depth") << QStringLiteral("unsupported_plan_adapter");
}

void TestCapabilityPlan::exactPortsAndContracts() {
  QFETCH(QString, mutation);
  QFETCH(QString, expected);
  auto a = idw();
  auto b = contour();
  auto edge = rasterEdge();
  if (mutation == QLatin1String("unknown-source")) edge.fromPort = QStringLiteral("ANY_NONEMPTY_NAME");
  if (mutation == QLatin1String("empty-source")) edge.fromPort.clear();
  if (mutation == QLatin1String("unknown-target")) edge.toPort = QStringLiteral("LINES");
  if (mutation == QLatin1String("empty-target")) edge.toPort.clear();
  if (mutation == QLatin1String("selected-output")) a.outputPort = QStringLiteral("nonexistent");
  if (mutation == QLatin1String("empty-output")) a.outputPort.clear();
  if (mutation == QLatin1String("unselected-output")) edge.fromPort = QStringLiteral("CONSTRAINT_RECEIPTS");
  if (mutation == QLatin1String("diagnostic-output")) a.outputPort = QStringLiteral("CONSTRAINT_RECEIPTS");
  if (mutation == QLatin1String("unknown-node")) edge.from = QStringLiteral("absent");
  if (mutation == QLatin1String("unknown-capability")) a.capabilityId = QStringLiteral("unknown.capability");
  if (mutation == QLatin1String("version")) a.contractVersion = 2;
  if (mutation == QLatin1String("internal")) a.capabilityId = QStringLiteral("facies.draft.publish");
  if (mutation == QLatin1String("unavailable")) {a.capabilityId = QStringLiteral("wellfacies.predict");a.contractVersion=2;}
  if (mutation == QLatin1String("multi-port-seismic")) {
    a.capabilityId = QStringLiteral("seismic.attributes.compute");
    a.outputPort = QStringLiteral("RMS");
    QCOMPARE(code({a}), expected);
    return;
  }
  if (mutation == QLatin1String("batch-value-depth")) {
    a.capabilityId = QStringLiteral("depth.convert_batch");
    a.outputPort = QStringLiteral("DEPTH");
    QCOMPARE(code({a}), expected);
    return;
  }
  QCOMPARE(code({a, b}, {edge}), expected);
}

void TestCapabilityPlan::graphRejectsConflictingAssignments() {
  QCOMPARE(code({}), QStringLiteral("empty_plan"));
  QCOMPARE(code({idw(QString())}), QStringLiteral("empty_node"));
  QCOMPARE(code({idw(), idw()}), QStringLiteral("duplicate_node"));
  QCOMPARE(code({idw(), idw(QStringLiteral("other"))}), QStringLiteral("unsupported_plan_shape"));
  QCOMPARE(code({idw(), contour()}, {rasterEdge(), rasterEdge()}), QStringLiteral("duplicate_edge"));
  QCOMPARE(code({idw(), idw(QStringLiteral("second")), contour()},
                {rasterEdge(), rasterEdge(QStringLiteral("second"))}), QStringLiteral("input_already_assigned"));
  QCOMPARE(code({idw(), contour(), contour(QStringLiteral("second"))},
                {rasterEdge(), rasterEdge(QStringLiteral("idw"), QStringLiteral("second"))}),
           QStringLiteral("unsupported_plan_shape"));
  auto b = contour();
  b.inputs.insert(QStringLiteral("raster_path"), QStringLiteral("already.tif"));
  QCOMPARE(code({idw(), b}, {rasterEdge()}), QStringLiteral("input_already_assigned"));
  b.inputs.clear();
  b.inputs.insert(QStringLiteral("RASTER"), QStringLiteral("wrong-alias.tif"));
  QCOMPARE(code({idw(), b}, {rasterEdge()}), QStringLiteral("unknown_input"));
  const QVector<PlanEdge> vectorFileToRows{{QStringLiteral("contour"), QStringLiteral("LINES"),
                                          QStringLiteral("idw2"), QStringLiteral("CONSTRAINTS")}};
  b.inputs.insert(QStringLiteral("raster_path"), QStringLiteral("host-issued.tif"));
  b.inputs.remove(QStringLiteral("RASTER"));
  QCOMPARE(code({b, idw(QStringLiteral("idw2"))}, vectorFileToRows), QStringLiteral("unsupported_edge_shape"));
}

void TestCapabilityPlan::graphRejectsCyclesWithoutAStringIdentity() {
  const auto a = idw(QStringLiteral("a|INPUT"));
  const auto b = idw(QStringLiteral("a"));
  const auto c = idw(QStringLiteral("c,第三"));
  auto edge = [](const QString &from, const QString &to) {
    return PlanEdge{from, QStringLiteral("OUTPUT"), to, QStringLiteral("INPUT")};
  };
  QCOMPARE(code({a}, {edge(a.id, a.id)}), QStringLiteral("cycle"));
  QCOMPARE(code({a, b, c}, {edge(a.id, b.id), edge(b.id, c.id), edge(c.id, a.id)}), QStringLiteral("cycle"));
  auto bad = edge(a.id, a.id);
  bad.fromPort = QStringLiteral("not-a-port");
  QCOMPARE(code({a}, {bad}), QStringLiteral("port_type"));
}

void TestCapabilityPlan::allNodesArePreflighted_data() {
  QTest::addColumn<QString>("mutation");
  QTest::addColumn<QString>("expected");
  QTest::newRow("downstream-required-param") << QStringLiteral("missing-param") << QStringLiteral("missing_required");
  QTest::newRow("downstream-param-type") << QStringLiteral("bad-param") << QStringLiteral("type_mismatch");
  QTest::newRow("parameter-port-bypass") << QStringLiteral("parameter-port") << QStringLiteral("unknown_param");
  QTest::newRow("unknown-param") << QStringLiteral("unknown-param") << QStringLiteral("unknown_param");
  QTest::newRow("missing-points") << QStringLiteral("missing-points") << QStringLiteral("missing_required");
  QTest::newRow("empty-points") << QStringLiteral("empty-points") << QStringLiteral("missing_required");
  QTest::newRow("bad-point-shape") << QStringLiteral("bad-point-shape") << QStringLiteral("input_type");
  QTest::newRow("point-value") << QStringLiteral("point-value") << QStringLiteral("invalid_point_value");
  QTest::newRow("missing-subinterval") << QStringLiteral("missing-subinterval") << QStringLiteral("missing_required");
  QTest::newRow("publish") << QStringLiteral("publish") << QStringLiteral("unsupported_plan_intent");
  QTest::newRow("prepare") << QStringLiteral("prepare") << QStringLiteral("unsupported_plan_intent");
  QTest::newRow("authority-input") << QStringLiteral("authority-input") << QStringLiteral("unknown_input");
  QTest::newRow("fractional-integer") << QStringLiteral("fractional-integer") << QStringLiteral("type_mismatch");
  QTest::newRow("boolean-string") << QStringLiteral("boolean-string") << QStringLiteral("type_mismatch");
}

void TestCapabilityPlan::allNodesArePreflighted() {
  QFETCH(QString, mutation);
  QFETCH(QString, expected);
  auto a = idw();
  auto b = contour();
  if (mutation == QLatin1String("missing-param")) b.parameters.clear();
  if (mutation == QLatin1String("bad-param")) b.parameters.insert(QStringLiteral("CONTOUR_LEVEL"), QStringLiteral("no"));
  if (mutation == QLatin1String("parameter-port")) b.parameters.insert(QStringLiteral("RASTER"), QStringLiteral("fake.tif"));
  if (mutation == QLatin1String("unknown-param")) b.parameters.insert(QStringLiteral("invented"), 2.0);
  if (mutation == QLatin1String("missing-points")) a.inputs.remove(QStringLiteral("points"));
  if (mutation == QLatin1String("empty-points")) a.inputs.insert(QStringLiteral("points"), QVariantList{});
  if (mutation == QLatin1String("bad-point-shape")) a.inputs.insert(QStringLiteral("points"), QStringLiteral("fake.gpkg"));
  if (mutation == QLatin1String("point-value")) a.inputs.insert(QStringLiteral("points"), QVariantList{QVariantMap{}});
  if (mutation == QLatin1String("missing-subinterval")) a.inputs.remove(QStringLiteral("subinterval_id"));
  if (mutation == QLatin1String("publish") || mutation == QLatin1String("prepare")) a.intent = mutation;
  if (mutation == QLatin1String("authority-input")) b.inputs.insert(QStringLiteral("approved"), true);
  if (mutation == QLatin1String("fractional-integer")) a.parameters.insert(QStringLiteral("FACIES_CODE"), 1.5);
  if (mutation == QLatin1String("boolean-string")) a.parameters.insert(QStringLiteral("FIELD"), true);
  QCOMPARE(code({a, b}, {rasterEdge()}), expected);
  QCOMPARE(code({contour()}), QStringLiteral("missing_required"));
}

void TestCapabilityPlan::boundedDescriptorAndExpansion() {
  QVector<PlanNode> tooMany;
  for (int i = 0; i < 33; ++i)
    tooMany.append(idw(QString::number(i)));
  QCOMPARE(code(tooMany), QStringLiteral("plan_too_large"));
  QVector<PlanEdge> edges(65, rasterEdge());
  QCOMPARE(code({idw(), contour()}, edges), QStringLiteral("plan_too_large"));
  auto a = idw();
  a.inputs.insert(QStringLiteral("points_ref"), QString(256 * 1024, QLatin1Char('x')));
  QCOMPARE(code({a}), QStringLiteral("plan_too_large"));
  a = idw();
  a.inputs.insert(QStringLiteral("points"), QVariantList(4097, QVariantMap{}));
  QCOMPARE(code({a}), QStringLiteral("plan_too_large"));
  a = idw();
  QVariant nested = 0;
  for (int i = 0; i < 20; ++i)
    nested = QVariantMap{{QStringLiteral("nested"), nested}};
  a.inputs.insert(QStringLiteral("nested"), nested);
  QCOMPARE(code({a}), QStringLiteral("plan_too_large"));
  a = idw();
  a.parameters.insert(QStringLiteral("CELL_SIZE"), std::numeric_limits<double>::infinity());
  QCOMPARE(code({a}), QStringLiteral("plan_too_large"));
  a = idw(QString(257, QLatin1Char('a')));
  QCOMPARE(code({a}), QStringLiteral("node_id_too_large"));
}

void TestCapabilityPlan::portSemantics_data() {
  QTest::addColumn<QString>("dimension");
  QTest::addColumn<QString>("expected");
  for (const auto &dimension : {QStringLiteral("unit"), QStringLiteral("axis"), QStringLiteral("datum"),
                                 QStringLiteral("geometry")}) {
    QTest::newRow(qPrintable(dimension + QStringLiteral("-conflict"))) << dimension << dimension + QStringLiteral("_mismatch");
    QTest::newRow(qPrintable(dimension + QStringLiteral("-unknown"))) << dimension + QStringLiteral("-unknown")
                                                                  << dimension + QStringLiteral("_unknown");
  }
  QTest::newRow("kind") << QStringLiteral("kind") << QStringLiteral("port_type");
  QTest::newRow("cardinality") << QStringLiteral("cardinality") << QStringLiteral("cardinality_mismatch");
  QTest::newRow("optional-output") << QStringLiteral("optional-output") << QStringLiteral("cardinality_mismatch");
  QTest::newRow("unknown-kind") << QStringLiteral("unknown-kind") << QStringLiteral("port_type");
  QTest::newRow("datum-placeholder") << QStringLiteral("datum-placeholder") << QStringLiteral("datum_unknown");
}

void TestCapabilityPlan::portSemantics() {
  QFETCH(QString, dimension);
  QFETCH(QString, expected);
  AlgoPort source;
  source.id = QStringLiteral("source");
  source.kind = QStringLiteral("raster");
  auto target = source;
  target.id = QStringLiteral("target");
  const bool unknown = dimension.endsWith(QLatin1String("-unknown"));
  if (dimension.startsWith(QLatin1String("unit"))) { source.unit = unknown ? QString() : QStringLiteral("ft"); target.unit = QStringLiteral("m"); }
  if (dimension.startsWith(QLatin1String("axis"))) { source.axis = unknown ? QString() : QStringLiteral("MD"); target.axis = QStringLiteral("TVD"); }
  if (dimension.startsWith(QLatin1String("datum"))) { source.verticalDatum = unknown ? QString() : QStringLiteral("MSL"); target.verticalDatum = QStringLiteral("KB"); }
  if (dimension.startsWith(QLatin1String("geometry"))) { source.geometryHint = unknown ? QString() : QStringLiteral("point"); target.geometryHint = QStringLiteral("line"); }
  if (dimension == QLatin1String("kind")) source.kind = QStringLiteral("vector");
  if (dimension == QLatin1String("unknown-kind")) source.kind.clear();
  if (dimension == QLatin1String("cardinality")) source.cardinalityMax = 2;
  if (dimension == QLatin1String("optional-output")) source.required = false;
  if (dimension == QLatin1String("datum-placeholder")) {
    source.verticalDatum = QStringLiteral("explicit");
    target.verticalDatum = QStringLiteral("explicit");
  }
  QCOMPARE(checkPortCompatibility(source, target).code, expected);
  source = target;
  if (dimension == QLatin1String("datum-placeholder")) {
    source.verticalDatum = QStringLiteral("MSL");
    target.verticalDatum = QStringLiteral("MSL");
  }
  QCOMPARE(checkPortCompatibility(source, target).code, QStringLiteral("ok"));
  source.unit = QStringLiteral("m");
  target.unit.clear();
  QCOMPARE(checkPortCompatibility(source, target).code, QStringLiteral("ok"));
}

void TestCapabilityPlan::parametersHaveOneSourceOfDefaults() {
  const auto plan = validatePlan({idw()}, {});
  QVERIFY(plan.ok);
  const auto expected = validateAndFillParams(QStringLiteral("paleo:paleo_constraint_idw"), 1, {});
  QVERIFY(expected.ok);
  QCOMPARE(plan.nodes.first().node.parameters, expected.effective);
  const auto specification = AlgorithmSpecRegistry::spec(QStringLiteral("native:factor_contour"));
  QCOMPARE(specification.inputs.size(), 1);
  QCOMPARE(specification.inputs.first().id, QStringLiteral("RASTER"));
}

QTEST_GUILESS_MAIN(TestCapabilityPlan)
#include "tst_capability_plan.moc"
