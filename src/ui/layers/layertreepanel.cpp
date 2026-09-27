// 层：视图
#include "layertreepanel.h"

#include <qgslayertreemodel.h>
#include <qgslayertreeview.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>

#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include "qgis/qgislayerservice.h"

// 骨架占位实现（wave/layer-platform 子任务 A 落地）。
LayerTreePanel::LayerTreePanel(QgsProject *project, QgsMapCanvas *canvas,
                               QgisLayerService *layerService, QWidget *parent)
    : QWidget(parent), m_project(project), m_canvas(canvas), m_layerService(layerService)
{
    m_view = new QgsLayerTreeView(this);
    m_view->setObjectName(QStringLiteral("layerTreeView"));
    m_emptyState = new QLabel(
        QStringLiteral("图层树是空的 — 导入数据后图层会出现在这里"), this);
    m_emptyState->setObjectName(QStringLiteral("layerTreeEmptyState"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    layout->addWidget(m_emptyState);
    if (project && project->layerTreeRoot())
    {
        auto *model = new QgsLayerTreeModel(project->layerTreeRoot(), m_view);
        model->setFlag(QgsLayerTreeModel::AllowNodeReorder);
        model->setFlag(QgsLayerTreeModel::AllowNodeRename);
        model->setFlag(QgsLayerTreeModel::AllowNodeChangeVisibility);
        m_view->setModel(model);
    }
    m_emptyState->setHidden(!(m_project && m_project->layerTreeRoot())
                            || !m_project->mapLayers().isEmpty());
}

QgsLayerTreeView *LayerTreePanel::treeView() const { return m_view; }
QgsLayerTreeModel *LayerTreePanel::layerTreeModel() const { return m_view->layerTreeModel(); }

void LayerTreePanel::setFilterText(const QString &text)
{
    Q_UNUSED(text);
}

QString LayerTreePanel::filterText() const { return QString(); }

void LayerTreePanel::refreshIndicators() {}
