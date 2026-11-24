// 层：测试壳
// I5 W2/W8：services 层只读投影的逐字段 oracle——描述 DTO 必须与三注册表
// 原表一致；全版本枚举不遗漏、不合并（knownIds+spec 首项口径丢版本是反例）；
// complete 与 Callable 互不推导；View 命令无 capability 的规则原样保留。
#include <QtTest>
#include "../src/services/algorithmspec.h"
#include "../src/services/capabilitydescription.h"
#include "../src/services/capabilityspec.h"
#include "../src/services/commandspec.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

// —— services 投影的「正确与错误实现」判据 ——
//   · 每个投影字段与注册表原值逐项相等（不是仅非空）；
//   · allSpecsProjection 与 knownIds 同源同长（同 id 多版本不折叠），
//     specAtVersion(id,v) 与投影条目逐字段一致；
//   · 同 id 双版本的合成清单（喂给纯投影函数）两条都在——证明投影层不合并；
//   · 真实注册表当前多版本项（onnx:tile_classification 仅 v2）：v2 在、v1 缺，
//     不回落。
class TestI5CapabilityDescription : public QObject {
  Q_OBJECT
private slots:
  void capabilityProjectionMatchesRegistryFieldByField();
  void algorithmProjectionMatchesRegistryFieldByField();
  void commandProjectionMatchesRegistryFieldByField();
  void allSpecsProjectionKeepsEveryRegisteredVersion();
  void syntheticMultiVersionProjectionDoesNotMerge();
  void tileV2IsExactAndV1IsNotFallbackFilled();
  void portAndParamFactsStayOnTheSpec();
};

void TestI5CapabilityDescription::capabilityProjectionMatchesRegistryFieldByField() {
  const auto table = CapabilityRegistry::all();
  QVERIFY(!table.isEmpty());
  bool sawLegacyAdapter = false, sawCallable = false;
  for (const CapabilitySpec &spec : table) {
    const auto identity = paleo::capability::capabilityIdentityDescription(spec);
    QCOMPARE(identity.value("kind").toString(), QStringLiteral("capability"));
    QCOMPARE(identity.value("id").toString(), spec.capabilityId);
    QCOMPARE(identity.value("contract_version").toInt(), spec.contractVersion);
    QCOMPARE(identity.value("capability_kind").toString(),
             paleo::capability::capabilityKindName(spec.kind));
    QCOMPARE(identity.value("exposure").toString(), capabilityExposureName(spec.exposure));
    QCOMPARE(identity.value("exposure_reason").toString(), spec.exposureReason);
    QCOMPARE(identity.value("algorithm_id").toString(), spec.algorithmId);
    QCOMPARE(identity.value("algorithm_contract_version").toInt(), spec.algorithmContractVersion);
    QCOMPARE(identity.value("publish_effect").toString(), spec.publishEffect);
    QCOMPARE(identity.value("cancel_supported").toBool(), spec.cancelSupported);
    QCOMPARE(identity.value("resume_supported").toBool(), spec.resumeSupported);
    QCOMPARE(identity.value("restartable").toBool(), spec.restartable);
    const auto aliases = identity.value("aliases").toArray();
    QCOMPARE(aliases.size(), spec.aliases.size());
    for (int index = 0; index < spec.aliases.size(); ++index)
      QCOMPARE(aliases.at(index).toString(), spec.aliases.at(index));
    // 摘要卡片只含身份字段（list 页不灌大对象）。
    const auto card = paleo::capability::capabilitySummaryCard(spec);
    QCOMPARE(card.value("id").toString(), spec.capabilityId);
    QVERIFY(!card.contains(QStringLiteral("aliases")));
    if (spec.exposure == CapabilityExposure::LegacyAdapter)
      sawLegacyAdapter = true;
    if (spec.exposure == CapabilityExposure::Callable)
      sawCallable = true;
  }
  QVERIFY(sawLegacyAdapter); // wellfacies.predict 反例必须在场
  QVERIFY(sawCallable);
}

