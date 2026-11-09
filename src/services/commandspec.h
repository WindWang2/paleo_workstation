// 层：数据
// services/commandspec.h — 统一命令注册表（UI 重设计 U0，docs/designs/ui-command-registry.md）。
//
// 纯数据：登记「有哪些命令、属于哪个工作流阶段、能从哪些入口调、计算类引用
// 哪条 CapabilitySpec」。不建 QAction、不执行（执行在 workflow/commanddispatcher，
// 绑定在 ui/commandactionbinder）。与 ShortcutRegistry 同族：无 QtWidgets，可脱离主窗单测。
//
// 计算类命令（Compute）的 commandId 与 capabilityId 同名，参数默认值仍只在
// AlgorithmSpec；本表不复制参数。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace paleo::command {

// 前四类与 CapabilityKind 同义；View = 只开/切视图或面板，不写文档。
enum class CommandKind { Query, Command, Compute, Workflow, View };

// 入口位或。Agent 位只允许 exposure == Callable 的能力（validate() 断言）。
enum CommandSurface : int {
  SurfaceNone = 0,
  SurfaceRibbon = 1,
  SurfaceLocator = 2,      // Ctrl+K
  SurfaceContextMenu = 4,
  SurfaceShortcut = 8,
  SurfaceAgent = 16,
  SurfaceMenu = 32,        // 文件菜单 / 快速访问（U4 Backstage 前的过渡入口）
};

struct CommandSpec {
  QString commandId;       // "constraint.idw" / "view.open.data" / "data.import.folder"
  int contractVersion = 1;
  CommandKind kind = CommandKind::Command;
  QString capabilityId;    // Compute/部分 Command 引用 CapabilityRegistry；View 为空
  const char *title = "";  // 中文源串（PaleoCommands 上下文，视图层 translate）
  const char *tooltip = "";
  QString stageId;         // project|data|strat|well|seismic|compose|release|view|map|edit
  QString ribbonGroup;     // 例 "单井相分析/预测运行"；不在 ribbon 上为空
  QStringList requiredContext; // project | activeHorizon | activeDocument:map | selection:well …
  QStringList docKinds;    // 适用的文档类型（U2 上下文页签用；U0 多为空）
  QString effect;          // none | stage | publish | export | ui
  int surfaces = SurfaceNone;
  QString shortcutId;      // 指向 ShortcutRegistry（空 = 无快捷键）

  bool isValid() const { return !commandId.isEmpty(); }
  bool hasSurface(CommandSurface s) const { return (surfaces & s) != 0; }
};

// 由宿主注入，Agent 不可伪造（dispatcher 只信宿主给的这份）。
struct CommandContext {
  QString documentId;      // 工程文档身份（project.paleo）
  QString activeDocId;     // U2 文档宿主前恒空
  QString activeViewId;    // 当前中央视图 / 页 id
  QString horizon;         // 当前层位（SelectionContext::activeHorizon）
  QString unitId;          // 当前分析单元（UnitSelection 的稳定 id；U0 只透传字符串）
  QStringList selectionIds; // 业务身份，非 QGIS FID
  QString callerScope = QStringLiteral("ui"); // ui | batch | agent:<session> | test
  QString clientRequestId;

  bool isAgent() const { return callerScope.startsWith(QLatin1String("agent")); }
};

// 统一的可用态：禁用必带原因（§35）。
struct CommandState {
  bool enabled = false;
  QString reason;
  bool checked = false;
  bool checkable = false;
};

QString commandKindName(CommandKind kind);

class CommandRegistry {
public:
  static QVector<CommandSpec> all();
  // 找不到返回 commandId 为空的条目。
  static CommandSpec spec(const QString &commandId, int contractVersion = 1);
  static bool contains(const QString &commandId);
  static QVector<CommandSpec> forStage(const QString &stageId);
  static QVector<CommandSpec> forDocKind(const QString &docKind);
  static QVector<CommandSpec> forSurface(CommandSurface surface);
  // capabilities.list 的候选：surfaces 含 Agent 的命令 id。
  static QStringList agentCommandIds();
  // 表自检：id 唯一/非空、title 非空、stage 在词表内、Compute 必带 capabilityId
  // 且能在 CapabilityRegistry 解析、Agent 位只给 Callable 能力、shortcutId 不重复。
  // 空 = 健康。
  static QStringList validate();
  static QStringList knownStages();
};

} // namespace paleo::command
