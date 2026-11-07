// 层：数据
// services/algorithmcontract.cpp — 见 algorithmcontract.h。
#include "algorithmcontract.h"

#include <QSet>

namespace {

bool isPort(const AlgorithmSpec &spec, const QString &key) {
  for (const AlgoPort &port : spec.inputs) {
    if (port.id == key)
      return true;
  }
  for (const AlgoPort &port : spec.outputs) {
    if (port.id == key)
      return true;
  }
  return false;
}

bool isNumber(const QVariant &value) {
  switch (value.userType()) {
  case QMetaType::Int:
  case QMetaType::UInt:
  case QMetaType::LongLong:
  case QMetaType::ULongLong:
  case QMetaType::Double:
  case QMetaType::Float:
    return true;
  default:
    return false;
  }
}

void addIssue(ValidatedParams *result, const QString &code, const QString &field, const QString &message) {
  ContractIssue issue;
  issue.code = code;
  issue.field = field;
  issue.message = message;
  result->issues.append(issue);
  if (result->code.isEmpty())
    result->code = code;
}

} // namespace

ValidatedParams validateAndFillParams(const QString &algorithmId, int requestedVersion, const QVariantMap &params) {
  ValidatedParams result;
  const AlgorithmSpec spec = AlgorithmSpecRegistry::specAtVersion(algorithmId, requestedVersion);
  if (spec.id.isEmpty()) {
    addIssue(&result, requestedVersion > 0 && AlgorithmSpecRegistry::knows(algorithmId) ? QStringLiteral("unsupported_version")
                                                                                        : QStringLiteral("unknown_algorithm"),
             algorithmId, QStringLiteral("algorithm version is not registered"));
    return result;
  }
  QSet<QString> known;
  for (const AlgorithmParamField &field : spec.params)
    known.insert(field.key);
  for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
    if (known.contains(it.key()) || isPort(spec, it.key()))
      continue;
    addIssue(&result, QStringLiteral("unknown_param"), it.key(), QStringLiteral("parameter is not in the spec"));
  }
  for (const AlgorithmParamField &field : spec.params) {
    const bool present = params.contains(field.key);
    if (!present) {
      if (field.required && !field.applyDefaultWhenAbsent) {
        addIssue(&result, QStringLiteral("missing_required"), field.key, QStringLiteral("required parameter is absent"));
        continue;
      }
      if (field.required && (!field.applyDefaultWhenAbsent || !field.defaultValue.isValid())) {
        addIssue(&result, QStringLiteral("missing_required"), field.key, QStringLiteral("required parameter is absent"));
        continue;
      }
      if (field.applyDefaultWhenAbsent && field.defaultValue.isValid())
        result.effective.insert(field.key, field.defaultValue);
      continue;
    }
    const QVariant value = params.value(field.key);
    if (field.type == AlgorithmParamField::Int || field.type == AlgorithmParamField::Double) {
      if (!isNumber(value)) {
        addIssue(&result, QStringLiteral("type_mismatch"), field.key, QStringLiteral("expected a number"));
        continue;
      }
      const double number = value.toDouble();
      if (field.hasMin && number < field.minValue) {
        addIssue(&result, QStringLiteral("out_of_range"), field.key, QStringLiteral("below minimum"));
        continue;
      }
      if (field.hasMax && number > field.maxValue) {
        addIssue(&result, QStringLiteral("out_of_range"), field.key, QStringLiteral("above maximum"));
        continue;
      }
      result.effective.insert(field.key, field.type == AlgorithmParamField::Int ? QVariant(value.toInt()) : value);
      continue;
    }
    if (field.type == AlgorithmParamField::String || field.type == AlgorithmParamField::InList) {
      if (value.userType() != QMetaType::QString && !value.canConvert<QString>()) {
        addIssue(&result, QStringLiteral("type_mismatch"), field.key, QStringLiteral("expected a string"));
        continue;
      }
      if (field.type == AlgorithmParamField::InList && !field.options.isEmpty()) {
        bool found = false;
        for (const AlgorithmParamOption &option : field.options) {
          if (option.value == value) {
            found = true;
            break;
          }
        }
        if (!found) {
          addIssue(&result, QStringLiteral("out_of_range"), field.key, QStringLiteral("value is not in the enum"));
          continue;
        }
      }
      if (field.required && value.toString().isEmpty()) {
        addIssue(&result, QStringLiteral("missing_required"), field.key, QStringLiteral("empty string"));
        continue;
      }
      result.effective.insert(field.key, value.toString());
      continue;
    }
    if (field.type == AlgorithmParamField::StringList) {
      if (value.userType() != QMetaType::QStringList && value.userType() != QMetaType::QVariantList) {
        addIssue(&result, QStringLiteral("type_mismatch"), field.key, QStringLiteral("expected a string list"));
        continue;
      }
      const QStringList items = value.toStringList();
      if (field.required && items.isEmpty()) {
        addIssue(&result, QStringLiteral("missing_required"), field.key, QStringLiteral("empty list"));
        continue;
      }
      bool rejected = false;
      for (const QString &item : items) {
        if (field.options.isEmpty())
          continue;
        bool found = false;
        for (const AlgorithmParamOption &option : field.options) {
          if (option.value.toString() == item) {
            found = true;
            break;
          }
        }
        if (!found) {
          addIssue(&result, QStringLiteral("out_of_range"), field.key, QStringLiteral("list item is not allowed"));
          rejected = true;
          break;
        }
      }
      if (rejected)
        continue;
      result.effective.insert(field.key, items);
      continue;
    }
    result.effective.insert(field.key, value);
  }
  for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
    if (isPort(spec, it.key()) && !result.effective.contains(it.key()))
      result.effective.insert(it.key(), it.value());
  }
  result.ok = result.issues.isEmpty();
  if (result.ok)
    result.code = QStringLiteral("ok");
  return result;
}
