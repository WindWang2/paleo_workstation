#pragma once
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantMap>

class QgisCanvasController;
class QTabWidget;
class ValidatePage;
class WellCorrelationPanel;

// linkage/threewaylocator — 问题 → 地图/连井联动（wave/mapping-pipeline
// 阶段C，PROJECT_AREA_PLAN §5C「问题 → 三视图」；预览壳重排后地震不再进
// 底栏——验证双击绝不再调地震 gotoLine，测线跳转走「在数据页看这条剖面」）。
//
// ValidatePage 的一条问题双击后驱动两个视图：
//   ① 地图：payload 的 well_x/well_y（或 wktLocation 的 POINT(x y)）→
//      zoomToPoint 到井点；问题没坐标时退回 layerId 图层缩放；
//   ② 连井剖面：先把底栏切到「连井剖面」标签，再 scrollToWellTop(wellId,
//      horizon)。
// payload 缺字段就跳对应视图——非残差问题只有地图缩放，不造假定位。
// 画布/面板指针可为空（未创建的面板直接跳过），便于测试与降级；
// lastMapPoint() 记录最近一次的地图点目标，供断言与诊断。
class ThreeWayLocator : public QObject
{
  Q_OBJECT
  public:
    ThreeWayLocator(QgisCanvasController *canvas, WellCorrelationPanel *wellPanel,
                    QTabWidget *bottomTabs, QObject *parent = nullptr);

    // 连接 ValidatePage 的双击定位信号（同信号只连一次）。
    void attach(ValidatePage *page);

    // 直接执行一次定位（attach 的等价手动入口，供测试与其他调用方）。
    void locate(const QString &layerId, const QString &wktLocation, const QVariantMap &payload);

    // 最近一次井点定位目标；hasMapPoint()==false 时 lastMapPoint 无意义。
    bool hasMapPoint() const { return m_hasMapPoint; }
    QPointF lastMapPoint() const { return m_lastMapPoint; }

  private:
    QgisCanvasController *m_canvas = nullptr;
    WellCorrelationPanel *m_wellPanel = nullptr;
    QTabWidget *m_bottomTabs = nullptr; // 底栏（连井剖面所在 QTabWidget），可为空
    bool m_hasMapPoint = false;
    QPointF m_lastMapPoint;
};
