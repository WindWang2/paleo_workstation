#pragma once
#include <QByteArray>
#include <QString>

// io/ — project_area 导入分类器（docs/PROJECT_AREA_PLAN.md §3 表格）。
// 规则移植自 paleo-merged-main/libs/ingest/src/classifier.cpp，只取本工区
// 用到的路径段/扩展名集；.dat 一律不再交给 OGR（§2）。
struct ProjectClassification
{
  QString type;   // 资产类型（well_head / well_log / horizon / seismic / ...）
  QString format; // 小写扩展名
  QString role;   // "input" | "reference"
};

// 纯路径分类。
ProjectClassification classifyProjectPath(const QString &path);

// 带 XML 内容判定：.xml 先看内容（井口或测井；判不出作参考）。
ProjectClassification classifyProjectImport(const QString &path, const QByteArray &content = QByteArray());

// 分类器实际可输出的类型词表（确认表「类型」下拉以此为准——不再硬编码）。
// 「tops」是关联角色不是类型（井分层文件的真实类型名是 well_stratification）；
// 未匹配的 .dat 落 tabular。词表额外收 "reference"——确认表可选的伪类型，
// 后端走辅助实体 + reference 关联（与 unknown/document 同一条路）。
QStringList projectClassifierTypes();
bool isClassifierType(const QString &type); // type ∈ 上面词表

// 阶段 D 固定辅助规则（T22 收窄）：只有 HZ28-6-1 命名的文件锁成参考资料——
// 确认表禁用改类型、后端不理会 override。「参考资料」目录段不再整体锁定：
// 同目录其他文件（含 XML）默认显示「参考」但可改（isDefaultReferencePath），
// 改成井类后后端照常解析，对不上 A1–A20 保持未决。
bool isFixedAuxiliaryPath(const QString &path);

// 「参考资料」目录段命中 → 确认表把井类/未判内容的行默认显示为「参考」
// （可改）。与锁定不同：改动作为 override 送达后端。
bool isDefaultReferencePath(const QString &path);
