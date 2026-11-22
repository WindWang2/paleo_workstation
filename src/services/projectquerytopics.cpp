// 层：数据
#include "projectquerytopics.h"
#include <cmath>

namespace paleo::query {
namespace {
// 与运行期执行面同值的预算域（改这里须同步执行面，反之亦然）：
//  · wells/horizons/assets/well_details/version_parents 走
//    ProjectQueryCapability::run() 的 integer()（page 1–256/缺省 50，
//    bytes 2048–65536/缺省 8192）；
//  · well_curves 页 1–16/缺省 16（domaintools 现行检查同值）；
//  · local_models 页 1–64/缺省 16、bytes 1024–32768/缺省 8192
//    （aichattoolrunner_models.cpp）。
QList<QueryTopicRule> buildRules() {
  return {
    {QStringLiteral("summary"), {}, {QStringLiteral("topic"), QStringLiteral("max_bytes")},
     false, 1, 256, 50, 2048, 65536, 8192},
    {QStringLiteral("wells"), {},
     {QStringLiteral("topic"), QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")}},
    {QStringLiteral("horizons"), {},
     {QStringLiteral("topic"), QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")}},
    {QStringLiteral("assets"), {},
     {QStringLiteral("topic"), QStringLiteral("asset_type"), QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")}},
    {QStringLiteral("well_details"), {},
     {QStringLiteral("topic"), QStringLiteral("well_id"), QStringLiteral("well_name"), QStringLiteral("role"),
      QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")}},
    {QStringLiteral("well_curves"), {QStringLiteral("well_ref")},
     {QStringLiteral("topic"), QStringLiteral("well_ref"), QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")},
     true, 1, 16, 16},
    {QStringLiteral("version"), {QStringLiteral("version_id")},
     {QStringLiteral("topic"), QStringLiteral("version_id"), QStringLiteral("catalog_revision")}, false},
    {QStringLiteral("version_parents"), {QStringLiteral("version_id")},
     {QStringLiteral("topic"), QStringLiteral("version_id"), QStringLiteral("catalog_revision"),
      QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")}},
    {QStringLiteral("well_facies_models"), {},
     {QStringLiteral("topic"), QStringLiteral("model_id")}, false},
    {QStringLiteral("well_facies_inputs"), {QStringLiteral("well_ref")},
     {QStringLiteral("topic"), QStringLiteral("well_ref")}, false},
    {QStringLiteral("local_models"), {},
     {QStringLiteral("topic"), QStringLiteral("model_name"), QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")},
     true, 1, 64, 16, 1024, 32768, 8192},
  };
}

// 严格整数预算（与运行期 integer()/pageInteger() 同口径）：
// 必须是 JSON double、有限、整值、落在 [minimum,maximum]。
bool budgetValue(const QJsonObject &args, const QString &key, int minimum, int maximum,
                 int fallback, int &out) {
  const auto value = args.value(key);
  if (value.isUndefined()) { out = fallback; return true; }
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) || number != std::floor(number) || number < minimum || number > maximum)
    return false;
  out = int(number);
  return true;
}
} // namespace

const QList<QueryTopicRule> &queryTopicRules() {
  static const QList<QueryTopicRule> rules = buildRules();
  return rules;
}

bool queryTopicRule(const QString &topic, QueryTopicRule &out) {
  for (const auto &rule : queryTopicRules())
    if (rule.topic == topic) { out = rule; return true; }
  return false;
}

QString validateQueryTopicArguments(const QJsonObject &args, QString *field) {
  const auto setField = [field](const QString &name) { if (field) *field = name; };
  QueryTopicRule rule;
  const QString topic = args.value(QStringLiteral("topic")).toString();
  if (!queryTopicRule(topic, rule))
    return QString(); // typed/未知主题：字段校验归专业解析器与执行面
  for (const auto &key : args.keys())
    if (!rule.allowed.contains(key)) {
      setField(key);
      return QStringLiteral("unsupported_topic_argument");
    }
  for (const auto &key : rule.required)
    if (!args.contains(key)) {
      setField(key);
      return QStringLiteral("missing_required_field");
    }
  int size = 0, bytes = 0;
  const bool pageSizeValid = budgetValue(args, QStringLiteral("page_size"), rule.pageSizeMin,
                                         rule.pageSizeMax, rule.pageSizeDefault, size);
  const bool maxBytesValid = budgetValue(args, QStringLiteral("max_bytes"), rule.maxBytesMin,
                                         rule.maxBytesMax, rule.maxBytesDefault, bytes);
  if (!pageSizeValid || !maxBytesValid) {
    // 字段归因：优先报实际无效的那个（page_size 有效而 max_bytes 越界时
    // 报 max_bytes，不误指 page_size）。
    setField(!pageSizeValid ? QStringLiteral("page_size") : QStringLiteral("max_bytes"));
    return QStringLiteral("query_budget_invalid");
  }
  return QString();
}
} // namespace paleo::query
