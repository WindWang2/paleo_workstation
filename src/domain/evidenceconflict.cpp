// 层：数据
#include "evidenceconflict.h"
#include <QHash>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::evidenceconflict {
namespace {
// 有空间落点的意见来源：矛盾按空间矛盾表达（可定位到井/井旁道）。
bool spatialKind(const QString &sourceKind) {
  return sourceKind == QLatin1String("well") || sourceKind == QLatin1String("well_interval") ||
         sourceKind == QLatin1String("seismic");
}
QString shortVersion(const QString &version) {
  const auto trimmed = version.trimmed();
  if (trimmed.isEmpty()) return QStringLiteral("无版本");
  return trimmed.size() <= 8 ? trimmed : trimmed.left(8) + QStringLiteral("…");
}
int percent(double fraction) {
  return qRound(std::max(0.0, std::min(1.0, fraction)) * 100.0);
}
QString displayName(const Opinion &op) {
  if (!op.label.trimmed().isEmpty()) return op.label;
  return op.sourceKind.isEmpty() ? QStringLiteral("未命名来源") : op.sourceKind;
}
QString kindOrOther(const Opinion &op) {
  return op.sourceKind.isEmpty() ? QStringLiteral("other") : op.sourceKind;
}
// 归因重放分布（汇总 {code,cells,fraction} 或 per-input {code,cells}）→
// 「62% 相 2、30% 相 5」文本；最多列 3 档再加「等」。
QString describeDistribution(const QVariantList &distribution) {
  QList<QPair<int, qint64>> rows;
  qint64 total = 0;
  for (const auto &value : distribution) {
    const auto entry = value.toMap();
    const qint64 cells = entry.value("cells").toLongLong();
    rows << qMakePair(entry.value("code").toInt(), cells);
    total += cells;
  }
  if (rows.isEmpty()) return QStringLiteral("无像元分布");
  std::stable_sort(rows.begin(), rows.end(),
                   [](const QPair<int, qint64> &a, const QPair<int, qint64> &b) { return a.second > b.second; });
  QStringList parts;
  const int limit = std::min(3, int(rows.size()));
  for (int n = 0; n < limit; ++n)
    parts << QStringLiteral("%1% 相 %2").arg(percent(total > 0 ? double(rows[n].second) / double(total) : 0.0)).arg(rows[n].first);
  if (rows.size() > limit) parts << QStringLiteral("等");
  return parts.join(QStringLiteral("、"));
}
// 既有 check() findings 的 code → conflict_type 归类（规则 6）。
QString conflictTypeForCode(const QString &code) {
  if (code.startsWith(QLatin1String("missing_source"))) return QStringLiteral("broken_reference");
  if (code == QLatin1String("contradicting_evidence") || code == QLatin1String("disputed_observation"))
    return QStringLiteral("manual_dispute");
  if (code == QLatin1String("stale_source") || code.startsWith(QLatin1String("stale"))) return QStringLiteral("broken_reference");
  if (code.startsWith(QLatin1String("geometry")) || code.startsWith(QLatin1String("classification")))
    return QStringLiteral("data_quality");
  return QStringLiteral("data_quality");
}
int severityRank(const QString &severity) {
  if (severity == QLatin1String("Blocking")) return 0;
  if (severity == QLatin1String("Error")) return 1;
  if (severity == QLatin1String("Warning")) return 2;
  return 3; // Info 及未知——未知不参与输出（已归一）。
}
int topCodeOf(const QVariantList &distribution, double *fraction = nullptr) {
  int top = 0;
  qint64 topCells = -1;
  double topFraction = 0.0;
  qint64 total = 0;
  for (const auto &value : distribution) {
    const auto entry = value.toMap();
    total += entry.value("cells").toLongLong();
  }
  for (const auto &value : distribution) {
    const auto entry = value.toMap();
    const qint64 cells = entry.value("cells").toLongLong();
    if (cells > topCells) {
      topCells = cells;
      top = entry.value("code").toInt();
      topFraction = entry.contains("fraction") ? entry.value("fraction").toDouble()
                                               : (total > 0 ? double(cells) / double(total) : 0.0);
    }
  }
  if (fraction) *fraction = topFraction;
  return top;
}
} // namespace

QStringList conflictTypes() {
  return {"data_quality", "broken_reference", "spatial_contradiction", "interpretation_contradiction",
          "model_uncertainty", "manual_dispute", "consistency_note"};
}
QStringList severities() { return {"Blocking", "Error", "Warning", "Info"}; }

