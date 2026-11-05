// 层：功能
#pragma once
#include "catalog/documentsourceprojection.h"
#include <QObject>
#include <QPointer>
#include <memory>
#include <functional>
class DataCatalog;
class PaleoProjectStore;
class PaleoTaskService;
namespace paleo::jobs { template<class T> class JobRunner; }
namespace paleo::document {
class DocumentSourceStore;
struct SourceJob;
// One active project, many consumers. Binding performs metadata-only restoration;
// source byte access is explicit and goes through the task service.
class DocumentSession : public QObject {
    Q_OBJECT
public:
    explicit DocumentSession(PaleoProjectStore *store,PaleoTaskService *tasks,QObject *parent=nullptr);
    ~DocumentSession() override;
    bool bind(DataCatalog *catalog,const QString &metaPath,const QString &documentId={},bool readOnly=false,QString *error=nullptr);
    void close();
    bool isOpen() const;
    bool readOnly() const;
    quint64 epoch() const {return m_epoch;}
    quint64 revision() const {return m_revision;}
    QString documentId() const {return m_documentId;}
    QString loadError() const {project();return m_loadError.isEmpty()?m_projection.error:m_loadError;}
    bool recoveryRequired() const {return !m_operations.isEmpty();}
    QStringList pendingOperations() const {return m_operations.keys();}
    QVector<SourceRecord> sources() const;
    SourceRecord source(const SourceRef &ref) const;
    SourceRecord sourceForVersion(const QString &versionId) const;
    QVector<SourceBinding> bindings(const QString &resourceKind,const QString &resourceId,
                                  const QString &role={},const QString &scope={},bool includeRejected=false) const;
    SourceState observedState(const SourceRef &ref) const;
    SourceCommand command(SourceCommand::Kind kind) const;
    SourceCommandResult execute(SourceCommand command);
signals:
    void sessionChanged(quint64 epoch);
    void sourcesInvalidated();
    void sourcesChanged(const paleo::document::SourceChangeSet &change);
    void sourceObserved(const paleo::document::SourceRef &ref,const paleo::document::SourceState &state);
    void commandFinished(const paleo::document::SourceCommandResult &result);
    void relocationFinished(const QString &oldVersionId,const QString &newVersionId,const QString &error);
private:
    void project() const;
    const QHash<QString,SourceBinding> &effectiveBindings() const;
    bool check(const SourceCommand &command,bool write,SourceCommandResult &result) const;
    SourceCommandResult editBindings(const SourceCommand &command);
    SourceCommandResult startInspection(const SourceCommand &command);
    SourceCommandResult startRelocation(const SourceCommand &command);
    bool commitRelocation(SourceJob &job,QString *error);
    bool queuedWrite(const std::function<bool(QString *)> &write,QString *error);
    void invalidate();
    QPointer<DataCatalog> m_catalog;
    PaleoProjectStore *m_projectStore=nullptr;
    PaleoTaskService *m_tasks=nullptr;
    QString m_documentId,m_projectDir,m_loadError;
    bool m_readOnly=false;
    quint64 m_epoch=0,m_revision=0;
    mutable quint64 m_projectionSequence=~quint64(0);
    mutable SourceProjection m_projection;
    mutable quint64 m_bindingProjectionRevision=~quint64(0);
    mutable QHash<QString,SourceBinding> m_effectiveBindings;
    mutable QMultiHash<QString,QString> m_resourceBindingIds;
    QHash<QString,QJsonObject> m_operations;
    QHash<QString,SourceState> m_observations;
    std::unique_ptr<DocumentSourceStore> m_sourceStore;
    std::unique_ptr<paleo::jobs::JobRunner<SourceJob>> m_inspection,m_relocation;
};
}
