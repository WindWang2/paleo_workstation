// 层：数据
#include "paleomapfacies.h"
#include "faciescatalog.h"
#include "faciestaxonomy.h"
#include <QColor>
#include <QObject>
#include <QSet>
namespace
{
QStringList path(const QVariantMap &row)
{
  return {row.value("facies").toString().trimmed(), row.value("subfacies").toString().trimmed(),
          row.value("microfacies").toString().trimmed()};
}
QVariantList matches(const QVariantList &schema, const QVariantMap &row)
{
  QVariantList found;
  const auto p = path(row);
  const auto label = p[2];
  QSet<QStringList> seen;
  for (const auto &v : schema)
  {
    const auto f = v.toMap();
    const auto candidate = path(f);
    if ((!p[0].isEmpty() && p[0] != candidate[0]) || (!p[1].isEmpty() && p[1] != candidate[1]))
      continue;
    if (label != candidate[2] && label != candidate.join(" / ") && label != candidate.join("/") &&
        label != f.value("name").toString() && !f.value("aliases").toStringList().contains(label))
      continue;
    if (!seen.contains(candidate))
    {
      seen.insert(candidate);
      found << f;
    }
  }
  return found;
}
} // namespace
namespace PaleoMapFacies
{
QVariantMap resolve(const QVariantList &project, const QVariantList &classes, QString *error)
{
  QVariantList schema;
  if (!FaciesTaxonomy::upgrade(project.isEmpty() ? FaciesCatalog::defaults() : project, &schema, error))
    return {};
  const auto templates = FaciesTaxonomy::templates();
  QSet<int> used;
  for (const auto &v : schema)
    used.insert(v.toMap().value("code").toInt());
  QVariantList codes;
  for (const auto &v : classes)
  {
    const auto row = v.toMap();
    const auto p = path(row);
    const auto reject = [&](const QString &why) -> QVariantMap
    {
      if (error)
        *error = why;
      return {};
    };
    if (p[2].isEmpty())
      return reject(QObject::tr("存在空微相，请先补齐该要素的微相归属"));
    auto found = matches(schema, row);
    if (found.isEmpty())
      found = matches(templates, row);
    if (found.size() > 1)
      return reject(QObject::tr("微相「%1」有多个归属，请提供相和亚相字段或使用完整三级路径").arg(p[2]));
    QVariantMap leaf;
    if (!found.isEmpty())
      leaf = found.first().toMap();
    else if (FaciesTaxonomy::complete(row))
    {
      leaf = row;
      leaf.insert("name", p[2]);
      leaf = FaciesCatalog::appearance(leaf);
    }
    else
      return reject(
          QObject::tr("无法确定微相「%1」的相／亚相，请先在工程三级分类中定义，或在图件中补齐父级字段")
              .arg(p[2]));
    // Project identities and appearance win over template codes and source codes.
    bool existing = false;
    for (const auto &entry : schema)
      if (path(entry.toMap()) == path(leaf))
      {
        leaf = entry.toMap();
        existing = true;
        break;
      }
    if (!existing)
    {
      int code = leaf.value("code").toInt();
      if (code < 1 || code > 32767 || used.contains(code))
      {
        code = 1;
        while (used.contains(code))
          ++code;
      }
      if (code > 32767)
        return reject(QObject::tr("微相编码已用尽"));
      leaf.insert("code", code);
      used.insert(code);
      schema << leaf;
    }
    codes << leaf.value("code");
  }
  QVariantList styled;
  for (const auto &entry : schema)
    styled << FaciesCatalog::appearance(entry.toMap());
  QVariantList normalized;
  if (!FaciesTaxonomy::normalize(styled, &normalized, error))
    return {};
  return {{"facies", normalized}, {"codes", codes}};
}
} // namespace PaleoMapFacies
