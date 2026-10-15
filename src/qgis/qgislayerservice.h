// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QString>
#include <QHash>
#include <QPointer>
#include "../metadata/layermanifest.h"

class QgsMapLayer;
class QgsVectorLayer;
class QgisProjectService;
class QgisEditingService;

// P0 spine service — instantiates QgsMapLayer objects on demand per horizon (§37).
// Manifest declares the full set; only the ACTIVE horizon's layers are materialized.
// Cross-horizon consumers (validation) call instantiate(horizon)/release(horizon)
// explicitly; layers instantiated on demand are released after use.
class QgisLayerService : public QObject
{
    Q_OBJECT
  public:
    QgisLayerService(QgisProjectService *projectSvc, LayerManifest *manifest, QObject *parent = nullptr);

    // releaseHorizon 遇到仍在编辑的图层时，经编辑服务回滚（busy 标记随会话
    // 释放）。未注入时维持旧行为（直接 rollBack，无 busy 释放）——测试裸用
    // 层服务的路径不受影响。
    // 裸指针非拥有：调用方须保证编辑服务活得比本服务久（组装根中两者同为
    // AppContext 子对象，构造序先 layer 后 edit，析构反序，成立）。
    void setEditingService(QgisEditingService *editSvc) { m_editSvc = editSvc; }

    bool declare(const LayerDeclaration &decl, QString *error = nullptr);
    bool removeDeclaration(const QString &layerId, QString *error = nullptr);
    bool removeDeclarations(const QStringList &layerIds, QString *error = nullptr);
    LayerDeclaration declaration(const QString &layerId) const;
    void refreshBasemaps();
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
    // 该层位（manifest 精确匹配）下是否有处于编辑态的实例。releaseHorizon 会回滚
    // 这些图层；调用方据此先拒绝，而不是静默丢掉未保存编辑（用户决定 b 保持回滚语义）。
    bool isEditingInHorizon(const QString &horizon, QString *layerName = nullptr) const;

    void setActiveHorizon(const QString &horizon);               // materialize active, release others
    QString activeHorizon() const { return m_activeHorizon; }

  signals:
    void layerInstantiated(const QString &layerId);
    void horizonReleased(const QString &horizon);
    // Emitted after a declaration lands in the manifest; consumers listing
    // declared layers (e.g. ComposePage) refresh off this. Empty id denotes a bulk removal.
    void layerDeclared(const QString &layerId);
    // Emitted when instantiateHorizon could not materialize one or more
    // declared layers (missing source, provider rejection…). failures carries
    // one human-readable reason per layer; the layers never entered the
    // project, so tree/canvas have no trace of them — the UI is expected to
    // surface this instead of letting the layer vanish silently.
    void horizonInstantiationFailed(const QString &horizon, const QStringList &failures);

  private:
    // The project owns every instantiated layer; project teardown (clear/read)
    // and direct layer removal delete them out from under the cache. These
    // hooks keep m_instances pointer-true so instantiate() never returns a
    // dangling layer (§37 spine discipline — raw pointers, project-owned).
    void purgeDanglingInstances();
    void trackInstance(const QString &layerId, QgsMapLayer *layer);
    // Re-register .qgz-restored layers (paleoLayerId custom property) into
    // m_instances on projectOpened — see .cpp for the full #287 rationale.
    void adoptProjectLayers();

    QgisProjectService *m_projectSvc;
    LayerManifest *m_manifest;
    QgisEditingService *m_editSvc = nullptr;
    QHash<QString, QPointer<QgsMapLayer>> m_instances; // layerId -> safe guarded layer
    QString m_activeHorizon;
};
