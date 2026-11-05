// 层：数据
#pragma once
#include "domain/documentsource.h"
#include <QHash>
class DataCatalog;
namespace paleo::document {
struct SourceProjection {
    QHash<QString,SourceRecord> records;
    QHash<QString,QString> versionKeys;
    QHash<QString,SourceBinding> bindings;
    QHash<QString,SourceBinding> savedBindings;
    QHash<QString,QString> fileRepresentatives; // slot + source key -> sole legacy-link representative.
    QString error;
};
// In-memory catalog projection only: no stat, hashing, model opens or file reads.
SourceProjection projectDocumentSources(const DataCatalog &catalog,const QString &documentId);
}
