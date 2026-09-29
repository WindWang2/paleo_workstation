// 层：视图
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <QPair>

// ui/wellcomposite/topseditor — D3.7 批量 TOPs 导入（CSV/剪贴板）
//
// 解析 name,top[,base] 列文本（逗号/制表符混认），对既有标志层做预校验
// （行级错误 + 同名深度冲突报告）；应用侧在 EditSession::applyBatchMarkers。

namespace WellComposite
{

struct TopsImportRow
{
  QString name;
  double top = 0.0;
  double base = -1.0; // 可选；-1 = 未提供
  int sourceLine = 0; // 原文本行号（1 起）
};

struct TopsImportReport
{
  QVector<TopsImportRow> parsedRows;   // 解析成功的行
  QStringList errors;                  // 解析/校验错误（含行号）
  QStringList conflicts;               // 与既有同名标志层的深度冲突（含行号）
  int validCount() const { return parsedRows.size(); }
  bool hasErrors() const { return !errors.isEmpty(); }
  bool hasConflicts() const { return !conflicts.isEmpty(); }
};

namespace TopsEditor
{

// CSV/TSV 解析：支持表头行（name/top 或 名/顶深 中文表头自动跳过）；
// 每行 name,top[,base]；空行跳过；坏行进 errors。
TopsImportReport parseImportText(const QString &text);

// 预校验：对既有 (name, depth) 标志层集做同名冲突检测。
// existing 形如 (名称, 深度)。
TopsImportReport validateAgainst(const TopsImportReport &parsed,
                                 const QVector<QPair<QString, double>> &existing);

// 冲突报告文本（对话框展示/测试断言）
QString conflictSummary(const TopsImportReport &report);

} // namespace TopsEditor

} // namespace WellComposite
