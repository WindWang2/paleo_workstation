// 层：视图
#include "layerprofilebar.h"

#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>

#include "qgis/qgislayerprofile.h"

// 骨架占位实现（wave/layer-platform 子任务 D 落地）。
LayerProfileBar::LayerProfileBar(QgisLayerProfileService *service, QWidget *parent)
    : QWidget(parent), m_service(service)
{
    m_combo = new QComboBox(this);
    m_pageLabel = new QLabel(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_combo);
    layout->addWidget(m_pageLabel);
}

void LayerProfileBar::setCurrentPage(const QString &pageId) { m_currentPage = pageId; }
QString LayerProfileBar::currentPage() const { return m_currentPage; }
QComboBox *LayerProfileBar::themeCombo() const { return m_combo; }
