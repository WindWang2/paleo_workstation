#include "paleopolygonize.h"
#include <qgsprocessingparameters.h>
#include <qgsexception.h>
#include <qgsrasterlayer.h>
#include <qgsfields.h>
#include <qgswkbtypes.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <cpl_conv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <memory>

void PaleoPolygonizeAlgorithm::initAlgorithm(const QVariantMap &)
{
  addParameter(new QgsProcessingParameterRasterLayer(QStringLiteral("INPUT"), QStringLiteral("Input raster")));
  addParameter(new QgsProcessingParameterFileDestination(QStringLiteral("OUTPUT"), QStringLiteral("Output polygons"),
                                                         QStringLiteral("GeoPackage (*.gpkg)")));
}

QVariantMap PaleoPolygonizeAlgorithm::processAlgorithm(const QVariantMap &parameters,
                                                       QgsProcessingContext &context,
                                                       QgsProcessingFeedback *feedback)
{
  QgsRasterLayer *rl = parameterAsRasterLayer(parameters, QStringLiteral("INPUT"), context);
  const QString outPath = parameterAsFileOutput(parameters, QStringLiteral("OUTPUT"), context);
  if (!rl || rl->source().isEmpty()) throw QgsProcessingException(QStringLiteral("Invalid input raster"));
  const QString src = rl->source();

  GDALDatasetH inDs = GDALOpen(src.toUtf8().constData(), GA_ReadOnly);
  if (!inDs) throw QgsProcessingException(QStringLiteral("GDAL cannot open %1").arg(src));

  // Build an OGR memory layer as GDALPolygonize target
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  const QString wkt = rl->crs().toWkt();
  const QByteArray wkb = wkt.toUtf8();
  char *wktPtr = const_cast<char *>(wkb.constData());
  OSRImportFromWkt(srs, &wktPtr);

  OGRSFDriverH memDrv = OGRGetDriverByName("Memory");
  OGRDataSourceH memDs = OGR_Dr_CreateDataSource(memDrv, "mem", nullptr);
  OGRLayerH memLayer = OGR_DS_CreateLayer(memDs, "polygonized", srs, wkbPolygon, nullptr);
  OGRFieldDefnH fld = OGR_Fld_Create("value", OFTInteger);
  OGR_L_CreateField(memLayer, fld, TRUE);
  OGR_Fld_Destroy(fld);

  GDALRasterBandH band = GDALGetRasterBand(inDs, 1);
  if (feedback) feedback->setProgress(20);
  const CPLErr err = GDALPolygonize(band, nullptr, memLayer, 0, nullptr, nullptr, nullptr);
  OSRDestroySpatialReference(srs);
  if (err != CE_None) {
    GDALClose(inDs); OGR_DS_Destroy(memDs);
    throw QgsProcessingException(QStringLiteral("GDALPolygonize failed"));
  }
  if (feedback) feedback->setProgress(70);

  // Persist memory layer to GPKG via OGR
  OGRSFDriverH gpkgDrv = OGRGetDriverByName("GPKG");
  OGRDataSourceH outDs = OGR_Dr_CreateDataSource(gpkgDrv, outPath.toUtf8().constData(), nullptr);
  if (!outDs) {
    GDALClose(inDs); OGR_DS_Destroy(memDs);
    throw QgsProcessingException(QStringLiteral("Cannot create %1").arg(outPath));
  }
  OGR_DS_CopyLayer(outDs, memLayer, "polygonized", nullptr);
  OGR_DS_Destroy(outDs);
  OGR_DS_Destroy(memDs);
  GDALClose(inDs);
  if (feedback) feedback->setProgress(100);

  QVariantMap out;
  out.insert(QStringLiteral("OUTPUT"), outPath);
  return out;
}
