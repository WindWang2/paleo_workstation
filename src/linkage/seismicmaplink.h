#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <qgspointxy.h>

#include "../services/seismicmapping.h"

class QgsMapCanvas;
class QgsVectorLayer;
class SelectionContext;
class QgsVertexMarker;
class SeismicSectionTool;

namespace seismic {
class SeismicSectionDockWidget;
class SgyVolume;
}

// linkage/ — SeismicMapLink maintains bidirectional linkage between map canvas and seismic views:
//   1. Feature selection sync between seismic line layer and SelectionContext.
//   2. Cross-view cursor tracking: section hover -> map vertex marker; section click -> map recenter.
//   3. Map polyline extraction -> asynchronous arbitrary/well section build.
class SeismicMapLink : public QObject
{
  Q_OBJECT
  public:
    SeismicMapLink(QgsMapCanvas *canvas, SelectionContext *ctx, QObject *parent = nullptr);
    ~SeismicMapLink() override;

    void setSeismicLayer(QgsVectorLayer *lineLayer, const QString &idField = QStringLiteral("line_id"));
    QgsVectorLayer *seismicLayer() const { return m_layer; }

    // Section dock attachment & cross-view cursor sync
    void attachSectionDock(seismic::SeismicSectionDockWidget *dock);
    seismic::SeismicSectionDockWidget *sectionDock() const { return m_sectionDock; }

    void setGridGeometry(const SurveyGridGeometry &geom);
    SurveyGridGeometry gridGeometry() const { return m_gridGeom; }

    void setActiveVolume(std::shared_ptr<const seismic::SgyVolume> volume);
    std::shared_ptr<const seismic::SgyVolume> activeVolume() const { return m_volume; }

    // Map tool activation
    void activateSectionCaptureTool();
    SeismicSectionTool *sectionCaptureTool() const { return m_tool; }

    // Polyline trigger
    void triggerSectionFromMapPolyline(const QVector<QgsPointXY> &mapPoints, const QString &title = QString());

  signals:
    void sectionExtractedFromMap(bool success, const QString &message);

  private slots:
    void onContextSelection(const QStringList &ids, const QString &origin);
    void onSectionTraceHovered(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void onSectionTraceClicked(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void onSectionPathCaptured(const QVector<QgsPointXY> &points);

  private:
    QgsMapCanvas *m_canvas;
    SelectionContext *m_ctx;
    QgsVectorLayer *m_layer = nullptr;
    QString m_idField;

    seismic::SeismicSectionDockWidget *m_sectionDock = nullptr;
    QgsVertexMarker *m_cursorMarker = nullptr;
    SurveyGridGeometry m_gridGeom;
    std::shared_ptr<const seismic::SgyVolume> m_volume;
    SeismicSectionTool *m_tool = nullptr;
};
