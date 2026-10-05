// 层：功能
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include <qgspointxy.h>

#include "../services/seismicmapping.h"

class QgsMapCanvas;
class QgsVectorLayer;
class SelectionContext;
class QgsVertexMarker;

namespace seismic {
class SgyVolume;
}

// linkage/ — SeismicMapLink maintains bidirectional linkage between map canvas and seismic views:
//   1. Feature selection sync between seismic line layer and SelectionContext.
//   2. Cross-view cursor tracking: section hover -> map vertex marker; section click -> map recenter.
//   3. Map polyline extraction -> asynchronous arbitrary/well section build.
//   4. Section capture on map: intent signal only (R4/方向 49) — the shell owns
//      SeismicSectionTool (a QgsMapTool = widget machinery, qgis 封装域); this
//      class never includes or holds it.
class SeismicMapLink : public QObject
{
  Q_OBJECT
  public:
    SeismicMapLink(QgsMapCanvas *canvas, SelectionContext *ctx, QObject *parent = nullptr);
    ~SeismicMapLink() override;

    void setSeismicLayer(QgsVectorLayer *lineLayer, const QString &idField = QStringLiteral("line_id"));
    QgsVectorLayer *seismicLayer() const;

    // 剖面跨视图联动（W3b）：linkage 不持有 ui 控件——壳把剖面画布的
    // traceHovered/traceClicked 连到下面的槽，把 sectionVolumeChanged 连到
    // 剖面控件的 setVolume。连接前先取 activeVolume() 做一次初始推送。
    void setGridGeometry(const SurveyGridGeometry &geom);
    SurveyGridGeometry gridGeometry() const { return m_gridGeom; }

    void setActiveVolume(std::shared_ptr<const seismic::SgyVolume> volume);
    std::shared_ptr<const seismic::SgyVolume> activeVolume() const { return m_volume; }

    // Section capture intent (R4 信号化，方向 49)：linkage 不持有
    // QgsMapTool——壳订阅 sectionCaptureRequested 后创建/激活
    // SeismicSectionTool，并把工具的 sectionPathCaptured 接回
    // triggerSectionFromMapPolyline（先例 threewaylocator 意图信号）。
    void requestSectionCapture();

    // Polyline trigger
    void triggerSectionFromMapPolyline(const QVector<QgsPointXY> &mapPoints, const QString &title = QString());

    // 工具路径回调（R4 信号化后由壳 connect：壳持有的 SeismicSectionTool
    // sectionPathCaptured 直连到此——语义与默认标题留在 linkage 侧）。
    void onSectionPathCaptured(const QVector<QgsPointXY> &points);

    // 剖面画布信号入口——由壳侧 connect（签名与剖面画布信号一致）。
    void onSectionTraceHovered(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void onSectionTraceClicked(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);

  signals:
    void sectionExtractedFromMap(bool success, const QString &message);
    // 当前活动地震体变化——壳连给剖面控件的 setVolume。
    void sectionVolumeChanged(std::shared_ptr<const seismic::SgyVolume> volume);
    // 地图折线剖面意图——壳连给剖面 dock：extractSectionFromVolumeAsync +
    // show/raise（linkage 不碰 ui 控件）。glm 向量按值传，直连同线程即可。
    void sectionExtractRequested(std::shared_ptr<const seismic::SgyVolume> volume,
                                 std::vector<glm::ivec2> pathPoints, QString title,
                                 std::vector<glm::dvec2> mapPolyline);
    // 剖面捕获被 Esc 取消——壳据此唤回设置对话框。
    void sectionCaptureCancelled();
    // 请求在地图上启动任意剖面捕获（R4 信号化，方向 49）：壳据此激活
    // 自持有的 SeismicSectionTool。linkage 不 include、不持有该工具。
    void sectionCaptureRequested();

  private slots:
    void onContextSelection(const QStringList &ids, const QString &origin);

  private:
    QPointer<QgsMapCanvas> m_canvas;
    SelectionContext *m_ctx;
    QPointer<QgsVectorLayer> m_layer;
    QString m_idField;

    QgsVertexMarker *m_cursorMarker = nullptr;
    SurveyGridGeometry m_gridGeom;
    std::shared_ptr<const seismic::SgyVolume> m_volume;
};
