// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <functional>

#include <qgssnappingconfig.h>

class QgsMapCanvas;
class QgsMapLayer;
class QgsMapTool;
class QgsLayerTreeMapCanvasBridge;
class QgsProject;
class QWidget;

// P0 spine service — owns the QgsMapCanvas and map-tool lifecycle.
// Esc deactivates current tool (§42.15); re-entrancy guard on selection broadcasts (§41.3).
class QgisCanvasController : public QObject
{
  Q_OBJECT
  public:
    explicit QgisCanvasController(QObject *parent = nullptr);
    ~QgisCanvasController() override;

    QgsMapCanvas *canvas();                       // created lazily; QWidget* for embedding
    // 捕捉原生配置（docs/QGIS_NATIVE_ADOPTION.md）：enabled + AllLayers +
    // VertexAndSegment + 10px。canvas() 创建时装到 snappingUtils；工程打开
    // 时再镜像进 project->setSnappingConfig（随 .qgz 持久化）——capture 与
    // 编辑工具经 QgsMapCanvas::snappingUtils() 原生拾取，工具零改动。
    static QgsSnappingConfig nativeSnappingConfig();
    void setMapTool(QgsMapTool *tool);            // deactivates previous
    QgsMapTool *activeTool() const;
    void deactivateTool();                        // Esc path

    void zoomToFullExtent();
    // Resolves a manifest layer id to the service-owned QgsMapLayer.
    // No QgsProject::instance() fallback — unset/null resolution is a no-op.
    using LayerResolver = std::function<QgsMapLayer *(const QString &layerId)>;
    void setLayerResolver(LayerResolver resolver);
    void zoomToLayer(const QString &layerId);
    // 把视野中心移到工程网格点 (x,y)（局部米）：保留当前视野宽高，
    // 视野为空时落一个 ~1km 的窗口。验证问题定位用它——井点坐标直接
    // 可用时绝不整幅栅格缩放（§4 预览壳重排）。
    void zoomToPoint(double x, double y);

    // Selection broadcast guard (§41.3): while a broadcast is in flight, incoming
    // selection echoes are swallowed; a coalesced re-broadcast fires at settle.
    void beginSelectionBroadcast();
    void endSelectionBroadcast();
    bool broadcasting() const { return m_broadcasting; }

  signals:
    void selectionBroadcast(const QStringList &featureIds, const QString &origin);

  private:
    // Attach a QgsLayerTreeMapCanvasBridge between the canvas and the resolved
    // project tree, and pin the datum-free engineering CRS on project + canvas.
    void bindProject();
    void applyProjectCrs(QgsProject *project);

    QgsMapCanvas *m_canvas = nullptr;
    QgsLayerTreeMapCanvasBridge *m_bridge = nullptr;
    bool m_broadcasting = false;
    LayerResolver m_layerResolver;
};
