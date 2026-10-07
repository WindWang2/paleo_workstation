// 层：数据
#include "constraintstore.h"
#include "gdalreg_internal.h"

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_geometry.h>
#include <cpl_conv.h>
#include <cpl_error.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <memory>

#include <mutex>
#include <vector>

namespace
{

using paleo::io_detail::ensureGdalRegistered;

  bool ensureField(OGRLayerH layer, const char *name, OGRFieldType type)
  {
    OGRFeatureDefnH layerDefn = OGR_L_GetLayerDefn(layer);
    if (OGR_FD_GetFieldIndex(layerDefn, name) >= 0)
      return true;
    OGRFieldDefnH fldDef = OGR_Fld_Create(name, type);
    const OGRErr err = OGR_L_CreateField(layer, fldDef, TRUE);
    OGR_Fld_Destroy(fldDef);
    return err == OGRERR_NONE
           && OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(layer), name) >= 0;
  }
} // namespace

ConstraintStore::ConstraintStore(const QString &gpkgPath, EnqueueFn enqueue)
  : m_gpkgPath(gpkgPath)
  , m_enqueue(std::move(enqueue))
{
}

ConstraintStore::ConstraintStore(const QString &gpkgPath, PaleoProjectStore *store)
  : m_gpkgPath(gpkgPath)
{
  if (store)
  {
    QPointer<PaleoProjectStore> safeStore(store);
    m_enqueue = [safeStore](const WriteFn &fn) -> PaleoProjectStore::WriteResult {
      PaleoProjectStore *storePtr = safeStore.data();
      if (!storePtr)
      {
        return {false, QStringLiteral("PaleoProjectStore destroyed or unavailable")};
      }
      return storePtr->enqueueWrite(fn);
    };
  }
}

bool ConstraintStore::append(const QString &horizon, const QString &id, const QString &wkt,
                             const QString &type, int faciesCode, QString *error, double weight)
{
  return appendInternal(horizon, id, wkt, type, faciesCode, nullptr, 0, error, weight);
}

bool ConstraintStore::appendExtended(const QString &horizon, const QString &id, const QString &wkt,
                                     const QString &type, int faciesCode, const QString &paramsJson,
                                     int schemaVersion, QString *error, double weight)
{
  return appendInternal(horizon, id, wkt, type, faciesCode, &paramsJson, schemaVersion, error, weight);
}

