// 层：视图
#pragma once

#include <QSet>
#include <QString>

#include "workbench/workbenchactioncatalog.h"

// workbenchcommandregistry — 制图工作台命令（MappingWorkbenchPage::commandRequested
// 的 action）显式登记表。runWorkbenchCommand 只受理登记表内动作：未登记动作、
// 或已登记却无分支受理的动作一律按失败回报（此前三族全 Skip 时落到「操作完成」
// 的默认成功文案）。tst_mappingworkbench 校验页面所有按钮动作 ⊆ 登记表。
// C-W1（#983）：登记、处理族、必需能力与副作用分类收敛为单份描述
// workbench/workbenchactioncatalog.h——knownActions 由目录派生，不再平行维护。
namespace paleo::workbenchcmd {

inline const QSet<QString> &knownActions() {
  static const QSet<QString> actions = [] {
    QSet<QString> ids;
    for (const auto &spec : catalog())
      ids.insert(QString::fromLatin1(spec.id));
    return ids;
  }();
  return actions;
}

// 页面内部消化、不经 commandRequested 外发的按钮动作（MappingWorkbenchPage::issue
// 早返回或独立信号）。
inline const QSet<QString> &pageLocalActions() {
  static const QSet<QString> actions = {
      QStringLiteral("historyOlder"), QStringLiteral("historyNewer"),
      QStringLiteral("evidenceGraph"), QStringLiteral("locateEvidence"),
      QStringLiteral("useEvidenceSource"),
  };
  return actions;
}

inline bool isKnownAction(const QString &action) { return knownActions().contains(action); }

} // namespace paleo::workbenchcmd
