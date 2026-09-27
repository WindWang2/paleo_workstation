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
    void setProviders(std::function<QString()> dbPath,
                      std::function<QVector<LayerDeclaration>()> manifestProvider);

    void refresh(); // reload releases + repopulate diff combos

  signals:
    void releaseCreated(const QString &releaseId);
    void statusMessage(const QString &text);

  private:
    std::function<QString()> m_dbPath;
    std::function<QVector<LayerDeclaration>()> m_manifestProvider;
};
