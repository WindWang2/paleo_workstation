// 层：QGIS 封装
#include "onlinebasemap.h"

#include <qgscoordinatereferencesystem.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUrl>

namespace paleo::onlinebasemap {
namespace {
const char *kSourcesKey = "basemap/onlineSources";
const char *kScope = "paleo";
const char *kEntry = "survey/onlineBasemap";

QString projectKey(const QgsProject *project)
{
  const QString path = project ? project->absoluteFilePath() : QString();
  if (path.isEmpty())
    return {};
  return QStringLiteral("basemap/choice/%1")
      .arg(QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)));
}
} // namespace

QString noneId() { return QStringLiteral("none"); }

QVector<Source> sources()
{
  QVector<Source> out{
      {QStringLiteral("osm"), QObject::tr("街道"), QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
       QObject::tr("© OpenStreetMap 贡献者（ODbL）"), 19},
      {QStringLiteral("esri_imagery"), QObject::tr("卫星"),
       QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"),
       QObject::tr("影像 © Esri, Maxar, Earthstar Geographics 等（遵守 Esri 使用条款）"), 19},
      {QStringLiteral("opentopomap"), QObject::tr("地形"), QStringLiteral("https://tile.opentopomap.org/{z}/{x}/{y}.png"),
       QObject::tr("© OpenStreetMap 贡献者, SRTM | 样式 © OpenTopoMap（CC-BY-SA）"), 17}};
  const auto doc = QJsonDocument::fromJson(QSettings().value(kSourcesKey).toByteArray());
  for (const auto &v : doc.array()) {
    const auto o = v.toObject();
    Source s{o.value("id").toString(), o.value("title").toString(), o.value("url").toString(),
             o.value("attribution").toString(), o.value("zmax").toInt(19)};
    if (s.id.isEmpty() || s.id == noneId() || !s.url.contains("{z}"))
      continue;
    bool dup = false;
    for (const auto &e : out) dup = dup || e.id == s.id;
    if (dup) continue;
    if (s.title.isEmpty()) s.title = s.id;
    out << s;
  }
  return out;
}

Source sourceById(const QString &id)
{
  if (id.isEmpty() || id == noneId()) return {};
  for (const auto &s : sources())
    if (s.id == id) return s;
  return {};
}

bool canOverlay(const QgsCoordinateReferenceSystem &crs, QString *reason)
{
  QString why;
  if (!crs.isValid())
    why = QObject::tr("工程未设置坐标系，无法叠加在线底图，请设置地理坐标系");
  else if (crs.type() == Qgis::CrsType::Engineering)
    why = QObject::tr("工程坐标系无法叠加在线底图，请设置地理坐标系");
  if (reason) *reason = why;
  return why.isEmpty();
}

bool canOverlay(const QgsMapCanvas *canvas, QString *reason)
{
  if (!canvas) {
    if (reason) *reason = QObject::tr("无地图画布");
    return false;
  }
  return canOverlay(canvas->mapSettings().destinationCrs(), reason);
}

void watchFailures(QgsMapCanvas *canvas, QgsMapLayer *context, int timeoutMs,
                   const std::function<void(const QString &)> &onFailure)
{
  if (!canvas || !context) return;
  QObject::connect(canvas, &QgsMapCanvas::renderErrorOccurred, context,
                   [onFailure](const QString &error, QgsMapLayer *layer) {
                     if (layer && !layer->customProperty(QStringLiteral("paleoOnlineBasemap")).toString().isEmpty())
                       onFailure(QObject::tr("在线底图加载失败（网络不可用或被拒绝）：%1").arg(error.left(160)));
                   });
  QPointer<QgsMapCanvas> guard(canvas);
  QTimer::singleShot(timeoutMs, context, [guard, onFailure] {
    if (guard && guard->isDrawing())
      onFailure(QObject::tr("在线底图加载超时，请检查网络；可切换为「无」离线浏览"));
  });
}

QString xyzUri(const Source &source)
{
  // URL 里的 & 等需百分号编码，否则被当成 URI 键分隔符。
  return QStringLiteral("type=xyz&url=%1&zmax=%2&zmin=0")
      .arg(QString::fromLatin1(QUrl::toPercentEncoding(source.url, "/:{}")))
      .arg(source.zmax);
}

QgsMapLayer *createLayer(const Source &source, QObject *parent)
{
  if (source.url.isEmpty()) return nullptr;
  auto *layer = new QgsRasterLayer(xyzUri(source), QObject::tr("在线底图：%1").arg(source.title), QStringLiteral("wms"));
  if (!layer->isValid()) {
    delete layer;
    return nullptr;
  }
  layer->setParent(parent);
  layer->setCustomProperty(QStringLiteral("paleoOnlineBasemap"), source.id);
  layer->setCustomProperty(QStringLiteral("paleoBasemap"), true); // 全图范围不计入（XYZ 范围是全球）
  layer->setCustomProperty(QStringLiteral("paleoBasemapAttribution"), source.attribution);
  return layer;
}

void destroyLayer(QgsMapLayer *layer)
{
  if (layer) layer->deleteLater();
}

QgsMapLayer *findLayer(const QList<QgsMapLayer *> &layers)
{
  for (auto *layer : layers)
    if (layer && !layer->customProperty(QStringLiteral("paleoOnlineBasemap")).toString().isEmpty()) return layer;
  return nullptr;
}

QString savedChoice(const QgsProject *project)
{
  if (project) {
    bool ok = false;
    const QString v = project->readEntry(kScope, kEntry, QString(), &ok);
    if (ok && !v.isEmpty() && v != QLatin1String("xyz")) return v;
  }
  const QString key = projectKey(project);
  return key.isEmpty() ? noneId() : QSettings().value(key, noneId()).toString();
}

void saveChoice(QgsProject *project, const QString &id)
{
  // 自定义 XYZ 地址可能带访问密钥：只记在本机设置，工程文件里只留标记。
  const bool privateUrl = id.startsWith(QLatin1String("xyz:"));
  if (project) project->writeEntry(kScope, kEntry, privateUrl ? QStringLiteral("xyz") : id);
  const QString key = projectKey(project);
  if (!key.isEmpty()) QSettings().setValue(key, id);
}
} // namespace paleo::onlinebasemap
