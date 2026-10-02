// 层：功能
#pragma once

#include <QByteArray>
#include <QString>

class ProjectDataFacade;
class DataCatalog;

namespace paleo
{

// goal/well-trajectory：井底位移轨迹线的 GeoJSON FeatureCollection
// （surface→各测斜站→TD 投影，LineString；CRS 同 wells 层的局部网格 WKT）。
// 无任何已决测斜 → 空 QByteArray（调用方不写文件不声明层）；读取失败
// （文件坏/站表坏）→ 空返回 + *error 记因——不静默吞。
QByteArray wellTrajectoriesGeoJson(ProjectDataFacade *data, DataCatalog *catalog,
                                   QString *error);

} // namespace paleo
