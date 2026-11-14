// 层：数据
// services/algorithmportcontract.cpp — 端口兼容性，不解析文件或推测单位。
#include "algorithmcontract.h"

ContractIssue checkPortCompatibility(const AlgoPort &source, const AlgoPort &target) {
  auto error = [&](const QString &code) { return ContractIssue{code, target.id, {}}; };
  if (source.kind.isEmpty() || target.kind.isEmpty() || source.kind != target.kind)
    return error(QStringLiteral("port_type"));
  if (source.effectiveCardinalityMin() < target.effectiveCardinalityMin() ||
      source.cardinalityMax < 1 || target.cardinalityMax < 1 ||
      source.effectiveCardinalityMin() > source.cardinalityMax ||
      target.effectiveCardinalityMin() > target.cardinalityMax ||
      source.cardinalityMax > target.cardinalityMax)
    return error(QStringLiteral("cardinality_mismatch"));
  const QVector<QPair<QString, QPair<QString, QString>>> semantics{
      {QStringLiteral("geometry"), {source.geometryHint, target.geometryHint}},
      {QStringLiteral("unit"), {source.unit, target.unit}},
      {QStringLiteral("axis"), {source.axis, target.axis}},
      {QStringLiteral("datum"), {source.verticalDatum, target.verticalDatum}}};
  for (const auto &semantic : semantics) {
    const QString expected = semantic.second.second;
    if (expected.isEmpty())
      continue;
    // "explicit" is a declaration that a concrete datum must be bound. Two
    // such placeholders are not evidence that the actual frames agree.
    if (semantic.second.first.isEmpty() || semantic.second.first == QLatin1String("explicit") ||
        expected == QLatin1String("explicit"))
      return error(semantic.first + QStringLiteral("_unknown"));
    if (semantic.second.first != expected)
      return error(semantic.first + QStringLiteral("_mismatch"));
  }
  return {QStringLiteral("ok"), {}, {}};
}
