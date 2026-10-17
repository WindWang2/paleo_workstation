// 层：数据
#include "faciestaxonomy.h"
#include "faciescatalog.h"
#include <QFile>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <algorithm>

namespace {
QStringList path(const QVariantMap &f) {
  return {f.value("facies").toString().trimmed(),
          f.value("subfacies").toString().trimmed(),
          f.value("microfacies").toString().trimmed()};
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
bool complete(const QVariantMap &leaf) {
  const auto p = path(leaf);
  return std::all_of(p.cbegin(), p.cend(), [](const auto &s) { return !s.isEmpty(); });
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
    auto f = value.toMap();
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
