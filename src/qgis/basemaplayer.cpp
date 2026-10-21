// 层：QGIS 封装
#include "basemaplayer.h"

#include "onlinebasemap.h"
#include "projectmapreference.h"

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsexception.h>
#include <qgslayertree.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

#include <QFileInfo>
#include <QObject>
#include <QUrl>

#include <cmath>
#include <memory>

namespace paleo::basemap {
namespace {
const QString kXyz = QStringLiteral("xyz:");
const QString kFile = QStringLiteral("file:");
const QString kLayer = QStringLiteral("layer:");

void setError(QString *error, const QString &text)
{
  if (error) *error = text;
}

bool isXyzLayer(const QgsMapLayer *layer)
{
  return layer && ((layer->providerType() == QLatin1String("wms") &&
                    layer->source().contains(QLatin1String("type=xyz"))) ||
                   (layer->providerType() == QLatin1String("gdal") &&
                    layer->source().startsWith(QLatin1String("<GDAL_WMS>"))));
}
} // namespace

Choice decode(const QString &spec)
{
  if (spec.isEmpty() || spec == onlinebasemap::noneId()) return {};
  if (spec.startsWith(kXyz)) return {Kind::Xyz, spec.mid(kXyz.size())};
  if (spec.startsWith(kFile)) return {Kind::File, spec.mid(kFile.size())};
  if (spec.startsWith(kLayer)) return {Kind::ProjectLayer, spec.mid(kLayer.size())};
  return {Kind::Preset, spec};
}

QString encode(const Choice &choice)
{
  switch (choice.kind) {
  case Kind::None: return onlinebasemap::noneId();
  case Kind::Preset: return choice.value;
  case Kind::Xyz: return kXyz + choice.value;
  case Kind::File: return kFile + choice.value;
  case Kind::ProjectLayer: return kLayer + choice.value;
  }
  return onlinebasemap::noneId();
}

bool isValidXyzUrl(const QString &url, QString *why)
{
  const QString t = url.trimmed();
  const QUrl parsed(t);
  const QString scheme = parsed.scheme().toLower();
  QString reason;
  if (t.isEmpty())
    reason = QObject::tr("瓦片地址为空");
  else if (scheme != QLatin1String("http") && scheme != QLatin1String("https") && scheme != QLatin1String("file"))
    reason = QObject::tr("瓦片地址须以 http://、https:// 或 file:// 开头");
  else if (!t.contains(QLatin1String("{x}")) || !t.contains(QLatin1String("{y}")) || !t.contains(QLatin1String("{z}")))
    reason = QObject::tr("瓦片地址须包含 {x}、{y}、{z} 占位符");
  if (why) *why = reason;
  return reason.isEmpty();
}

QString displayName(const Choice &choice, const QgsProject *project)
{
  switch (choice.kind) {
  case Kind::None: return QObject::tr("无");
  case Kind::Preset: {
    const auto s = onlinebasemap::sourceById(choice.value);
    return s.title.isEmpty() ? choice.value : s.title;
  }
  case Kind::Xyz: {
    const QUrl url(choice.value);
    const QString host = url.scheme() == QLatin1String("file") ? QObject::tr("本地瓦片") : url.host();
    return QObject::tr("自定义瓦片（%1）").arg(host.isEmpty() ? QObject::tr("未知主机") : host);
  }
  case Kind::File: return QFileInfo(choice.value).fileName();
  case Kind::ProjectLayer: {
    const auto *layer = project ? project->mapLayer(choice.value) : nullptr;
    return layer ? layer->name() : QObject::tr("工程图层");
  }
  }
  return {};
}

void mark(QgsMapLayer *layer, const QString &id, const QString &attribution)
{
  if (!layer) return;
  layer->setCustomProperty(QStringLiteral("paleoOnlineBasemap"), id);
  layer->setCustomProperty(QStringLiteral("paleoBasemap"), true); // 全图范围不计入底图
  layer->setCustomProperty(QStringLiteral("paleoBasemapAttribution"), attribution);
}

QgsMapLayer *createXyz(const QString &url, QObject *parent, QString *error)
{
  QString why;
  if (!isValidXyzUrl(url, &why)) {
    setError(error, why);
    return nullptr;
  }
  onlinebasemap::Source source;
  source.id = encode({Kind::Xyz, url.trimmed()});
  source.url = url.trimmed();
  source.title = displayName({Kind::Xyz, source.url});
  auto *layer = onlinebasemap::createLayer(source, parent, error);
  if (layer) mark(layer, source.id, source.title);
  return layer;
}

QgsMapLayer *createFromFile(const QString &path, QObject *parent, QString *error)
{
  const QFileInfo info(path);
  if (!info.isFile()) {
    setError(error, QObject::tr("底图文件不存在：%1").arg(info.fileName()));
    return nullptr;
  }
  const QString abs = info.absoluteFilePath();
  std::unique_ptr<QgsMapLayer> layer;
  if (info.suffix().compare(QLatin1String("mbtiles"), Qt::CaseInsensitive) == 0) {
    layer.reset(paleo::mapreference::offlineBasemap(abs, info.completeBaseName()));
  } else {
    // 先按栅格试读，失败再按矢量；两者都不行才报错。
    QgsRasterLayer::LayerOptions ro;
    ro.skipCrsValidation = true;
    layer = std::make_unique<QgsRasterLayer>(abs, info.completeBaseName(), QStringLiteral("gdal"), ro);
    if (!layer->isValid()) {
      QgsVectorLayer::LayerOptions vo;
      vo.skipCrsValidation = true;
      layer = std::make_unique<QgsVectorLayer>(abs, info.completeBaseName(), QStringLiteral("ogr"), vo);
    }
  }
  if (!layer || !layer->isValid()) {
    setError(error, QObject::tr("无法读取底图文件（不是 GDAL/OGR 支持的栅格或矢量）：%1").arg(info.fileName()));
    return nullptr;
  }
  const QString attribution = layer->customProperty("paleoBasemapAttribution").toString();
  layer->setParent(parent);
  mark(layer.get(), encode({Kind::File, abs}),
       attribution.isEmpty() ? QObject::tr("本地底图：%1").arg(info.fileName()) : attribution);
  return layer.release();
}

QgsMapLayer *cloneProjectLayer(const QgsMapLayer *source, QObject *parent, QString *error)
{
  if (!source || !source->isValid()) {
    setError(error, QObject::tr("工程图层不存在或已失效"));
    return nullptr;
  }
  QgsMapLayer *layer = source->clone();
  if (!layer || !layer->isValid()) {
    delete layer;
    setError(error, QObject::tr("工程图层无法复制：%1").arg(source->name()));
    return nullptr;
  }
  layer->setParent(parent);
  const QString attribution = source->customProperty("paleoBasemapAttribution").toString();
  mark(layer, encode({Kind::ProjectLayer, source->id()}),
       attribution.isEmpty() ? QObject::tr("工程图层：%1").arg(source->name()) : attribution);
  return layer;
}

QList<QgsMapLayer *> projectCandidates(const QgsProject *project)
{
  QList<QgsMapLayer *> out;
  if (!project) return out;
  for (auto *node : project->layerTreeRoot()->findLayers()) {
    auto *layer = node->layer();
    if (!layer || !layer->isValid()) continue;
    if (layer->type() != Qgis::LayerType::Raster && layer->type() != Qgis::LayerType::Vector) continue;
    out << layer;
  }
  return out;
}

bool checkCrs(QgsMapLayer *layer, const QgsCoordinateReferenceSystem &canvasCrs,
              const QgsCoordinateTransformContext &context, QString *message)
{
  QString text;
  bool ok = true;
  if (!layer) {
    text = QObject::tr("底图无效");
    ok = false;
  } else if (isXyzLayer(layer)) {
    ok = onlinebasemap::canOverlay(canvasCrs, &text);
  } else if (!layer->crs().isValid()) {
    if (canvasCrs.isValid()) layer->setCrs(canvasCrs);
    text = QObject::tr("底图「%1」没有坐标系，已按画布坐标系直接叠加，位置可能不准；"
                       "请为底图文件补充坐标系（.prj/.aux.xml）")
               .arg(layer->name());
  } else if (!canvasCrs.isValid()) {
    text = QObject::tr("当前画布没有坐标系，底图按原始坐标叠加，未做投影转换");
  } else if (layer->crs() != canvasCrs) {
    const QgsCoordinateTransform ct(layer->crs(), canvasCrs, context);
    bool transformed = ct.isValid();
    if (transformed) {
      try {
        const auto box = ct.transformBoundingBox(layer->extent());
        transformed = !box.isNull() && std::isfinite(box.xMinimum()) && std::isfinite(box.yMaximum());
      } catch (const QgsCsException &) {
        transformed = false;
      }
    }
    if (transformed) {
      text = QObject::tr("底图坐标系 %1 已实时重投影到 %2")
                 .arg(layer->crs().userFriendlyIdentifier(), canvasCrs.userFriendlyIdentifier());
    } else {
      ok = false;
      text = QObject::tr("底图坐标系 %1 无法转换到画布坐标系 %2（工程坐标系未配准？），已拒绝叠加")
                 .arg(layer->crs().userFriendlyIdentifier(), canvasCrs.userFriendlyIdentifier());
    }
  }
  if (message) *message = text;
  return ok;
}

QgsMapLayer *build(const QString &spec, QgsMapCanvas *canvas, QObject *projectObject, QObject *parent,
                   QString *message)
{
  auto *project = qobject_cast<QgsProject *>(projectObject);
  const Choice choice = decode(spec);
  QString error;
  QgsMapLayer *layer = nullptr;
  switch (choice.kind) {
  case Kind::None: break;
  case Kind::Preset: {
    const auto source = onlinebasemap::sourceById(choice.value);
    if (source.url.isEmpty())
      error = QObject::tr("在线底图源无效：%1").arg(choice.value);
    else if (onlinebasemap::canOverlay(canvas, &error))
      layer = onlinebasemap::createLayer(source, parent, &error);
    if (!layer && error.isEmpty()) error = QObject::tr("在线底图源无效：%1").arg(source.title);
    break;
  }
  case Kind::Xyz: layer = createXyz(choice.value, parent, &error); break;
  case Kind::File: layer = createFromFile(choice.value, parent, &error); break;
  case Kind::ProjectLayer:
    layer = cloneProjectLayer(project ? project->mapLayer(choice.value) : nullptr, parent, &error);
    break;
  }
  if (layer) {
    QString note;
    const bool ok = canvas && checkCrs(layer, canvas->mapSettings().destinationCrs(),
                                       canvas->mapSettings().transformContext(), &note);
    if (!ok) {
      error = canvas ? note : QObject::tr("无地图画布");
      delete layer; // 尚未进画布
      layer = nullptr;
    } else {
      error = note;
    }
  }
  if (message) *message = error;
  return layer;
}

QString displayName(const QString &spec, QObject *project)
{
  return displayName(decode(spec), qobject_cast<QgsProject *>(project));
}

QList<QPair<QString, QString>> projectCandidateNames(QObject *project)
{
  QList<QPair<QString, QString>> out;
  for (auto *layer : projectCandidates(qobject_cast<QgsProject *>(project))) out.append({layer->id(), layer->name()});
  return out;
}

QString savedChoice(QObject *project) { return onlinebasemap::savedChoice(qobject_cast<QgsProject *>(project)); }

void saveChoice(QObject *project, const QString &spec)
{
  if (auto *p = qobject_cast<QgsProject *>(project)) onlinebasemap::saveChoice(p, spec);
}

void onCanvasCrsChanged(QgsMapCanvas *canvas, QObject *context, const std::function<void()> &callback)
{
  if (canvas && context) QObject::connect(canvas, &QgsMapCanvas::destinationCrsChanged, context, callback);
}
} // namespace paleo::basemap