QString normalizeSeverity(const QString &severity) {
  const QString key = severity.trimmed().toLower();
  if (key == QLatin1String("blocking") || key == QLatin1String("block") ||
      key == QLatin1String("critical") || key == QLatin1String("fatal"))
    return QStringLiteral("Blocking");
  if (key == QLatin1String("error") || key == QLatin1String("major") || key == QLatin1String("serious"))
    return QStringLiteral("Error");
  if (key == QLatin1String("info") || key == QLatin1String("note") ||
      key == QLatin1String("informational") || key == QLatin1String("hint") || key == QLatin1String("debug"))
    return QStringLiteral("Info");
  return QStringLiteral("Warning"); // warning/warn/minor/未知：宁可多显示。
}

QVariantList analyze(const QList<Opinion> &opinions, int targetFaciesCode,
                     const QVariantMap &target, const QVariantMap &attribution,
                     const QVariantList &existingFindings) {
  QVariantList findings;
  const QVariantMap targetRef{{"feature_uid", target.value("feature_uid")},
                              {"horizon", target.value("horizon")},
                              {"version_id", target.value("version_id")}};
  auto add = [&](const QString &severity, const QString &code, const QString &conflictType,
                 const QString &source, const QString &label, const QString &versionId,
                 const QString &message, const QString &basis, const QString &suggestion) {
    findings << QVariantMap{{"severity", severity}, {"code", code}, {"message", message},
      {"target", targetRef}, {"evidence", QVariantMap{{"source", source}, {"label", label}, {"version_id", versionId}}},
      {"conflict_type", conflictType}, {"basis", basis}, {"suggestion", suggestion},
      {"status", QStringLiteral("open")}};
  };

  const QString attributionStatus = attribution.value("status").toString();
  // 自动归因（含 unresolved）：图斑编码作为一票参与多数方；人工或无自动
  // 归因：图斑编码不计票，与人工结论的矛盾单独表达。
  const bool autoAttributed =
      attributionStatus == QLatin1String("ok") || attributionStatus == QLatin1String("unresolved");

  // —— 意见计票（known=true 且编码非 0 才构成相意见；未知不参与）——
  QHash<int, int> votes;      // code -> 意见数
  QMap<int, int> firstSeen;   // code -> 首次出现下标（平票定序）
  QList<int> knownIndexes;
  for (int n = 0; n < opinions.size(); ++n) {
    const auto &op = opinions[n];
    if (!op.known || op.faciesCode == 0) continue;
    knownIndexes << n;
    ++votes[op.faciesCode];
    if (!firstSeen.contains(op.faciesCode)) firstSeen.insert(op.faciesCode, n);
  }
  auto effectiveVotes = votes;
  if (autoAttributed && targetFaciesCode != 0) ++effectiveVotes[targetFaciesCode];

  // 多数方：众数优先；平票先倾向图斑编码，再按首见顺序（确定性）。
  int majorityCode = std::numeric_limits<int>::min();
  for (auto it = effectiveVotes.cbegin(); it != effectiveVotes.cend(); ++it) {
    if (majorityCode == std::numeric_limits<int>::min() ||
        it.value() > effectiveVotes.value(majorityCode)) {
      majorityCode = it.key();
      continue;
    }
    if (it.value() != effectiveVotes.value(majorityCode)) continue;
    const bool candidateTarget = it.key() == targetFaciesCode;
    const bool majorityTarget = majorityCode == targetFaciesCode;
    if (candidateTarget && !majorityTarget) {
      majorityCode = it.key();
    } else if (candidateTarget == majorityTarget &&
               firstSeen.value(it.key(), std::numeric_limits<int>::max()) <
                   firstSeen.value(majorityCode, std::numeric_limits<int>::max())) {
      majorityCode = it.key();
    }
  }
  const int majorityOpinionCount = votes.value(majorityCode, 0); // 不含图斑票

  // 规则 2：known 编码 >=2 个不同值 → 每个少数方一条 Warning。
  QSet<int> flaggedMinority;
  if (effectiveVotes.size() >= 2) {
    for (int index : knownIndexes) {
      const auto &op = opinions[index];
      if (op.faciesCode == majorityCode) continue;
      flaggedMinority.insert(index);
      const bool spatial = spatialKind(op.sourceKind);
      const QString conflictType = spatial ? QStringLiteral("spatial_contradiction")
                                          : QStringLiteral("interpretation_contradiction");
      QString basis;
      if (op.faciesCode != targetFaciesCode)
        basis = QStringLiteral("『%1』解释为相 %2（版本 %3），图斑为相 %4")
                    .arg(displayName(op))
                    .arg(op.faciesCode)
                    .arg(shortVersion(op.versionId))
                    .arg(targetFaciesCode);
      else
        basis = QStringLiteral("『%1』解释为相 %2（版本 %3），与图斑一致；另有 %4 个意见判为相 %5")
                    .arg(displayName(op))
                    .arg(op.faciesCode)
                    .arg(shortVersion(op.versionId))
                    .arg(majorityOpinionCount)
                    .arg(majorityCode);
      if (op.faciesCode != targetFaciesCode && majorityOpinionCount > 0)
        basis += QStringLiteral("；%1 个意见判为相 %2").arg(majorityOpinionCount).arg(majorityCode);
      if (op.weight > 0.0 && op.weight < 1.0)
        basis += QStringLiteral("；该意见占重放像元 %1%").arg(percent(op.weight));
      const QString message =
          spatial
              ? QStringLiteral("『%1』判为相 %2（版本 %3），与图斑相 %4 空间矛盾")
                    .arg(displayName(op))
                    .arg(op.faciesCode)
                    .arg(shortVersion(op.versionId))
                    .arg(targetFaciesCode)
              : QStringLiteral("『%1』判为相 %2（版本 %3），与图斑相 %4 的解释相矛盾")
                    .arg(displayName(op))
                    .arg(op.faciesCode)
                    .arg(shortVersion(op.versionId))
                    .arg(targetFaciesCode);
      const QString suggestion = spatial
                                    ? QStringLiteral("定位『%1』复核解释与图斑边界").arg(displayName(op))
                                    : QStringLiteral("复核『%1』意见与图斑分类的取舍，必要时人工裁决").arg(displayName(op));
      add(QStringLiteral("Warning"), QStringLiteral("minority_opinion"), conflictType, kindOrOther(op),
          displayName(op), op.versionId, message, basis, suggestion);
    }
  }

  // 规则 3：人工/无自动归因时，known 意见 ≠ 图斑编码 → 解释矛盾。
  if (!autoAttributed) {
    for (int index : knownIndexes) {
      if (flaggedMinority.contains(index)) continue; // 已由少数方 finding 点名
      const auto &op = opinions[index];
      if (op.faciesCode == targetFaciesCode) continue;
      add(QStringLiteral("Warning"), QStringLiteral("manual_target_conflict"),
          QStringLiteral("interpretation_contradiction"), kindOrOther(op), displayName(op), op.versionId,
          QStringLiteral("人工解释图斑为相 %1，『%2』意见为相 %3（版本 %4）")
              .arg(targetFaciesCode)
              .arg(displayName(op))
              .arg(op.faciesCode)
              .arg(shortVersion(op.versionId)),
          QStringLiteral("attribution.status=%1；『%2』解释为相 %3，图斑（人工判定）为相 %4")
              .arg(attributionStatus.isEmpty() ? QStringLiteral("缺失") : attributionStatus)
              .arg(displayName(op))
              .arg(op.faciesCode)
              .arg(targetFaciesCode),
          QStringLiteral("定位『%1』复核该意见，或修正人工解释").arg(displayName(op)));
    }
  }

  // 规则 8 + 约束状态：意见附带上下文的数据质量检查（known 与否都查）。
  const QString targetHorizon = target.value("horizon").toString();
  for (const auto &op : opinions) {
    const QString opinionHorizon = op.extra.value("horizon").toString();
    if (!opinionHorizon.isEmpty() && opinionHorizon != targetHorizon)
      add(QStringLiteral("Warning"), QStringLiteral("opinion_horizon_mismatch"), QStringLiteral("data_quality"),
          kindOrOther(op), displayName(op), op.versionId,
          QStringLiteral("『%1』的意见来自层位 %2，与图斑层位 %3 不符")
              .arg(displayName(op))
              .arg(opinionHorizon)
              .arg(targetHorizon),
          QStringLiteral("extra.horizon=%1 ≠ target.horizon=%2").arg(opinionHorizon).arg(targetHorizon),
          QStringLiteral("核实『%1』的层位归属或更新其版本").arg(displayName(op)));
    if (op.sourceKind == QLatin1String("constraint") && op.extra.value("constraint_inactive").toBool())
      add(QStringLiteral("Warning"), QStringLiteral("constraint_inactive"), QStringLiteral("data_quality"),
          kindOrOther(op), displayName(op), op.versionId,
          QStringLiteral("约束『%1』已停用或状态已变化（版本 %2），其相意见可能不再适用")
              .arg(displayName(op))
              .arg(shortVersion(op.versionId)),
          QStringLiteral("extra.constraint_inactive=true；停用约束的意见仍参与一致性统计"),
          QStringLiteral("重算前先确认约束『%1』的状态").arg(displayName(op)));
  }

  // 规则 4：归因 unresolved / 部分输入缺失 → 模型不确定性。
  const QVariantList unresolvedList = attribution.value("unresolved").toList();
  if (attributionStatus == QLatin1String("unresolved") || !unresolvedList.isEmpty()) {
    QStringList reasons;
    for (const auto &value : unresolvedList) {
      const auto entry = value.toMap();
      const QString layer = entry.value("layer_id").toString();
      const QString reason = entry.value("reason").toString();
      if (!layer.isEmpty())
        reasons << QStringLiteral("输入『%1』（版本 %2）：%3")
                       .arg(layer, shortVersion(entry.value("version_id").toString()),
                            reason.isEmpty() ? QStringLiteral("原因未登记") : reason);
      else if (!reason.isEmpty())
        reasons << reason;
    }
    if (attributionStatus == QLatin1String("unresolved")) {
      const QString message = attribution.value("message").toString();
      if (!message.isEmpty() && !reasons.contains(message)) reasons << message;
    }
    if (reasons.isEmpty()) reasons << QStringLiteral("归因状态为 %1").arg(attributionStatus);
    add(QStringLiteral("Warning"), QStringLiteral("attribution_unresolved"), QStringLiteral("model_uncertainty"),
        QStringLiteral("attribution"), QStringLiteral("图斑归因重放"),
        attribution.value("calculation_version").toString(),
        QStringLiteral("图斑归因未完全确定：%1").arg(reasons.join(QStringLiteral("；"))),
        QStringLiteral("attribution.status=%1；unresolved 输入 %2 个")
            .arg(attributionStatus.isEmpty() ? QStringLiteral("缺失") : attributionStatus)
            .arg(unresolvedList.size()),
        QStringLiteral("检查冻结输入版本的可用性后重新归因"));
  }

  // 规则 5：重放分布与图斑编码不一致（0 ≤ fraction < 1）。完全不一致
  // （fraction==0）必须告警——图斑编码不在任何重放像元中，静默零告警
  // 会让「完全不符」比「部分不符」更不可见。
  if (attribution.contains("consistent_fraction")) {
    const double fraction = attribution.value("consistent_fraction").toDouble();
    if (fraction >= 0.0 && fraction < 1.0) {
      const bool majorityStill = fraction >= 0.5;
      add(majorityStill ? QStringLiteral("Info") : QStringLiteral("Warning"), QStringLiteral("partial_consistency"),
          QStringLiteral("model_uncertainty"), QStringLiteral("attribution"), QStringLiteral("图斑归因重放"),
          attribution.value("calculation_version").toString(),
          QStringLiteral("重放像元分布与图斑编码不完全一致：%1；图斑为相 %2")
              .arg(describeDistribution(attribution.value("code_distribution").toList()))
              .arg(targetFaciesCode),
          QStringLiteral("consistent_fraction=%1（重放像元 %2% 与图斑编码一致；平滑聚合的固有差异）")
              .arg(QString::number(fraction, 'f', 2))
              .arg(percent(fraction)),
          QStringLiteral("检查图斑边界处的混合像元，必要时重算分类"));
    }
  }

  // 规则 1：全部 known 意见一致且等于图斑编码 → 一致性说明（Info）。
  if (!knownIndexes.isEmpty() && votes.size() == 1 && votes.contains(targetFaciesCode)) {
    QStringList names;
    for (int index : knownIndexes) names << displayName(opinions[index]);
    const QString shown =
        names.size() <= 3 ? names.join(QStringLiteral("、"))
                          : names.mid(0, 3).join(QStringLiteral("、")) + QStringLiteral(" 等");
    add(QStringLiteral("Info"), QStringLiteral("consistent_sources"), QStringLiteral("consistency_note"),
        QStringLiteral("aggregation"), QStringLiteral("%1 个相意见来源").arg(knownIndexes.size()), QString(),
        QStringLiteral("%1 个来源一致判为相 %2，与图斑分类一致")
            .arg(knownIndexes.size())
            .arg(targetFaciesCode),
        QStringLiteral("来源：%1；图斑为相 %2").arg(shown).arg(targetFaciesCode),
        QStringLiteral("无需处理，可正常确认解释"));
  }

  // 规则 6：既有 check() findings 原样并入（severity 归一 + 冲突归类）。
  for (const auto &value : existingFindings) {
    auto finding = value.toMap();
    const QString code = finding.value("code").toString();
    const QString evidenceId = finding.value("evidence_id").toString();
    finding.insert("severity", normalizeSeverity(finding.value("severity").toString()));
    finding.insert("conflict_type", conflictTypeForCode(code));
    finding.insert("target", targetRef);
    finding.insert("evidence", QVariantMap{{"source", QStringLiteral("evidence_record")},
                                           {"label", evidenceId.isEmpty() ? QStringLiteral("证据记录") : evidenceId},
                                           {"version_id", finding.value("source_version")}});
    finding.insert("status", QStringLiteral("open"));
    if (!finding.contains("message") || finding.value("message").toString().isEmpty())
      finding.insert("message", code);
    findings << finding;
  }

  // 规则 7：无任何 known 意见、无既有 findings、也无其他产出 → 诚实空态。
  if (knownIndexes.isEmpty() && existingFindings.isEmpty() && findings.isEmpty())
    add(QStringLiteral("Info"), QStringLiteral("no_known_opinions"), QStringLiteral("model_uncertainty"),
        QStringLiteral("aggregation"), QStringLiteral("相意见来源"), QString(),
        opinions.isEmpty() ? QStringLiteral("无已知相意见来源，图斑分类缺少交叉验证")
                           : QStringLiteral("无已知相意见来源（%1 个来源未给出有效相意见），图斑分类缺少交叉验证")
                                 .arg(opinions.size()),
        QStringLiteral("未收集到 known=true 的相意见；未知不视作一致或冲突"),
        QStringLiteral("补充井/预测/约束等意见来源后重新分析"));

  std::stable_sort(findings.begin(), findings.end(), [](const QVariant &a, const QVariant &b) {
    return severityRank(a.toMap().value("severity").toString()) <
           severityRank(b.toMap().value("severity").toString());
  });
  return findings;
}

