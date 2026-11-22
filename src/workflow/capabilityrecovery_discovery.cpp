// 层：功能
// workflow/capabilityrecovery_discovery.cpp — H4 只读发现入口（owner 冻结
// 可信值 → TaskService worker 扫描 → 完成时复核归属）。口径对齐 read()：
// 入口与回调都验证 owner 存活、非空会话、Read 效果和 document/epoch/caller
// 归属；重入回调（taskAdded 同步换绑/销毁）不交付旧摘要。列表服务自带
// Read 门——写权限不足不阻止合法只读发现，也不假设调用方替它做检查。
#include "capabilityrecovery_internal.h"
#include "operationdiscovery.h"
#include "services/paleotaskservice.h"
#include <QFileInfo>
#include <QPointer>
#include <QThread>

namespace paleo::capability {
namespace {
QJsonObject discoveryFailure(const QString &code) {
  return {{QStringLiteral("contract"), QLatin1String(OperationDiscoveryContract)},
          {QStringLiteral("ok"), false}, {QStringLiteral("code"), code}};
}

// 完成时复核：与 currentReadError 同族——宿主身份漂移、工程切换或锚点
// 作用域变化都按独立 code 拒绝，不把旧 worker 的内容并进新上下文。
QString currentDiscoveryError(const RecoveryHost &current, const host::CallContext &expected,
                              const DiscoveryScan &scan) {
  const auto contextError = host::checkContext(current.context, expected, host::Read);
  if (!contextError.isEmpty())
    return contextError;
  if (QFileInfo(current.projectDir).canonicalFilePath() != scan.projectDir)
    return QStringLiteral("stale_session");
  if (current.context.documentId != scan.documentId)
    return QStringLiteral("stale_session");
  return {};
}
} // namespace

PaleoTask *CapabilityRecovery::listOperations(const QString &callerScope, const QJsonObject &arguments,
                                              QObject *subscriber, std::function<void(QJsonObject)> finished,
                                              QString *error) {
  const auto reject = [&](const QString &code) -> PaleoTask * {
    if (error)
      *error = code;
    return nullptr;
  };
  if (QThread::currentThread() != thread())
    return reject(QStringLiteral("owner_thread_required"));
  const auto h = live();
  if (!h.context.open || callerScope.isEmpty() || h.context.documentId.isEmpty())
    return reject(QStringLiteral("document_unavailable"));
  // Read 门：发现不需要写权限；效果被拒（含只读文档之外的越权）如实失败。
  const auto contextError = host::checkContext(h.context, h.context, host::Read);
  if (!contextError.isEmpty())
    return reject(contextError);
  if (!m_tasks || !subscriber)
    return reject(QStringLiteral("task_service_unavailable"));
  if (!m_discovery)
    m_discovery = std::make_shared<OperationDiscovery>();

  // worker 只拿冻结值：无活 Host/Catalog/GUI 对象；服务经 shared_ptr 保活，
  // Recovery 销毁后 worker 仍能安全收尾（结果不再投递）。
  const DiscoveryScan scan{QFileInfo(h.projectDir).canonicalFilePath(), h.operationDir,
                           h.context.documentId, callerScope, h.writeClaim};
  const auto expected = h.context;
  const auto service = m_discovery;
  auto output = std::make_shared<QJsonObject>(discoveryFailure(QStringLiteral("discovery_unavailable")));
  // 守卫先于 start() 构造（read() 同款）：taskAdded 可能同步销毁订阅者或
  // 关闭 owner，事后构造 QPointer 本身就是对悬空指针的解引用。
  const QPointer<CapabilityRecovery> guard(this);
  const QPointer<QObject> listener(subscriber);
  auto *task = m_tasks->start(QStringLiteral("List scoped operations"),
      [scan, arguments, service, output](PaleoTask *task) {
        *output = service->query(scan, arguments, [task] { return task->cancelRequested(); });
        return output->value("ok").toBool() ? QString() : output->value("code").toString();
      });
  if (!guard || !listener) {
    if (task)
      task->requestCancel();
    return task;
  }
  QObject::connect(task, &PaleoTask::finished, guard.data(),
      [guard, task = QPointer<PaleoTask>(task), listener, expected, scan, output,
       finished = std::move(finished), service] {
        if (!listener)
          return;
        auto code = guard ? currentDiscoveryError(guard->live(), expected, scan) : QStringLiteral("document_unavailable");
        if (code.isEmpty() && guard->m_discovery != service)
          code = QStringLiteral("stale_session");
        // 取消晚于 worker 最后一次自查时同样不交付（read() 同款窄窗防线）。
        if (code.isEmpty() && (!task || task->cancelRequested()))
          code = QStringLiteral("cancelled");
        if (!code.isEmpty())
          *output = discoveryFailure(code);
        if (finished)
          finished(*output);
      }, Qt::SingleShotConnection); // 释放冻结扫描值与页数据。
  return task;
}

} // namespace paleo::capability
