// 层：视图
#pragma once

#include <QObject>
#include <QPointer>
#include <QHash>
#include <QStringList>

class MappingEvidencePanel;
class MappingReferencePanel;
class MappingWorkbench;
class QDockWidget;
class QgisCanvasController;
class QgisLayerService;
class QgisProjectService;
class SelectionContext;

namespace paleo::workbench {

class MappingReferenceController final : public QObject
{
    Q_OBJECT
public:
    MappingReferenceController(QDockWidget &dock, MappingWorkbench &workbench,
                               QgisCanvasController &canvas, QgisLayerService &layers,
                               QgisProjectService *project, SelectionContext &selection);
    bool showSource(const QString &layerId, QString *error);
    QStringList comparisonSources(const QString &primaryId);
    MappingEvidencePanel *panel() const { return m_panel; }

private:
    void refreshSources();
    void loadReference(MappingReferencePanel *view, const QString &layerId);

    QPointer<MappingWorkbench> m_workbench;
    QPointer<QgisLayerService> m_layers;
    QPointer<QgisProjectService> m_project;
    QPointer<SelectionContext> m_selection;
    MappingEvidencePanel *m_panel;
    QString m_primaryId;
    QHash<QString, QString> m_userSource;
};

} // namespace paleo::workbench
