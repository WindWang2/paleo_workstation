// 层：QGIS 封装
#include "agentmapnavigation.h"

#include "../catalog/datacatalog.h"
#include "projectmapreference.h"
#include "qgiscanvascontroller.h"
#include "qgislayerservice.h"
#include "qgisprojectservice.h"

#include <qgscoordinatereferencesystem.h>
#include <qgslayertree.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsrectangle.h>
#include <qgsvectorlayer.h>

#include <QJsonArray>

#include <cmath>

// qgis/agentmapnavigation.cpp — I4：MapAccess 生产实现。
//
// 严格 CRS 纪律（W4）：所有目标坐标先经 paleo::mapreference::transformPoint/
// mapExtent 用画布 transform context 变换到目标 CRS；任何一步失败（非有限、
// CRS 无效、无可用转换、空/不合理范围）都在 apply 之前拒绝——旧视野不动。
// setExtent 后回读实际 extent/CRS 并核含目标包络；可见性如实报告（隐藏
// 目标仍隐藏——本类绝不改 layer tree 可见性，那是 I3 的显示域）。

namespace {
constexpr double kPointBufferMeters = 500.0;   // 零宽点目标的适度缓冲（每侧 500m）
constexpr double kFitMargin = 1.08;            // fit 外扩比例（与 zoomToLayer 语义一致）
constexpr double kMaxSaneExtent = 1e8;         // 不合理范围上限（与全图缩放同口径）

QJsonObject merged(QJsonObject base, const QJsonObject &addition) {
  for (auto it = addition.constBegin(); it != addition.constEnd(); ++it)
    base.insert(it.key(), it.value());
  return base;
}

QJsonArray extentToJson(const QgsRectangle &extent) {
  return {extent.xMinimum(), extent.yMinimum(), extent.xMaximum(), extent.yMaximum()};
}

bool saneExtent(const QgsRectangle &extent) {
  return extent.isFinite() && !extent.isNull() &&
         std::abs(extent.width()) <= kMaxSaneExtent &&
         std::abs(extent.height()) <= kMaxSaneExtent;
}

QString crsLabel(const QgsCoordinateReferenceSystem &crs) {
  if (!crs.isValid())
    return QStringLiteral("invalid");
  const QString authid = crs.authid();
  return authid.isEmpty() ? crs.description() : authid;
}
} // namespace

AgentMapNavigation::AgentMapNavigation(QgisCanvasController *canvasCtl,
                                       QgisLayerService *layerSvc,
                                       QgisProjectService *projectSvc, QObject *parent)
  : QObject(parent), m_canvasCtl(canvasCtl), m_layerSvc(layerSvc), m_projectSvc(projectSvc)
{
}

QString AgentMapNavigation::unavailabilityReason() const
{
  if (m_canvasCtl.isNull())
    return QStringLiteral("map_provider_unavailable");
  return QString();
}

QgsMapCanvas *AgentMapNavigation::existingCanvas() const
{
  return m_canvasCtl ? m_canvasCtl->canvasIfCreated() : nullptr;
}

bool AgentMapNavigation::layerTreeVisible(const QString &manifestLayerId) const
{
  // 受管图层可见性归 layer tree 管（QGIS 4 语义）：node->isVisible() 已含
  // 隐藏父组的传递。层未实例化 = 未上画布，按不可见如实报告。
  if (m_layerSvc.isNull() || manifestLayerId.isEmpty())
    return false;
  QgsMapLayer *layer = m_layerSvc->layer(manifestLayerId);
  if (!layer)
    return false;
  if (m_projectSvc && m_projectSvc->project()) {
    if (auto *node = m_projectSvc->project()->layerTreeRoot()->findLayer(layer->id()))
      return node->isVisible();
  }
  return false;
}

QJsonObject AgentMapNavigation::snapshot() const
{
  QJsonObject facts;
  QgsMapCanvas *canvas = existingCanvas();
  facts.insert(QStringLiteral("available"), bool(canvas));
  facts.insert(QStringLiteral("canvas_present"), bool(canvas));
  if (!canvas)
    return facts;
  const QgsRectangle extent = canvas->extent();
  facts.insert(QStringLiteral("extent"), extentToJson(extent));
  facts.insert(QStringLiteral("crs"), crsLabel(canvas->mapSettings().destinationCrs()));
  facts.insert(QStringLiteral("has_layers"), !canvas->layers().isEmpty());
  return facts;
}

