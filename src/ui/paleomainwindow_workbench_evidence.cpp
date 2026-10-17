// 层：视图
// paleomainwindow_workbench_evidence — attachWorkbench 证据域接线段（#473）：
// 证据面板/图谱联动接线逐字平移自 paleomainwindow_workbench.cpp，行为守恒。
// 私有成员访问经 paleomainwindow.h 的成员函数声明（同 W4 拆分段先例）。
#include "paleomainwindow.h"

#include "../workflow/evidenceinteraction.h"
#include "pages/datagraphpage.h"
#include "pages/datapage.h"
#include "pages/mappingworkbenchpage.h"

#include <QStatusBar>

void PaleoMainWindow::attachWorkbenchEvidence(EvidenceInteraction *evidence,
                                              const QList<MappingWorkbenchPage *> &pages) {
  connect(evidence, &EvidenceInteraction::navigationFailed, this, [this](const QString &error) { statusBar()->showMessage(error, 12000); });
  for (auto *page : pages) {
    connect(page, &MappingWorkbenchPage::targetLayerChanged, evidence, [evidence, page](const QString &id) {
      if (page->isVisible()) evidence->setLayer(id);
    });
    connect(page, &MappingWorkbenchPage::evidenceActivated, evidence, [evidence](const QVariantMap &row) { evidence->navigate(row); });
    // X10：反查命中行定位命中图斑本身（含旧版本），回退走研究目标栈。
    connect(page, &MappingWorkbenchPage::reverseHitActivated, evidence, [evidence](const QVariantMap &row) { evidence->navigateFeature(row); });
    connect(page, &MappingWorkbenchPage::evidenceBackRequested, evidence, [evidence] { evidence->back(); });
    connect(evidence, &EvidenceInteraction::historyChanged, page, &MappingWorkbenchPage::setCanGoBack);
    page->setCanGoBack(evidence->canGoBack());
    connect(page, &MappingWorkbenchPage::evidenceGraphRequested, this, [this](const QVariantMap &snapshot) {
      openDataGraph(snapshot.value("asset_id").toString(), snapshot.value("version_id").toString());
      if (auto *data = findChild<DataPage *>()) data->graphPage()->setFeatureEvidence(snapshot, true);
    });
  }
  connect(evidence, &EvidenceInteraction::detailsChanged, this, [this, pages](const QVariantMap &snapshot, quint64) {
    for (auto *page : pages) {
      if (page->isVisible() && page->horizon() == snapshot.value("horizon").toString()) page->selectLayer(snapshot.value("layer_id").toString());
      if (snapshot.isEmpty() || page->selectedLayer() == snapshot.value("layer_id").toString()) page->setEvidenceSnapshot(snapshot);
    }
    if (auto *data = findChild<DataPage *>()) data->graphPage()->setFeatureEvidence(snapshot);
  });
  if (auto *data = findChild<DataPage *>())
    connect(data->graphPage(), &DataGraphPage::evidenceTargetRequested, evidence, &EvidenceInteraction::navigateGraphNode);
}