bool ConstraintStore::appendInternal(const QString &horizon, const QString &id, const QString &wkt,
                                     const QString &type, int faciesCode, const QString *paramsJson,
                                     int schemaVersion, QString *error, double weight)
{
  auto runWrite = [this](const WriteFn &fn) -> PaleoProjectStore::WriteResult {
    if (m_enqueue)
      return m_enqueue(fn);
    return fn();
  };

  PaleoProjectStore::WriteResult res = runWrite([&]() -> PaleoProjectStore::WriteResult {
    ensureGdalRegistered();

    QByteArray wktBytes = wkt.toUtf8();
    char *pszWkt = wktBytes.data();
    OGRGeometryH geom = nullptr;
    OGRErr ogrErr = OGR_G_CreateFromWkt(&pszWkt, nullptr, &geom);
    if (ogrErr != OGRERR_NONE || !geom)
    {
      return {false, QStringLiteral("Invalid WKT geometry: ") + wkt};
    }

    const QString dirPath = QFileInfo(m_gpkgPath).absolutePath();
    if (!QDir().mkpath(dirPath))
    {
      OGR_G_DestroyGeometry(geom);
      return {false, QStringLiteral("Failed to create directory: %1").arg(dirPath)};
    }

    GDALDatasetH ds = nullptr;
    if (QFile::exists(m_gpkgPath))
    {
      ds = GDALOpenEx(m_gpkgPath.toUtf8().constData(),
                      GDAL_OF_UPDATE | GDAL_OF_VECTOR,
                      nullptr, nullptr, nullptr);
      if (!ds)
      {
        OGR_G_DestroyGeometry(geom);
        return {false, QStringLiteral("Failed to open GeoPackage at %1: %2")
                       .arg(m_gpkgPath, QString::fromUtf8(CPLGetLastErrorMsg()))};
      }
    }
    else
    {
      GDALDriverH drv = GDALGetDriverByName("GPKG");
      if (!drv)
      {
        OGR_G_DestroyGeometry(geom);
        return {false, QStringLiteral("GDAL GPKG driver not available")};
      }
      ds = GDALCreate(drv, m_gpkgPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
      if (!ds)
      {
        OGR_G_DestroyGeometry(geom);
        return {false, QStringLiteral("Failed to create GeoPackage at %1: %2")
                       .arg(m_gpkgPath, QString::fromUtf8(CPLGetLastErrorMsg()))};
      }
    }

    OGRLayerH layer = GDALDatasetGetLayerByName(ds, "constraints");
    if (!layer)
    {
      layer = GDALDatasetCreateLayer(ds, "constraints", nullptr, wkbUnknown, nullptr);
      if (!layer)
      {
        OGR_G_DestroyGeometry(geom);
        GDALClose(ds);
        return {false, QStringLiteral("Failed to create 'constraints' layer: %1")
                       .arg(QString::fromUtf8(CPLGetLastErrorMsg()))};
      }
    }

    if (!ensureField(layer, "id", OFTString)
        || !ensureField(layer, "horizon", OFTString)
        || !ensureField(layer, "type", OFTString)
        || !ensureField(layer, "facies_code", OFTInteger)
        || !ensureField(layer, "weight", OFTReal)
        || !ensureField(layer, "params_json", OFTString)
        || !ensureField(layer, "schema_version", OFTInteger))
    {
      OGR_G_DestroyGeometry(geom);
      GDALClose(ds);
      return {false, QStringLiteral("Failed to create constraint fields: %1")
                     .arg(QString::fromUtf8(CPLGetLastErrorMsg()))};
    }

    OGRFeatureDefnH layerDefn = OGR_L_GetLayerDefn(layer);
    OGRFeatureH feat = OGR_F_Create(layerDefn);
    if (!feat)
    {
      OGR_G_DestroyGeometry(geom);
      GDALClose(ds);
      return {false, QStringLiteral("Failed to create feature")};
    }

    int idIdx = OGR_FD_GetFieldIndex(layerDefn, "id");
    if (idIdx >= 0)
      OGR_F_SetFieldString(feat, idIdx, id.toUtf8().constData());

    int horizonIdx = OGR_FD_GetFieldIndex(layerDefn, "horizon");
    if (horizonIdx >= 0)
      OGR_F_SetFieldString(feat, horizonIdx, horizon.toUtf8().constData());

    int typeIdx = OGR_FD_GetFieldIndex(layerDefn, "type");
    if (typeIdx >= 0)
      OGR_F_SetFieldString(feat, typeIdx, type.toUtf8().constData());

    int faciesIdx = OGR_FD_GetFieldIndex(layerDefn, "facies_code");
    if (faciesIdx >= 0)
      OGR_F_SetFieldInteger(feat, faciesIdx, faciesCode);

    int weightIdx = OGR_FD_GetFieldIndex(layerDefn, "weight");
    if (weightIdx >= 0)
      OGR_F_SetFieldDouble(feat, weightIdx, weight);

    if (paramsJson)
    {
      const int paramsIdx = OGR_FD_GetFieldIndex(layerDefn, "params_json");
      const int schemaIdx = OGR_FD_GetFieldIndex(layerDefn, "schema_version");
      if (paramsIdx >= 0)
        OGR_F_SetFieldString(feat, paramsIdx, paramsJson->toUtf8().constData());
      if (schemaIdx >= 0)
        OGR_F_SetFieldInteger(feat, schemaIdx, schemaVersion);
    }

    OGR_F_SetGeometryDirectly(feat, geom);

    OGRErr cErr = OGR_L_CreateFeature(layer, feat);
    OGR_F_Destroy(feat);
    GDALClose(ds);

    if (cErr != OGRERR_NONE)
    {
      return {false, QStringLiteral("Failed to create feature in layer: %1")
                     .arg(QString::fromUtf8(CPLGetLastErrorMsg()))};
    }

    return {true, QString()};
  });

  if (!res.ok)
  {
    if (error)
      *error = res.error;
    return false;
  }
  return true;
}

QVector<QVariantMap> ConstraintStore::load(const QString &horizon) const
{
  ensureGdalRegistered();
  QVector<QVariantMap> result;

  if (!QFile::exists(m_gpkgPath))
    return result;

  GDALDatasetH ds = GDALOpenEx(m_gpkgPath.toUtf8().constData(),
                               GDAL_OF_READONLY | GDAL_OF_VECTOR,
                               nullptr, nullptr, nullptr);
  if (!ds)
    return result;

  OGRLayerH layer = GDALDatasetGetLayerByName(ds, "constraints");
  if (!layer)
  {
    GDALClose(ds);
    return result;
  }

  OGRFeatureDefnH layerDefn = OGR_L_GetLayerDefn(layer);
  int idIdx = OGR_FD_GetFieldIndex(layerDefn, "id");
  int horizonIdx = OGR_FD_GetFieldIndex(layerDefn, "horizon");
  int typeIdx = OGR_FD_GetFieldIndex(layerDefn, "type");
  int faciesIdx = OGR_FD_GetFieldIndex(layerDefn, "facies_code");
  int weightIdx = OGR_FD_GetFieldIndex(layerDefn, "weight");
  int paramsIdx = OGR_FD_GetFieldIndex(layerDefn, "params_json");
  int schemaIdx = OGR_FD_GetFieldIndex(layerDefn, "schema_version");

  OGR_L_ResetReading(layer);
  OGRFeatureH feat = nullptr;
  while ((feat = OGR_L_GetNextFeature(layer)) != nullptr)
  {
    QString featHorizon;
    if (horizonIdx >= 0)
    {
      const char *hStr = OGR_F_GetFieldAsString(feat, horizonIdx);
      if (hStr)
        featHorizon = QString::fromUtf8(hStr);
    }

    if (!horizon.isEmpty() && featHorizon != horizon)
    {
      OGR_F_Destroy(feat);
      continue;
    }

    QVariantMap map;
    map.insert(QStringLiteral("fid"), static_cast<qlonglong>(OGR_F_GetFID(feat)));
    if (idIdx >= 0)
    {
      const char *idStr = OGR_F_GetFieldAsString(feat, idIdx);
      map.insert(QStringLiteral("id"), idStr ? QString::fromUtf8(idStr) : QString());
    }
    else
    {
      map.insert(QStringLiteral("id"), QString());
    }

    map.insert(QStringLiteral("horizon"), featHorizon);

    if (typeIdx >= 0)
    {
      const char *typeStr = OGR_F_GetFieldAsString(feat, typeIdx);
      map.insert(QStringLiteral("type"), typeStr ? QString::fromUtf8(typeStr) : QString());
    }
    else
    {
      map.insert(QStringLiteral("type"), QString());
    }

    int fc = (faciesIdx >= 0) ? OGR_F_GetFieldAsInteger(feat, faciesIdx) : 0;
    map.insert(QStringLiteral("facies_code"), fc);
    map.insert(QStringLiteral("target_facies_code"), fc);

    double wt = (weightIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, weightIdx))
                    ? OGR_F_GetFieldAsDouble(feat, weightIdx)
                    : 1.0;
    map.insert(QStringLiteral("weight"), wt);

    if (paramsIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, paramsIdx))
    {
      const char *params = OGR_F_GetFieldAsString(feat, paramsIdx);
      map.insert(QStringLiteral("params_json"), params ? QString::fromUtf8(params) : QString());
    }
    if (schemaIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, schemaIdx))
      map.insert(QStringLiteral("schema_version"), OGR_F_GetFieldAsInteger(feat, schemaIdx));

    QString wktStr;
    OGRGeometryH geom = OGR_F_GetGeometryRef(feat);
    if (geom)
    {
      OGRWktOptions options(17, false);
      options.format = OGRWktFormat::G; // round-trip every binary64 coordinate
      wktStr = QString::fromStdString(OGRGeometry::FromHandle(geom)->exportToWkt(options));
    }
    map.insert(QStringLiteral("wkt"), wktStr);

    result.append(map);
    OGR_F_Destroy(feat);
  }

  GDALClose(ds);
  return result;
}

