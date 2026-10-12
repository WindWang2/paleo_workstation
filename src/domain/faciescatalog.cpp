// 层：数据
#include "faciescatalog.h"
#include <QDirIterator>
#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QMap>
#include <QHash>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <limits>
static void initGeology() {
  static const bool initialized = [] {
    Q_INIT_RESOURCE(geology);
    return true;
  }();
  Q_UNUSED(initialized);
}
namespace FaciesCatalog {
QString resourcePath(const QString &texture) {
  initGeology();
  if (texture.isEmpty() || texture.contains("..") || texture.startsWith('/'))
    return {};
  const auto path = QStringLiteral(":/geology/") + texture;
  return QFile::exists(path) ? path : QString();
}
QString fillPath(const QString &texture) {
  if (texture.isEmpty() || texture.contains("..") || texture.startsWith('/'))
    return {};
  if (texture.endsWith(QStringLiteral("_fill.svg")))
    return resourcePath(texture);
  return resourcePath(QString(texture).replace(QStringLiteral(".svg"),
                                               QStringLiteral("_fill.svg")));
}
QVariantList library() {
  static const QVariantList items = [] {
    initGeology();
    QHash<QString, QString> paths;
    QDirIterator it(":/geology", {"*.svg"}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      paths.insert(it.fileName(), it.filePath().mid(10));
    }
    QFile file(":/geology/catalog.json");
    if (!file.open(QIODevice::ReadOnly))
      return QVariantList();
    QVariantList out;
    for (const auto &v :
         QJsonDocument::fromJson(file.readAll()).toVariant().toList()) {
      auto f = v.toMap();
      const auto card = paths.value(f.value("file").toString());
      if (card.isEmpty())
        continue;
      f.insert("icon", card);
      // 可平铺填充变体 <base>_fill.svg：图签卡（带相名的插图）不参与纹理
      // 填充，缺变体即纯色填充、相名走 label 标注。
      const auto base = f.value("file").toString();
      f.insert("texture",
               paths.value(base.endsWith(QStringLiteral("_fill.svg")) ? base
                   : QString(base).replace(QStringLiteral(".svg"), QStringLiteral("_fill.svg"))));
      f.insert("name", f.value("chinese_name"));
      out << f;
    }
    return out;
  }();
  return items;
}
QVariantMap appearance(const QVariantMap &facies) {
  struct Index {
    QHash<QString, QVariantMap> textures, names;
  };
  static const Index index = [] {
    Index out;
    for (const auto &value : library()) {
      const auto item = value.toMap();
      const auto path = fillPath(item.value("texture").toString());
      if (!path.isEmpty()) out.textures.insert(path, item);
      if (!item.value("category").toString().contains(QStringLiteral("相"))) continue;
      out.names.insert(item.value("name").toString(), item);
      for (const auto &alias : item.value("aliases").toStringList()) out.names.insert(alias, item);
    }
    return out;
  }();
  auto result = facies;
  const auto texture = facies.value("texture").toString();
  const auto path = fillPath(texture);
  // SVG 纹理含自己的底色；点、图例和井道使用同一纹理条目的颜色。
  if (!path.isEmpty()) {
    const auto item = index.textures.value(path);
    if (!item.isEmpty()) result.insert("color", item.value("color"));
    return result;
  }
  const QColor color(facies.value("color").toString());
  if (color.isValid() && color != QColor(QStringLiteral("#9AA7B4")))
    return result;
  for (const auto &field : {"microfacies", "name", "subfacies", "facies"}) {
    const auto name = facies.value(field).toString().trimmed();
    if (name.isEmpty()) continue;
    const auto item = index.names.value(name);
    if (!item.isEmpty()) {
      result.insert("color", item.value("color"));
      result.insert("texture", item.value("texture"));
      return result;
    }
  }
  result.insert("color", QStringLiteral("#9AA7B4"));
  return result;
}
QVariantList defaults() {
  QVariantList out;
  for (const auto &filename :
       {"strat_fluvial.svg", "strat_deltaic.svg", "strat_lacustrine.svg"})
    for (const auto &v : library()) {
      auto f = v.toMap();
      if (f.value("file") == filename) {
        f.insert("code", out.size() + 1);
        f.insert("facies", f.value("name"));
        f.insert("subfacies", QString());
        f.insert("microfacies", QString());
        out << f;
        break;
      }
    }
  return out;
}
QVariantMap find(const QVariantList &schema, const QVariant &code) {
  if (!code.isNull())
    for (const auto &v : schema) {
      auto f = v.toMap();
      if (f.value("code").toString() == code.toString())
        return f;
    }
  return {{"code", code},
          {"name", code.isNull()
                       ? QObject::tr("未分类")
                       : QObject::tr("其他类别（%1）").arg(code.toString())},
          {"color", "#9AA7B4"}};
}
QVariantMap attributes(const QVariantList &schema, const QVariant &code) {
  const auto f = find(schema, code);
  return {{"facies_name", f.value("facies").toString().isEmpty()
                              ? f.value("name")
                              : f.value("facies")},
          {"subfacies", f.value("subfacies", QString())},
          {"microfacies", f.value("microfacies", QString())},
          {"facies_label", f.value("name")},
          {"texture", f.value("texture", QString())}};
}
int dominantCode(const QVariantList &intervals, double top, double base) {
  QMap<int, double> thickness;
  for (const auto &v : intervals) {
    const auto r = v.toMap();
    const double t = std::max(top, r.value("top").toDouble());
    const double b = std::min(base, r.value("bottom").toDouble());
    if (b > t)
      thickness[r.value("code").toInt()] += b - t;
  }
  int dominant = 0;
  double best = 0;
  for (auto it = thickness.cbegin(); it != thickness.cend(); ++it)
    if (it.value() > best) {
      best = it.value();
      dominant = it.key();
    }
  return dominant;
}
QVariantList recodeIntervals(const QVariantList &intervals, int code,
                             double top, double base) {
  QVariantList out;
  for (const auto &v : intervals) {
    const auto r = v.toMap();
    const double t = r.value("top").toDouble(), b = r.value("bottom").toDouble();
    QList<double> cuts{t};
    for (double edge : {top, base})
      if (edge > t && edge < b)
        cuts << edge;
    cuts << b;
    for (qsizetype i = 0; i + 1 < cuts.size(); ++i) {
      auto part = r;
      part.insert("top", cuts[i]);
      part.insert("bottom", cuts[i + 1]);
      if (cuts[i] >= top && cuts[i + 1] <= base)
        part.insert("code", code);
      out << part;
    }
  }
  return out;
}
QVariantMap formationPrediction(const QVariantMap &source, const QVariantList &schema,
                                const QString &horizon) {
  auto point = source;
  const QString formation = horizon.isEmpty()
      ? point.value("formation", point.value("horizon")).toString() : horizon;
  double top = 0, base = 0;
  formationRange(point.value("formation_top"), point.value("formation_base"), &top, &base);
  QVariantMap unit;
  const auto stored = point.value("formation_intervals");
  const auto formations = stored.metaType().id() == QMetaType::QVariantList ? stored.toList()
      : QJsonDocument::fromJson(stored.toString().toUtf8()).toVariant().toList();
  for (const auto &v : formations) {
    const auto f = v.toMap();
    if (f.value("name").toString().compare(formation, Qt::CaseInsensitive) != 0) continue;
    unit = f;
    if (!std::isfinite(top) || !std::isfinite(base))
      formationRange(f.value("top"), f.value("bottom"), &top, &base);
    break;
  }
  const bool range = std::isfinite(top) && std::isfinite(base) && base > top;
  const auto code = point.value("facies_code");
  const bool predicted = range && !code.isNull() && code.toInt() > 0 &&
      point.value("prediction_status").toString() != QStringLiteral("未预测");
  const auto attrs = attributes(schema, code);
  const QString label = predicted ? attrs.value("facies_label").toString() : QStringLiteral("未预测");
  for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
    point.insert(a.key(), predicted ? a.value() : QVariant());
  point.insert("facies_label", label);
  point.insert("predicted_facies", predicted ? QVariant(label) : QVariant());
  point.insert("facies_code", predicted ? code : QVariant());
  point.insert("formation", formation);
  point.insert("facies_scope", QStringLiteral("formation"));
  point.insert("result_scope", QStringLiteral("current_formation"));
  point.insert("result_kind", QStringLiteral("formation_prediction"));
  if (range) {
    point.insert("formation_top", top); point.insert("formation_base", base);
    unit.insert("name", formation); unit.insert("top", top); unit.insert("bottom", base);
  } else {
    point.insert("formation_top", QVariant());
    point.insert("formation_base", QVariant());
  }
  if (!predicted) {
    point.insert("prediction_status", QStringLiteral("未预测"));
    if (point.value("prediction_reason").toString().isEmpty())
      point.insert("prediction_reason", range ? QObject::tr("当前地层没有预测相")
                                              : QObject::tr("缺少当前地层的有效顶底深"));
  }
  QVariantList final;
  if (predicted && range) final << QVariantMap{{"top", top}, {"bottom", base}, {"code", code}};
  const auto encode = [](const QVariantList &v) {
    return QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact));
  };
  point.insert("facies_intervals", encode(final));
  point.insert("predicted_intervals", encode(final));
  point.insert("formation_intervals", encode(range ? QVariantList{unit} : QVariantList{}));
  for (const auto &key : {"dominant_facies", "predicted_dominant", "dominant_thickness_md",
      "dominant_ratio", "facies_ratio", "facies_composition", "tie"}) point.remove(key);
  return point;
}

void formationRange(const QVariant &top, const QVariant &base, double *outTop,
                    double *outBase) {
  bool topOk = false, baseOk = false;
  const double t = top.toDouble(&topOk), b = base.toDouble(&baseOk);
  const bool valid = !top.isNull() && !base.isNull() && topOk && baseOk &&
                     std::isfinite(t) && std::isfinite(b) && b > t;
  *outTop = valid ? t : -std::numeric_limits<double>::infinity();
  *outBase = valid ? b : std::numeric_limits<double>::infinity();
}
} // namespace FaciesCatalog
