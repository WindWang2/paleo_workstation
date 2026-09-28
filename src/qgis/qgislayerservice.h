// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QString>
#include <QHash>
#include "../metadata/layermanifest.h"

class QgsMapLayer;
class QgisProjectService;

// P0 spine service — instantiates QgsMapLayer objects on demand per horizon (§37).
// Manifest declares the full set; only the ACTIVE horizon's layers are materialized.
// Cross-horizon consumers (validation) call instantiate(horizon)/release(horizon)
// explicitly; layers instantiated on demand are released after use.
class QgisLayerService : public QObject
{
  Q_OBJECT
  public:
    QgisLayerService(QgisProjectService *projectSvc, LayerManifest *manifest, QObject *parent = nullptr);

    bool declare(const LayerDeclaration &decl, QString *error = nullptr);
    QVector<LayerDeclaration> declared() const { return m_manifest->all(); }
    // False when the manifest cannot be read. Callers must not treat that as
    // an empty declaration set.
    bool tryDeclared(QVector<LayerDeclaration> *out, QString *error = nullptr) const;

    // Returns instantiated layer, creating it from its declaration if needed. nullptr + error on failure.
    QgsMapLayer *instantiate(const QString &layerId, QString *error = nullptr);
    int instantiateHorizon(const QString &horizon);              // count materialized
    void releaseHorizon(const QString &horizon);                 // drop instances (manifest keeps decl)
    QgsMapLayer *layer(const QString &layerId) const;            // instantiated only, nullptr otherwise
    bool isInstantiated(const QString &layerId) const;
    bool isEditingAnyLayer(QString *layerName = nullptr) const;

    void setActiveHorizon(const QString &horizon);               // materialize active, release others
    QString activeHorizon() const { return m_activeHorizon; }

  signals:
    void layerInstantiated(const QString &layerId);
    void horizonReleased(const QString &horizon);
    // Emitted after a declaration lands in the manifest; consumers listing
    // declared layers (e.g. ComposePage) refresh off this.
    void layerDeclared(const QString &layerId);

  private:
    // The project owns every instantiated layer; project teardown (clear/read)
    // and direct layer removal delete them out from under the cache. These
    // hooks keep m_instances pointer-true so instantiate() never returns a
    // dangling layer (§37 spine discipline — raw pointers, project-owned).
    void purgeDanglingInstances();
    void trackInstance(const QString &layerId, QgsMapLayer *layer);

    QgisProjectService *m_projectSvc;
    LayerManifest *m_manifest;
    QHash<QString, QgsMapLayer *> m_instances; // layerId -> layer (owned by QgsProject)
    QString m_activeHorizon;
};