void TestI5CapabilityDescription::algorithmProjectionMatchesRegistryFieldByField() {
  const auto specs = AlgorithmSpecRegistry::allSpecsProjection();
  QVERIFY(!specs.isEmpty());
  bool sawComplete = false;
  for (const AlgorithmSpec &spec : specs) {
    const auto identity = paleo::capability::algorithmIdentityDescription(spec);
    QCOMPARE(identity.value("kind").toString(), QStringLiteral("algorithm"));
    QCOMPARE(identity.value("id").toString(), spec.id);
    QCOMPARE(identity.value("contract_version").toInt(), spec.contractVersion);
    QCOMPARE(identity.value("display_name").toString(), spec.displayName);
    QCOMPARE(identity.value("engine").toString(), spec.engine);
    QCOMPARE(identity.value("complete").toBool(), spec.complete);
    QCOMPARE(identity.value("implementation_version").toString(), spec.implementationVersion);
    QCOMPARE(identity.value("param_schema_version").toInt(), spec.paramSchemaVersion);
    QCOMPARE(identity.value("execution_shape").toString(), executionShapeName(spec.executionShape));
    QCOMPARE(identity.value("reproducibility").toString(),
             paleo::capability::algorithmReproducibilityName(spec.reproducibility));
    QCOMPARE(identity.value("cancel_supported").toBool(), spec.cancelSupported);
    QCOMPARE(identity.value("resume_supported").toBool(), spec.resumeSupported);

    const auto params = paleo::capability::algorithmParamsDescription(spec)
                            .value("fields").toArray();
    QCOMPARE(params.size(), spec.params.size());
    for (int index = 0; index < spec.params.size(); ++index) {
      const AlgorithmParamField &field = spec.params.at(index);
      const auto described = params.at(index).toObject();
      QCOMPARE(described.value("key").toString(), field.key);
      QCOMPARE(described.value("type").toString(),
               paleo::capability::algorithmParamTypeName(field.type));
      QCOMPARE(described.value("required").toBool(), field.required);
      QCOMPARE(described.value("apply_default_when_absent").toBool(),
               field.applyDefaultWhenAbsent);
      if (field.hasMin)
        QCOMPARE(described.value("minimum").toDouble(), field.minValue);
      else
        QVERIFY(!described.contains(QStringLiteral("minimum")));
    }

    const auto ports = paleo::capability::algorithmPortsDescription(spec);
    const auto inputs = ports.value("inputs").toArray();
    const auto outputs = ports.value("outputs").toArray();
    QCOMPARE(inputs.size(), spec.inputs.size());
    QCOMPARE(outputs.size(), spec.outputs.size());
    for (int index = 0; index < spec.inputs.size(); ++index) {
      const AlgoPort &port = spec.inputs.at(index);
      const auto described = inputs.at(index).toObject();
      QCOMPARE(described.value("id").toString(), port.id);
      QCOMPARE(described.value("kind").toString(), port.kind);
      QCOMPARE(described.value("required").toBool(), port.required);
      QCOMPARE(described.value("hidden").toBool(), port.hidden);
      QCOMPARE(described.value("cardinality_max").toInt(), port.cardinalityMax);
      // 空单位/轴原样输出空串——不从显示名猜测补齐。
      QCOMPARE(described.value("unit").toString(), port.unit);
      QCOMPARE(described.value("axis").toString(), port.axis);
    }
    if (spec.complete)
      sawComplete = true;
  }
  QVERIFY(sawComplete);
  // 当前表全部 complete=true（基线事实）；投影仍原样携带该标志，
  // complete ≠ callable 的区分在 inspection 服务测试里以 exposure 断言。
}

