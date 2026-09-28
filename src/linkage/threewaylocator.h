// 层：功能
#pragma once
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantMap>

class QgisCanvasController;

// linkage/threewaylocator — 问题 → 地图/连井联动（wave/mapping-pipeline
// 阶段C，PROJECT_AREA_PLAN §5C「问题 → 三视图」；预览壳重排后地震不再进
// 底栏——验证双击绝不再调地震 gotoLine，测线跳转走「在数据页看这条剖面」）。
//
// 本类是功能层联动器，不持有任何 UI 控件——「切底栏页签」与「滚到井分层」
// 经意图信号回壳订阅（correlationFocusRequested / bottomTabFocusRequested）。
// ValidatePage 的一条问题双击驱动：
//   ① 地图：payload 的 well_x/well_y（或 wktLocation 的 POINT(x y)）→
//      zoomToPoint 到井点；问题没坐标时退回 layerId 图层缩放；
//   ② 连井剖面：先发 bottomTabFocusRequested("correlation")（停靠里的面板
//      要先可见），再发 correlationFocusRequested(wellId, horizon)。
// payload 缺字段就跳对应视图——非残差问题只有地图缩放，不造假定位。
// 画布指针可为空（未创建的画布直接跳过），便于测试与降级；
// lastMapPoint() 记录最近一次的地图点目标，供断言与诊断。
class ThreeWayLocator : public QObject
{
  Q_OBJECT
  public:
    explicit ThreeWayLocator(QgisCanvasController *canvas, QObject *parent = nullptr);

    // 定位入口——壳把 ValidatePage::locateRequested 连到本槽（或直接调）。
    void locate(const QString &layerId, const QString &wktLocation, const QVariantMap &payload);

    // 最近一次井点定位目标；hasMapPoint()==false 时 lastMapPoint 无意义。
    bool hasMapPoint() const { return m_hasMapPoint; }
    QPointF lastMapPoint() const { return m_lastMapPoint; }

  signals:
    // 底栏切页签意图：tabId 是语义令牌（当前唯一值 "correlation" = 连井
    // 剖面页）。壳把令牌映射到底栏里的面板控件。
    void bottomTabFocusRequested(const QString &tabId);
    // 连井剖面滚动到指定井的指定分层（紧随 bottomTabFocusRequested 发出）。
    void correlationFocusRequested(const QString &wellId, const QString &horizon);

  private:
    QgisCanvasController *m_canvas = nullptr;
    bool m_hasMapPoint = false;
    QPointF m_lastMapPoint;
};
