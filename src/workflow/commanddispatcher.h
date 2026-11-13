// 层：功能
// workflow/commanddispatcher.h — 统一命令分派（UI 重设计 U1）。
//
// Ribbon、文件菜单、Ctrl+K、右键菜单、快捷键与后期 Agent/批处理都经同一个
// CommandDispatcher：先按 CommandRegistry 查登记、再问状态（禁用必带原因）、
// 最后调宿主绑定的处理器。处理器由视图层（ui/commandactionbinder、主窗命令 TU）
// 注入——本层不 include ui/、不碰 QtWidgets。
//
// 计算类命令（CommandKind::Compute）另有 prepare/execute 桥：把命令 + 宿主上下文
// 转成 capability::ExecuteRequest，交 CapabilityGateway（不建第二套执行链）。
#pragma once

#include "capabilitygateway.h"
#include "capabilityhost.h"
#include "../services/commandspec.h"

#include <QHash>
#include <QObject>
#include <QVariantMap>

#include <functional>

namespace paleo::command {

// 准备好的计划：不发布、不执行；execute 前可展示/复制为 JSON（与 Agent 调用同格式）。
struct PreparedPlan {
  bool ok = false;
  QString code;            // 失败码：unknown_command | not_compute | capability_missing |
                           // capability_not_callable | surface_denied | parameter_invalid
  QString commandId;
  QString hostHandle;      // Opaque host ticket; the public fields are not authority.
  CapabilitySpec capability;
  capability::ExecuteRequest request;
  QVariantMap effectiveParameters; // AlgorithmSpec 填默认后的有效参数（只读展示）
};

class CommandDispatcher : public QObject {
  Q_OBJECT
public:
  using Handler = std::function<void(const CommandContext &, const QVariantMap &)>;
  using StateProvider = std::function<CommandState(const CommandContext &)>;
  using Executor = std::function<capability::ExecuteResult(const capability::ExecuteRequest &)>;
  // 宿主补执行身份（operationDir / catalog / projectDir），在 prepare 末尾调用。
  using HostBinder = std::function<void(capability::ExecuteRequest &)>;

  explicit CommandDispatcher(QObject *parent = nullptr);

  // 绑定处理器。未登记的 commandId 拒绝（error 写原因）——入口不能绕过注册表。
  // 重复绑定覆盖旧处理器（workbench 重建镜像时用）。
  bool bind(const QString &commandId, Handler handler, StateProvider state = {}, QString *error = nullptr);
  void unbind(const QString &commandId);
  bool isBound(const QString &commandId) const;
  QStringList boundIds() const;

  // 状态：未登记 / 未接入 / 入口不允许 / 能力不可用 → 禁用并给原因；否则问 StateProvider。
  CommandState state(const QString &commandId, const CommandContext &ctx) const;
  // 触发：state 不放行则不调处理器，reason 写原因并发 commandRejected。
  bool trigger(const QString &commandId, const CommandContext &ctx, const QVariantMap &params = {},
               QString *reason = nullptr);

  // ---- 计算桥 ----
  PreparedPlan prepare(const QString &commandId, const CommandContext &ctx, const QVariantMap &params,
                       const QVariantMap &inputs = {},
                       const QString &publishIntent = QStringLiteral("prepare")) const;
  capability::ExecuteResult execute(const PreparedPlan &plan);
  PaleoTask *start(const PreparedPlan &plan, QObject *receiver, const QString &title,
                   std::function<void(capability::ExecuteResult)> finished, QString *error = nullptr);
  void setExecutor(Executor executor);    // 缺省 capability::execute（测试注入桩）
  void setHostBinder(HostBinder binder);
  void setCapabilityHost(capability::CapabilityHost *host, host::Caller caller = host::Caller::Ui);

  // 宿主上下文（主窗注入；Agent 只能在此之上改 callerScope/clientRequestId）。
  void setContextProvider(std::function<CommandContext()> provider);
  CommandContext currentContext() const;

signals:
  void commandTriggered(const QString &commandId, const QString &callerScope);
  void commandRejected(const QString &commandId, const QString &reason);
  void operationChanged(const QString &operationId, const QString &commandId);
  // View 类命令请求文档宿主打开视图（U2 DocumentTabHost 订阅；U1 无订阅者）。
  void viewRequested(const QString &docId, const QString &viewKind);

private:
  struct Binding {
    Handler handler;
    StateProvider state;
  };
  QHash<QString, Binding> m_bindings;
  Executor m_executor;
  HostBinder m_hostBinder;
  QPointer<capability::CapabilityHost> m_host;
  host::Caller m_hostCaller = host::Caller::Ui;
  bool m_hostRequired = false;
  std::function<CommandContext()> m_context;
};

// 入口面对应的 callerScope 是否允许调这条命令（Agent 位 + Callable 能力）。
bool surfaceAllows(const CommandSpec &spec, const CommandContext &ctx, QString *reason);

} // namespace paleo::command
