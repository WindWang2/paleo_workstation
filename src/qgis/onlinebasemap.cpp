// 层：QGIS 封装
#include "onlinebasemap.h"

#include <qgscoordinatereferencesystem.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsdatasourceuri.h>
#include <qgshttpheaders.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsnetworkaccessmanager.h>
#include <qgsproviderregistry.h>
#include <qgsmaprendererparalleljob.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamWriter>
#include <QEvent>
#include <QLabel>
#include <QPixmap>

namespace paleo::onlinebasemap {
namespace {
const char *kSourcesKey = "basemap/onlineSources";
const char *kScope = "paleo";
const char *kEntry = "survey/onlineBasemap";
const QString kUserAgent = QStringLiteral("PaleoWorkbench (+https://github.com/WindWang2/paleo_workstation)");

// 原生整图的标注阶段要等所有图层完成，慢瓦片会连井点/井名一起拖住。
// 临时前景仍完全交给 QGIS 渲染；透明结果只在整图尚未完成时展示。
class ResponsiveForeground final : public QObject
{
public:
  explicit ResponsiveForeground(QgsMapCanvas *canvas) : QObject(canvas), m_canvas(canvas)
  {
    setObjectName(QStringLiteral("responsiveForegroundRenderer"));
    m_overlay = new QLabel(canvas->viewport());
    m_overlay->setObjectName(QStringLiteral("responsiveForegroundOverlay"));
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_overlay->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_overlay->setScaledContents(true);
    m_overlay->hide();
    canvas->viewport()->installEventFilter(this);
    connect(canvas, &QgsMapCanvas::renderStarting, this, [this] { start(); });
    connect(canvas, &QgsMapCanvas::mapCanvasRefreshed, this, [this] { invalidate(); });
    connect(canvas, &QgsMapCanvas::extentsChanged, this, [this] { start(); });
    connect(canvas, &QgsMapCanvas::layersChanged, this, [this] { start(); });
    connect(canvas, &QgsMapCanvas::rotationChanged, this, [this] { start(); });
    connect(canvas, &QgsMapCanvas::destinationCrsChanged, this, [this] { start(); });
  }

