// 层：数据
#pragma once
#include "areasourceprojection.h"
#include <QHash>
#include <QPointer>
#include <paleo_data/model_changes.h>
#include <memory>

namespace paleo::area_projection {
struct Cache {
  QPointer<DataCatalog> owner;
  QString generation, path;
  quint64 mutationSequence=~quint64(0);
  QHash<qint64,QString> sources,wells,surveys;
};
std::shared_ptr<Cache> cacheFor(DataCatalog &catalog,const QString &generation,AreaSourceProjectionStats *stats);
bool applyItem(DataCatalog &catalog,Cache &cache,const paleo::data::ModelChangeItem &item,qint64 sequence,QString *error);
bool prepareItem(DataCatalog &catalog,Cache &cache,const paleo::data::ModelChangeItem &item,QString *error);
}
bool bootstrapAreaSources(DataCatalog &catalog,const QString &generation,bool allowLegacy,QString *error);