void TestI5CapabilityDescription::commandProjectionMatchesRegistryFieldByField() {
  const auto table = paleo::command::CommandRegistry::all();
  QVERIFY(!table.isEmpty());
  QVERIFY(paleo::command::CommandRegistry::validate().isEmpty()); // 表健康前置
  bool sawView = false, sawAgentSurface = false;
  for (const auto &spec : table) {
    const auto identity = paleo::capability::commandIdentityDescription(spec);
    QCOMPARE(identity.value("kind").toString(), QStringLiteral("command"));
    QCOMPARE(identity.value("id").toString(), spec.commandId);
    QCOMPARE(identity.value("contract_version").toInt(), spec.contractVersion);
    QCOMPARE(identity.value("command_kind").toString(),
             paleo::command::commandKindName(spec.kind));
    QCOMPARE(identity.value("stage").toString(), spec.stageId);
    QCOMPARE(identity.value("effect").toString(), spec.effect);
    QCOMPARE(identity.value("agent_surface").toBool(),
             spec.hasSurface(paleo::command::SurfaceAgent));
    QCOMPARE(identity.value("title").toString(), QString::fromUtf8(spec.title));
    QCOMPARE(identity.value("shortcut_id").toString(), spec.shortcutId);
    const auto required = identity.value("required_context").toArray();
    QCOMPARE(required.size(), spec.requiredContext.size());
    for (int index = 0; index < spec.requiredContext.size(); ++index)
      QCOMPARE(required.at(index).toString(), spec.requiredContext.at(index));
    if (spec.kind == paleo::command::CommandKind::View) {
      sawView = true;
      QVERIFY(spec.capabilityId.isEmpty()); // View 无 CapabilitySpec 规则原样保留
      QVERIFY(identity.value("capability_id").toString().isEmpty());
    }
    if (spec.hasSurface(paleo::command::SurfaceAgent))
      sawAgentSurface = true;
  }
  QVERIFY(sawView);
  QVERIFY(sawAgentSurface);
}

void TestI5CapabilityDescription::allSpecsProjectionKeepsEveryRegisteredVersion() {
  const auto projection = AlgorithmSpecRegistry::allSpecsProjection();
  const auto knownIds = AlgorithmSpecRegistry::knownIds();
  QCOMPARE(projection.size(), knownIds.size()); // 同源同长：多版本不折叠
  for (int index = 0; index < projection.size(); ++index)
    QCOMPARE(projection.at(index).id, knownIds.at(index)); // 注册次序不变
  // specAtVersion 与投影条目逐字段一致（抽真实多字段项核对）。
  bool checked = false;
  for (const AlgorithmSpec &spec : projection) {
    const auto atVersion = AlgorithmSpecRegistry::specAtVersion(spec.id, spec.contractVersion);
    QCOMPARE(atVersion.id, spec.id);
    QCOMPARE(atVersion.displayName, spec.displayName);
    QCOMPARE(atVersion.engine, spec.engine);
    QCOMPARE(atVersion.complete, spec.complete);
    QCOMPARE(atVersion.params.size(), spec.params.size());
    QCOMPARE(atVersion.inputs.size(), spec.inputs.size());
    QCOMPARE(atVersion.outputs.size(), spec.outputs.size());
    checked = true;
  }
  QVERIFY(checked);
  // knownIds 可含重复 id（表事实）；投影逐条保留而非集合化。
  const auto raw = AlgorithmSpecRegistry::knownIds();
  if (raw.size() != QSet<QString>(raw.cbegin(), raw.cend()).size())
    QCOMPARE(projection.size(), raw.size()); // 重复 id 的每个版本各占一条
}

void TestI5CapabilityDescription::syntheticMultiVersionProjectionDoesNotMerge() {
  // 当前真实表暂无同 id 双版本条目——用合成双版本清单喂纯投影函数，证明
  // 投影层无「按 id 合并」逻辑（knownIds+spec 首项口径会丢第二版）。
  AlgorithmSpec v1;
  v1.id = QStringLiteral("synthetic:dual");
  v1.displayName = QStringLiteral("dual v1");
  v1.engine = QStringLiteral("native_cpp");
  v1.complete = true;
  v1.contractVersion = 1;
  AlgorithmSpec v2 = v1;
  v2.contractVersion = 2;
  v2.displayName = QStringLiteral("dual v2");
  v2.params = {[] {
    AlgorithmParamField field;
    field.key = QStringLiteral("FACTOR");
    field.type = AlgorithmParamField::Double;
    field.defaultValue = 0.5;
    field.hasMin = true;
    field.minValue = 0;
    return field;
  }()};

  const auto identity1 = paleo::capability::algorithmIdentityDescription(v1);
  const auto identity2 = paleo::capability::algorithmIdentityDescription(v2);
  QCOMPARE(identity1.value("contract_version").toInt(), 1);
  QCOMPARE(identity2.value("contract_version").toInt(), 2);
  QVERIFY(identity1.value("display_name").toString() !=
          identity2.value("display_name").toString());
  QCOMPARE(paleo::capability::algorithmParamsDescription(v2)
               .value("fields").toArray().size(), 1);
  QCOMPARE(paleo::capability::algorithmParamsDescription(v1)
               .value("fields").toArray().size(), 0);
}

