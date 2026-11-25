// 层：视图
#include "datalist.h"
#include "datalist_navspec_internal.h"
#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"
#include "../../services/areainventory.h"
#include "../../services/wellreadiness.h"
#include <cmath>

bool DataListPanel::appendAreaModelWellSeismic(QVector<NavNodeSpec> &tops) const {
  DataCatalog *catalog = m_doc ? m_doc->catalog() : nullptr;
  if (!catalog) return false;
  const auto model = paleo::inventory::readAreaInventory(*catalog);
  if (!model || (model->wells.isEmpty() && model->volumes.isEmpty() && model->horizons.isEmpty())) return false;
  const auto &wells = model->wells;
  const auto &volumes = model->volumes;
  const auto &horizons = model->horizons;

  NavNodeSpec wellRoot;
  wellRoot.text0 = tr("测井 (%1 井)").arg(int(wells.size()));
  wellRoot.text1 = tr("双击有井道资料的井打开井道图；其余井保留时深/辅助资料");
  wellRoot.type = QStringLiteral("category");
  wellRoot.icon = QStringLiteral("mIconPointLayer.svg");
  // 数据库存与分析适用性分开：井树只显示资料是否存在。
  for (const auto &well : wells) {
    NavNodeSpec item;
    item.role4 = well.id;
    item.eid = m_doc->entityIdForModelWell(well.id);
    item.type = QStringLiteral("well");
    item.icon = QStringLiteral("mIconPointLayer.svg");
    item.text0 = well.name;
    const auto support=well.availability;
    item.tooltip0 = support.label;
    item.sub = support.tracks ? QStringLiteral("well_tracks") : QStringLiteral("well_auxiliary");
    if (std::isfinite(well.x) && std::isfinite(well.y)) {
      item.text1 = QStringLiteral("X: %1, Y: %2")
                       .arg(QString::number(well.x, 'f', 1))
                       .arg(QString::number(well.y, 'f', 1));
      item.monoDetail = true;
    } else {
      item.text1 = item.tooltip0;
    }
    paleo::WellReadiness inventory;
    inventory.hasCurves=support.curves;inventory.hasTimeDepth=support.timeDepth;
    applyWellReadinessBadge(item,inventory);
    wellRoot.children.append(item);
  }
  tops.append(wellRoot);

  NavNodeSpec seismicRoot;
  seismicRoot.text0 = tr("地震 (%1)").arg(int(volumes.size() + horizons.size()));
  seismicRoot.text1 = tr("数据基座：地震体 / 层位");
  seismicRoot.type = QStringLiteral("category");
  seismicRoot.icon = QStringLiteral("mIconPolygonLayer.svg");
  for (const auto &entry : volumes) {
    NavNodeSpec item;
    item.text0 = entry.name;
    item.id = entry.asset;
    item.type = QStringLiteral("seismic_volume");
    item.icon = QStringLiteral("mIconPolygonLayer.svg");
    if (entry.hasLines) {
      item.text1 = QStringLiteral("Inline %1–%2 · Xline %3–%4 · %5%6")
                       .arg(entry.inlineMin).arg(entry.inlineMax)
                       .arg(entry.crosslineMin).arg(entry.crosslineMax)
                       .arg(entry.interval, 0, 'g', 6)
                       .arg(entry.timeUnit);
      if (!item.id.isEmpty()) {
        NavNodeSpec inl;
        inl.text0 = tr("Inline 剖面 (%1–%2)").arg(entry.inlineMin).arg(entry.inlineMax);
        inl.text1 = tr("双击预览剖面");
        inl.id = item.id; inl.type = QStringLiteral("seismic_line"); inl.sub = QStringLiteral("inline");
        inl.icon = QStringLiteral("mIconLineLayer.svg");
        NavNodeSpec xl = inl;
        xl.text0 = tr("Crossline 剖面 (%1–%2)").arg(entry.crosslineMin).arg(entry.crosslineMax);
        xl.sub = QStringLiteral("crossline");
        item.children.append(inl);
        item.children.append(xl);
        item.expanded = true;
      }
    } else {
      item.text1 = entry.fileName;
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
    item.text0 = entry.name;
    item.text1 = tr("%1 点").arg(entry.pointCount);
    item.id = entry.asset;
    item.type = QStringLiteral("horizon");
    item.icon = QStringLiteral("mActionOpenTable.svg");
    horizonRoot.children.append(item);
  }
  seismicRoot.children.append(horizonRoot);
  tops.append(seismicRoot);
  return true;
}
