// 层：功能
#pragma once
#include "capabilityrecovery.h"
#include <atomic>
#include <QSet>
namespace paleo::capability::recovery_detail {
struct ActiveAttempt {
  std::atomic_bool cancelled{false};
  std::atomic_bool interrupted{false};
  std::atomic_bool workFinished{false};
  std::shared_ptr<const ProjectWriteClaim> claim;
  std::shared_ptr<const std::atomic_bool> contextInvalidated;
};
std::shared_ptr<ActiveAttempt> acquire(const QString &root, const QString &id);
bool active(const QString &root, const QString &id);
void cancelActive(const QString &root, const QString &id);
void release(const QString &root, const QString &id);
// G4：把磁盘读取状态带出日志层。ok 之外，operation_log_unavailable（整本
// 不可读/损坏）与 operation_record_quarantined（目标记录被隔离）必须能与
// operation_not_found 区分——日志不可读不等于 operation 不存在，被隔离的
// 身份也不得被当作 not found 二次立项。跨 scope/文档探查仍统一呈现
// operation_not_found，不泄露存在性。
struct AuthorizedRecord {
  QString code;
  OperationLog::Record record;
};
AuthorizedRecord authorized(OperationLog &log, const RecoveryHost &host, const QString &scope, const QString &id);
QSet<QString> managedTargets(const OperationLog::Record &record, const QString &root);
ExecuteResult result(const OperationLog::Record &record);
unsigned operationEffects(const OperationLog::Record &record);
QString store(OperationLog &log, OperationLog::Record &record, OperationLog::Transition why = OperationLog::Transition::Advance);
OperationWorkResult run(const RecoveryHost &host, OperationLog::Record record,
                        OperationWorker worker, OperationInputValidator validator, const std::shared_ptr<ActiveAttempt> &attempt,
                        const std::function<void(const QString &)> &checkpoint);
}
