// 层：QGIS 封装
#include "qgisprojectservice.h"
#include "projectmapreference.h"
#include "qgiserrors_internal.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <cmath>
#include <memory>
#include <qgsproject.h>
#include <qgsrasterlayer.h>

void QgisProjectService::applyMapConfiguration()
{
  // 裸 QGZ 也带显示配置；有 project.paleo 时以清单为准。
  if (m_mapConfiguration.name.isEmpty()) {
    const auto map = QJsonDocument::fromJson(m_project->readEntry("paleo", "mapConfiguration").toUtf8()).object();
    m_mapConfiguration.mapCrs = map.value("crs").toString(QStringLiteral("EPSG:3857"));
    m_mapConfiguration.basemapTopo = map.value("topo").toString();
    m_mapConfiguration.basemapHillshade = map.value("hillshade").toString();
    m_mapConfiguration.basemapEnabled = map.value("enabled").toBool(true);
  }
  m_mapConfiguration.georeference = m_georeference;
  QString error;
  if (!paleo::mapreference::configure(m_project, m_georeference, m_mapConfiguration.mapCrs, &error))
    m_errors << tr("地图配准未启用：%1").arg(error);
  m_project->writeEntry("paleo", "mapConfiguration", QString::fromUtf8(QJsonDocument(QJsonObject{
      {"crs", m_mapConfiguration.mapCrs}, {"topo", m_mapConfiguration.basemapTopo},
      {"hillshade", m_mapConfiguration.basemapHillshade}, {"enabled", m_mapConfiguration.basemapEnabled}}).toJson(QJsonDocument::Compact)));
}

void QgisProjectService::setGeoreference(const PaleoGeoreference &reference)
{
  m_georeference = reference;
  applyMapConfiguration();
  emit mapConfigurationChanged();
}

void QgisProjectService::clearGeoreference()
{
  m_georeference.reset();
  m_project->removeEntry("paleo", "georeference");
  applyMapConfiguration();
  emit mapConfigurationChanged();
}

bool QgisProjectService::updateMapConfiguration(const PaleoProjectFile &configuration, QString *error)
{
  const auto fail = [&](const QString &text) {
    paleo::qgis_detail::setError(error, text);
    return false;
  };
  if (m_path.isEmpty() || m_opening) return fail(tr("请先打开工程并等待读取完成"));
  if (m_readOnly) return fail(tr("工程以只读模式打开，不能保存坐标与底图设置"));
  // 用独立工程校验，坏参数不改变正在显示的工程。
  QgsProject validation;
  validation.setTransformContext(m_project->transformContext());
  if (!paleo::mapreference::configure(&validation, configuration.georeference,
                                     configuration.mapCrs, error)) return false;
  const QDir directory(QFileInfo(m_path).absolutePath());
  for (const auto &path : {configuration.basemapTopo, configuration.basemapHillshade}) {
    if (path.isEmpty() || !configuration.basemapEnabled) continue;
    if (!configuration.georeference) return fail(tr("启用底图前请设置有效的工程坐标配准"));
    std::unique_ptr<QgsRasterLayer> layer(paleo::mapreference::offlineBasemap(directory.filePath(path), QString()));
    if (!layer) return fail(tr("离线底图无法读取：%1").arg(path));
  }
  bool ok = false;
  auto file = readProjectFile(paleoProjectFilePath(directory.path()), &ok, error);
  if (!ok) return false;
  file.georeference = configuration.georeference;
  file.georeferenceError.clear();
  if (file.georeference) {
    // 编辑参数后重新计算原控制点残差，避免保留旧拟合精度说明。
    file.georeference->maxResidualM = 0;
    for (auto &control : file.georeference->controlPoints) {
      double lon, lat;
      if (!applyGeoreference(*file.georeference, control.x, control.y, &lon, &lat))
        return fail(tr("坐标转换超出经纬度范围：%1").arg(control.well));
      control.residualM = std::hypot((lon - control.lon) * file.georeference->metersPerDegLon,
                                    (lat - control.lat) * file.georeference->metersPerDegLat);
      file.georeference->maxResidualM = std::max(file.georeference->maxResidualM, control.residualM);
    }
  }
  file.mapCrs = configuration.mapCrs;
  file.basemapEnabled = configuration.basemapEnabled;
  file.basemapTopo = configuration.basemapTopo.isEmpty() ? QString() : directory.relativeFilePath(directory.filePath(configuration.basemapTopo));
  file.basemapHillshade = configuration.basemapHillshade.isEmpty() ? QString() : directory.relativeFilePath(directory.filePath(configuration.basemapHillshade));
  if (!writeProjectFile(directory.path(), file, error)) return false;
  m_mapConfiguration = file;
  m_georeference = file.georeference;
  if (!m_georeference) m_project->removeEntry("paleo", "georeference");
  applyMapConfiguration();
  m_project->setDirty(true);
  emit mapConfigurationChanged();
  return true;
}
