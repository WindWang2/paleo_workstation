// 层：视图
#include "layerpropertiesdialog.h"

#include <QWidget>

#include "qgis/qgislayerservice.h"

// 骨架占位实现（wave/layer-platform 子任务 B 落地）。
LayerPropertiesDialog::LayerPropertiesDialog(QgisLayerService *layerService,
                                             const Deps &deps, QObject *parent)
    : QObject(parent), m_layerService(layerService), m_canvas(deps.canvas),
      m_messageBar(deps.messageBar)
{
}

LayerPropertiesDialog::~LayerPropertiesDialog() = default;

QWidget *LayerPropertiesDialog::createBusinessPage(const QString &layerId, QWidget *parent)
{
    Q_UNUSED(layerId);
    Q_UNUSED(parent);
    return nullptr;
}

void LayerPropertiesDialog::openLayerProperties(const QString &layerId)
{
    Q_UNUSED(layerId);
}
