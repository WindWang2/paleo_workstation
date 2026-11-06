// 层：数据
#pragma once
#include "datacatalog.h"
#include <vector>
namespace paleo::data {struct ModelChangeHead;struct ModelChangeItem;}
struct AreaSourceProjectionStats {
  int bootstrapScans=0,cacheBuilds=0,pages=0,items=0,batches=0;
  bool unchanged=false,legacy=false;
};

// Project the work-area model's sources into the separate artifact index without
// reopening, hashing or parsing the original files.
bool registerAreaSources(DataCatalog &catalog, QString *error);
bool registerAreaSourcesWithStats(DataCatalog &catalog,QString *error,AreaSourceProjectionStats *stats);
// Trusted journal after-images; no SDK/current-model reads. Useful to adapters
// replaying a complete committed batch and to deterministic fault tests.
bool applyAreaSourceChangeBatch(DataCatalog &catalog,const paleo::data::ModelChangeHead &head,
    qint64 sequence,const std::vector<paleo::data::ModelChangeItem> &items,QString *error);
bool applyAreaPointMapping(const QString &project, const QString &sourcePath,
                           const QVariantMap &mapping, QString *error);
