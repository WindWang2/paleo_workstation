#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QgsProject;
class QgsLayout;

// qgis/ — QgisLayoutService owns QgsLayoutManager lifecycle for the project.
// Per ET9 audit + D12: the full designer dialog is src/app-only; we implement
// QgsLayoutDesignerInterface shell later (P1 UI). Service here = layout CRUD,
// export, and designer-launch hook (signal, shell attaches when built).
class QgisLayoutService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisLayoutService(QgsProject *project, QObject *parent = nullptr);

    QgsLayout *createLayout(const QString &name, QString *error = nullptr);
    bool removeLayout(const QString &name);
    QStringList layoutNames() const;
    QgsLayout *layout(const QString &name) const;
    bool exportPdf(const QString &layoutName, const QString &outPath, QString *error = nullptr);

  signals:
    void designerRequested(const QString &layoutName); // UI shell connects when implemented
    void layoutAdded(const QString &name);
    void layoutRemoved(const QString &name);

  private:
    QgsProject *m_project;
};
