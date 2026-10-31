#pragma once
// #740：影响分析 / 重算对比后台任务终态的呈现文案（纯函数，可单测）。
// 主窗口只负责把 ImpactOutcome 投递到状态栏、工作台页内持久提示与
// ErrorHub 历史；文案组装与成败判定集中在此，脱离 PaleoMainWindow。
#include <QString>
#include <QVariantMap>

namespace paleo::workbench {

struct ImpactOutcome {
  bool ok = false;
  QString title;   // 历史条目标题（短）
  QString message; // 完整结论（页内持久提示 / 状态栏 / 历史正文）
};

ImpactOutcome describeImpactOutcome(const QVariantMap &report,
                                    const QString &error);
ImpactOutcome describeCompareOutcome(const QVariantMap &report,
                                     const QString &error);

} // namespace paleo::workbench
