// 层：数据
#pragma once
#include "domain/documentsource.h"
#include <QHash>
#include <QThread>

namespace paleo::document {
struct SourceStoreSnapshot {
    QString documentId;
    QHash<QString,QJsonObject> pendingOperations;
};
// Document identity and cross-store operation intents only. Bindings live in
// catalog version metadata so legacy primary links and bindings commit together.
class DocumentSourceStore {
public:
    explicit DocumentSourceStore(const QString &path);
    ~DocumentSourceStore();
    bool load(const QString &documentId,SourceStoreSnapshot &snapshot,QString *error);
    bool ensureIdentity(const QString &documentId,QString *error);
    bool writeOperation(const QString &documentId,const QString &id,const QJsonObject &operation,QString *error);
    bool finishOperation(const QString &documentId,const QString &id,QString *error);
private:
    bool owner(QString *error) const;
    QString m_path,m_connection;
    QThread *m_owner = nullptr;
};
}
