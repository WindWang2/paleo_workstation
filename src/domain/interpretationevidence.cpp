// 层：数据
#include "interpretationevidence.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <cmath>

namespace paleo::evidence {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("InterpretationEvidence", s); }
QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool fail(QString *error, const QString &message) {
  if (error) *error = message;
  return false;
}
bool revisionNumber(const QVariant &value) {
  bool ok = false;
  const double n = value.toDouble(&ok);
  return ok && std::isfinite(n) && n >= 0 && n == std::floor(n) && n <= 9007199254740991.0;
}
bool decode(const QString &json, bool array, QVariant *value, QString *error) {
  if (json.trimmed().isEmpty()) { *value = array ? QVariant(QVariantList{}) : QVariant(QVariantMap{}); return true; }
  QJsonParseError e;
  const auto doc = QJsonDocument::fromJson(json.toUtf8(), &e);
  if (e.error != QJsonParseError::NoError || (array ? !doc.isArray() : !doc.isObject()))
    return fail(error, tr("证据或解释内容损坏，原记录未覆盖"));
  *value = doc.toVariant();
  return true;
}
QString recordId(const QVariantMap &r) { return r.value("id", r.value("evidence_id")).toString(); }
} // namespace
QStringList roles() { return {"SUPPORTED_BY", "CONTRADICTED_BY", "REFERENCES", "CONSTRAINED_BY"}; }
QStringList types() { return {"well", "seismic", "single_factor", "constraint", "rule", "manual", "reference", "computation"}; }
QStringList interpretationStates() { return {"Proposed", "NeedsReview", "Confirmed", "Rejected", "Disputed", "Superseded"}; }
bool canTransition(const QString &before, const QString &after) {
  if (!interpretationStates().contains(before) || !interpretationStates().contains(after) || before == "Superseded") return false;
  if (before == after) return true;
  if (after == "Superseded" || after == "NeedsReview" || after == "Disputed") return true;
  if (before == "Confirmed" || before == "Rejected") return false;
  return after == "Confirmed" || after == "Rejected" || after == "Proposed";
}
QString encode(const QVariant &value) { return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact)); }
bool decodeRecords(const QString &json, QVariantList *records, QString *error) {
  if (!records) return fail(error, tr("未提供证据读取目标"));
  QVariant value;
  if (!decode(json, true, &value, error) || !validateRecords(value.toList(), error)) return false;
  *records = value.toList(); return true;
}
bool decodeInterpretation(const QString &json, QVariantMap *interpretation, QString *error) {
  if (!interpretation) return fail(error, tr("未提供解释读取目标"));
  QVariant value;
  if (!decode(json, false, &value, error) || !validateInterpretation(value.toMap(), error)) return false;
  *interpretation = value.toMap(); return true;
}
bool validateRecords(const QVariantList &records, QString *error) {
  if (records.size() > 10000) return fail(error, tr("单要素证据记录超过 10000 条，请整理关联"));
  QSet<QString> ids;
  for (const auto &value : records) {
    if (value.typeId() != QMetaType::QVariantMap) return fail(error, tr("证据列表包含无效记录"));
    const auto r = value.toMap(); const auto key = recordId(r);
    if (key.isEmpty() || ids.contains(key)) return fail(error, tr("证据身份缺失或重复"));
    ids.insert(key);
    if (r.contains("role") && !roles().contains(r.value("role").toString())) return fail(error, tr("证据作用类型无效"));
    if (r.contains("type") && !types().contains(r.value("type").toString())) return fail(error, tr("证据类型无效"));
    if (r.contains("verification") && !QStringList{"Unverified", "Verified", "Disputed"}.contains(r.value("verification").toString()))
      return fail(error, tr("数据核验状态无效"));
    if (r.contains("revision") && !revisionNumber(r.value("revision"))) return fail(error, tr("证据修订号无效"));
  }
  return true;
}
bool validateInterpretation(const QVariantMap &i, QString *error) {
  if (i.isEmpty()) return true; // early projects have no interpretation object.
  if (i.value("id").toString().isEmpty() || i.value("assertion_id").toString().isEmpty() ||
      i.value("feature_uid").toString().isEmpty() || !interpretationStates().contains(i.value("status").toString()) ||
      !revisionNumber(i.value("revision"))) return fail(error, tr("解释身份、状态或修订号无效"));
  if (i.contains("history") && i.value("history").typeId() != QMetaType::QVariantList) return fail(error, tr("解释历史格式无效"));
  QSet<QString> events;
  quint64 previous = 0;
  for (const auto &value : i.value("history").toList()) {
    const auto e = value.toMap(); const auto key = e.value("event_id").toString();
    if (value.typeId() != QMetaType::QVariantMap || key.isEmpty() || events.contains(key) ||
        e.value("actor_id").toString().isEmpty() || e.value("reason").toString().trimmed().isEmpty() ||
        !QDateTime::fromString(e.value("timestamp").toString(), Qt::ISODateWithMs).isValid() ||
        !revisionNumber(e.value("before_revision")) || !revisionNumber(e.value("after_revision")) ||
        e.value("before_revision").toULongLong() != previous || e.value("after_revision").toULongLong() != previous + 1)
      return fail(error, tr("解释审计事件无效"));
    events.insert(key);
    previous = e.value("after_revision").toULongLong();
  }
  if (previous != i.value("revision").toULongLong()) return fail(error, tr("解释修订号与审计历史不一致"));
  return true;
}
bool revise(const QVariantList &records, const QVariantMap &interpretation,
            const QVariantMap &target, const QString &action, const QVariantMap &change,
            const QString &actor, const QString &reason, quint64 expectedRevision,
            Revision *result, QString *error) {
  if (!result || actor.trimmed().isEmpty() || actor.size() > 200 || reason.trimmed().isEmpty() || reason.size() > 10000)
    return fail(error, tr("业务修改必须填写本地操作人及理由"));
  if (!validateRecords(records, error) || !validateInterpretation(interpretation, error)) return false;
  const auto uid = target.value("feature_uid").toString();
  if (uid.isEmpty()) return fail(error, tr("要素缺少稳定身份，请先创建编辑副本"));
  auto i = interpretation;
  if (!i.isEmpty() && (i.value("feature_uid").toString() != uid || i.value("horizon").toString() != target.value("horizon").toString()))
    return fail(error, tr("解释与要素身份或层位不一致，请复核来源"));
  quint64 before = i.value("revision", 0).toULongLong();
  if (before != expectedRevision) return fail(error, tr("解释已被更新，请刷新后重试"));
  if (i.value("status").toString() == "Superseded" || i.value("history").toList().size() >= 10000) {
    const auto oldHistory = i.value("history").toList();
    const auto lastVersion = oldHistory.isEmpty() ? QString() : oldHistory.last().toMap().value("version_id").toString();
    const auto savedVersion = target.value("version_id").toString();
    if (lastVersion.isEmpty() || savedVersion.isEmpty() || lastVersion == savedVersion)
      return fail(error, tr("已替代解释或历史容量上限只读；请保存新成果版本后在其编辑副本修订"));
    // The target already has a distinct immutable saved snapshot. Fork only
    // on this explicit business change, inside its native undo transaction.
    // Never reset or rewrite the parent's audit events or published bytes.
    const auto parent = i;
    i = {{"id", id()}, {"assertion_id", id()}, {"feature_uid", uid}, {"horizon", target.value("horizon")},
      {"status", "Proposed"}, {"revision", 0}, {"parent_interpretation_id", parent.value("id")},
      {"parent_version_id", savedVersion}, {"inherited_interpretation", parent}};
    before = 0;
  }
  if (i.isEmpty()) i = {{"id", id()}, {"assertion_id", id()}, {"feature_uid", uid}, {"horizon", target.value("horizon")}, {"status", "Proposed"}, {"revision", 0}};
  const auto beforeState = i;
  QVariantList next = records;
  QStringList linked;
  QVariantMap previousRecord, afterRecord;
  if (action == "link") {
    auto r = change;
    const auto key = recordId(r).isEmpty() ? id() : recordId(r);
    for (const auto &value : next) {
      const auto existing = value.toMap();
      if (recordId(existing) == key) return fail(error, tr("证据身份重复"));
      if (!existing.value("unlinked").toBool() && !r.value("source_version").toString().isEmpty() &&
          existing.value("source_version") == r.value("source_version") && existing.value("source_feature_uid") == r.value("source_feature_uid") &&
          existing.value("role", "REFERENCES") == r.value("role", "REFERENCES") && existing.value("text") == r.value("text"))
        return fail(error, tr("相同来源和作用的证据关联已存在"));
    }
    const auto text = r.value("text").toString().trimmed();
    if (text.isEmpty() || text.size() > 10000) return fail(error, tr("证据说明须为 1–10000 字"));
    r.insert("id", key); r.insert("evidence_id", key); r.insert("text", text);
    r.insert("role", r.value("role", "REFERENCES")); r.insert("type", r.value("type", "manual"));
    r.insert("verification", "Unverified"); r.insert("revision", 1);
    r.insert("origin", "manual_interpretation");
    r.insert("feature_uid", uid); r.insert("horizon", target.value("horizon"));
    r.insert("geometry_hash", target.value("geometry_hash")); r.insert("facies_schema_hash", target.value("facies_schema_hash"));
    r.insert("actor_id", actor.trimmed()); r.insert("created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    next << r; linked << key; afterRecord = r;
  } else if (action == "unlink" || action == "update_evidence" || action == "verify_evidence") {
    const auto key = change.value("evidence_id").toString();
    int row = -1;
    for (int n = 0; n < next.size(); ++n) if (recordId(next[n].toMap()) == key) row = n;
    if (row < 0 || next[row].toMap().value("unlinked").toBool()) return fail(error, tr("证据关联不存在或已解除"));
    auto r = next[row].toMap(); previousRecord = r;
    if (action == "unlink") r.insert("unlinked", true);
    if (action == "update_evidence") {
      if (change.contains("role")) r.insert("role", change.value("role"));
      if (change.contains("source_feature_uid")) {
        r.insert("source_feature_uid", change.value("source_feature_uid"));
        r.insert("source_feature_missing", false);
        r.insert("source_feature_horizon", change.value("source_feature_horizon"));
        if (change.contains("spatial_intersection")) r.insert("spatial_intersection", change.value("spatial_intersection"));
        else r.remove("spatial_intersection");
        r.insert("geometry_hash", target.value("geometry_hash"));
        r.insert("verification", "Unverified");
      }
      if (change.contains("text")) {
        const auto text = change.value("text").toString().trimmed();
        if (text.isEmpty() || text.size() > 10000) return fail(error, tr("证据说明须为 1–10000 字"));
        r.insert("text", text);
      }
    }
    if (action == "verify_evidence") r.insert("verification", change.value("verification"));
    r.insert("revision", r.value("revision", 0).toULongLong() + 1); next[row] = r; linked << key; afterRecord = r;
  } else if (action == "review") {
    const auto state = change.value("status").toString();
    if (!canTransition(i.value("status").toString(), state)) return fail(error, tr("解释状态转换无效；请先标记待复核"));
    if (state == "Confirmed") {
      for (const auto &finding : check(next, i, target)) if (finding.toMap().value("severity").toString() == "Blocking")
        return fail(error, tr("存在破坏关联完整性的问题，不能确认解释"));
    }
    i.insert("status", state);
  } else if (action == "edit") {
    i.insert("status", "NeedsReview");
    i.insert("facies_code", change.value("facies_code", target.value("facies_code")));
  } else return fail(error, tr("未知解释修改动作"));
  if (!validateRecords(next, error)) return false;
  if (action != "review") i.insert("status", "NeedsReview");
  i.insert("revision", before + 1); i.insert("actor_id", actor.trimmed());
  i.insert("geometry_hash", target.value("geometry_hash"));
  i.insert("facies_schema_hash", target.value("facies_schema_hash"));
  i.insert("facies_code", target.value("facies_code", i.value("facies_code")));
  QVariantList history = i.value("history").toList();
  const auto stateSummary = [](QVariantMap state) {
    state.remove("history"); state.remove("inherited_interpretation"); state.remove("parent_interpretations");
    state.remove("carried_events"); state.remove("ancestor_interpretation_ids");
    return state;
  };
  auto beforeData = stateSummary(beforeState);
  auto afterData = stateSummary(i);
  QVariantMap event{{"event_id", id()}, {"operation_id", change.value("operation_id", id())},
    {"target_object_id", i.value("id")}, {"target_object_type", "Interpretation"}, {"feature_uid", uid},
    {"version_id", target.value("version_id")}, {"action", action}, {"before_revision", before}, {"after_revision", before + 1},
    {"actor_id", actor.trimmed()}, {"reason", reason.trimmed()}, {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    {"linked_evidence_ids", linked}, {"before", beforeData}, {"after", afterData}};
  auto eventChange = change;
  if (action == "edit" && eventChange.value("before").typeId() == QMetaType::QVariantMap) {
    auto nativeBefore = eventChange.value("before").toMap();
    // The journal already retains its prior events and inherited parents.
    // Embedding the complete prior JSON in every spatial event recursively
    // doubles persisted history. Keep the native geometry and revision identity;
    // complete deletion/merge snapshots belong to version spatial_edit_events.
    const auto raw = nativeBefore.value("interpretation");
    const auto previous = raw.typeId() == QMetaType::QVariantMap ? raw.toMap() :
        QJsonDocument::fromJson(raw.toString().toUtf8()).toVariant().toMap();
    nativeBefore.insert("interpretation", stateSummary(previous));
    nativeBefore.remove("evidence");
    eventChange.insert("before", nativeBefore);
  }
  event.insert("change", eventChange);
  if (!previousRecord.isEmpty()) event.insert("before_record", previousRecord);
  if (!afterRecord.isEmpty()) event.insert("after_record", afterRecord);
  history << event; i.insert("history", history);
  if (!validateInterpretation(i, error)) return false;
  result->records = next; result->interpretation = i; return true;
}
QVariantList check(const QVariantList &records, const QVariantMap &interpretation, const QVariantMap &target) {
  QVariantList findings;
  auto add = [&](const QString &severity, const QString &code, const QVariantMap &r, const QString &message) {
    findings << QVariantMap{{"severity", severity}, {"code", code}, {"evidence_id", recordId(r)}, {"source_version", r.value("source_version")}, {"message", message}};
  };
  QString error;
  if (!validateRecords(records, &error) || !validateInterpretation(interpretation, &error)) { add("Blocking", "corrupt_record", {}, error); return findings; }
  for (const auto &value : records) {
    const auto r = value.toMap(); if (r.value("unlinked").toBool()) continue;
    if (r.value("source_missing").toBool()) add("Blocking", "missing_source", r, tr("引用的来源版本已不存在"));
    if (r.value("source_feature_missing").toBool()) add("Blocking", "missing_source_feature", r, tr("引用的来源图元不存在或稳定身份重复"));
    if (!r.value("horizon").toString().isEmpty() && r.value("horizon") != target.value("horizon")) add("Blocking", "horizon_mismatch", r, tr("证据层位与解释层位不一致"));
    if (!r.value("source_horizon").toString().isEmpty() && r.value("source_horizon") != target.value("horizon")) add("Blocking", "source_horizon_mismatch", r, tr("来源数据属于其他层位"));
    if (!r.value("source_feature_horizon").toString().isEmpty() && r.value("source_feature_horizon") != target.value("horizon")) add("Blocking", "source_feature_horizon_mismatch", r, tr("来源图元属于其他层位"));
    if (!r.value("geometry_hash").toString().isEmpty() && r.value("geometry_hash") != target.value("geometry_hash")) add("Warning", "geometry_changed", r, tr("关联后几何已改变，空间适用范围需复核"));
    if (!r.value("facies_schema_hash").toString().isEmpty() && r.value("facies_schema_hash") != target.value("facies_schema_hash")) add("Warning", "classification_changed", r, tr("相分类表已改变，解释适用性需复核"));
    if (r.value("source_stale").toBool()) add("Warning", "stale_source", r, tr("来源已有更新或被标记过期；保留当时版本引用"));
    else if (!r.value("source_current_version").toString().isEmpty() && r.value("source_current_version") != r.value("source_version"))
      add("Warning", "newer_source_version", r, tr("来源资产已有较新版本；本解释仍引用当时的旧版本"));
    if (!r.value("source_facies_schema_hash").toString().isEmpty() && !target.value("facies_schema_hash").toString().isEmpty() &&
        r.value("source_facies_schema_hash") != target.value("facies_schema_hash"))
      add("Warning", "source_classification_mismatch", r, tr("来源相分类表与当前解释不同，不能直接比较相编码"));
    if (r.contains("spatial_intersection") && !r.value("spatial_intersection").toBool()) add("Warning", "outside_extent", r, tr("来源有效范围与图斑范围不相交"));
    if (r.value("role").toString() == "CONTRADICTED_BY") add("Warning", "contradicting_evidence", r, tr("保留反驳证据，请查看争议来源"));
    if (r.value("verification").toString() == "Disputed") add("Warning", "disputed_observation", r, tr("数据观察本身存在争议"));
    if (!r.contains("role")) add("Info", "legacy_role", r, tr("早期证据未登记作用类型"));
  }
  return findings;
}
} // namespace paleo::evidence
