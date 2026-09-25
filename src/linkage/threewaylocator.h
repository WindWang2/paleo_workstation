#pragma once
#include <QObject>
#include <QString>
#include <QVariantMap>

class QgisCanvasController;
class ValidatePage;
class WellCorrelationPanel;
class SeismicPreviewPanel;

// linkage/threewaylocator — 三视图联动（wave/mapping-pipeline 阶段C，
// PROJECT_AREA_PLAN §5C「问题 → 三视图」）。
//
// ValidatePage 的一条问题双击后，同时驱动三个视图：
//   ① 地图：zoomToLayer 到问题携带的图层（原有行为）；
//   ② 连井剖面：scrollToWellTop(wellId, horizon)（面板本链路新增 API）；
//   ③ 地震剖面：gotoLine(inline, timeMs)（面板本链路新增 API）。
// payload 缺哪个字段就跳哪个视图——非残差问题只有地图缩放，不造假定位。
// 画布/面板指针可为空（未创建的面板直接跳过），便于测试与降级。
class ThreeWayLocator : public QObject
{
  Q_OBJECT
  public:
    ThreeWayLocator(QgisCanvasController *canvas, WellCorrelationPanel *wellPanel,
                    SeismicPreviewPanel *seismicPanel, QObject *parent = nullptr);

    // 连接 ValidatePage 的双击定位信号（同信号只连一次）。
    void attach(ValidatePage *page);

    // 直接执行一次定位（attach 的等价手动入口，供测试与其他调用方）。
    void locate(const QString &layerId, const QString &wktLocation, const QVariantMap &payload);

  private:
    QgisCanvasController *m_canvas = nullptr;
    WellCorrelationPanel *m_wellPanel = nullptr;
    SeismicPreviewPanel *m_seismicPanel = nullptr;
};
