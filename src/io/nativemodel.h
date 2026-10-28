// 层：数据
#pragma once
#include "lasdoc.h"
#include "../domain/wellrecords.h"
#include <QString>

class DataCatalog;
struct CatalogVersion;
struct ImportSession;
namespace paleo::dataimport_detail { struct FamilyContext; struct LogInspection; }

namespace paleo::native_model {
// Prepared in the import worker, published with RAW bytes by the existing
// staging/journal transaction. Each version owns an immutable native data project.
bool prepare(ImportSession &session, const CatalogVersion &version,
             const dataimport_detail::FamilyContext &context,
             const dataimport_detail::LogInspection *log, QString *error);
void registerCatalog(const DataCatalog &catalog, const void *owner);
void unregisterCatalog(const void *owner);
enum class Read { Unbound, Ready, Failed };
Read logHeader(const QString &path, LasHeaderInfo &header, QString *error);
Read log(const QString &path, LasHeaderInfo &header, QList<LasCurve> &curves,
         QString *error);
bool hasLog(const QString &path);
QString cacheToken(const QString &path);
Read heads(const QString &path, QVector<WellHeadRecord> &rows, QString *error, WellParseReport *report = nullptr);
Read tops(const QString &path, QVector<WellTopRecord> &rows, QString *error, WellParseReport *report = nullptr);
Read timeDepth(const QString &path, TimeDepthTable &table, QString *error);
Read deviation(const QString &path, DeviationTable &table, QString *error);
}