QJsonObject AgentMapNavigation::applyLocate(const QgsRectangle &target, bool fit,
                                            bool targetVisible)
{
  QgsMapCanvas *canvas = existingCanvas();
  if (!canvas)
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("code"), QStringLiteral("map_canvas_unavailable")}};
  if (!saneExtent(target))
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("code"), QStringLiteral("invalid_target_extent")}};
  const QgsRectangle before = canvas->extent();
  QgsRectangle next = target;
  if (fit) {
    next.scale(kFitMargin);
  } else {
    // center：保留当前视野宽高（用户选好的比例尺）；视野无效/不合理时退
    // 目标包络外扩一档——不动旧视野的前提下给出有意义的窗口。
    const QgsRectangle current = canvas->extent();
    if (current.isFinite() && !current.isNull() &&
        std::abs(current.width()) <= kMaxSaneExtent &&
        std::abs(current.height()) <= kMaxSaneExtent) {
      const QgsPointXY center = target.center();
      const double halfWidth = current.width() / 2, halfHeight = current.height() / 2;
      next = QgsRectangle(center.x() - halfWidth, center.y() - halfHeight,
                          center.x() + halfWidth, center.y() + halfHeight);
    } else {
      next.scale(kFitMargin);
    }
  }
  if (!saneExtent(next))
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("code"), QStringLiteral("invalid_target_extent")}};
  canvas->setExtent(next);
  canvas->refresh();
  const QgsRectangle actual = canvas->extent();
  // 回读核含：实际视野须覆盖目标包络（含浮点容差），否则按失败回报且不
  // 假称已定位（setExtent 同步生效，此断言防未来管线漂移）。
  if (!actual.contains(target.buffered(std::max(1e-6, std::abs(target.width()) * 1e-9)))) {
    // 审查修复（I4-R2）：防御性回读失败时还原旧视野——失败结果不得伴随
    // 已移动的画布（同步 setExtent 正常不可达此分支，防未来管线漂移）。
    canvas->setExtent(before);
    canvas->refresh();
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("code"), QStringLiteral("extent_apply_failed")},
            {QStringLiteral("extent"), extentToJson(canvas->extent())},
            {QStringLiteral("crs"), crsLabel(canvas->mapSettings().destinationCrs())}};
  }
  QJsonObject facts;
  facts.insert(QStringLiteral("extent"), extentToJson(actual));
  facts.insert(QStringLiteral("crs"), crsLabel(canvas->mapSettings().destinationCrs()));
  facts.insert(QStringLiteral("target_visible"), targetVisible);
  if (!targetVisible)
    facts.insert(QStringLiteral("code"), QStringLiteral("located_not_visible"));
  facts.insert(QStringLiteral("ok"), true);
  return facts;
}

