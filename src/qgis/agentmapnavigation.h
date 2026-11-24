// 层：QGIS 封装
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "../services/agentnavigationaccess.h" // MapAccess 契约（数据层——qgis 封装不反向依赖功能层）

class QgisCanvasController;
class QgisLayerService;
class QgisProjectService;
class QgsMapCanvas;
class QgsRectangle;

// qgis/agentmapnavigation.h — I4：主画布业务定位的 QGIS 封装实现。
//
// 实现 workflow/agentnavigation.h 的 MapAccess：井包络/受管图层的严格
// CRS 定位与真实范围回读。与 QgisCanvasController::zoomToLayer 的 UI 语义
// 差异（任务书 W4）：转换失败/非有限坐标/空范围一律拒绝且旧视野不动——
// 不回落图层原始范围；零宽点目标给适度缓冲；不改 renderer/可见性/层序。
// 井坐标的源域是工程网格局部米（与 zoomToPoint 同源），转换走工程
// transform context（projectmapreference 基线助手）。
class AgentMapNavigation : public QObject, public paleo::agent::navigation::MapAccess
{
  Q_OBJECT
public:
  AgentMapNavigation(QgisCanvasController *canvasCtl, QgisLayerService *layerSvc,
                     QgisProjectService *projectSvc, QObject *parent = nullptr);

  QString unavailabilityReason() const override;
  QJsonObject snapshot() const override;
  QJsonObject locateWells(const QVector<paleo::agent::navigation::MapAccess::WellTarget> &wells,
                          bool fit) override;
  QJsonObject locateLayer(const QString &layerId, bool fit) override;

private:
  // 定位并回读：target 为画布 CRS 下的目标包络；返回结果 facts（含
  // extent/crs/target_visible），失败时 code 非空且画布未动。
  QJsonObject applyLocate(const QgsRectangle &target, bool fit, bool targetVisible);
  QgsMapCanvas *existingCanvas() const;
  bool layerTreeVisible(const QString &manifestLayerId) const;

  QPointer<QgisCanvasController> m_canvasCtl;
  QPointer<QgisLayerService> m_layerSvc;
  QPointer<QgisProjectService> m_projectSvc;
};
