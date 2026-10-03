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
    QVector<LayerDeclaration> declared() const { return m_manifest->all(); }
    // False when the manifest cannot be read. Callers must not treat that as
    // an empty declaration set.
    bool tryDeclared(QVector<LayerDeclaration> *out, QString *error = nullptr) const;

    // Returns instantiated layer, creating it from its declaration if needed. nullptr + error on failure.
    QgsMapLayer *instantiate(const QString &layerId, QString *error = nullptr);
    int instantiateHorizon(const QString &horizon);              // count materialized
    void releaseHorizon(const QString &horizon);                 // drop instances (manifest keeps decl)

    // 一次性调和存量树：把根上直挂的声明图层搬进「地层/工作流组」路径——
    // .qgz 读档恢复的平铺节点不过 instantiate()，工程打开后调用一次。
    // 只动根直挂层；嵌在用户自建组里的位置是用户排版，不重排。
    void reconcileTreeGrouping();
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
    QgisEditingService *m_editSvc = nullptr;
    QHash<QString, QPointer<QgsMapLayer>> m_instances; // layerId -> safe guarded layer
    QString m_activeHorizon;
};
