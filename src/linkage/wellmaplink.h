// 层：功能
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QHash>
#include <qgsfeatureid.h>

class QgsMapCanvas;
class QgsVectorLayer;
class SelectionContext;

// linkage/ — WellMapLink keeps map-canvas selection and well-panel selection
// in sync for a well point layer (§31 well–map linkage; seismic analog is
// SeismicMapLink, same pattern, added with seismic ingestion).
// Direction A: canvas select -> SelectionContext (origin "well_map").
// Direction B: SelectionContext (origin != "well_map") -> highlight on layer.
class WellMapLink : public QObject
{
  Q_OBJECT
  public:
    WellMapLink(QgsMapCanvas *canvas, SelectionContext *ctx, QObject *parent = nullptr);

    void setWellLayer(QgsVectorLayer *layer, const QString &idField = QStringLiteral("id"));
    QgsVectorLayer *wellLayer() const;
    // I4：井层原生选中集计数（Agent 井选择回读事实；-1 = 层未绑定）。
    // ui 侧经此只读出口取数，不直连 qgs* 头（qgs-audit/qgis-spread 闸）。
    int selectedWellFeatureCount() const;

  private slots:
    void onContextSelection(const QStringList &ids, const QString &origin);

  private:
    QPointer<QgsMapCanvas> m_canvas;
    QPointer<SelectionContext> m_ctx;
    QPointer<QgsVectorLayer> m_layer;
    QString m_idField;
    QHash<QString, QgsFeatureIds> m_identityIndex;
    bool m_indexDirty = true, m_restorePending = false;
    void invalidateIndex();
    void rebuildIndex();
};
