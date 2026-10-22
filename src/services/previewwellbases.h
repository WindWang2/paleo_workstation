// 层：数据
#pragma once
#include <QStringList>
#include <QVector>
class DataCatalog;
struct WellTopRecord;
struct WellTop;
QVector<WellTopRecord> previewWellBases(DataCatalog *catalog, const QString &wellId,
                                      const QVector<WellTop> &current, QStringList *warnings);
