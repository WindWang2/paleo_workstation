#include "constraintstore.h"

#include <gdal.h>
#include <ogr_api.h>
#include <cpl_conv.h>
#include <cpl_error.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <mutex>
#include <vector>

namespace
{
  void ensureGdalRegistered()
  {
    static std::once_flag flag;
    std::call_once(flag, []() {
      GDALAllRegister();
    });
  }

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
    m_enqueue = [store](const WriteFn &fn) {
      return store->enqueueWrite(fn);
    };
  }
}

bool ConstraintStore::append(const QString &horizon, const QString &id, const QString &wkt,
                             const QString &type, int faciesCode, QString *error, double weight)
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
        || !ensureField(layer, "weight", OFTReal))
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

    QString wktStr;
    OGRGeometryH geom = OGR_F_GetGeometryRef(feat);
    if (geom)
    {
      char *pszWkt = nullptr;
      if (OGR_G_ExportToWkt(geom, &pszWkt) == OGRERR_NONE && pszWkt)
      {
        wktStr = QString::fromUtf8(pszWkt);
        CPLFree(pszWkt);
      }
    }
    map.insert(QStringLiteral("wkt"), wktStr);

    result.append(map);
    OGR_F_Destroy(feat);
  }

  GDALClose(ds);
  return result;
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
