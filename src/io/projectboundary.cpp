// 层：数据
#include "projectboundary.h"
#include "dataimport_internal.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <memory>
namespace paleo::dataimport_detail {
QString importProjectBoundary(FamilyContext &ctx)
{
  GDALAllRegister();
  auto *ds = static_cast<GDALDataset *>(GDALOpenEx(ctx.sourcePath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  if (!ds) return QObject::tr("无法读取测区范围矢量");
  QVector<CatalogEntity> areas;
  QString error;
  for (int i = 0; i < ds->GetLayerCount() && error.isEmpty(); ++i) {
    auto *layer = ds->GetLayer(i);
    if (!layer) continue;
    std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)> transform(nullptr, OCTDestroyCoordinateTransformation);
    if (layer->GetSpatialRef() && ctx.s->georeference && ctx.s->georeference->kind == QLatin1String("crs")) {
      OGRSpatialReference source(*layer->GetSpatialRef()), destination;
      source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
      if (destination.SetFromUserInput(ctx.s->georeference->sourceCrs.toUtf8().constData()) == OGRERR_NONE) {
        destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        transform.reset(OGRCreateCoordinateTransformation(&source, &destination));
      }
      if (!transform) { error = QObject::tr("测区范围坐标无法转换到工程坐标系"); break; }
    }
    while (auto *feature = layer->GetNextFeature()) {
      auto *g = feature->GetGeometryRef();
      if (g && !g->IsEmpty() && (wkbFlatten(g->getGeometryType()) == wkbPolygon || wkbFlatten(g->getGeometryType()) == wkbMultiPolygon)) {
        if (transform && g->transform(transform.get()) != OGRERR_NONE) error = QObject::tr("测区范围坐标转换失败");
        if (error.isEmpty()) {
          CatalogEntity entity;
          entity.entityType = QStringLiteral("study_area"); entity.name = ctx.stem;
          char *json = g->exportToJson();
          const auto geometry = QJsonDocument::fromJson(QByteArray(json)).object(); CPLFree(json);
          OGREnvelope envelope; g->getEnvelope(&envelope);
          entity.corners = {{envelope.MinX, envelope.MinY}, {envelope.MaxX, envelope.MinY}, {envelope.MaxX, envelope.MaxY}, {envelope.MinX, envelope.MaxY}};
          entity.extra.insert(QStringLiteral("boundaryGeometry"), geometry.toVariantMap());
          entity.extra.insert(QStringLiteral("sourceAsset"), ctx.assetId); areas << entity;
        }
      } else error = QObject::tr("测区范围包含空几何或非面要素，请选择面矢量");
      OGRFeature::DestroyFeature(feature);
      if (!error.isEmpty()) break;
    }
  }
  GDALClose(ds);
  if (!error.isEmpty()) return error;
  if (areas.isEmpty()) return QObject::tr("测区范围需要有效的面矢量");
  for (auto &entity : areas) {
    entity.id = ctx.cat->nextEntityId(entity.entityType);
    if (!ctx.cat->addEntity(entity, ctx.error)) return *ctx.error;
    EntityAssetLink link; link.entityType = entity.entityType; link.entityId = entity.id;
    link.assetId = ctx.assetId; link.role = QStringLiteral("boundary"); link.isPrimary = true;
    if (!ctx.cat->addLink(link, ctx.error)) return *ctx.error;
  }
  return {};
}
}
