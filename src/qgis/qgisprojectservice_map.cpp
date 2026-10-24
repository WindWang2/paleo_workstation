// 层：QGIS 封装
#include "qgisprojectservice.h"
#include "projectmapreference.h"
#include "onlinebasemap.h"
#include "qgiserrors_internal.h"
#include "../io/paleomapfiles.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <cmath>
#include <memory>
#include <qgsproject.h>
#include <qgspathresolver.h>
#include <qgsrasterlayer.h>
#include <qgslayertree.h>

namespace {
void embedMapConfiguration(QgsProject *project, const PaleoProjectFile &file)
{
  project->writeEntry("paleo", "mapConfiguration", QString::fromUtf8(QJsonDocument(QJsonObject{
      {"crs", file.mapCrs}, {"topo", file.basemapTopo},
      {"hillshade", file.basemapHillshade}, {"enabled", file.basemapEnabled}}).toJson(QJsonDocument::Compact)));
}

// External basemaps become project members. Content-addressed names avoid
// overwriting an existing map when two sources have the same file name.
bool projectLocalBasemap(const QDir &directory, QString path, QString *local, QString *error)
{
  if (path.isEmpty()) { local->clear(); return true; }
  const auto fail = [&](const QString &reason) {
    paleo::qgis_detail::setError(error, QObject::tr("无法将底图保存到工程：%1（%2）").arg(path, reason));
    return false;
  };
  const QFileInfo sourceInfo(directory.filePath(path));
  if (!sourceInfo.isFile()) return fail(QObject::tr("文件不存在"));
  const QString relative = directory.relativeFilePath(sourceInfo.canonicalFilePath());
  if (!QDir::isAbsolutePath(relative) && relative != QLatin1String("..") &&
      !relative.startsWith(QLatin1String("../"))) {
    *local = relative;
    return true;
  }
  QFile source(sourceInfo.canonicalFilePath());
  if (sourceInfo.suffix().compare("mbtiles", Qt::CaseInsensitive) != 0) {
    if (!directory.mkpath("basemap")) return fail(QObject::tr("无法创建底图目录"));
    QTemporaryDir stage(directory.filePath("basemap/.settings-XXXXXX"));
    QString detail;
    const auto members = PaleoMapFiles::copyDataset(sourceInfo.absoluteFilePath(), stage.path(), {}, &detail);
    if (members.isEmpty()) return fail(detail);
    const auto relativeDir = "basemap/" + PaleoMapFiles::fingerprint(members);
    if (QFileInfo::exists(directory.filePath(relativeDir))) for (const auto &value : members) {
      const auto member = value.toMap(); QFile saved(directory.filePath(relativeDir + '/' + member.value("file").toString()));
      QCryptographicHash digest(QCryptographicHash::Sha256);
      if (!saved.open(QIODevice::ReadOnly) || !digest.addData(&saved) ||
          QString::fromLatin1(digest.result().toHex()) != member.value("sha256").toString()) return fail(QObject::tr("工程中的底图副本损坏"));
    }
    if (!QFileInfo::exists(directory.filePath(relativeDir)) && !QDir().rename(stage.path(), directory.filePath(relativeDir)))
      return fail(QObject::tr("无法保存底图文件组"));
    *local = relativeDir + '/' + sourceInfo.fileName(); return true;
  }
  if (!source.open(QIODevice::ReadOnly)) return fail(source.errorString());
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&source)) return fail(source.errorString());
  *local = QStringLiteral("basemap/%1.mbtiles").arg(QString::fromLatin1(hash.result().toHex()));
  const QString target = directory.filePath(*local);
  QFile existing(target);
  if (existing.open(QIODevice::ReadOnly)) {
    QCryptographicHash check(QCryptographicHash::Sha256);
    if (check.addData(&existing) && check.result() == hash.result()) return true;
    existing.close();
  }
  if (!directory.mkpath(QStringLiteral("basemap"))) return fail(QObject::tr("无法创建底图目录"));
  if (!source.seek(0)) return fail(source.errorString());
  QSaveFile output(target);
  if (!output.open(QIODevice::WriteOnly)) return fail(output.errorString());
  QCryptographicHash copied(QCryptographicHash::Sha256);
  while (!source.atEnd()) {
    const QByteArray bytes = source.read(1024 * 1024);
    if (source.error() != QFile::NoError) return fail(source.errorString());
    copied.addData(bytes);
    if (output.write(bytes) != bytes.size()) return fail(output.errorString());
  }
  if (copied.result() != hash.result()) return fail(QObject::tr("复制期间源文件发生变化"));
  return output.commit() ? true : fail(output.errorString());
}
}

bool QgisProjectService::prepareBasemapMembers(QString *error)
{
  const QDir directory(QFileInfo(m_path).absolutePath());
  auto file = m_mapConfiguration;
  if (!projectLocalBasemap(directory, file.basemapTopo, &file.basemapTopo, error) ||
      !projectLocalBasemap(directory, file.basemapHillshade, &file.basemapHillshade, error)) return false;
  if (file.basemapTopo != m_mapConfiguration.basemapTopo ||
      file.basemapHillshade != m_mapConfiguration.basemapHillshade) {
    if (!writeProjectFile(directory.path(), file, error)) return false;
    m_mapConfiguration = file;
    emit mapConfigurationChanged(); // Refresh layer URIs/declarations before serialization.
  }
  embedMapConfiguration(m_project, m_mapConfiguration);
  return true;
}

void QgisProjectService::applyMapConfiguration()
{
  for (auto *layer : m_project->mapLayers())
    if (layer->customProperty("paleoBasemap").toBool()) {
      const auto path = layer->customProperty("paleoBasemapPath").toString();
      if (!path.isEmpty())
        layer->setCustomProperty("paleoBasemapPath", m_project->pathResolver().readPath(path));
    }
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
  embedMapConfiguration(m_project, m_mapConfiguration);
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

bool QgisProjectService::validateMapReference(const PaleoProjectFile &configuration, QString *error)
{
  QgsProject validation;
  return paleo::mapreference::configure(&validation, configuration.georeference, configuration.mapCrs, error);
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
  if (!projectLocalBasemap(directory, configuration.basemapTopo, &file.basemapTopo, error) ||
      !projectLocalBasemap(directory, configuration.basemapHillshade, &file.basemapHillshade, error)) return false;
  if (!writeProjectFile(directory.path(), file, error)) return false;
  const bool activateOffline = file.basemapEnabled && (file.basemapTopo != m_mapConfiguration.basemapTopo ||
      file.basemapHillshade != m_mapConfiguration.basemapHillshade || !m_mapConfiguration.basemapEnabled);
  m_mapConfiguration = file;
  m_georeference = file.georeference;
  if (!m_georeference) m_project->removeEntry("paleo", "georeference");
  applyMapConfiguration();
  if (activateOffline) {
    paleo::onlinebasemap::saveChoice(m_project, paleo::onlinebasemap::noneId());
    for (auto *layer : m_project->mapLayers())
      if (!layer->customProperty("paleoOnlineBasemap").toString().isEmpty())
        if (auto *node = m_project->layerTreeRoot()->findLayer(layer->id())) node->setItemVisibilityChecked(false);
  }
  m_project->setDirty(true);
  emit mapConfigurationChanged();
  return true;
}
