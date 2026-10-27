// 层：QGIS 封装
#include "coordinateformat.h"
#include "catalog/datacatalog.h"

#include <QLocale>
#include <QRegularExpression>
#include <cmath>
#include <qgscoordinateformatter.h>
#include <qgscoordinatetransform.h>
#include <qgscoordinateutils.h>
#include <qgsexception.h>
#include <qgsproject.h>
#include <qgsprojectdisplaysettings.h>

namespace paleo::coordinates
{
QgsCoordinateReferenceSystem engineeringGrid()
{
  return QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
}

namespace
{
bool fail(QString *error, const QString &message)
{
  if (error) *error = message;
  return false;
}

bool decimal(const QString &text, double *value)
{
  const QString trimmed = text.trimmed();
  bool ok = false;
  double result = QLocale::c().toDouble(trimmed, &ok);
  if (!ok && trimmed.contains(QLocale().decimalPoint()))
    result = QLocale().toDouble(trimmed, &ok);
  if (!ok || !std::isfinite(result)) return false;
  *value = result;
  return true;
}

bool ordinate(const QString &text, bool easting, const QgsCoordinateReferenceSystem &crs,
              OrdinateFormat format, double *value, QString *error)
{
  const QString input = text.trimmed();
  if (input.isEmpty()) return fail(error, QObject::tr("坐标不能为空。"));
  if (!crs.isGeographic() || format == OrdinateFormat::Decimal)
  {
    bool suffixOk = false, suffixEasting = false;
    QString numeric = input;
    if (input.count(QChar(0x00B0)) == 1) numeric.remove(QChar(0x00B0));
    if (crs.isGeographic())
    {
      const double degrees = QgsCoordinateUtils::degreeToDecimal(numeric, &suffixOk, &suffixEasting);
      if (suffixOk && suffixEasting == easting && std::isfinite(degrees)) *value = degrees;
    }
    if (!suffixOk && !decimal(numeric, value))
      return fail(error, QObject::tr("“%1”不是有效的十进制坐标；经纬度也可带 N/S/E/W 后缀。").arg(input));
  }
  else
  {
    bool ok = false, inputEasting = easting;
    const double result = QgsCoordinateUtils::dmsToDecimal(input, &ok, &inputEasting);
    if (!ok || inputEasting != easting || !std::isfinite(result))
      return fail(error, QObject::tr("“%1”不是有效的%2度分秒坐标，或半球方向与坐标轴不符。")
                          .arg(input, easting ? QObject::tr("经度") : QObject::tr("纬度")));
    *value = result;
  }
  if (crs.isGeographic())
  {
    const double limit = easting ? 180.0 : 90.0;
    if (std::abs(*value) > limit)
      return fail(error, easting ? QObject::tr("经度必须在 -180 到 180 度之间。")
                                 : QObject::tr("纬度必须在 -90 到 90 度之间。"));
  }
  return true;
}
}

QString formatOrdinate(double value, bool easting, const QgsCoordinateReferenceSystem &crs,
                       OrdinateFormat format, int precision)
{
  if (!std::isfinite(value)) return QString();
  QgsCoordinateFormatter::Format qgsFormat = QgsCoordinateFormatter::FormatPair;
  if (crs.isGeographic() && format == OrdinateFormat::DegreesMinutes)
    qgsFormat = QgsCoordinateFormatter::FormatDegreesMinutes;
  else if (crs.isGeographic() && format == OrdinateFormat::DegreesMinutesSeconds)
    qgsFormat = QgsCoordinateFormatter::FormatDegreesMinutesSeconds;
  else if (crs.isGeographic()) qgsFormat = QgsCoordinateFormatter::FormatDecimalDegrees;
  const auto flags = QgsCoordinateFormatter::FlagDegreesUseStringSuffix |
                     QgsCoordinateFormatter::FlagDegreesPadMinutesSeconds;
  return easting ? QgsCoordinateFormatter::formatX(value, qgsFormat, precision, flags)
                 : QgsCoordinateFormatter::formatY(value, qgsFormat, precision, flags);
}

QString formatPoint(const QgsPointXY &point, const QgsCoordinateReferenceSystem &crs,
                    OrdinateFormat format, int precision)
{
  if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return QString();
  if (format == OrdinateFormat::Decimal)
    return QgsCoordinateFormatter::asPair(point.x(), point.y(), precision, Qgis::CoordinateOrder::XY);
  return QStringLiteral("%1%2 %3").arg(formatOrdinate(point.x(), true, crs, format, precision),
                                      QgsCoordinateFormatter::separator(),
                                      formatOrdinate(point.y(), false, crs, format, precision));
}

bool parsePoint(const QString &x, const QString &y, const QgsCoordinateReferenceSystem &crs,
                OrdinateFormat format, QgsPointXY *point, QString *error)
{
  if (error) error->clear();
  if (!crs.isValid()) return fail(error, QObject::tr("请先选择来源坐标系，不能猜测坐标基准。"));
  if (!point) return fail(error, QObject::tr("坐标输出目标不可用。"));
  double xValue = 0.0, yValue = 0.0;
  if (!ordinate(x, true, crs, format, &xValue, error) ||
      !ordinate(y, false, crs, format, &yValue, error)) return false;
  *point = QgsPointXY(xValue, yValue);
  return true;
}

bool transformPoint(const QgsPointXY &point, const QgsCoordinateReferenceSystem &source,
                    const QgsCoordinateReferenceSystem &destination,
                    const QgsCoordinateTransformContext &context,
                    QgsPointXY *result, QString *error)
{
  if (error) error->clear();
  if (!source.isValid() || !destination.isValid())
    return fail(error, QObject::tr("来源和目标坐标系都必须明确。"));
  if (!result || !std::isfinite(point.x()) || !std::isfinite(point.y()))
    return fail(error, QObject::tr("坐标不是有限数值，无法转换。"));
  if (source == destination)
  {
    *result = point;
    return true;
  }
  try
  {
    QgsCoordinateTransform transform(source, destination, context);
    if (!transform.isValid()) return fail(error, QObject::tr("无法建立坐标转换，请检查坐标系和基准操作。"));
    const QgsPointXY transformed = transform.transform(point);
    if (!std::isfinite(transformed.x()) || !std::isfinite(transformed.y()))
      return fail(error, QObject::tr("坐标转换返回了无效位置。"));
    *result = transformed;
    return true;
  }
  catch (const QgsCsException &exception)
  {
    return fail(error, QObject::tr("坐标转换失败：%1").arg(exception.what()));
  }
}

QPair<QString, QString> mapReadoutParts(const QgsPointXY &point,
                                         const QgsCoordinateReferenceSystem &mapCrs,
                                         const QgsCoordinateTransformContext &context,
                                         QgsProject *project)
{
  if (!mapCrs.isValid()) return {QObject::tr("坐标系未设置"), QString()};
  QgsCoordinateReferenceSystem displayCrs = mapCrs;
  if (mapCrs.type() != Qgis::CrsType::Engineering && project &&
      project->displaySettings()->coordinateCrs().isValid())
    displayCrs = project->displaySettings()->coordinateCrs();
  QgsPointXY displayPoint = point;
  if (displayCrs != mapCrs &&
      !transformPoint(point, mapCrs, displayCrs, context, &displayPoint, nullptr))
    return {QObject::tr("坐标转换不可用"), QString()};
  const int precision = project ? QgsCoordinateUtils::calculateCoordinatePrecisionForCrs(displayCrs, project)
                                : QgsCoordinateUtils::calculateCoordinatePrecision(displayCrs);
  QString x, y;
  if (project)
    QgsCoordinateUtils::formatCoordinatePartsForProject(project, displayPoint, displayCrs,
                                                        precision, x, y);
  else
  {
    x = formatOrdinate(displayPoint.x(), true, displayCrs, OrdinateFormat::Decimal, precision);
    y = formatOrdinate(displayPoint.y(), false, displayCrs, OrdinateFormat::Decimal, precision);
  }
  return {x, y};
}

QString mapReadout(const QgsPointXY &point, const QgsCoordinateReferenceSystem &mapCrs,
                   const QgsCoordinateTransformContext &context, QgsProject *project)
{
  const auto parts = mapReadoutParts(point, mapCrs, context, project);
  return parts.second.isEmpty() ? parts.first
                                : parts.first + QgsCoordinateFormatter::separator() + parts.second;
}
}
