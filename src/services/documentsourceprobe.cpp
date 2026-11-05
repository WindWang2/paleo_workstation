// 层：数据
#include "documentsourceprobe.h"
#include "io/sourcefingerprint.h"
#include "fspathutils.h"
#include "Engine/PagedWorkspace.h"
#include <paleo_data/service.h>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <filesystem>

namespace paleo::document {
namespace {
void modelState(const SourceRecord &source,SourceState &state) {
    if(source.modelDirectory.isEmpty()) return;
    paleo::data::DataService service;paleo::data::SourceIndexEntry entry;std::string error;
    if(!service.openProjectReadOnly(source.modelDirectory.toUtf8().toStdString(),error) ||
       !service.readSourceIndexEntry(source.ref.sourceId,entry,error)) {
        state.model=ModelAvailability::Failed;state.code="model_unavailable";state.detail=QString::fromUtf8(error);return;
    }
    if(entry.sha256!=source.fingerprint.stored().toStdString() || !entry.active) {
        state.model=ModelAvailability::Failed;state.code="model_identity_changed";return;
    }
    state.model=entry.status=="parsed"?ModelAvailability::Ready:entry.status=="review"?ModelAvailability::Review:ModelAvailability::Registered;
}
void pagedState(const SourceRecord &source,QFile &file,SourceState &state) {
    const auto prefix=file.read(16);state.bytesRead+=prefix.size();
    if(prefix.size()!=16 || prefix.left(8)!="SF3PWG01") {state.derivative=DerivativeState::Failed;state.code="paged_header_invalid";return;}
    const auto version=qFromLittleEndian<quint32>(prefix.constData()+8);
    const quint32 headerBytes=version==6?256:version==7?qFromLittleEndian<quint32>(prefix.constData()+12):0;
    if(!headerBytes || headerBytes>1024*1024) {state.derivative=DerivativeState::Failed;state.code="paged_header_budget";return;}
    seismic::engine::PagedWorkspaceReader reader;std::string error;
    if(!reader.Open(paleo::toFsPath(source.path),error,true)) {
        state.derivative=DerivativeState::Failed;state.code="paged_header_invalid";state.detail=QString::fromUtf8(error);return;
    }
    state.bytesRead+=16+headerBytes;const auto &info=reader.Info();
    state.parentPath=QString::fromUtf8(info.sourcePath);
    state.derivative=info.complete && !source.path.endsWith(".partial",Qt::CaseInsensitive)?DerivativeState::HeaderComplete:DerivativeState::Incomplete;
    state.code=state.derivative==DerivativeState::Incomplete?"paged_incomplete":"paged_header_complete";
}
}
SourceState inspectDocumentSource(const SourceRecord &source,bool verify,const std::function<bool()> &cancelled) {
    auto state=source.state;state.bytesRead=0;state.observedUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto stopped=[&]{return cancelled && cancelled();};
    if(stopped()) {state.code="cancelled";return state;}
    modelState(source,state);
    const auto stamp=sourceFileStamp(source.path);state.size=stamp.size;state.modifiedMs=stamp.modified;
    if(stamp.size<0) {state.file=FileAvailability::Missing;state.code="source_missing";return state;}
    QFile file(source.path);
    if(!file.open(QIODevice::ReadOnly)) {state.file=FileAvailability::Unreadable;state.detail=file.errorString();return state;}
    state.file=FileAvailability::Present;
    if(source.path.endsWith(".sf3p",Qt::CaseInsensitive) || source.path.endsWith(".sf3p.partial",Qt::CaseInsensitive)) pagedState(source,file,state);
    file.close();
    if(verify && !stopped()) {
        if(!source.fingerprint.valid() || (source.fingerprint.kind==FingerprintKind::SegyQuickV1 &&
           (source.ref.modelStoreId!="area-model" || state.model==ModelAvailability::Failed))) {
            state.verification=Verification::Failed;state.code="verification_identity_unavailable";return state;
        }
        FileStamp verified;qint64 bytes=0;QString error;
        if(verifySourceFingerprint(source.path,source.fingerprint,verified,bytes,&error,cancelled))
            state.verification=source.fingerprint.kind==FingerprintKind::SegyQuickV1?Verification::FastVerified:Verification::HashVerified;
        else {state.verification=stopped()?Verification::Unverified:Verification::Changed;state.detail=error;state.code=stopped()?"cancelled":"source_identity_changed";}
        state.bytesRead+=bytes;
    }
    if(stamp!=sourceFileStamp(source.path)) {state.verification=Verification::Changed;state.code="source_changed_during_read";}
    return state;
}
}
