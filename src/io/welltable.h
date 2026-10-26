// 层：数据
#pragma once
#include "../domain/wellrecords.h"
#include <QVariantMap>
struct ImportSession;
struct CatalogVersion;

namespace paleo::io {
struct WellTable
{
  QString type, crs, error;
  QStringList sheets, columns;
  QVariantMap mapping;
  QVector<QStringList> preview;
  QVector<WellHeadRecord> heads;
  QVector<WellTopRecord> tops;
  WellParseReport report;
  QByteArray normalized;
  bool scanLimited = false; // Scan sample only; full validation/counting is deferred.
};
bool isWellTableContainer(const QString &path);
QString wellSourceCrsHint(const QString &text);
bool projectWellCoordinates(QVector<WellHeadRecord> *heads, const QString &source, const QString &destination, QString *error);
WellTable readWellTable(const QString &path, const QVariantMap &mapping = {});
WellTable previewWellTable(const QString &path, const QVariantMap &mapping = {});
// 同一表的显式层底界；使用独立边界记录，MD/TVD 缺失标记保持一致。
QVector<WellTopRecord> readWellTableBases(const QString &path);
QByteArray writeWellLayerTable(const QVector<WellTopRecord> &tops, const QVector<WellTopRecord> &bases);
bool resolveImportedWellDepths(ImportSession &session, QString *error);
struct ValidatedWellInput {
  QVector<WellHeadRecord> heads;
  QVector<WellTopRecord> tops;
  TimeDepthTable td;
  DeviationTable deviation;
  WellParseReport report;
  QByteArray normalized;
};
bool readImportWellInput(const QString &path, const QString &type, const QString &expectedHash,
                        const QVariantMap &mapping, ValidatedWellInput *out, QString *error);
bool storeNormalizedWellTable(ImportSession &session, const CatalogVersion &raw,
                             const QByteArray &text, QString *error);
}
