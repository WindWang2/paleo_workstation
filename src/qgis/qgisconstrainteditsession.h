// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QVector>
#include <QVariantMap>
#include <QHash>
#include <functional>
#include <memory>
#include "../io/constraintstore.h"

class QgsVectorLayer;
class PaleoProjectStore;
class QTemporaryDir;

// 原生 edit command 边界 → store 原子快照；undo/redo 走同一路径。
// 保存结束 buffer，取消恢复起始快照。身份/层位在会话中不可改。
class QgisConstraintEditSession : public QObject
{
  Q_OBJECT
  public:
    QgisConstraintEditSession(QgsVectorLayer *layer, PaleoProjectStore *store,
                              std::function<void(const QString &)> failed,
                              const ConstraintStore::AuditContext &context = {});
    ~QgisConstraintEditSession() override;
    bool initialize(QString *error);
    bool finish(bool save, QString *error);
    QString horizon() const { return m_horizon; }
    bool setAuditContext(const ConstraintStore::AuditContext &context, QString *error = nullptr);
  signals:
    // G05：约束编辑保存成功后通知下游（id 为 store 稳定身份；几何/语义有变
    // 或新增的进 changed，会话期间删除的进 removed）。消费方据此做影响
    // 分析——不会自动改动任何成果版本。
    void constraintsSaved(const QString &horizon, const QStringList &changedIds,
                          const QStringList &removedIds);
  private:
    QVector<QVariantMap> snapshot(QString *error) const;
    qlonglong storeMaxFid() const; // 全表最大 fid；读失败返回 -1
    bool persist(const QVector<QVariantMap> &rows, QString *error, const QString &action = QString());
    void synchronize(int index, const QString &forcedAction = {});
    void restoreSource();
    QPointer<QgsVectorLayer> m_layer;
    QPointer<PaleoProjectStore> m_store;
    std::function<void(const QString &)> m_failed;
    QString m_path, m_horizon;
    QString m_source, m_subset, m_name;
    std::unique_ptr<QTemporaryDir> m_workingDir;
    QVector<QVariantMap> m_initial, m_current;
    QHash<qlonglong, QString> m_identities;
    // P2：会话内新绘约束（缓冲区负 fid）→ 预分配的 store fid 与稳定身份。
    // 首次快照时分配并固定，undo/redo 复用同一映射，保证 replaceHorizon 幂等。
    mutable QHash<qlonglong, qlonglong> m_newFids;
    mutable QHash<qlonglong, QString> m_newIdentities;
    mutable qlonglong m_nextFid = 1;
    bool m_newCommand = false;
    bool m_finished = false;
    QMetaObject::Connection m_connection;
    int m_index = 0;
    bool m_writing = false;
    ConstraintStore::AuditContext m_audit;
};
