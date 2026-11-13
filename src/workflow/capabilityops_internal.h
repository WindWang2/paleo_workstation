// 层：功能
#pragma once
#include <QMutex>
#include <QString>
namespace paleo::capability::operation_detail {
QMutex &mutexFor(const QString &path);
QString key(const QString &documentId, const QString &callerScope, const QString &clientRequestId);
}
