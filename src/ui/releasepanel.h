// 层：视图
#pragma once
#include <QWidget>
#include <QString>
#include <functional>

#include "../metadata/layermanifest.h"

// ui/ — ReleasePanel: 发布列表 + 新建发布 + 两版本差异视图 (§44 发布语义 UI).
// Pure shell over ReleaseStore — the store path and the manifest snapshot are
// injected as providers so the panel works against whichever project is open
// (and stays testable without a project at all).
class ReleaseStore;

class ReleasePanel : public QWidget
{
  Q_OBJECT
  public:
    explicit ReleasePanel(QWidget *parent = nullptr);

    // dbPath: project sqlite path (ReleaseStore location). manifestProvider:
    // the CURRENT declared set — snapshotted by createRelease.
    // readOnlyProvider: 单写实例降级（§6）——工程被另一实例持锁（工程存储
    // setReadOnly(true)）时为真：面板禁用创建，且 ReleaseStore 走只读连接
    // （不建表、createRelease 显式拒绝）。缺省 nullptr = 恒可写。
    void setProviders(std::function<QString()> dbPath,
                      std::function<QVector<LayerDeclaration>()> manifestProvider,
                      std::function<bool()> readOnlyProvider = nullptr);

    void refresh(); // reload releases + repopulate diff combos

  signals:
    void releaseCreated(const QString &releaseId);
    void statusMessage(const QString &text);

  private:
    // 当前实例对工程库是否只读（无 provider 时可写）。
    bool projectReadOnly() const { return m_readOnlyProvider && m_readOnlyProvider(); }

    std::function<QString()> m_dbPath;
    std::function<QVector<LayerDeclaration>()> m_manifestProvider;
    std::function<bool()> m_readOnlyProvider;
};
