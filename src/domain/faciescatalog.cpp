// 层：数据
#include "faciescatalog.h"
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QObject>
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
               paths.value(QString(base).replace(QStringLiteral(".svg"),
                                                 QStringLiteral("_fill.svg"))));
      f.insert("name", f.value("chinese_name"));
      out << f;
    }
    return out;
  }();
  return items;
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
} // namespace FaciesCatalog
