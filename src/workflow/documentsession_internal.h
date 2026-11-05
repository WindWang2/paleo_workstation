// 层：功能
#pragma once
#include "documentsession.h"
#include "services/jobrunner.h"
#include "catalog/datacatalog.h"
#include <paleo_data/source_relocation.h>

namespace paleo::document {
class NativeSourceSnapshot;
struct SourceJob {
    SourceCommand command;
    SourceRecord record;
    CatalogVersion parentVersion;
    SourceState state;
    FileStamp stamp;
    QString error,projectDirectory,newVersionId,journalId,expectedCurrentVersion;
    qint64 verifiedBytes=0;
    bool computed=false;
    bool recovering=false;
    std::shared_ptr<const paleo::data::PreparedSourceRelocation> prepared;
    std::shared_ptr<NativeSourceSnapshot> nativeSnapshot;
    QVariantMap nativeRecoveryBinding;
};
}
