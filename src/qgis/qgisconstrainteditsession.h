// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QVector>
#include <QVariantMap>
#include <QHash>
#include <functional>
#include <memory>

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
                              std::function<void(const QString &)> failed);
    ~QgisConstraintEditSession() override;
    bool initialize(QString *error);
    bool finish(bool save, QString *error);
    QString horizon() const { return m_horizon; }
  private:
    QVector<QVariantMap> snapshot(QString *error) const;
    bool persist(const QVector<QVariantMap> &rows, QString *error);
    void synchronize(int index);
    void restoreSource();
    QPointer<QgsVectorLayer> m_layer;
    QPointer<PaleoProjectStore> m_store;
    std::function<void(const QString &)> m_failed;
    QString m_path, m_horizon;
    QString m_source, m_subset, m_name;
    std::unique_ptr<QTemporaryDir> m_workingDir;
    QVector<QVariantMap> m_initial, m_current;
    QHash<qlonglong, QString> m_identities;
    bool m_newCommand = false;
    bool m_finished = false;
    QMetaObject::Connection m_connection;
    int m_index = 0;
    bool m_writing = false;
};
