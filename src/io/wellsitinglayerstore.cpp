// 层：数据
#include "wellsitinglayerstore.h"
#include "gdalreg_internal.h"
#include "../catalog/datacatalog.h"
#include "../domain/wellsitinghole.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QTemporaryDir>

#include <cpl_error.h>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include <cmath>
#include <initializer_list>
#include <memory>
#include <vector>

namespace
{
using Dataset = std::unique_ptr<void, decltype(&GDALClose)>;
using Feature = std::unique_ptr<void, decltype(&OGR_F_Destroy)>;
using Geometry = std::unique_ptr<void, decltype(&OGR_G_DestroyGeometry)>;
using Crs = std::unique_ptr<void, decltype(&OSRDestroySpatialReference)>;

struct Field
{
  const char *name;
  OGRFieldType type;
};

bool execute(GDALDatasetH dataset, const QByteArray &statement)
{
  CPLErrorReset();
  if (const auto result = GDALDatasetExecuteSQL(dataset, statement.constData(), nullptr, nullptr))
    GDALDatasetReleaseResultSet(dataset, result);
  return CPLGetLastErrorType() < CE_Failure;
}

bool fail(QString *error, const QString &path)
{
  if (error)
    *error = QObject::tr("无法写入工程矢量图层 %1：%2")
                 .arg(path, QString::fromUtf8(CPLGetLastErrorMsg()));
  return false;
}

OGRLayerH createLayer(GDALDatasetH dataset, const char *table, OGRwkbGeometryType type,
                      std::initializer_list<Field> fields)
{
  const Crs crs(OSRNewSpatialReference(nullptr), OSRDestroySpatialReference);
  if (OSRSetFromUserInput(crs.get(), DataCatalog::localGridCrsWkt().toUtf8().constData()) != OGRERR_NONE)
    return nullptr;
  const auto layer = GDALDatasetCreateLayer(dataset, table, crs.get(), type, nullptr);
  if (!layer)
    return nullptr;
  for (const Field &field : fields)
  {
    const auto definition = OGR_Fld_Create(field.name, field.type);
    const auto result = OGR_L_CreateField(layer, definition, FALSE);
    OGR_Fld_Destroy(definition);
    if (result != OGRERR_NONE)
      return nullptr;
  }
  return layer;
}

bool matchesSchema(OGRLayerH layer, OGRwkbGeometryType type, std::initializer_list<Field> fields)
{
  if (!layer || OGR_L_GetGeomType(layer) != type)
    return false;
  const auto definition = OGR_L_GetLayerDefn(layer);
  for (const Field &field : fields)
  {
    const int index = OGR_FD_GetFieldIndex(definition, field.name);
    if (index < 0 || OGR_Fld_GetType(OGR_FD_GetFieldDefn(definition, index)) != field.type)
      return false;
  }
  return true;
}

bool clearFeatures(OGRLayerH layer)
{
  CPLErrorReset();
  std::vector<GIntBig> ids;
  while (auto next = OGR_L_GetNextFeature(layer))
  {
    const Feature feature(next, OGR_F_Destroy);
    ids.push_back(OGR_F_GetFID(feature.get()));
  }
  OGR_L_ResetReading(layer);
  if (CPLGetLastErrorType() >= CE_Failure)
    return false;
  for (const auto id : ids)
    if (OGR_L_DeleteFeature(layer, id) != OGRERR_NONE)
      return false;
  return true;
}

template <typename Populate>
bool writeFeatures(const QString &path, const char *table, OGRwkbGeometryType type,
                   std::initializer_list<Field> fields, bool create, Populate populate, QString *error)
{
  paleo::io_detail::ensureGdalRegistered();
  CPLErrorReset();
  const auto filename = path.toUtf8();
  const char *drivers[] = {"GPKG", nullptr};
  const char *creationOptions[] = {"CRS_WKT_EXTENSION=YES", nullptr};
  const auto driver = GDALGetDriverByName("GPKG");
  Dataset dataset(create ? GDALCreate(driver, filename.constData(), 0, 0, 0, GDT_Unknown, creationOptions)
      : GDALOpenEx(filename.constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, drivers, nullptr, nullptr), GDALClose);
  if (!dataset || GDALDatasetStartTransaction(dataset.get(), FALSE) != OGRERR_NONE)
    return fail(error, path);

  const auto layer = create ? createLayer(dataset.get(), table, type, fields)
                            : GDALDatasetGetLayerByName(dataset.get(), table);
  const QByteArray extent = QByteArray("RECOMPUTE EXTENT ON ") + table;
  if (!matchesSchema(layer, type, fields))
  {
    if (error)
      *error = QObject::tr("工程矢量图层的结构不匹配：%1").arg(path);
    GDALDatasetRollbackTransaction(dataset.get());
    return false;
  }
  if ((create || clearFeatures(layer)) &&
      populate(layer) && (create || execute(dataset.get(), extent)) && OGR_L_SyncToDisk(layer) == OGRERR_NONE &&
      GDALDatasetCommitTransaction(dataset.get()) == OGRERR_NONE)
    return true;

  fail(error, path);
  GDALDatasetRollbackTransaction(dataset.get());
  return false;
}

template <typename Populate>
bool writeProjection(const QString &path, const char *table, OGRwkbGeometryType type,
                     std::initializer_list<Field> fields, Populate populate, QString *error)
{
  if (QFile::exists(path))
    return writeFeatures(path, table, type, fields, false, populate, error);

  // Publish a complete, closed package on first write. Subsequent updates use
  // SQLite transactions because the map provider may have the file open.
  const QString directory = QFileInfo(path).absolutePath();
  if (!QDir().mkpath(directory))
  {
    if (error) *error = QObject::tr("无法创建图层目录 %1").arg(directory);
    return false;
  }
  QTemporaryDir temporary(QDir(directory).filePath(QStringLiteral(".wellsiting-XXXXXX")));
  if (!temporary.isValid())
  {
    if (error) *error = temporary.errorString();
    return false;
  }
  QFile staged(temporary.filePath(QFileInfo(path).fileName()));
  if (!writeFeatures(staged.fileName(), table, type, fields, true, populate, error))
    return false;
  if (staged.rename(path))
    return true;
  if (error) *error = staged.errorString();
  return false;
}

bool writeWell(OGRLayerH layer, const QVariantMap &well)
{
  const Feature feature(OGR_F_Create(OGR_L_GetLayerDefn(layer)), OGR_F_Destroy);
  const Geometry point(OGR_G_CreateGeometry(wkbPoint), OGR_G_DestroyGeometry);
  OGR_G_SetPoint_2D(point.get(), 0, well.value(QStringLiteral("x")).toDouble(),
                    well.value(QStringLiteral("y")).toDouble());
  for (const char *name : {"id", "name"})
    OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), name),
                         well.value(QString::fromLatin1(name)).toString().toUtf8().constData());
  return OGR_F_SetGeometry(feature.get(), point.get()) == OGRERR_NONE &&
         OGR_L_CreateFeature(layer, feature.get()) == OGRERR_NONE;
}

