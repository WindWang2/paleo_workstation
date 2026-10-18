// 层：数据
#include "faciestaxonomy.h"
#include "faciescatalog.h"
#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <algorithm>

namespace {
QVariantMap recoverLegacyAppearanceColumns(QVariantMap entry) {
  // Old seven-column records used name/facies/subfacies/colour/texture
  // positions inconsistently. Repair only the unambiguous stored tuple:
  // microfacies = colour, colour = bundled SVG, texture = empty.
  const auto name = entry.value("name").toString().trimmed();
  const auto colour = entry.value("microfacies").toString();
  const auto texture = entry.value("color").toString();
  if (!name.isEmpty() && !QColor(name).isValid() && QColor(colour).isValid() &&
      !QColor(texture).isValid() && entry.value("texture").toString().trimmed().isEmpty() &&
      !FaciesCatalog::fillPath(texture).isEmpty()) {
    entry.insert("microfacies", name);
    entry.insert("color", colour);
    entry.insert("texture", texture);
  }
  return entry;
}
QStringList path(const QVariantMap &f) {
  return {f.value("facies").toString().trimmed(),
          f.value("subfacies").toString().trimmed(),
          f.value("microfacies").toString().trimmed()};
}
// 内置相级词表：模板未细分的常见沉积相，旧分类的相级词面据此安放，
// 不必为每个相造占位微相条目。
QStringList knownPhases() {
  return {QStringLiteral("三角洲相"), QStringLiteral("河流相"),
          QStringLiteral("湖泊相"),   QStringLiteral("冲积扇相"),
          QStringLiteral("辫状河相"), QStringLiteral("曲流河相"),
          QStringLiteral("浊积相"),   QStringLiteral("滨海相"),
          QStringLiteral("浅海相"),   QStringLiteral("半深海相"),
          QStringLiteral("深海相"),   QStringLiteral("沼泽相")};
}
bool reject(QString *error, const QString &reason) {
  if (error) *error = reason;
  return false;
}
}
namespace FaciesTaxonomy {
QVariantList library() {
  // resourcePath initializes the bundled resource for standalone domain tests.
  QFile file(FaciesCatalog::resourcePath("facies-hierarchy.json"));
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QJsonDocument::fromJson(file.readAll()).toVariant().toList();
}
QVariantList templates() {
  auto result = library();
  QFile file(FaciesCatalog::resourcePath("facies-templates-extra.json"));
  if (file.open(QIODevice::ReadOnly))
    result.append(QJsonDocument::fromJson(file.readAll()).toVariant().toList());
  return result;
}
bool complete(const QVariantMap &leaf) {
  const auto p = path(leaf);
  return std::all_of(p.cbegin(), p.cend(), [](const auto &s) { return !s.isEmpty(); });
}
QVariantMap place(const QVariantMap &source) {
  const auto entry = recoverLegacyAppearanceColumns(source);
  auto p = path(entry);
  if (!p.contains(QString())) {
    auto placed = entry;
    placed.insert("facies", p[0]); placed.insert("subfacies", p[1]);
    placed.insert("microfacies", p[2]);
    if (placed.value("name").toString().trimmed().isEmpty())
      placed.insert("name", p[2]);
    return placed;
  }
  // 深度优先取词面：微相 → 名称 → 亚相 → 相。
  QString label;
  for (const auto &s : p)
    if (!s.isEmpty()) { label = s; break; }
  if (label.isEmpty())
    label = entry.value("name").toString().trimmed();
  const auto known = resolve(library(), label);
  if (!known.isEmpty()) {
    // 词面能解析到微相：信任模板路径。与已有字段的冲突来自平级旧存储，不是父级知识。
    p = path(known);
  } else {
    // 词面停在相级或亚相级：安放到「未细分」，不造子级名。
    QString facies, subfacies;
    for (const auto &v : library()) {
      const auto kp = path(v.toMap());
      if (label == kp[0]) { facies = kp[0]; subfacies = QStringLiteral("未细分"); break; }
      if (label == kp[1]) { facies = kp[0]; subfacies = kp[1]; break; }
    }
    if (facies.isEmpty() && knownPhases().contains(label)) {
      facies = label;
      subfacies = QStringLiteral("未细分");
    }
    if (!facies.isEmpty()) {
      p = QStringList{facies, subfacies, QStringLiteral("未细分")};
    } else if (!p[0].isEmpty()) {
      // 有明确相、缺中间级：补「未细分」，不造子级名。
      for (int i = 1; i < 3; ++i)
        if (p[i].isEmpty()) p[i] = QStringLiteral("未细分");
    } else {
      // 连相都没有：落「其他 / 未归类」，词面保留为微相。
      p = QStringList{QStringLiteral("其他"), QStringLiteral("未归类"),
                      label.isEmpty() ? QStringLiteral("未分类") : label};
    }
  }
  auto placed = entry;
  placed.insert("facies", p[0]); placed.insert("subfacies", p[1]);
  placed.insert("microfacies", p[2]);
  const auto originalName = placed.value("name").toString().trimmed();
  if (!originalName.isEmpty() && originalName != p[2]) {
    auto aliases = placed.value("aliases").toStringList();
    if (!aliases.contains(originalName)) aliases << originalName;
    placed.insert("aliases", aliases);
  }
  placed.insert("name", p[2]);
  return placed;
}
bool upgrade(const QVariantList &source, QVariantList *result, QString *error) {
  QVariantList normalized;
  if (normalize(source, &normalized, error)) {
    // normalize 顺带补齐了可识别条目的模板路径，返回补全后的结果。
    *result = normalized;
    return true;
  }
  if (error) error->clear();
  QVariantList upgraded;
  QSet<QString> used;
  for (const auto &value : source) {
    auto placed = place(value.toMap());
    auto p = path(placed);
    auto key = QString::fromUtf8(
        QJsonDocument::fromVariant(p).toJson(QJsonDocument::Compact));
    if (used.contains(key)) {
      const auto label = placed.value("name").toString().trimmed();
      p = QStringList{QStringLiteral("其他"), QStringLiteral("未归类"),
                      label.isEmpty() ? p[2] : label};
      for (int n = 2; used.contains(key); ++n) {
        p[2] = QStringLiteral("%1 %2").arg(label.isEmpty() ? p[2] : label).arg(n);
        key = QString::fromUtf8(
            QJsonDocument::fromVariant(p).toJson(QJsonDocument::Compact));
      }
      placed.insert("facies", p[0]); placed.insert("subfacies", p[1]);
      placed.insert("microfacies", p[2]);
      placed.insert("name", p[2]);
    }
    used.insert(key);
    upgraded << placed;
  }
  QString reason;
  if (!normalize(upgraded, result, &reason)) {
    if (error) *error = reason;
    return false;
  }
  return true;
}
QVariantMap resolve(const QVariantList &schema, const QString &label, QString *error) {
  QVariantMap found;
  const auto text = label.trimmed();
  if (text.isEmpty()) return {};
  for (const auto &value : schema) {
    const auto f = value.toMap();
    const auto p = path(f);
    if ((text == p[0] || text == p[1]) && text != p[2]) {
      reject(error, QObject::tr("「%1」是相或亚相，预测必须返回具有完整归属的微相").arg(text));
      return {};
    }
    if (text != f.value("name").toString() && text != p[2] &&
        text != p.join(" / ") && !f.value("aliases").toStringList().contains(text)) continue;
    if (!complete(f)) {
      reject(error, QObject::tr("类别「%1」缺少完整的相、亚相、微相归属，请先维护三级分类").arg(text));
      return {};
    }
    if (!found.isEmpty() && found.value("code") != f.value("code")) {
      reject(error, QObject::tr("微相「%1」有多个归属，请使用完整的相 / 亚相 / 微相路径").arg(text));
      return {};
    }
    found = f;
  }
  if (found.isEmpty()) reject(error, QObject::tr("「%1」不是已定义的微相，请在三级分类中补充归属；相和亚相不能作为预测结果").arg(text));
  return found;
}
bool normalize(const QVariantList &source, QVariantList *result, QString *error) {
  QVariantList normalized;
  QSet<int> codes;
  QSet<QString> paths;
  for (const auto &value : source) {
    auto f = recoverLegacyAppearanceColumns(value.toMap());
    auto p = path(f);
    const auto label = f.value("name").toString().trimmed();
    if (p[2].isEmpty() && !p[0].isEmpty() && !p[1].isEmpty()) p[2] = label;
    if (p.contains(QString())) {
      const auto known = resolve(library(), p[2].isEmpty() ? label : p[2]);
      if (!known.isEmpty()) {
        const auto kp = path(known);
        bool compatible = true;
        for (int i = 0; i < 3; ++i) compatible &= p[i].isEmpty() || p[i] == kp[i];
        if (compatible) p = kp;
      }
    }
    if (p.contains(QString()))
      return reject(error, QObject::tr("类别「%1」必须填写相、亚相和微相；旧分类请先补齐归属").arg(label));
    bool validCode = false;
    const int code = f.value("code").toInt(&validCode);
    const auto key = QString::fromUtf8(QJsonDocument::fromVariant(p).toJson(QJsonDocument::Compact));
    if (!validCode || code < 1 || code > 32767 || codes.contains(code) || paths.contains(key))
      return reject(error, QObject::tr("微相编码和完整三级路径均须唯一，编码范围为 1–32767"));
    codes.insert(code); paths.insert(key);
    if (!label.isEmpty() && label != p[2]) {
      auto aliases = f.value("aliases").toStringList();
      if (!aliases.contains(label)) aliases << label;
      f.insert("aliases", aliases);
    }
    f.insert("code", code); f.insert("name", p[2]);
    f.insert("facies", p[0]); f.insert("subfacies", p[1]); f.insert("microfacies", p[2]);
    normalized << f;
  }
  if (normalized.isEmpty()) return reject(error, QObject::tr("请至少定义一个具有完整归属的微相"));
  *result = normalized;
  return true;
}
bool extend(const QVariantList &schema, const QStringList &labels,
            QVariantList *result, QString *error) {
  QVariantList out;
  if (!normalize(schema.isEmpty() ? library() : schema, &out, error)) return false;
  QStringList sorted = labels;
  sorted.removeDuplicates(); sorted.sort();
  int next = 1;
  for (const auto &v : out) next = std::max(next, v.toMap().value("code").toInt() + 1);
  for (const auto &label : sorted) {
    QString reason;
    if (!resolve(out, label, &reason).isEmpty()) continue;
    // A declared but ambiguous label cannot be resolved through a template.
    bool declared = false;
    for (const auto &v : out) {
      const auto f = v.toMap();
      declared |= f.value("name") == label || f.value("microfacies") == label ||
                  f.value("aliases").toStringList().contains(label);
    }
    if (declared) return reject(error, reason);
    auto f = resolve(library(), label, &reason);
    if (f.isEmpty()) return reject(error, reason);
    // Do not append the same path under another alias/code.
    bool exists = false;
    for (const auto &v : out) exists |= path(v.toMap()) == path(f);
    if (exists) return reject(error, QObject::tr("微相「%1」的别名未在项目分类中登记，请维护该类别的别名").arg(label));
    if (next > 32767) return reject(error, QObject::tr("微相编码已用尽"));
    f.insert("code", next++); out << f;
  }
  *result = out;
  return true;
}
} // namespace FaciesTaxonomy
