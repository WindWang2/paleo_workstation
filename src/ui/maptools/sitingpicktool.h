// 层：视图
#pragma once

#include <qgsmaptoolemitpoint.h>
#include <qgssnapindicator.h>

#include <memory>

class QgsMapCanvas;
class QgsSnapIndicator;

// ui/maptools/ — PaleoSitingPickTool：井网辅助（方向 34）的地图布点工具。
// 单击拾取一个点位（画布系坐标，吸附走画布 snappingUtils——
// QgisCanvasController::nativeSnappingConfig 装的配置），发 pointPicked(x,y)
// 后自动停用（单发布点，不连续放）。Esc/右键取消发 pickAborted——owner
// 负责拆工具（§42.15 同款语义）。悬停有 QgsSnapIndicator 可视反馈
//（vertexeditortools 同款 affordance）。
class PaleoSitingPickTool : public QgsMapToolEmitPoint
{
  Q_OBJECT
  public:
    explicit PaleoSitingPickTool( QgsMapCanvas *canvas );

    void activate() override;
    void deactivate() override;

  signals:
    void pointPicked( double x, double y ); // 画布系（工程局部米制网格）
    void pickAborted();

  protected:
    void canvasMoveEvent( QgsMapMouseEvent *e ) override;
    void canvasReleaseEvent( QgsMapMouseEvent *e ) override;
    void keyPressEvent( QKeyEvent *e ) override;

  private:
    std::unique_ptr<class QgsSnapIndicator> m_snapIndicator; // 非 QObject——自持
};