  ~ResponsiveForeground() override
  {
    invalidate();
    delete m_overlay.data();
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override
  {
    if (event->type() == QEvent::Resize || event->type() == QEvent::Hide)
      invalidate(); // 不把上一视野的井点图拉伸到新范围。
    return QObject::eventFilter(object, event);
  }

private:
  void invalidate()
  {
    ++m_generation;
    if (m_overlay) m_overlay->hide();
    if (m_job) {
      auto *old = m_job.data();
      m_job = nullptr;
      old->cancelWithoutBlocking();
    }
  }

  void start()
  {
    invalidate();
    bool online = false;
    QList<QgsMapLayer *> foreground;
    for (auto *layer : m_canvas->layers()) {
      online = online || !layer->customProperty(QStringLiteral("paleoOnlineBasemap")).toString().isEmpty();
      if (layer->type() == Qgis::LayerType::Vector && !layer->customProperty(QStringLiteral("paleoBasemap")).toBool())
        foreground.append(layer);
    }
    if (!online || foreground.isEmpty()) return;
    auto settings = m_canvas->mapSettings();
    if (settings.outputSize().isEmpty()) return;
    settings.setLayers(foreground);
    settings.setBackgroundColor(Qt::transparent);
    auto *job = new QgsMapRendererParallelJob(settings);
    job->setParent(this);
    m_job = job;
    const auto generation = m_generation;
    connect(job, &QgsMapRendererJob::finished, this, [this, job, generation] {
      if (generation == m_generation && m_overlay && m_canvas->isDrawing()) {
        m_overlay->setPixmap(QPixmap::fromImage(job->renderedImage()));
        m_overlay->setGeometry(m_canvas->viewport()->rect());
        m_overlay->raise();
        m_overlay->show();
      }
      if (m_job == job) m_job = nullptr;
      job->deleteLater();
    });
    job->start();
  }

  QgsMapCanvas *m_canvas;
  QPointer<QLabel> m_overlay;
  QPointer<QgsMapRendererParallelJob> m_job;
  quint64 m_generation = 0;
};

QString gdalXyzUri(const Source &source)
{
  QString xml;
  QXmlStreamWriter writer(&xml);
  writer.writeStartElement(QStringLiteral("GDAL_WMS"));
  writer.writeStartElement(QStringLiteral("Service"));
  writer.writeAttribute(QStringLiteral("name"), QStringLiteral("TMS"));
  QString url = source.url;
  for (const auto &axis : {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")})
    url.replace(QLatin1Char('{') + axis + QLatin1Char('}'), QStringLiteral("${%1}").arg(axis));
  writer.writeTextElement(QStringLiteral("ServerUrl"), url);
  writer.writeEndElement();
  writer.writeStartElement(QStringLiteral("DataWindow"));
  writer.writeTextElement(QStringLiteral("UpperLeftX"), QStringLiteral("-20037508.342789244"));
  writer.writeTextElement(QStringLiteral("UpperLeftY"), QStringLiteral("20037508.342789244"));
  writer.writeTextElement(QStringLiteral("LowerRightX"), QStringLiteral("20037508.342789244"));
  writer.writeTextElement(QStringLiteral("LowerRightY"), QStringLiteral("-20037508.342789244"));
  writer.writeTextElement(QStringLiteral("TileLevel"), QString::number(source.zmax));
  writer.writeTextElement(QStringLiteral("TileCountX"), QStringLiteral("1"));
  writer.writeTextElement(QStringLiteral("TileCountY"), QStringLiteral("1"));
  writer.writeTextElement(QStringLiteral("YOrigin"), QStringLiteral("top"));
  writer.writeEndElement();
  writer.writeTextElement(QStringLiteral("Projection"), QStringLiteral("EPSG:3857"));
  writer.writeTextElement(QStringLiteral("BlockSizeX"), QStringLiteral("256"));
  writer.writeTextElement(QStringLiteral("BlockSizeY"), QStringLiteral("256"));
  writer.writeTextElement(QStringLiteral("BandsCount"), QStringLiteral("4"));
  writer.writeTextElement(QStringLiteral("OverviewCount"), QString::number(source.zmax));
  writer.writeTextElement(QStringLiteral("UserAgent"), kUserAgent);
  writer.writeTextElement(QStringLiteral("MaxConnections"), QStringLiteral("2"));
  writer.writeTextElement(QStringLiteral("Timeout"), QStringLiteral("15"));
  writer.writeTextElement(QStringLiteral("ZeroBlockHttpCodes"), QStringLiteral("204,404"));
  writer.writeStartElement(QStringLiteral("Cache"));
  // GDAL 原生缓存（默认用户缓存目录），至少 7 天；不预取瓦片。
  writer.writeTextElement(QStringLiteral("Expires"), QStringLiteral("604800"));
  writer.writeEndElement();
  writer.writeEndElement();
  return xml;
}

QString projectKey(const QgsProject *project)
{
  const QString path = project ? project->absoluteFilePath() : QString();
  if (path.isEmpty())
    return {};
  return QStringLiteral("basemap/choice/%1")
      .arg(QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)));
}
} // namespace

void enableResponsiveForeground(QgsMapCanvas *canvas)
{
  if (canvas && !canvas->findChild<QObject *>(QStringLiteral("responsiveForegroundRenderer"), Qt::FindDirectChildrenOnly))
    new ResponsiveForeground(canvas);
}

QString noneId() { return QStringLiteral("none"); }

QVector<Source> sources()
{
  QVector<Source> out{
      {QStringLiteral("osm"), QObject::tr("街道（OpenStreetMap）"), QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
       QObject::tr("© OpenStreetMap 贡献者（ODbL）"), 19,
       QObject::tr("开放街道数据，适合道路与地名参考；仅按当前视野浏览，不批量下载。"),
       QStringLiteral("https://operations.osmfoundation.org/policies/tiles/")},
      {QStringLiteral("esri_imagery"), QObject::tr("卫星影像（Esri，受使用条款约束）"),
       QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"),
       QObject::tr("影像 © Esri, Vantor, Earthstar Geographics 及 GIS 用户社区（遵守 Esri 使用条款）"), 19,
       QObject::tr("全球卫星与航空影像背景；按 Esri 条款使用。网络可达性取决于当前线路。"),
       QStringLiteral("https://www.esri.com/en-us/legal/terms/full-master-agreement")},
      {QStringLiteral("esri_street"), QObject::tr("街道（Esri）"),
       QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}"),
       QObject::tr("© Esri, HERE, Garmin, USGS, OpenStreetMap 贡献者及 GIS 用户社区"), 19,
       QObject::tr("道路、城市与地名参考；按 Esri 条款使用。网络可达性取决于当前线路。"),
       QStringLiteral("https://doc.arcgis.com/zh-cn/arcgis-online/reference/access-use-constraints.htm")},
      {QStringLiteral("esri_topo"), QObject::tr("地形（Esri）"),
       QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}"),
       QObject::tr("© Esri, HERE, Garmin, GEBCO, USGS, OpenStreetMap 贡献者及 GIS 用户社区"), 19,
       QObject::tr("带地名与道路的地形背景；按 Esri 条款使用。网络可达性取决于当前线路。"),
       QStringLiteral("https://doc.arcgis.com/zh-cn/arcgis-online/reference/access-use-constraints.htm")},
      {QStringLiteral("esri_light_gray"), QObject::tr("浅灰底图（Esri）"),
       QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Light_Gray_Base/MapServer/tile/{z}/{y}/{x}"),
       QObject::tr("© Esri, HERE, Garmin, OpenStreetMap 贡献者及 GIS 用户社区"), 19,
       QObject::tr("低干扰浅灰背景，便于叠加彩色相图；按 Esri 条款使用。"),
       QStringLiteral("https://doc.arcgis.com/zh-cn/arcgis-online/reference/access-use-constraints.htm")},
      {QStringLiteral("esri_dark_gray"), QObject::tr("深灰底图（Esri）"),
       QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}"),
       QObject::tr("© Esri, HERE, Garmin, OpenStreetMap 贡献者及 GIS 用户社区"), 19,
       QObject::tr("低干扰深灰背景，适合突出井点与线条；按 Esri 条款使用。"),
       QStringLiteral("https://doc.arcgis.com/zh-cn/arcgis-online/reference/access-use-constraints.htm")},
      {QStringLiteral("esri_relief"), QObject::tr("地形晕渲（Esri）"),
       QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Shaded_Relief/MapServer/tile/{z}/{y}/{x}"),
       QObject::tr("地形晕渲 © Esri"), 13,
       QObject::tr("全球地貌晕渲背景，原生最高 13 级；按 Esri 条款使用。"),
       QStringLiteral("https://doc.arcgis.com/zh-cn/arcgis-online/reference/access-use-constraints.htm")},
      {QStringLiteral("opentopomap"), QObject::tr("地形（OpenTopoMap）"), QStringLiteral("https://tile.opentopomap.org/{z}/{x}/{y}.png"),
       QObject::tr("© OpenStreetMap 贡献者, SRTM | 样式 © OpenTopoMap（CC-BY-SA）"), 17,
       QObject::tr("含等高线与地名的地形图，适合地貌参考。"), QStringLiteral("https://opentopomap.org/about")},
      {QStringLiteral("nasa_blue_marble"), QObject::tr("NASA 全球真彩影像（概览）"),
       QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/BlueMarble_NextGeneration/default/GoogleMapsCompatible_Level8/{z}/{y}/{x}.jpeg"),
       QObject::tr("影像：NASA GIBS / ESDIS，Blue Marble（MODIS）"), 8,
       QObject::tr("全球拼接影像，适合区域概览；原生最高 8 级，不是实时或高分辨率卫星影像。"),
       QStringLiteral("https://nasa-gibs.github.io/gibs-api-docs/#data-use-guidance-and-acknowledgements")},
      {QStringLiteral("nasa_relief_bathymetry"), QObject::tr("NASA 全球地形与海底地貌（概览）"),
       QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/BlueMarble_ShadedRelief_Bathymetry/default/GoogleMapsCompatible_Level8/{z}/{y}/{x}.jpeg"),
       QObject::tr("影像：NASA GIBS / ESDIS，Blue Marble 地形与海底地貌"), 8,
       QObject::tr("全球地形晕渲与海底地貌，适合盆地及陆海背景；原生最高 8 级。"),
       QStringLiteral("https://nasa-gibs.github.io/gibs-api-docs/#data-use-guidance-and-acknowledgements")},
      {QStringLiteral("nasa_aster_color"), QObject::tr("NASA ASTER 彩色地形"),
       QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/ASTER_GDEM_Color_Shaded_Relief/default/GoogleMapsCompatible_Level12/{z}/{y}/{x}.jpeg"),
       QObject::tr("影像：NASA GIBS / ESDIS，ASTER GDEM（METI / NASA）"), 12,
       QObject::tr("彩色高程晕渲，适合地貌与构造背景；原生最高 12 级，覆盖范围外可能无数据。"),
       QStringLiteral("https://nasa-gibs.github.io/gibs-api-docs/#data-use-guidance-and-acknowledgements")},
      {QStringLiteral("nasa_aster_gray"), QObject::tr("NASA ASTER 灰度地形"),
       QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/ASTER_GDEM_Greyscale_Shaded_Relief/default/GoogleMapsCompatible_Level12/{z}/{y}/{x}.jpeg"),
       QObject::tr("影像：NASA GIBS / ESDIS，ASTER GDEM（METI / NASA）"), 12,
       QObject::tr("灰度地形晕渲，便于叠加彩色地质图件；原生最高 12 级，覆盖范围外可能无数据。"),
       QStringLiteral("https://nasa-gibs.github.io/gibs-api-docs/#data-use-guidance-and-acknowledgements")}};
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
  // 主图重复选择同一预设会复用图层，不重复安装失败通知。
  if (context->property("paleoBasemapFailureWatchInstalled").toBool()) return;
  context->setProperty("paleoBasemapFailureWatchInstalled", true);
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
  QgsDataSourceUri uri;
  uri.setParam(QStringLiteral("type"), QStringLiteral("xyz"));
  uri.setParam(QStringLiteral("crs"), QStringLiteral("EPSG:3857"));
  uri.setParam(QStringLiteral("format"), QString());
  uri.setParam(QStringLiteral("url"), source.url);
  uri.setParam(QStringLiteral("zmax"), QString::number(source.zmax));
  uri.setParam(QStringLiteral("zmin"), QStringLiteral("0"));
  uri.setHttpHeaders(QgsHttpHeaders(QVariantMap{{QStringLiteral("User-Agent"),
      kUserAgent}}));
  return QString::fromUtf8(uri.encodedUri());
}

