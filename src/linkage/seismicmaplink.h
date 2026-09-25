#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QgsMapCanvas;
class QgsVectorLayer;
class SelectionContext;

// linkage/ — SeismicMapLink mirrors WellMapLink for seismic line layers
// (lines instead of points). Same bidirectional contract:
// canvas select → SelectionContext (origin "canvas"); foreign-origin
// selection → highlight matching lines on the layer. Ping-pong guarded.
// Seismic preview panel (unbinned viewer) attaches separately — this class
// only owns selection sync.
class SeismicMapLink : public QObject
{
  Q_OBJECT
  public:
    SeismicMapLink(QgsMapCanvas *canvas, SelectionContext *ctx, QObject *parent = nullptr);
    void setSeismicLayer(QgsVectorLayer *lineLayer, const QString &idField = QStringLiteral("line_id"));
    QgsVectorLayer *seismicLayer() const { return m_layer; }

  private slots:
    void onContextSelection(const QStringList &ids, const QString &origin);

  private:
    QgsMapCanvas *m_canvas;
    SelectionContext *m_ctx;
    QgsVectorLayer *m_layer = nullptr;
    QString m_idField;
};
