// 层：数据
#include "areamodel.h"
#include "projectcoordinates.h"

#include <QFileInfo>
#include <paleo_data/service.h>

namespace paleo::area_model {
QString confirmedProjectCrs(const QString &project)
{
  const QString root = directory(project);
  if (!QFileInfo::exists(root + QStringLiteral("/data.sqlite"))) return {};
  paleo::data::DataService service;
  std::string detail;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), detail)) return {};
  paleo::data::ProjectCoordinates info;
  if (!service.readProjectCoordinates(info, detail) || info.coordinateUnit != "m") return {};
  const auto crs = QString::fromUtf8(info.coordinateSystem);
  return paleo::io::isMetricProjectCrs(crs) ? crs : QString();
}
}