bool ConstraintStore::updateParameters(const QString &id, const QString &paramsJson, int schemaVersion,
                                       const QString &semanticType, QString *error)
{
  auto runWrite = [this](const WriteFn &fn) -> PaleoProjectStore::WriteResult {
    if (m_enqueue)
      return m_enqueue(fn);
    return fn();
  };

  PaleoProjectStore::WriteResult res = runWrite([this, id, paramsJson, schemaVersion, semanticType]() {
    ensureGdalRegistered();
    if (!QFile::exists(m_gpkgPath))
      return PaleoProjectStore::WriteResult{false, QStringLiteral("Constraint file does not exist")};

    GDALDatasetH ds = GDALOpenEx(m_gpkgPath.toUtf8().constData(),
                                 GDAL_OF_UPDATE | GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (!ds)
      return PaleoProjectStore::WriteResult{false, QStringLiteral("Failed to open constraint GeoPackage: %1")
                     .arg(QString::fromUtf8(CPLGetLastErrorMsg()))};

    OGRLayerH layer = GDALDatasetGetLayerByName(ds, "constraints");
    if (!layer)
    {
      GDALClose(ds);
      return PaleoProjectStore::WriteResult{false, QStringLiteral("Constraint layer is missing")};
    }
    if (GDALDatasetStartTransaction(ds, FALSE) != OGRERR_NONE)
    {
      GDALClose(ds);
      return PaleoProjectStore::WriteResult{false, QStringLiteral("Failed to start constraint transaction")};
    }
    auto rollback = [&](const QString &message) {
      GDALDatasetRollbackTransaction(ds);
      GDALClose(ds);
      return PaleoProjectStore::WriteResult{false, message};
    };
    if (!ensureField(layer, "params_json", OFTString) || !ensureField(layer, "schema_version", OFTInteger))
      return rollback(QStringLiteral("Failed to extend constraint fields"));

    OGRFeatureDefnH layerDefn = OGR_L_GetLayerDefn(layer);
    const int idIdx = OGR_FD_GetFieldIndex(layerDefn, "id");
    const int paramsIdx = OGR_FD_GetFieldIndex(layerDefn, "params_json");
    const int schemaIdx = OGR_FD_GetFieldIndex(layerDefn, "schema_version");
    const int typeIdx = OGR_FD_GetFieldIndex(layerDefn, "type");
    if (idIdx < 0 || paramsIdx < 0 || schemaIdx < 0)
      return rollback(QStringLiteral("Constraint parameter fields are missing"));

    bool found = false;
    OGR_L_ResetReading(layer);
    OGRFeatureH feat = nullptr;
    while ((feat = OGR_L_GetNextFeature(layer)) != nullptr)
    {
      const char *featId = OGR_F_GetFieldAsString(feat, idIdx);
      if (featId && QString::fromUtf8(featId) == id)
      {
        found = true;
        OGR_F_SetFieldString(feat, paramsIdx, paramsJson.toUtf8().constData());
        OGR_F_SetFieldInteger(feat, schemaIdx, schemaVersion);
        if (!semanticType.isEmpty() && typeIdx >= 0)
          OGR_F_SetFieldString(feat, typeIdx, semanticType.toUtf8().constData());
        if (OGR_L_SetFeature(layer, feat) != OGRERR_NONE)
        {
          OGR_F_Destroy(feat);
          return rollback(QStringLiteral("Failed to update constraint parameters: %1")
                              .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
        }
      }
      OGR_F_Destroy(feat);
    }
    if (!found)
      return rollback(QStringLiteral("Constraint id was not found"));
    if (GDALDatasetCommitTransaction(ds) != OGRERR_NONE)
    {
      GDALDatasetRollbackTransaction(ds);
      GDALClose(ds);
      return PaleoProjectStore::WriteResult{false, QStringLiteral("Failed to commit constraint parameters")};
    }
    GDALClose(ds);
    return PaleoProjectStore::WriteResult{true, QString()};
  });

  if (!res.ok)
  {
    if (error)
      *error = res.error;
    return false;
  }
  return true;
}

bool ConstraintStore::remove(const QString &id, QString *error)
{
  auto runWrite = [this](const WriteFn &fn) -> PaleoProjectStore::WriteResult {
    if (m_enqueue)
      return m_enqueue(fn);
    return fn();
  };

  PaleoProjectStore::WriteResult res = runWrite([this, id]() -> PaleoProjectStore::WriteResult {
    ensureGdalRegistered();
    if (!QFile::exists(m_gpkgPath))
      return {true, QString()};

    GDALDatasetH ds = GDALOpenEx(m_gpkgPath.toUtf8().constData(),
                                 GDAL_OF_UPDATE | GDAL_OF_VECTOR,
                                 nullptr, nullptr, nullptr);
    if (!ds)
      return {false, QStringLiteral("Failed to open constraint GeoPackage: %1")
                     .arg(QString::fromUtf8(CPLGetLastErrorMsg()))};

    OGRLayerH layer = GDALDatasetGetLayerByName(ds, "constraints");
    if (!layer)
    {
      GDALClose(ds);
      return {false, QStringLiteral("Constraint layer is missing")};
    }

    OGRFeatureDefnH layerDefn = OGR_L_GetLayerDefn(layer);
    int idIdx = OGR_FD_GetFieldIndex(layerDefn, "id");
    if (idIdx < 0)
    {
      GDALClose(ds);
      return {false, QStringLiteral("Constraint layer has no id field")};
    }

    std::vector<GIntBig> fidsToDelete;
    OGR_L_ResetReading(layer);
    OGRFeatureH feat = nullptr;
    while ((feat = OGR_L_GetNextFeature(layer)) != nullptr)
    {
      const char *featId = OGR_F_GetFieldAsString(feat, idIdx);
      if (featId && QString::fromUtf8(featId) == id)
      {
        fidsToDelete.push_back(OGR_F_GetFID(feat));
      }
      OGR_F_Destroy(feat);
    }

    for (GIntBig fid : fidsToDelete)
    {
      const OGRErr delErr = OGR_L_DeleteFeature(layer, fid);
      if (delErr != OGRERR_NONE)
      {
        GDALClose(ds);
        return {false, QStringLiteral("Failed to delete constraint feature: %1")
                       .arg(QString::fromUtf8(CPLGetLastErrorMsg()))};
      }
    }

    GDALClose(ds);
    return {true, QString()};
  });

  if (!res.ok)
  {
    if (error)
      *error = res.error;
    return false;
  }
  return true;
}

bool ConstraintStore::replaceHorizon(const QString &horizon, const QVector<QVariantMap> &rows,
                                     QString *error)
{
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    ensureGdalRegistered();
    if (horizon.isEmpty() || !QFile::exists(m_gpkgPath))
      return {false, QObject::tr("约束层位或资产缺失")};
    std::unique_ptr<void, decltype(&GDALClose)> ds(
        GDALOpenEx(m_gpkgPath.toUtf8().constData(), GDAL_OF_UPDATE | GDAL_OF_VECTOR,
                   nullptr, nullptr, nullptr), GDALClose);
    if (!ds)
      return {false, QObject::tr("无法打开约束资产")};
    OGRLayerH layer = GDALDatasetGetLayerByName(ds.get(), "constraints");
    if (!layer || GDALDatasetStartTransaction(ds.get(), FALSE) != OGRERR_NONE)
      return {false, QObject::tr("无法开始约束快照事务")};
    const auto fail = [&](const QString &reason) -> PaleoProjectStore::WriteResult {
      GDALDatasetRollbackTransaction(ds.get());
      return {false, reason};
    };
    OGRFeatureDefnH def = OGR_L_GetLayerDefn(layer);
    const int horizonIndex = OGR_FD_GetFieldIndex(def, "horizon");
    const int idIndex = OGR_FD_GetFieldIndex(def, "id");
    if (horizonIndex < 0 || idIndex < 0)
      return fail(QObject::tr("约束身份字段缺失"));

    QSet<qlonglong> keep;
    QSet<QString> ids;
    for (const QVariantMap &row : rows)
    {
      const qlonglong fid = row.value(QStringLiteral("fid"), -1).toLongLong();
      const QString id = row.value(QStringLiteral("id")).toString();
      if (fid < 0 || id.isEmpty() || keep.contains(fid) || ids.contains(id) ||
          row.value(QStringLiteral("horizon")).toString() != horizon)
        return fail(QObject::tr("约束快照身份无效或重复"));
      keep.insert(fid);
      ids.insert(id);
      std::unique_ptr<void, decltype(&OGR_F_Destroy)> feature(OGR_L_GetFeature(layer, fid), OGR_F_Destroy);
      const bool exists = bool(feature);
      if (exists && (QString::fromUtf8(OGR_F_GetFieldAsString(feature.get(), horizonIndex)) != horizon ||
                     QString::fromUtf8(OGR_F_GetFieldAsString(feature.get(), idIndex)) != id))
        return fail(QObject::tr("约束快照会覆盖其他要素身份"));
      if (!exists)
      {
        feature.reset(OGR_F_Create(def));
        OGR_F_SetFID(feature.get(), fid);
      }
      QByteArray bytes = row.value(QStringLiteral("wkt")).toString().toUtf8();
      char *wkt = bytes.data();
      OGRGeometryH geometry = nullptr;
      const OGRErr parsed = OGR_G_CreateFromWkt(&wkt, nullptr, &geometry);
      std::unique_ptr<void, decltype(&OGR_G_DestroyGeometry)> ownedGeometry(geometry, OGR_G_DestroyGeometry);
      if (parsed != OGRERR_NONE || !geometry || OGR_F_SetGeometry(feature.get(), geometry) != OGRERR_NONE)
        return fail(QObject::tr("约束快照几何无效"));
      // Preserve schema scalar fields and binary64 precision during rollback.
      for (int i = 0; i < OGR_FD_GetFieldCount(def); ++i)
      {
        const QString name = QString::fromUtf8(OGR_Fld_GetNameRef(OGR_FD_GetFieldDefn(def, i)));
        if (!row.contains(name))
          continue;
        const QVariant value = row.value(name);
        if (value.isNull())
          OGR_F_SetFieldNull(feature.get(), i);
        else
        {
          switch (OGR_Fld_GetType(OGR_FD_GetFieldDefn(def, i)))
          {
            case OFTInteger: OGR_F_SetFieldInteger(feature.get(), i, value.toInt()); break;
            case OFTInteger64: OGR_F_SetFieldInteger64(feature.get(), i, value.toLongLong()); break;
            case OFTReal: OGR_F_SetFieldDouble(feature.get(), i, value.toDouble()); break;
            case OFTString: OGR_F_SetFieldString(feature.get(), i, value.toString().toUtf8().constData()); break;
            default: return fail(QObject::tr("约束快照包含不支持的字段类型：%1").arg(name));
          }
        }
      }
      if ((exists ? OGR_L_SetFeature(layer, feature.get()) : OGR_L_CreateFeature(layer, feature.get())) != OGRERR_NONE)
        return fail(QObject::tr("写入约束快照失败：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg())));
    }
    QList<qlonglong> remove;
    OGR_L_ResetReading(layer);
    while (OGRFeatureH raw = OGR_L_GetNextFeature(layer))
    {
      std::unique_ptr<void, decltype(&OGR_F_Destroy)> feature(raw, OGR_F_Destroy);
      const qlonglong fid = OGR_F_GetFID(raw);
      if (QString::fromUtf8(OGR_F_GetFieldAsString(raw, horizonIndex)) == horizon && !keep.contains(fid))
        remove.append(fid);
    }
    for (qlonglong fid : remove)
      if (OGR_L_DeleteFeature(layer, fid) != OGRERR_NONE)
        return fail(QObject::tr("删除约束快照要素失败"));
    if (GDALDatasetCommitTransaction(ds.get()) != OGRERR_NONE)
      return fail(QObject::tr("提交约束快照失败"));
    return {true, QString()};
  };
  const auto result = m_enqueue ? m_enqueue(write) : write();
  if (!result.ok && error)
    *error = result.error;
  return result.ok;
}
