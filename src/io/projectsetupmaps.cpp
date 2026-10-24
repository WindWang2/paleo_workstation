// 层：数据
#include "projectsetupscan.h"
#include "../services/paleomapfiles.h"
#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace {
QString mapCrs(const QString &path)
{
  if (!QStringList{"shp", "gpkg", "geojson", "json", "tif", "tiff", "img"}.contains(QFileInfo(path).suffix().toLower())) return {};
  GDALAllRegister();
  auto *dataset = static_cast<GDALDataset *>(GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  if (!dataset) return {};
  const OGRSpatialReference *srs = dataset->GetLayerCount() && dataset->GetLayer(0) ? dataset->GetLayer(0)->GetSpatialRef() : dataset->GetSpatialRef();
  QString result;
  if (srs && (srs->IsProjected() || srs->IsGeographic())) {
    if (srs->IsGeographic()) result = QStringLiteral("EPSG:3857"); // Metre-based computation grid; map layers retain their own CRS.
    else if (srs->GetAuthorityName(nullptr) && srs->GetAuthorityCode(nullptr)) result = QString::fromUtf8(srs->GetAuthorityName(nullptr)) + ':' + QString::fromUtf8(srs->GetAuthorityCode(nullptr));
    else { char *wkt = nullptr; srs->exportToWkt(&wkt); result = QString::fromUtf8(wkt); CPLFree(wkt); }
  }
  GDALClose(dataset); return result;
}
}

void scanProjectMaps(const ProjectSetupSources &sources, ProjectSetupPreview *out,
                     const std::function<bool()> &cancelled)
{
  if (sources.paleomaps.isEmpty()) return;
  const QFileInfo source(sources.paleomaps);
  if (!source.exists() || !source.isReadable()) { out->sourceProblems.insert(sources.paleomaps, QObject::tr("古地理图路径不存在或不可读")); return; }
  const auto files = PaleoMapFiles::discover(source.isFile() ? QStringList{source.absoluteFilePath()} : QStringList{},
                                           source.isDir() ? source.absoluteFilePath() : QString(), cancelled);
  if (files.isEmpty()) out->notes << QObject::tr("古地理图来源中没有支持的图件；SHP 配套文件不会单独作为图件导入。");
  for (const auto &path : files) {
    if (cancelled && cancelled()) return;
    const QFileInfo file(path);
    ProjectSetupMap map; map.path = path;
    map.crs = mapCrs(path);
    map.horizon = PaleoMapFiles::inferredHorizon(path, mappingHorizons(), {});
    map.members = PaleoMapFiles::members(path, &map.problem);
    const bool preferGpkg = source.isDir() && file.suffix().compare("shp", Qt::CaseInsensitive) == 0 &&
        QFileInfo::exists(file.dir().filePath(file.completeBaseName() + ".gpkg"));
    const bool preview = QRegularExpression(QStringLiteral("_(preview|topology_detail)$"),
        QRegularExpression::CaseInsensitiveOption).match(file.completeBaseName()).hasMatch();
    map.included = !preferGpkg && !preview && map.problem.isEmpty();
    if (sources.mapChoices.contains(path)) {
      const auto choice = sources.mapChoices.value(path).toMap();
      map.included = choice.value("included", map.included).toBool();
      map.horizon = choice.value("horizon", map.horizon).toString();
    }
    for (const auto &member : map.members) {
      QString error;
      const auto hash = DataCatalog::sha256FileHex(member, &error, cancelled);
      if (hash.isEmpty()) { map.problem = QObject::tr("无法核对图件配套文件：%1；%2").arg(member, error); map.included = false; break; }
      out->mapHashes.insert(member, hash);
    }
    if (!map.problem.isEmpty()) out->sourceProblems.insert(map.path, map.problem);
    out->maps << map;
  }
  out->notes << QObject::tr("现有古地理图按套导入。默认优先同名 GPKG，预览图默认不选；可在其他资料页改选 SHP 或调整层位。");
}