QgsMapLayer *createLayer(const Source &source, QObject *parent, QString *error)
{
  if (source.url.isEmpty()) return nullptr;
  auto *network = QgsNetworkAccessManager::instance();
  if (!network->cache()) network->setupDefaultProxyAndCache();
  const auto title = QObject::tr("在线底图：%1").arg(source.title);
  // file:// 模板若走 wms/xyz，层可以 isValid 但不同步解本地瓦片，block() 得到 nan。
  const bool localTemplate = source.url.startsWith(QLatin1String("file:"), Qt::CaseInsensitive);
  auto *layer = !localTemplate && QgsProviderRegistry::instance()->providerMetadata(QStringLiteral("wms"))
      ? new QgsRasterLayer(xyzUri(source), title, QStringLiteral("wms")) : nullptr;
  // 裁剪的 vendored QGIS 可能没有 WMS 插件；由 QGIS 的 GDAL provider
  // 驱动原生 TMS/缓存，不在应用层实现瓦片请求或地图渲染。
  if (!layer || !layer->isValid()) {
    delete layer;
    layer = new QgsRasterLayer(gdalXyzUri(source), title, QStringLiteral("gdal"));
  }
  if (!layer->isValid()) {
    if (error) *error = QObject::tr("在线瓦片图层无法创建：%1").arg(layer->error().summary());
    delete layer;
    return nullptr;
  }
  layer->setParent(parent);
  layer->setCustomProperty(QStringLiteral("paleoOnlineBasemap"), source.id);
  layer->setCustomProperty(QStringLiteral("paleoBasemap"), true); // 全图范围不计入（XYZ 范围是全球）
  layer->setCustomProperty(QStringLiteral("paleoBasemapAttribution"), source.attribution);
  return layer;
}

