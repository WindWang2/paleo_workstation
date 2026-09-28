// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "../metadata/layermanifest.h"

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

    // §37 projection hook: when set, writeProject() embeds the provider's
    // declared set into the .qgz (ManifestProjection custom property) so the
    // saved file describes all declarations, not just instantiated layers.
    // Wire e.g. to QgisLayerService::tryDeclared() / LayerManifest::readAll().
    // Provider must report read failure (false + error) — an empty set would
    // otherwise be indistinguishable from a legitimately empty manifest and
    // silently drop every declaration on write.
    void setDeclarationProvider(const std::function<bool(QVector<LayerDeclaration> *, QString *)> &provider);

  signals:
    void projectOpened(const QString &path);
    void projectWritten(const QString &path);

  private:
    QgsProject *m_project = nullptr;
    QString m_path;
    QStringList m_errors;
    std::function<bool(QVector<LayerDeclaration> *, QString *)> m_declarationProvider;
};
