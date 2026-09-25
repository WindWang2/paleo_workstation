#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QgsProject;

// P0 spine service — owns QgsProject open/save. Never write .qgz directly:
// writes are sequenced by PaleoProjectStore (gpkg commit -> .qgz backup -> atomic .qgz write).
// Lazy loading: the project file persists the DECLARED layer set (manifest), not just
// instantiated layers — uninstantiated layers serialize as placeholder references (§37).
class QgisProjectService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisProjectService(QObject *parent = nullptr);
    ~QgisProjectService() override;

    QgsProject *project() const;                    // never null after open/create
    bool openProject(const QString &qgzPath);       // resolves manifest placeholders on demand
    bool createProject(const QString &qgzPath);
    bool writeProject();                            // atomic temp+rename via store ordering
    QString projectPath() const;
    QStringList lastErrors() const { return m_errors; }

  signals:
    void projectOpened(const QString &path);
    void projectWritten(const QString &path);

  private:
    QgsProject *m_project = nullptr;
    QString m_path;
    QStringList m_errors;
};
