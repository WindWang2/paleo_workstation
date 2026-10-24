// 层：视图
#include "paleomainwindow.h"
#include "pages/mappingreferencepanel.h"
#include "../domain/facieshierarchy.h"
#include "../qgis/mappingreferenceloader.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../workflow/mappingworkbench.h"
#include "../linkage/selectioncontext.h"
#include <QDir>
#include <QFileInfo>

bool PaleoMainWindow::showMappingReference(const QString &id, QString *error)
{
  if (!m_workbench) return false;
  auto *dock = findChild<PaleoDockWidget *>(QStringLiteral("mappingReferenceDock"));
  auto *panel = dock ? dock->findChild<MappingEvidencePanel *>() : nullptr;
  const bool created = !dock;
  if (!dock) {
    dock = new PaleoDockWidget(tr("参考证据"), this);
    dock->setObjectName(QStringLiteral("mappingReferenceDock"));
    dock->setProperty("paleo.mappingReference", true);
    panel = new MappingEvidencePanel(m_canvasCtl->canvas(),
        m_projectSvc ? m_projectSvc->project() : nullptr, dock);
    dock->setWidget(panel);
    connect(dock, &QDockWidget::visibilityChanged, panel, &MappingEvidencePanel::setViewActive);
    connect(panel, &MappingEvidencePanel::referenceRequested, panel,
            [this](MappingReferencePanel *view, const QString &sourceId) {
      const auto declaration = m_workbench->declaration(sourceId);
      auto *layer = MappingReferenceLoader::load(declaration, m_layerSvc->layer(sourceId),
          m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).dir().filePath("styles") : QString());
      if (!layer) {
        view->clearReference(tr("参考图件无法读取，请检查文件是否存在，或选择其他图件。"));
        return;
      }
      // 只替换请求来源的视图，其他视图的选择与当前绘制状态均保留。
      // 参考窗口与主图同口径显示相类固定：以主图图层当前固定为准（含未入版本的修改）。
      auto metadata = m_workbench->versionForLayer(sourceId).extra;
      if (m_layerSvc->layer(sourceId))
        metadata.insert(QStringLiteral("pinned_levels"),
                        FaciesHierarchy::encodePins(m_workbench->pinnedLevels(sourceId)));
      view->setReference(layer, metadata);
    });
    const auto refresh = [this, panel] {
      panel->setSources(m_workbench->products(QString()), m_selection->activeHorizon());
    };
    connect(m_workbench, &MappingWorkbench::changed, panel, refresh);
    connect(m_selection, &SelectionContext::activeHorizonChanged, panel, refresh);
    connect(m_workbench, &MappingWorkbench::catalogBound, panel, [panel, dock] {
      panel->setSources({}, QString());
      panel->clearReferences(tr("工程已切换，请选择当前工程的参考证据。"));
      dock->hide();
    });
    refresh();
    addDockWidget(Qt::BottomDockWidgetArea, dock);
  }
  dock->show();
  dock->raise();
  // 只在首次打开时设置默认高度；增减内部视图不改变外层面板的尺寸。
  if (created && !dock->isFloating() && dockWidgetArea(dock) == Qt::BottomDockWidgetArea)
    resizeDocks({dock}, {qMin(360, qMax(240, height() / 2))}, Qt::Vertical);
  panel->setViewActive(true);
  if (!id.isEmpty()) {
    auto *view = panel->showSource(id);
    if (!view->hasReference()) {
      if (error) *error = tr("参考图件无法读取，请在参考面板中重新选择");
      return false;
    }
  }
  return true;
}
