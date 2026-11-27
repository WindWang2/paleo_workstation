// 层：数据
#include "areainventory.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QFileInfo>

namespace paleo::inventory {
std::optional<Area> readAreaInventory(const DataCatalog &catalog) {
  const auto root = area_model::directory(catalog.projectDir());
  if (!QFileInfo::exists(root + "/data.sqlite")) return {};
  data::DataService service;
  std::string why;
  std::vector<data::WellInfoRecord> wells;
  std::vector<data::DataEntry> volumes, horizons;
  QHash<qlonglong, WellDataAvailability> availability;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), why) ||
      !service.listWellInfos(wells, why) || !service.listData("volumes", 0, volumes, why) ||
      !service.listData("horizons", 0, horizons, why) || !readWellDataAvailability(service, availability)) return {};
  Area out;
  for (const auto &well : wells)
    out.wells.append({well.id, QString::fromUtf8(well.value.wellName), well.value.wellheadX,
                      well.value.wellheadY, availability.value(well.id)});
  if (volumes.empty() && horizons.empty()) return out;
  QHash<qlonglong, QString> assets;
  for (const auto &version : catalog.versions()) {
    const auto source = version.extra.value("areaModelSourceId").toLongLong();
    if (source) assets.insert(source, version.assetId);
  }
  for (const auto &entry : volumes) {
    Volume row;
    row.asset = assets.value(entry.sourceId);
    row.name = QString::fromUtf8(entry.name);
    row.fileName = QFileInfo(QString::fromUtf8(entry.detail)).fileName();
    data::SeismicVolume metadata;
    if (service.readVolume(entry.id, metadata, why)) {
      if (!metadata.volumeName.empty()) row.name = QString::fromUtf8(metadata.volumeName);
      const auto &geometry = metadata.geometry;
      row.inlineMin = geometry.inlineMin; row.inlineMax = geometry.inlineMax;
      row.crosslineMin = geometry.crosslineMin; row.crosslineMax = geometry.crosslineMax;
      row.hasLines = geometry.traceCount > 0 && row.inlineMax > row.inlineMin;
      row.interval = metadata.sampling.sampleInterval;
      row.timeUnit = QString::fromUtf8(metadata.sampling.timeUnit);
    }
    out.volumes.append(row);
  }
  for (const auto &entry : horizons)
    out.horizons.append({assets.value(entry.sourceId), QString::fromUtf8(entry.name), entry.count});
  return out;
}
}
