// 层：数据
#pragma once
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// 一个编码唯一标识一条完整的 相 → 亚相 → 微相 路径。项目分类优先于模板。
namespace FaciesTaxonomy {
// Stable default schema; existing codes and historical migration use this set.
QVariantList library();
// Optional templates for explicit selection; never replace the project schema.
QVariantList templates();
bool complete(const QVariantMap &leaf);
// 把单条类别安放进三级体系：微相可识别则采用模板路径；相/亚相级旧类别落
// 「未细分」；无法识别的落「其他 / 未归类」。不复制父级名称伪造子级。
QVariantMap place(const QVariantMap &source);
// 旧分类整表迁移：逐条安放并去重路径；已合规的输入原样返回。
bool upgrade(const QVariantList &source, QVariantList *result, QString *error = nullptr);
// 仅补全已知微相或有明确父级的自定义微相；不把父级名称复制为子级。
bool normalize(const QVariantList &source, QVariantList *result, QString *error = nullptr);
// 支持完整路径和明确别名；同名不同归属必须使用完整路径，不取首个。
QVariantMap resolve(const QVariantList &schema, const QString &label, QString *error = nullptr);
bool extend(const QVariantList &schema, const QStringList &labels,
            QVariantList *result, QString *error = nullptr);
} // namespace FaciesTaxonomy
