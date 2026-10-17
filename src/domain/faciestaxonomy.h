// 层：数据
#pragma once
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// 一个编码唯一标识一条完整的 相 → 亚相 → 微相 路径。项目分类优先于模板。
namespace FaciesTaxonomy {
QVariantList library();
bool complete(const QVariantMap &leaf);
// 仅补全已知微相或有明确父级的自定义微相；不把父级名称复制为子级。
bool normalize(const QVariantList &source, QVariantList *result, QString *error = nullptr);
// 支持完整路径和明确别名；同名不同归属必须使用完整路径，不取首个。
QVariantMap resolve(const QVariantList &schema, const QString &label, QString *error = nullptr);
bool extend(const QVariantList &schema, const QStringList &labels,
            QVariantList *result, QString *error = nullptr);
} // namespace FaciesTaxonomy
