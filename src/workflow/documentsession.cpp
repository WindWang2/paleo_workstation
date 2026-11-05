// 层：功能
#include "documentsession_internal.h"
#include "catalog/datacatalog.h"
#include "metadata/documentsourcestore.h"
#include "metadata/paleoprojectstore.h"
#include <QThread>
#include <QUuid>
#include <algorithm>

namespace paleo::document {
DocumentSession::DocumentSession(PaleoProjectStore *store,PaleoTaskService *tasks,QObject *parent)
    :QObject(parent),m_projectStore(store),m_tasks(tasks) {
    qRegisterMetaType<SourceRef>();qRegisterMetaType<SourceState>();qRegisterMetaType<SourceChangeSet>();qRegisterMetaType<SourceCommandResult>();
}
DocumentSession::~DocumentSession() {close();}
bool DocumentSession::bind(DataCatalog *catalog,const QString &metaPath,const QString &documentId,bool readOnly,QString *error) {
    close();
    if(!catalog || !catalog->isOpen() || metaPath.trimmed().isEmpty() || catalog->thread()!=thread() || QThread::currentThread()!=thread()) {
        m_loadError=QStringLiteral("来源文档需要同线程已打开的目录");if(error)*error=m_loadError;return false;
    }
    auto store=std::make_unique<DocumentSourceStore>(metaPath);SourceStoreSnapshot snapshot;
    if(!store->load(documentId,snapshot,&m_loadError)) {if(error)*error=m_loadError;return false;}
    m_documentId=snapshot.documentId.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):snapshot.documentId;
    m_sourceStore=std::move(store);m_operations=std::move(snapshot.pendingOperations);
    m_revision=catalog->catalogRevision();m_catalog=catalog;m_projectDir=catalog->projectDir();m_readOnly=readOnly;
    m_inspection=std::make_unique<paleo::jobs::JobRunner<SourceJob>>(this);m_inspection->setTaskService(m_tasks);
    m_relocation=std::make_unique<paleo::jobs::JobRunner<SourceJob>>(this);m_relocation->setTaskService(m_tasks);
    connect(catalog,&DataCatalog::changed,this,&DocumentSession::invalidate);
    emit sessionChanged(m_epoch);if(error)error->clear();return true;
}
void DocumentSession::close() {
    ++m_epoch;m_inspection.reset();m_relocation.reset();
    if(m_catalog) disconnect(m_catalog,nullptr,this,nullptr);
    m_catalog=nullptr;m_sourceStore.reset();m_operations.clear();m_observations.clear();m_projection={};
    m_projectionSequence=~quint64(0);m_revision=0;m_documentId.clear();m_projectDir.clear();m_loadError.clear();m_readOnly=false;
    m_bindingProjectionRevision=~quint64(0);m_effectiveBindings.clear();m_resourceBindingIds.clear();
    emit sessionChanged(m_epoch);
}
bool DocumentSession::isOpen() const {return m_catalog && m_catalog->isOpen() && m_sourceStore;}
bool DocumentSession::readOnly() const {
    return !isOpen() || m_readOnly || m_catalog->refusesWrites() || (m_projectStore && m_projectStore->isReadOnly());
}
void DocumentSession::invalidate() {
    if(m_catalog)m_revision=m_catalog->catalogRevision();
    m_projectionSequence=~quint64(0);m_bindingProjectionRevision=~quint64(0);m_observations.clear();emit sourcesInvalidated();
}
void DocumentSession::project() const {
    Q_ASSERT(QThread::currentThread()==thread());
    if(!isOpen()) return;
    if(m_projectionSequence!=m_catalog->mutationSeq()) {
        m_projection=projectDocumentSources(*m_catalog,m_documentId);m_projectionSequence=m_catalog->mutationSeq();
        m_bindingProjectionRevision=~quint64(0);
    }
    if(m_bindingProjectionRevision==m_revision) return;
    m_effectiveBindings=m_projection.bindings;
    QHash<QString,QString> legacySlots;
    for(const auto &binding:m_projection.bindings) legacySlots.insert(binding.slotKey()+binding.source.key(),binding.id);
    for(auto it=m_projection.savedBindings.cbegin();it!=m_projection.savedBindings.cend();++it) {
        auto binding=it.value();
        // Default-scope catalog links own primary selection for old and new consumers.
        const auto legacy=legacySlots.value(binding.slotKey()+binding.source.key());
        if(binding.scope.isEmpty() && binding.locator.kind==LocatorKind::File) {
            binding.primary=binding.decision==BindingDecision::Confirmed && !legacy.isEmpty() && m_projection.bindings.value(legacy).primary;
            const auto current=m_projection.bindings.constFind(binding.id);
            if(current!=m_projection.bindings.cend() && current->source!=binding.source && binding.decision==BindingDecision::Confirmed)
                binding.decision=BindingDecision::Unresolved; // Keep the selected revision; never retarget it silently.
            if(!legacy.isEmpty() && binding.id!=legacy) m_effectiveBindings.remove(legacy);
        }
        m_effectiveBindings.insert(it.key(),binding);
    }
    m_resourceBindingIds.clear();
    for(const auto &binding:m_effectiveBindings) m_resourceBindingIds.insert(binding.resourceKey(),binding.id);
    m_bindingProjectionRevision=m_revision;
}
QVector<SourceRecord> DocumentSession::sources() const {
    project();QVector<SourceRecord> records;records.reserve(m_projection.records.size());
    auto keys=m_projection.records.keys();std::sort(keys.begin(),keys.end());
    for(const auto &key:keys) records.append(m_projection.records.value(key));return records;
}
SourceRecord DocumentSession::source(const SourceRef &ref) const {
    project();return ref.documentId==m_documentId?m_projection.records.value(ref.key()):SourceRecord{};
}
SourceRecord DocumentSession::sourceForVersion(const QString &versionId) const {
    project();return m_projection.records.value(m_projection.versionKeys.value(versionId));
}
const QHash<QString,SourceBinding> &DocumentSession::effectiveBindings() const {
    project();return m_effectiveBindings;
}
QVector<SourceBinding> DocumentSession::bindings(const QString &kind,const QString &id,const QString &role,const QString &scope,bool includeRejected) const {
    QVector<SourceBinding> result;project();SourceBinding resource;resource.resourceKind=kind;resource.resourceId=id;
    const auto ids=m_resourceBindingIds.values(resource.resourceKey());
    for(const auto &bindingId:ids) {
        const auto binding=m_effectiveBindings.value(bindingId);
        if(binding.resourceKind==kind && binding.resourceId==id && (role.isEmpty() || binding.role==role) &&
           binding.scope==scope && (includeRejected || binding.decision!=BindingDecision::Rejected)) result.append(binding);
    }
    std::sort(result.begin(),result.end(),[](const auto &a,const auto &b){return a.id<b.id;});return result;
}
SourceState DocumentSession::observedState(const SourceRef &ref) const {return m_observations.value(ref.key(),source(ref).state);}
SourceCommand DocumentSession::command(SourceCommand::Kind kind) const {
    SourceCommand value;value.kind=kind;value.expectedEpoch=m_epoch;value.expectedRevision=m_revision;return value;
}
bool DocumentSession::check(const SourceCommand &command,bool write,SourceCommandResult &result) const {
    if(QThread::currentThread()!=thread() || !isOpen()) {result.code="document_unavailable";result.error=QStringLiteral("来源文档未打开或线程不符");return false;}
    project();
    if(!m_projection.error.isEmpty()) {result.code="source_metadata_invalid";result.error=m_projection.error;return false;}
    if(command.expectedEpoch!=m_epoch || command.expectedRevision!=m_revision) {result.code="stale_command";result.error=QStringLiteral("来源文档或关联已变化，请刷新后重试");return false;}
    if(write && readOnly()) {result.code="read_only";result.error=QStringLiteral("来源文档为只读");return false;}
    return true;
}
SourceCommandResult DocumentSession::execute(SourceCommand command) {
    SourceCommandResult result;result.operationId=command.operationId.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):command.operationId;
    command.operationId=result.operationId;result.revision=m_revision;
    if(!check(command,command.kind!=SourceCommand::Kind::Inspect,result)) return result;
    if(command.kind==SourceCommand::Kind::Inspect) return startInspection(command);
    if(command.kind==SourceCommand::Kind::Relocate) return startRelocation(command);
    if(command.kind!=SourceCommand::Kind::RegisterBinding && command.kind!=SourceCommand::Kind::ConfirmBinding &&
       command.kind!=SourceCommand::Kind::SelectPrimary && command.kind!=SourceCommand::Kind::RemoveBinding) {
        result.code="unknown_command";result.error=QStringLiteral("不支持的来源命令");return result;
    }
    return editBindings(command);
}
bool DocumentSession::queuedWrite(const std::function<bool(QString *)> &write,QString *error) {
    if(!m_projectStore) return write(error);
    const auto result=m_projectStore->enqueueWrite([&]{QString why;const bool ok=write(&why);return PaleoProjectStore::WriteResult{ok,why};});
    if(error)*error=result.error;return result.ok;
}
}
