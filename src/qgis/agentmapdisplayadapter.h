// 层：QGIS 封装
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QgisProjectService;
class QgisLayerService;
class QgisCanvasController;
class QgsMapLayer;

// qgis/agentmapdisplayadapter.h — I3：Agent 地图显示访问的生产适配器。
//
// Access 是宿主注入的显示访问接口（runner 编排在 workflow，组装根装
// 配）：接口面只有纯 Qt 类型，功能层 include 本头不引入 Qgs 符号
//（qgis-spread 闸）。AgentMapDisplayAdapter 解析 manifest 声明与当前
// QgsProject 实例、产出只读显示快照、执行可见性/透明度/受限样式修改并
// 回读原生状态。所有 QGIS 调用都发生在所属线程；对象指针经 QPointer
// 在每次调用时复核（工程 clear/重读、层删除、画布销毁不悬垂）。
namespace paleo::agent::mapdisplay {

// 结果契约版本（回执的 contract 字段；语义变更时新增版本）。单一权威在
// 本头：功能层与 ai/chat 工具表引用此常量，不复制第二份字面值。
inline constexpr auto kContract = "paleo.map-display.v1";

class Access {
public:
  virtual ~Access() = default;
  // provider/工程可用性：空串=可用；非空=机器可读原因。readiness 广播
  // 与直接调用共用同一来源（同一原因，不双标）。
  virtual QString unavailabilityReason() const = 0;
  // 只读目录：request = {horizon?, type?, page_size, cursor?}（已过严格
  // 校验）。查询不实例化、不切层位、不新建画布。
  virtual QJsonObject list(const QJsonObject &request) = 0;
  // 单层显示状态与适用选项；未实例化层如实标记 materialized=false。
  virtual QJsonObject get(const QString &layerId) = 0;
  // 三个显示修改：state_token 与当前实际状态不符时零副作用拒绝。
  // applyStyle 的 namedPreset 与 semanticFamily/semanticId 恰好一方非空
  //（上层校验后传入；adapter 对空串侧自证互斥）。
  virtual QJsonObject setVisibility(const QString &layerId, bool visible,
                                    const QString &stateToken) = 0;
  virtual QJsonObject setOpacity(const QString &layerId, double opacity,
                                 const QString &stateToken) = 0;
  virtual QJsonObject applyStyle(const QString &layerId, const QString &namedPreset,
                                 const QString &semanticFamily, const QString &semanticId,
                                 const QString &stateToken) = 0;
};

} // namespace paleo::agent::mapdisplay

class AgentMapDisplayAdapter : public QObject, public paleo::agent::mapdisplay::Access {
  Q_OBJECT
public:
  // projectSvc/layerSvc 为裸非拥有指针：组装根中三者同为 AppContext 子对
  // 象、同寿命（层服务头注释同款契约）。projectSvc 允许为空（测试裸用层
  // 服务时回落 QgsProject::instance()，与 QgisLayerService 同口径）。
  // canvasCtl 可空：无画布控制器时回执如实报 canvas_present=false，查询
  // 不触发惰性创建。
  AgentMapDisplayAdapter(QgisProjectService *projectSvc, QgisLayerService *layerSvc,
                         QgisCanvasController *canvasCtl, QObject *parent = nullptr);

  QString unavailabilityReason() const override;
  QJsonObject list(const QJsonObject &request) override;
  QJsonObject get(const QString &layerId) override;
  QJsonObject setVisibility(const QString &layerId, bool visible,
                            const QString &stateToken) override;
  QJsonObject setOpacity(const QString &layerId, double opacity,
                         const QString &stateToken) override;
  QJsonObject applyStyle(const QString &layerId, const QString &namedPreset,
                         const QString &semanticFamily, const QString &semanticId,
                         const QString &stateToken) override;

private:
  // 目标解析：声明存在 + 已实例化 + 实例归属自证（paleoLayerId 一致）。
  // 失败时 result 已填好诚实终态，返回空指针。
  QgsMapLayer *resolveManagedLayer(const QString &layerId, QJsonObject *result) const;
  // 全部受管层的显示状态摘要（实例化状态 + 每层 token 材料）：任何显示
  // 事实变化都会得到不同值。list 的游标与修改回执共用本口径。
  QString displayRevision() const;

  QPointer<QgisProjectService> m_projectSvc;
  QPointer<QgisLayerService> m_layerSvc;
  QPointer<QgisCanvasController> m_canvasCtl;
};