QList<Opinion> opinionsFromAttribution(const QVariantMap &attribution) {
  QList<Opinion> out;
  if (attribution.value("status").toString() == QLatin1String("manual")) return out;
  for (const auto &value : attribution.value("inputs").toList()) {
    const auto input = value.toMap();
    // 重放会列出赢了 0 像元的输入（P0-A 起为展示对齐口径）：未参与本图斑
    // 决策，不应成为 known=false 的意见来源。
    if (input.value("cells").toLongLong() <= 0)
      continue;
    Opinion opinion;
    opinion.sourceKind = QStringLiteral("single_factor");
    const QString title = input.value("title").toString();
    opinion.label = !title.isEmpty() ? title : input.value("layer_id").toString();
    opinion.versionId = input.value("version_id").toString();
    opinion.weight = input.value("fraction").toDouble();
    const QString decision = input.value("decision").toString();
    const int priority = input.value("priority").toInt();
    opinion.detail = decision.isEmpty() ? QStringLiteral("优先级 %1").arg(priority)
                                        : QStringLiteral("优先级 %1 · %2").arg(priority).arg(decision);
    opinion.faciesCode = topCodeOf(input.value("code_distribution").toList());
    opinion.known = opinion.faciesCode != 0;
    opinion.extra = QVariantMap{{"layer_id", input.value("layer_id")},
                                {"priority", priority},
                                {"decision", input.value("decision")},
                                {"cells", input.value("cells")}};
    out << opinion;
  }
  // inputs 为空而汇总分布存在：一条汇总意见（诚实保留已知部分）。
  if (out.isEmpty()) {
    double topFraction = 0.0;
    const int code = topCodeOf(attribution.value("code_distribution").toList(), &topFraction);
    if (code != 0) {
      Opinion opinion;
      opinion.sourceKind = QStringLiteral("single_factor");
      opinion.label = QStringLiteral("归因重放（汇总分布）");
      opinion.detail = QStringLiteral("汇总 code_distribution");
      opinion.faciesCode = code;
      opinion.known = true;
      opinion.weight = topFraction;
      opinion.extra = QVariantMap{{"aggregated", true}};
      out << opinion;
    }
  }
  return out;
}
} // namespace paleo::evidenceconflict