QgsMapLayer *addPresetToProject(QgsProject *project, const QString &id, QString *error)
{
  QString reason;
  if (!project || !canOverlay(project->crs(), &reason)) {
    if (error) *error = project ? reason : QObject::tr("请先打开工程");
    return nullptr;
  }
  const auto source = sourceById(id);
  if (source.id.isEmpty()) {
    if (error) *error = QObject::tr("在线底图预设不存在：%1").arg(id);
    return nullptr;
  }
  QgsMapLayer *selected = nullptr;
  for (auto *layer : project->mapLayers())
    if (layer->customProperty(QStringLiteral("paleoOnlineBasemap")).toString() == id) {
      selected = layer;
      break;
    }
  auto *root = project->layerTreeRoot();
  if (!selected) {
    selected = createLayer(source, nullptr, error);
    if (!selected) {
      return nullptr;
    }
    project->addMapLayer(selected, false);
    root->addLayer(selected);
    if (root->hasCustomLayerOrder()) {
      auto order = root->customLayerOrder();
      order.removeAll(selected);
      order.append(selected);
      root->setCustomLayerOrder(order);
    }
  }
  // 选择在线底图时保留其它底图供切换，只关闭它们的显示。
  for (auto *node : root->findLayers())
    if (node->layer() && node->layer()->customProperty(QStringLiteral("paleoBasemap")).toBool())
      node->setItemVisibilityChecked(node->layer() == selected);
  if (auto *node = root->findLayer(selected->id()))
    node->setItemVisibilityCheckedParentRecursive(true);
  saveChoice(project, id);
  return selected;
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