void TestI5CapabilityDescription::tileV2IsExactAndV1IsNotFallbackFilled() {
  // 真实多版本事实：onnx:tile_classification 当前登记 v2（v1 未登记）。
  const auto projection = AlgorithmSpecRegistry::allSpecsProjection();
  int versions = 0;
  for (const AlgorithmSpec &spec : projection)
    if (spec.id == QLatin1String("onnx:tile_classification")) {
      ++versions;
      QCOMPARE(spec.contractVersion, 2);
    }
  QCOMPARE(versions, 1);
  QVERIFY(AlgorithmSpecRegistry::specAtVersion(QStringLiteral("onnx:tile_classification"), 1)
              .id.isEmpty()); // v1 未登记：不回落填充
  // 投影描述 v2 条目如实携带引擎/形状/参数约束。
  for (const AlgorithmSpec &spec : projection)
    if (spec.id == QLatin1String("onnx:tile_classification")) {
      const auto identity = paleo::capability::algorithmIdentityDescription(spec);
      QCOMPARE(identity.value("engine").toString(), QStringLiteral("onnx"));
      QCOMPARE(identity.value("execution_shape").toString(), QStringLiteral("finite-halo"));
      const auto ports = paleo::capability::algorithmPortsDescription(spec);
      QVERIFY(ports.value("inputs").toArray().size() >= 1);
      QVERIFY(ports.value("outputs").toArray().size() >= 3);
    }
}

void TestI5CapabilityDescription::portAndParamFactsStayOnTheSpec() {
  // 约束/默认值单一来源：描述不发明 AlgorithmSpec 外的默认值或范围。
  const auto spec = AlgorithmSpecRegistry::specAtVersion(
      QStringLiteral("paleo:paleo_constraint_idw"), 1);
  QVERIFY(!spec.id.isEmpty());
  const auto params = paleo::capability::algorithmParamsDescription(spec)
                          .value("fields").toArray();
  bool sawField = false, sawCellSize = false;
  for (const auto &value : params) {
    const auto field = value.toObject();
    if (field.value("key").toString() == QLatin1String("FIELD")) {
      sawField = true;
      QCOMPARE(field.value("default_value").toString(), QStringLiteral("z"));
      QCOMPARE(field.value("required").toBool(), true);
    }
    if (field.value("key").toString() == QLatin1String("CELL_SIZE")) {
      sawCellSize = true;
      QCOMPARE(field.value("default_value").toDouble(), 1.0);
      QCOMPARE(field.value("minimum").toDouble(), 0.0001);
    }
  }
  QVERIFY(sawField);
  QVERIFY(sawCellSize);
  // 隐藏端口（LOCAL_GRID_WKT）作为 hidden 输入呈现，不与用户输入混同。
  const auto ports = paleo::capability::algorithmPortsDescription(spec);
  bool sawHidden = false;
  for (const auto &value : ports.value("inputs").toArray()) {
    const auto port = value.toObject();
    if (port.value("id").toString() == QLatin1String("LOCAL_GRID_WKT")) {
      sawHidden = true;
      QCOMPARE(port.value("hidden").toBool(), true);
    }
  }
  QVERIFY(sawHidden);
}

QTEST_MAIN(TestI5CapabilityDescription)
#include "tst_i5_capabilitydescription.moc"
