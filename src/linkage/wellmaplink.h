// 层：功能
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QgsMapCanvas;
class QgsVectorLayer;
class SelectionContext;

// linkage/ — WellMapLink keeps map-canvas selection and well-panel selection
// in sync for a well point layer (§31 well–map linkage; seismic analog is
// SeismicMapLink, same pattern, added with seismic ingestion).
// Direction A: canvas select -> SelectionContext (origin "canvas").
// Direction B: SelectionContext (origin != "canvas") -> highlight on layer.
class WellMapLink : public QObject
{
  Q_OBJECT
  public:
    WellMapLink(QgsMapCanvas *canvas, SelectionContext *ctx, QObject *parent = nullptr);

    void setWellLayer(QgsVectorLayer *layer, const QString &idField = QStringLiteral("id"));
    QgsVectorLayer *wellLayer() const;

  private slots:
    void onContextSelection(const QStringList &ids, const QString &origin);

  private:
    QPointer<QgsMapCanvas> m_canvas;
    SelectionContext *m_ctx;
    QPointer<QgsVectorLayer> m_layer;
    QString m_idField;
};
