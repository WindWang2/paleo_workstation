// 层：数据
#include "facieshierarchy.h"
#include "faciescatalog.h"
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <algorithm>
namespace FaciesHierarchy {
QStringList levels() { return {"facies", "sub_facies", "micro_facies"}; }
QString title(const QString &level) {
  if (level == "facies")
    return QObject::tr("相（1 级）");
  if (level == "sub_facies")
    return QObject::tr("亚相（2 级）");
  return QObject::tr("微相（3 级）");
}
QString resolveLevel(const QString &mode, double scale) {
  if (levels().contains(mode))
    return mode;
  // Match paleo_project/main geo-viz-engine PaleoMapCanvas thresholds.
  return scale > 8000000.0   ? "facies"
         : scale > 4000000.0 ? "sub_facies"
                              : "micro_facies";
}
QStringList path(const QVariantMap &f) {
  QStringList raw{
      f.value("facies", f.value("facies_name")).toString().trimmed(),
      f.value("subfacies").toString().trimmed(),
      f.value("microfacies").toString().trimmed()};
  if (raw.join(QString()).isEmpty())
    raw[0] = f.value("name").toString().trimmed();
  QStringList result = raw;
  for (int i = 0; i < 3; ++i) {
    if (!result[i].isEmpty())
      continue;
    for (int j = i - 1; j >= 0; --j)
      if (!raw[j].isEmpty()) {
        result[i] = raw[j];
        break;
      }
    if (result[i].isEmpty())
      for (int j = i + 1; j < 3; ++j)
        if (!raw[j].isEmpty()) {
          result[i] = raw[j];
          break;
        }
    if (result[i].isEmpty())
      result[i] = QObject::tr("未分类");
  }
  return result;
}
QString key(const QVariantMap &f, const QString &level) {
  const int index = levels().indexOf(level);
  return QString::fromUtf8(
      QJsonDocument::fromVariant(path(f).mid(0, index < 0 ? 3 : index + 1))
          .toJson(QJsonDocument::Compact));
}
QVariantList legend(const QVariantList &schema, const QString &level,
                    const QVariantList &codes) {
  QVariantList sorted = schema, result;
  std::sort(sorted.begin(), sorted.end(),
            [](const QVariant &a, const QVariant &b) {
              return a.toMap().value("code").toInt() <
                     b.toMap().value("code").toInt();
            });
  QSet<QString> present;
  for (const auto &c : codes)
    present.insert(c.isNull() ? QString() : c.toString());
  QMap<QString, int> groups;
  QSet<QString> known;
  for (const auto &v : sorted) {
    const auto f = v.toMap();
    const auto code = f.value("code");
    known.insert(code.toString());
    // Choose the same representative color even if that leaf is absent.
    const auto k = key(f, level);
    if (!groups.contains(k)) {
      auto row = f;
      row.insert("key", k);
      row.insert("name",
                 path(f).value(levels().indexOf(level), path(f).last()));
      row.insert("path",
                 path(f).mid(0, levels().indexOf(level) + 1).join(" / "));
      row.insert("codes", QVariantList());
      groups.insert(k, result.size());
      result << row;
    }
    if (present.contains(code.toString())) {
      auto row = result[groups[k]].toMap();
      auto values = row.value("codes").toList();
      values << code;
      row.insert("codes", values);
      result[groups[k]] = row;
    }
  }
  for (int i = result.size() - 1; i >= 0; --i)
    if (result[i].toMap().value("codes").toList().isEmpty())
      result.removeAt(i);
  QMap<QString, int> names;
  for (const auto &v : result)
    ++names[v.toMap().value("name").toString()];
  for (auto &v : result) {
    auto row = v.toMap();
    if (names[row.value("name").toString()] > 1)
      row.insert("name", row.value("path"));
    v = row;
  }
  bool unknown = false;
  for (const auto &c : present)
    unknown |= !known.contains(c);
  if (unknown)
    result << QVariantMap{{"key", "unknown"},
                          {"name", QObject::tr("其他 / 未分类")},
                          {"color", "#9AA7B4"},
                          {"codes", QVariantList()}};
  return result;
}
int reassignedCode(const QVariantList &schema, int oldCode, int targetCode,
                   const QString &level) {
  const int n = levels().indexOf(level);
  if (n < 0 || n == 2)
    return targetCode;
  const auto old = path(FaciesCatalog::find(schema, oldCode));
  const auto target = FaciesCatalog::find(schema, targetCode);
  int best = targetCode, score = -1;
  for (const auto &v : schema) {
    const auto f = v.toMap();
    if (key(f, level) != key(target, level))
      continue;
    const auto p = path(f);
    int match = 0;
    for (int j = n + 1; j < 3 && p[j] == old[j]; ++j)
      ++match;
    const int code = f.value("code").toInt();
    if (match > score || (match == score && code == targetCode)) {
      score = match;
      best = code;
    }
  }
  return best;
}
} // namespace FaciesHierarchy
