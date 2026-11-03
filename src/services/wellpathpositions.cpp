// 层：数据
#include "wellpathpositions.h"
#include <ogr_spatialref.h>
#include <cmath>
#include <memory>

void attachTimeDepthPositions(const paleo::data::TimeDepthTable &table,
                             const QString &projectCrs, QVector<TdSample> &rows)
{
  const auto sourceCrs = table.coordinateSystem.empty() ? projectCrs : QString::fromStdString(table.coordinateSystem);
  QString issue;
  OGRSpatialReference source, target;
  source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  if (source.SetFromUserInput(sourceCrs.toUtf8().constData()) != OGRERR_NONE ||
      target.SetFromUserInput(projectCrs.toUtf8().constData()) != OGRERR_NONE || !target.IsProjected())
    issue = QObject::tr("轨迹坐标系无效，未叠加井轨迹");
  const double unit = table.coordinateUnit == "m" ? 1. : table.coordinateUnit == "ft" ? .3048 : qQNaN();
  if (!std::isfinite(unit) || !source.IsProjected())
    issue = QObject::tr("轨迹缺少米或英尺平面坐标，未叠加井轨迹");
  using Transform = std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)>;
  Transform transform(nullptr, OCTDestroyCoordinateTransformation);
  if (issue.isEmpty()) transform.reset(OGRCreateCoordinateTransformation(&source, &target));
  if (issue.isEmpty() && !transform) issue = QObject::tr("轨迹坐标转换失败，未叠加井轨迹");
  const double scale = issue.isEmpty() ? unit / source.GetLinearUnits() : 1.;
  for (qsizetype i = 0; i < rows.size() && size_t(i) < table.depthTimePairs.size(); ++i) {
    auto &row = rows[i]; const auto &point = table.depthTimePairs[size_t(i)];
    row.z = point.z; row.positionSource = QString::fromStdString(table.sourceFile);
    row.positionCrs = projectCrs; row.positionError = issue;
    if (!issue.isEmpty() || !std::isfinite(point.x) || !std::isfinite(point.y)) continue;
    double x = point.x * scale, y = point.y * scale;
    if (transform->Transform(1, &x, &y) && std::isfinite(x) && std::isfinite(y)) {
      row.x = x; row.y = y;
    } else row.positionError = QObject::tr("轨迹坐标转换失败，未叠加井轨迹");
    // Petrel Z without a declared direction/reference is NOT TVD or MD.
    if (!std::isfinite(row.tvd) && table.positionDepthInfo.depthType == "TVD") {
      const auto &u = table.positionDepthInfo.depthUnit;
      if (u == "m" || u == "ft") row.tvd = point.z * (u == "ft" ? .3048 : 1.);
    }
  }
}
