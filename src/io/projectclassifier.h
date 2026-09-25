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
