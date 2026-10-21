// 层：数据
#include "facieshierarchy.h"
#include "faciescatalog.h"
#include "faciestaxonomy.h"
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <algorithm>
#include <cmath>
namespace FaciesHierarchy {
QStringList levels() { return {"facies", "sub_facies", "micro_facies"}; }
QString title(const QString &level) {
  if (level == "facies")
    return QObject::tr("相（1 级）");
  if (level == "sub_facies")
    return QObject::tr("亚相（2 级）");
  return QObject::tr("微相（3 级）");
}
bool validThresholds(const ScaleThresholds &thresholds) {
  return std::isfinite(thresholds.faciesToSubfacies) &&
      std::isfinite(thresholds.subfaciesToMicrofacies) &&
      thresholds.faciesToSubfacies > thresholds.subfaciesToMicrofacies &&
      thresholds.subfaciesToMicrofacies > 0;
}
QString productDisplayMode(const QString &kind, const QString &mode) {
  if ((kind == QLatin1String("wells_prediction") || kind == QLatin1String("seismic_prediction")) &&
      (mode.isEmpty() || mode == QLatin1String("auto")))
    return QStringLiteral("micro_facies");
  return mode.isEmpty() ? QStringLiteral("auto") : mode;
}
QString resolveLevel(const QString &mode, double scale, const ScaleThresholds &thresholds) {
  if (levels().contains(mode))
    return mode;
  const auto limits = validThresholds(thresholds) ? thresholds : ScaleThresholds{};
  return scale > limits.faciesToSubfacies   ? "facies"
         : scale > limits.subfaciesToMicrofacies ? "sub_facies"
                              : "micro_facies";
}
QStringList path(const QVariantMap &f) {
  // 缺级词面经 FaciesTaxonomy::place 安放进三级体系（未细分/未归类），
  // 路径三级恒有值——项目里不出现未定义相。
  const auto placed = FaciesTaxonomy::place(f);
  QStringList result{
      placed.value("facies", placed.value("facies_name")).toString().trimmed(),
      placed.value("subfacies").toString().trimmed(),
      placed.value("microfacies").toString().trimmed()};
  if (result.join(QString()).isEmpty())
    result = QStringList{QObject::tr("其他"), QObject::tr("未归类"),
                          QObject::tr("未分类")};
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
  const auto resolvedLevel = levels().contains(level) ? level : QStringLiteral("micro_facies");
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
    const auto f = FaciesTaxonomy::place(v.toMap());
    const auto code = f.value("code");
    known.insert(code.toString());
    // Choose the same representative color even if that leaf is absent.
    const auto k = key(f, resolvedLevel);
    if (!groups.contains(k)) {
      auto row = f;
      row.insert("key", k);
      row.insert("level", resolvedLevel);
      row.insert("name",
                 path(f).value(levels().indexOf(resolvedLevel), path(f).last()));
      const auto hierarchyPath = path(f).mid(0, levels().indexOf(resolvedLevel) + 1);
      row.insert("hierarchy_path", hierarchyPath);
      row.insert("path", hierarchyPath.join(" / "));
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
                          {"level", resolvedLevel},
                          {"color", "#9AA7B4"},
                          {"codes", QVariantList()}};
  return result;
}
QVariantList legendTree(const QVariantList &entries) {
  if (!entries.isEmpty() && !entries.first().toMap().contains("path") &&
      entries.first().toMap().contains("microfacies")) {
    QVariantList codes;
    for (const auto &row : entries) codes << row.toMap().value("code");
    return legendTree(legend(entries, "micro_facies", codes));
  }
  QVariantList result, sorted = entries;
  std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
    return a.toMap().value("path").toString() < b.toMap().value("path").toString();
  });
  QMap<QString, int> positions;
  for (const auto &v : sorted) {
    auto row = v.toMap();
    const auto parts = row.contains("hierarchy_path") ? row.value("hierarchy_path").toStringList()
        : row.value("path").toString().split(" / ", Qt::SkipEmptyParts);
    const auto keyFor = [&parts](int length) {
      return QString::fromUtf8(QJsonDocument::fromVariant(parts.mid(0, length)).toJson(QJsonDocument::Compact));
    };
    for (int i = 0; i + 1 < parts.size(); ++i) {
      const auto key = keyFor(i + 1);
      if (!positions.contains(key)) {
        QVariantMap heading{{"name", parts[i]}, {"depth", i}, {"heading", true},
                            {"key", keyFor(i + 1)}, {"parent_key", i ? keyFor(i) : QString()}};
        positions.insert(key, result.size());
        result << heading;
      }
    }
    row.insert("depth", qMax(0, int(parts.size()) - 1));
    row.insert("parent_key", parts.size() > 1 ? keyFor(parts.size() - 1) : QString());
    if (!parts.isEmpty()) {
      row.insert("name", parts.last());
      positions.insert(keyFor(parts.size()), result.size());
    }
    result << row;
  }
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