bool appendPolygon(OGRGeometryH multi, const std::vector<paleo::wellsiting::Point2> &boundary)
{
  const Geometry ring(OGR_G_CreateGeometry(wkbLinearRing), OGR_G_DestroyGeometry);
  for (const auto &point : boundary)
    OGR_G_AddPoint_2D(ring.get(), point.x, point.y);
  OGR_G_CloseRings(ring.get());
  const Geometry polygon(OGR_G_CreateGeometry(wkbPolygon), OGR_G_DestroyGeometry);
  return OGR_G_AddGeometry(polygon.get(), ring.get()) == OGRERR_NONE &&
         OGR_G_AddGeometry(multi, polygon.get()) == OGRERR_NONE;
}

bool writeHole(OGRLayerH layer, const paleo::wellsiting::HoleRegion &region)
{
  const Geometry geometry(OGR_G_CreateGeometry(wkbMultiPolygon), OGR_G_DestroyGeometry);
  for (const auto &boundary : region.boundaryParts)
    if (!appendPolygon(geometry.get(), boundary))
      return false;
  const Feature feature(OGR_F_Create(OGR_L_GetLayerDefn(layer)), OGR_F_Destroy);
  OGR_F_SetFieldInteger(feature.get(), OGR_F_GetFieldIndex(feature.get(), "hole_id"), region.id);
  OGR_F_SetFieldDouble(feature.get(), OGR_F_GetFieldIndex(feature.get(), "area_m2"), region.area);
  if (std::isfinite(region.maxDistance))
    OGR_F_SetFieldDouble(feature.get(), OGR_F_GetFieldIndex(feature.get(), "max_distance_m"), region.maxDistance);
  return OGR_F_SetGeometry(feature.get(), geometry.get()) == OGRERR_NONE &&
         OGR_L_CreateFeature(layer, feature.get()) == OGRERR_NONE;
}
}

bool paleo::wellsiting::writePlannedWellsLayer(const QString &path, const QList<QVariantMap> &wells, QString *error)
{
  return writeProjection(path, "planned_wells", wkbPoint, {{"id", OFTString}, {"name", OFTString}},
      [&](OGRLayerH layer) {
        for (const auto &well : wells)
          if (!writeWell(layer, well))
            return false;
        return true;
      }, error);
}

bool paleo::wellsiting::writeCoverageHolesLayer(const QString &path, const std::vector<HoleRegion> &regions,
                                             QString *error)
{
  return writeProjection(path, "wellsiting_holes", wkbMultiPolygon,
      {{"hole_id", OFTInteger}, {"area_m2", OFTReal}, {"max_distance_m", OFTReal}},
      [&](OGRLayerH layer) {
        for (const auto &region : regions)
          if (!region.boundaryParts.empty() && !writeHole(layer, region))
            return false;
        return true;
      }, error);
}
