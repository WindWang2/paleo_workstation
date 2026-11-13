// 层：功能
#pragma once
#include "capabilityrecovery.h"
#include <atomic>
#include <QSet>
namespace paleo::capability::recovery_detail {
struct ActiveAttempt { std::atomic_bool cancelled{false}; std::shared_ptr<const ProjectWriteClaim> claim; };
std::shared_ptr<ActiveAttempt> acquire(const QString &root, const QString &id);
bool active(const QString &root, const QString &id);
void cancelActive(const QString &root, const QString &id);
void release(const QString &root, const QString &id);
OperationLog::Record authorized(OperationLog &log, const RecoveryHost &host, const QString &scope, const QString &id);
QSet<QString> managedTargets(const OperationLog::Record &record, const QString &root);
ExecuteResult result(const OperationLog::Record &record);
QString store(OperationLog &log, OperationLog::Record &record, OperationLog::Transition why = OperationLog::Transition::Advance);
OperationWorkResult run(const RecoveryHost &host, OperationLog::Record record,
                        OperationWorker worker, OperationInputValidator validator, const std::shared_ptr<ActiveAttempt> &attempt,
                        const std::function<void(const QString &)> &checkpoint);
}
