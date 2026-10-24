// 层：视图
#pragma once

#include <QSet>
#include <QString>

// workbenchcommandregistry — 制图工作台命令（MappingWorkbenchPage::commandRequested
// 的 action）显式登记表。runWorkbenchCommand 只受理登记表内动作：未登记动作、
// 或已登记却无分支受理的动作一律按失败回报（此前三族全 Skip 时落到「操作完成」
// 的默认成功文案）。tst_mappingworkbench 校验页面所有按钮动作 ⊆ 登记表，并扫描
// 分派源码保证登记表与 `action == "…"` 分支一致。
namespace paleo::workbenchcmd {

inline const QSet<QString> &knownActions() {
  static const QSet<QString> actions = {
      // 编辑流（paleomainwindow_workbench_editflow.cpp）
      QStringLiteral("captureEditEvidence"), QStringLiteral("setEditEvidence"),
      QStringLiteral("checkTopology"), QStringLiteral("exportVector"), QStringLiteral("exportMap"),
      // 查询/编辑/生命周期三族（paleomainwindow_workbench*.cpp）
      QStringLiteral("addEvidence"), QStringLiteral("assignFacies"), QStringLiteral("cancel"),
      QStringLiteral("catalog"), QStringLiteral("compare"), QStringLiteral("compareRecompute"),
      QStringLiteral("compose"), QStringLiteral("consistency"), QStringLiteral("contours"),
      QStringLiteral("copy"), QStringLiteral("delete"), QStringLiteral("displayLevel"),
      QStringLiteral("draw"), QStringLiteral("factor"), QStringLiteral("featureDisplayLevel"),
      QStringLiteral("graph"), QStringLiteral("impactAnalysis"), QStringLiteral("import"),
      QStringLiteral("labels"), QStringLiteral("newMap"), QStringLiteral("pinFaciesLevel"),
      QStringLiteral("polygonize"), QStringLiteral("predict"), QStringLiteral("recompute"),
      QStringLiteral("referencePanel"), QStringLiteral("references"), QStringLiteral("removeEvidence"),
      QStringLiteral("reverseQuery"), QStringLiteral("reviewInterpretation"), QStringLiteral("save"),
      QStringLiteral("schema"), QStringLiteral("selectHierarchy"), QStringLiteral("show"),
      QStringLiteral("updateEvidence"), QStringLiteral("verifyEvidence"),
      QStringLiteral("wellattributes"), QStringLiteral("wellformations"), QStringLiteral("welltracks"),
  };
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
