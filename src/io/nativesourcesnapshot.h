// 层：数据
#pragma once
#include "domain/documentsource.h"
#include <functional>
#include <memory>
class QTemporaryDir;

namespace paleo::document {
struct NativeSourceRelocationRequest {
    QString projectDirectory, candidatePath;
    SourceRecord source;
    QVariantMap originalBinding, recoveryBinding;
};
// The worker writes only a private clone. Existing immutable model DBs are never changed.
class NativeSourceSnapshot {
public:
    ~NativeSourceSnapshot();
    static std::shared_ptr<NativeSourceSnapshot> prepare(const NativeSourceRelocationRequest &request,
        QString *error, const std::function<bool()> &cancelled);
    bool publish(QString *error);
    void retain() { m_retained=true; }
    const QVariantMap &binding() const { return m_binding; }
    const FileStamp &candidateStamp() const { return m_candidateStamp; }
    qint64 verifiedBytes() const { return m_verifiedBytes; }
private:
    NativeSourceSnapshot()=default;
    QString m_project,m_finalRoot,m_database;
    QVariantMap m_binding;
    FileStamp m_candidateStamp,m_databaseStamp;
    qint64 m_verifiedBytes=0;
    bool m_publishedOwned=false,m_retained=false;
    std::unique_ptr<QTemporaryDir> m_stage;
};
}
