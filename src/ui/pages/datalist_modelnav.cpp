// 层：视图
#include "datalist.h"
#include "datalist_navspec_internal.h"
#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <QHash>
#include <cmath>

namespace {
QString utf8(const std::string &value) { return QString::fromUtf8(value); }
QString fileNameOf(const std::string &path) { return QFileInfo(utf8(path)).fileName(); }
}

bool DataListPanel::appendAreaModelWellSeismic(QVector<NavNodeSpec> &tops) const {
  DataCatalog *catalog = m_doc ? m_doc->catalog() : nullptr;
  if (!catalog) return false;
  const QString root = areaModelDirectory(catalog->projectDir());
  if (!QFileInfo::exists(root + QStringLiteral("/data.sqlite"))) return false;
  paleo::data::DataService service;
  std::string error;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), error)) return false;
  // 打开工程只取井名和井口坐标。曲线、分层在双击井道图时再按这口井读取。
  std::vector<paleo::data::WellInfoRecord> wells;
  std::vector<paleo::data::DataEntry> volumes, horizons;
  if (!service.listWellInfos(wells, error)
      || !service.listData("volumes", 0, volumes, error)
      || !service.listData("horizons", 0, horizons, error))
    return false;
  if (wells.empty() && volumes.empty() && horizons.empty()) return false;

  QHash<qlonglong, QString> assetBySource;
  if (!volumes.empty() || !horizons.empty()) {
    for (const CatalogVersion &version : catalog->versions()) {
      const qlonglong id = version.extra.value(QStringLiteral("areaModelSourceId")).toLongLong();
      if (id) assetBySource.insert(id, version.assetId);
    }
  }

  NavNodeSpec wellRoot;
  wellRoot.text0 = tr("测井 (%1 井)").arg(int(wells.size()));
  wellRoot.text1 = tr("数据基座：双击井名打开井道图");
  wellRoot.type = QStringLiteral("category");
  wellRoot.icon = QStringLiteral("mIconPointLayer.svg");
  for (const auto &well : wells) {
    NavNodeSpec item;
    item.role4 = well.id;
    item.type = QStringLiteral("well");
    item.icon = QStringLiteral("mIconPointLayer.svg");
    item.text0 = utf8(well.value.wellName);
    item.tooltip0 = tr("双击打开这口井的井道图");
    if (std::isfinite(well.value.wellheadX) && std::isfinite(well.value.wellheadY)) {
      item.text1 = QStringLiteral("X: %1, Y: %2")
                       .arg(QString::number(well.value.wellheadX, 'f', 1))
                       .arg(QString::number(well.value.wellheadY, 'f', 1));
      item.monoDetail = true;
    } else {
      item.text1 = item.tooltip0;
    }
    wellRoot.children.append(item);
  }
  tops.append(wellRoot);

  NavNodeSpec seismicRoot;
  seismicRoot.text0 = tr("地震 (%1)").arg(int(volumes.size() + horizons.size()));
  seismicRoot.text1 = tr("数据基座：地震体 / 层位");
  seismicRoot.type = QStringLiteral("category");
  seismicRoot.icon = QStringLiteral("mIconPolygonLayer.svg");
  for (const auto &entry : volumes) {
    paleo::data::SeismicVolume volume;
    std::string volumeError;
    const bool haveVolume = service.readVolume(entry.id, volume, volumeError);
    NavNodeSpec item;
    item.text0 = haveVolume && !volume.volumeName.empty() ? utf8(volume.volumeName) : utf8(entry.name);
    item.id = assetBySource.value(entry.sourceId);
    item.type = QStringLiteral("seismic_volume");
    item.icon = QStringLiteral("mIconPolygonLayer.svg");
    if (haveVolume && volume.geometry.traceCount > 0 && volume.geometry.inlineMax > volume.geometry.inlineMin) {
      const auto &geometry = volume.geometry;
      const auto &sampling = volume.sampling;
      item.text1 = QStringLiteral("Inline %1–%2 · Xline %3–%4 · %5%6")
                       .arg(geometry.inlineMin).arg(geometry.inlineMax)
                       .arg(geometry.crosslineMin).arg(geometry.crosslineMax)
                       .arg(sampling.sampleInterval, 0, 'g', 6)
                       .arg(utf8(sampling.timeUnit));
      if (!item.id.isEmpty()) {
        NavNodeSpec inl;
        inl.text0 = tr("Inline 剖面 (%1–%2)").arg(geometry.inlineMin).arg(geometry.inlineMax);
        inl.text1 = tr("双击预览剖面");
        inl.id = item.id; inl.type = QStringLiteral("seismic_line"); inl.sub = QStringLiteral("inline");
        inl.icon = QStringLiteral("mIconLineLayer.svg");
        NavNodeSpec xl = inl;
        xl.text0 = tr("Crossline 剖面 (%1–%2)").arg(geometry.crosslineMin).arg(geometry.crosslineMax);
        xl.sub = QStringLiteral("crossline");
        item.children.append(inl);
        item.children.append(xl);
        item.expanded = true;
      }
    } else {
      item.text1 = fileNameOf(entry.detail);
    }
    seismicRoot.children.append(item);
  }
  NavNodeSpec horizonRoot;
  horizonRoot.text0 = tr("层位 (%1)").arg(int(horizons.size()));
  horizonRoot.text1 = tr("数据基座中的解释层位");
  horizonRoot.type = QStringLiteral("category");
  horizonRoot.icon = QStringLiteral("mActionOpenTable.svg");
  for (const auto &entry : horizons) {
    NavNodeSpec item;
    item.text0 = utf8(entry.name);
    item.text1 = tr("%1 点").arg(entry.count);
    item.id = assetBySource.value(entry.sourceId);
    item.type = QStringLiteral("horizon");
    item.icon = QStringLiteral("mActionOpenTable.svg");
    horizonRoot.children.append(item);
  }
  seismicRoot.children.append(horizonRoot);
  tops.append(seismicRoot);
  return true;
}