QJsonObject AgentMapNavigation::locateWells(
    const QVector<paleo::agent::navigation::MapAccess::WellTarget> &wells, bool fit)
{
  QJsonObject base{{QStringLiteral("ok"), false},
                   {QStringLiteral("target_count"), wells.size()},
                   {QStringLiteral("mode"), fit ? QStringLiteral("fit")
                                                : QStringLiteral("center")}};
  QgsMapCanvas *canvas = existingCanvas();
  if (!canvas)
    return merged(base, {{QStringLiteral("code"),
                                QStringLiteral("map_canvas_unavailable")}});
  if (wells.isEmpty())
    return merged(base, {{QStringLiteral("code"), QStringLiteral("no_targets")}});
  const QgsCoordinateReferenceSystem local =
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  if (!local.isValid())
    return merged(base, {{QStringLiteral("code"),
                                QStringLiteral("source_crs_unavailable")}});
  const QgsCoordinateReferenceSystem dest = canvas->mapSettings().destinationCrs();
  // 逐点严格变换（工程 transform context；失败即拒绝，旧视野不动）。
  QgsRectangle envelope;
  QStringList resolved;
  for (const auto &well : wells) {
    if (!std::isfinite(well.x) || !std::isfinite(well.y))
      return merged(base, {{QStringLiteral("code"),
                                  QStringLiteral("invalid_coordinates")}});
    QgsPointXY mapped;
    if (!paleo::mapreference::transformPoint(canvas, QgsPointXY(well.x, well.y), local, dest,
                                             &mapped))
      return merged(base, {{QStringLiteral("code"),
                                  QStringLiteral("coordinate_transform_failed")}});
    envelope.setXMinimum(std::min(envelope.xMinimum(), mapped.x()));
    envelope.setXMaximum(std::max(envelope.xMaximum(), mapped.x()));
    envelope.setYMinimum(std::min(envelope.yMinimum(), mapped.y()));
    envelope.setYMaximum(std::max(envelope.yMaximum(), mapped.y()));
    resolved.append(well.id);
  }
  // 零宽/极小包络：适度缓冲成明确窗口（单井点定位语义）。
  if (envelope.width() < 1e-9 && envelope.height() < 1e-9) {
    envelope.setXMinimum(envelope.xMinimum() - kPointBufferMeters);
    envelope.setXMaximum(envelope.xMaximum() + kPointBufferMeters);
    envelope.setYMinimum(envelope.yMinimum() - kPointBufferMeters);
    envelope.setYMaximum(envelope.yMaximum() + kPointBufferMeters);
  }
  // 审查修复（I4-R3）：井层可见性按 manifest 声明解析（wells 或 wells@<层位>
  // 作用域变体，同 constraintfactorjobs 的 wellsLayerIdFor 口径）——只认已
  // 实例化层的真实树勾选，不做假阴性/假阳性。
  bool wellsVisible = false;
  {
    QVector<LayerDeclaration> declared;
    if (m_layerSvc->tryDeclared(&declared)) {
      for (const LayerDeclaration &d : declared) {
        if (d.type.compare(QStringLiteral("vector"), Qt::CaseInsensitive) != 0 ||
            !d.layerId.startsWith(QStringLiteral("wells")))
          continue;
        if (layerTreeVisible(d.layerId)) {
          wellsVisible = true;
          break;
        }
      }
    }
  }
  QJsonObject facts = applyLocate(envelope, fit, wellsVisible);
  facts.insert(QStringLiteral("target_count"), wells.size());
  facts.insert(QStringLiteral("resolved_count"), resolved.size());
  facts.insert(QStringLiteral("mode"),
               fit ? QStringLiteral("fit") : QStringLiteral("center"));
  return facts;
}

QJsonObject AgentMapNavigation::locateLayer(const QString &layerId, bool fit)
{
  QJsonObject base{{QStringLiteral("ok"), false},
                   {QStringLiteral("mode"),
                    fit ? QStringLiteral("fit") : QStringLiteral("center")}};
  if (layerId.isEmpty())
    return merged(base, {{QStringLiteral("code"), QStringLiteral("invalid_layer_id")}});
  QgsMapCanvas *canvas = existingCanvas();
  if (!canvas)
    return merged(base, {{QStringLiteral("code"),
                                QStringLiteral("map_canvas_unavailable")}});
  if (m_layerSvc.isNull())
    return merged(base, {{QStringLiteral("code"),
                                QStringLiteral("map_provider_unavailable")}});
  // 仅已实例化层（不实例化非当前层位——任务书 I4 边界）。
  QgsMapLayer *layer = m_layerSvc->layer(layerId);
  if (!layer)
    return merged(base, {{QStringLiteral("code"), QStringLiteral("layer_not_present")}});
  QgsRectangle sourceExtent = layer->extent();
  if (!sourceExtent.isFinite() || sourceExtent.isNull())
    return merged(base, {{QStringLiteral("code"),
                                QStringLiteral("layer_extent_unavailable")}});
  if (sourceExtent.isEmpty()) {
    // 单点/零宽层：源坐标域先给有效范围再变换（zoomToLayer 同先例）。
    sourceExtent.grow(layer->crs().isGeographic() ? 0.005 : kPointBufferMeters);
  }
  // 严格变换：失败即拒绝（不回落图层原始范围——那是 UI 容错，不是 Agent
  // 成功路径）。
  const QgsRectangle mapped =
      paleo::mapreference::mapExtent(canvas, sourceExtent, layer->crs());
  if (mapped.isNull() || mapped.isEmpty() || !mapped.isFinite())
    return merged(base, {{QStringLiteral("code"),
                                QStringLiteral("coordinate_transform_failed")}});
  QJsonObject facts = applyLocate(mapped, fit, layerTreeVisible(layerId));
  facts.insert(QStringLiteral("mode"),
               fit ? QStringLiteral("fit") : QStringLiteral("center"));
  return facts;
}
