// 层：视图
#include "../services/paleotaskservice.h"
#include "../services/previewdoc.h"
#include "../workflow/dataexportworkflow.h"
#include "../workflow/mappingworkbench.h"
#include "dialogs/dataexportdialog.h"
#include "pages/composepage.h"
#include "pages/mappingeditbasis.h"
#include "pages/mappingreferencepanel.h"
#include "pages/mappingworkbenchpage.h"
#include "paleomainwindow.h"
#include <QDialog>
#include <QSet>
#include <QStatusBar>
#include <algorithm>

void PaleoMainWindow::attachMappingEditFlow(MappingWorkbench *workbench) {
  workbench->setManualEvidenceRequired(currentPage() == QLatin1String("compose"));
  // 切工程时丢弃旧影响报告；会话依据由功能层一并清空。
  connect(workbench, &MappingWorkbench::catalogBound, this, [this] { m_lastImpact.clear(); });
  // 后台影响分析终态（GUI 线程发布）：成功填 m_lastImpact（受控重算入口）
  // 并翻页内可用态；空报告 = 失败/取消/过期（error 给因，可用态翻回 false）。
  connect(workbench, &MappingWorkbench::impactAnalysisReady, this,
          [this](const QVariantMap &report, const QString &error) {
    const bool ok = !report.isEmpty();
    m_lastImpact = report;
    for (auto *page : findChildren<MappingWorkbenchPage *>())
      page->setImpactAvailable(ok);
    if (!ok) {
      statusBar()->showMessage(
          error.isEmpty() ? tr("影响分析未完成。") : error, 20000);
      return;
    }
    const auto determined = report.value("determined").toList();
    qint64 changedCells = 0;
    for (const auto &value : determined)
      changedCells += value.toMap().value("changed_cells").toLongLong();
    const auto reportPath = report.value("report_path").toString();
    statusBar()->showMessage(
        reportPath.isEmpty()
            ? tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定"
                 "变化 %3 像元（报告未能保存到工程目录）")
                  .arg(report.value("direct_products").toList().size())
                  .arg(report.value("potential_products").toList().size())
                  .arg(changedCells)
            : tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定变化 "
                 "%3 像元（详见 %4）")
                  .arg(report.value("direct_products").toList().size())
                  .arg(report.value("potential_products").toList().size())
                  .arg(changedCells)
                  .arg(reportPath),
        20000);
  });
  // 后台版本对比终态：与旧同步分支相同的结果呈现（转移矩阵 Top3）。
  connect(workbench, &MappingWorkbench::versionCompareReady, this,
          [this](const QVariantMap &report, const QString &error) {
    if (report.isEmpty()) {
      statusBar()->showMessage(
          error.isEmpty() ? tr("重算对比未完成。") : error, 20000);
      return;
    }
    auto matrix = report.value("code_matrix").toList();
    std::sort(matrix.begin(), matrix.end(),
              [](const QVariant &a, const QVariant &b) {
                return a.toMap().value("cells").toLongLong() >
                       b.toMap().value("cells").toLongLong();
              });
    QStringList top;
    for (const auto &value : matrix.mid(0, 3))
      top << tr("%1：%2 像元")
                 .arg(value.toMap().value("transition").toString())
                 .arg(value.toMap().value("cells").toLongLong());
    statusBar()->showMessage(
        tr("重算对比：变化 %1/%2 像元；主要转移 %3（详见 %4）")
            .arg(report.value("cells_changed").toLongLong())
            .arg(report.value("total_cells").toLongLong())
            .arg(top.isEmpty() ? tr("无") : top.join("；"))
            .arg(report.value("report_path").toString()),
        20000);
  });
}

bool PaleoMainWindow::runMappingEditFlow(MappingWorkbenchPage *page, const QString &action,
                                         const QVariantMap &parameters, const WorkbenchShowFn &prepareMap) {
  if (!m_workbench)
    return false;
  const auto notify = [this, page](const QString &text) {
    page->showMessage(text);
    statusBar()->showMessage(text, 15000);
  };
  QString error;
  const auto horizon = parameters.value("horizon", page->horizon()).toString();
  const auto id = parameters.value("layer", page->selectedLayer()).toString();
  if (action == "captureEditEvidence") {
    QVariantList references;
    QSet<QString> seen;
    for (auto *view : findChildren<MappingReferencePanel *>()) {
      if (!view->isVisible() || !view->hasReference())
        continue;
      // 读取视图正在显示的版本，不用下拉框当前解析出的最新版本替换它。
      const auto versionId = view->property("paleo.referenceVersion").toString();
      const auto version = m_previewDoc && m_previewDoc->catalog()
                               ? m_previewDoc->catalog()->versionById(versionId)
                               : CatalogVersion{};
      if (version.id.isEmpty() || version.extra.value("horizon").toString() != horizon ||
          view->sourceId().startsWith("draft.") || seen.contains(versionId))
        continue;
      seen.insert(versionId);
      references << QVariantMap{
          {"version_id", versionId},
          {"name",
           tr("%1 · v%2")
               .arg(version.extra
                        .value("title", m_previewDoc->catalog()->assetById(version.assetId).displayName)
                        .toString())
               .arg(version.versionNumber)}};
    }
    if (auto *basis = page->findChild<MappingEditBasis *>())
      basis->setReferences(references);
    notify(references.isEmpty()
               ? tr("当前没有本层位已保存的可见参考图。可打开参考证据面板，或直接填写人工依据。")
               : tr("已选取 %1 个参考版本，请补充解释说明并启用。").arg(references.size()));
  } else if (action == "setEditEvidence") {
    const bool ok = m_workbench->setEditEvidence(horizon, parameters, &error);
    notify(ok ? tr("本次解释依据已启用，之后的元素修改会自动记录依据。") : error);
  } else if (action == "checkTopology") {
    const bool ok = m_workbench->checkEditingTopology(id, &error);
    notify(ok ? tr("三级相面拓扑检查通过：几何有效，共边一致，覆盖范围保持。") : error);
  } else if (action == "exportVector") {
    const auto version = m_workbench->versionForLayer(id);
    auto *catalog = m_previewDoc ? m_previewDoc->catalog() : nullptr;
    if (!catalog || !catalog->isOpen() || version.id.isEmpty() || m_workbench->hasUnpublishedEdits(id)) {
      notify(tr("请先保存编辑及图件新版本，再导出矢量成果。"));
      return true;
    }
    const auto directory = catalog->projectDir();
    const auto session = m_taskSvc ? m_taskSvc->session() : 0;
    DataExportDialog dialog(catalog, {version.assetId}, m_taskSvc, this, {version.id});
    if (dialog.exec() != QDialog::Accepted)
      return true;
    if (!m_taskSvc || m_taskSvc->session() != session || catalog->projectDir() != directory) {
      notify(tr("工程已切换，请重新打开导出。"));
      return true;
    }
    auto *task = startDataExport(
        catalog, m_taskSvc, dialog.request(), page,
        [notify](const paleo::exporting::Result &result) {
          notify(result.cancelled          ? tr("矢量导出已取消")
                 : !result.error.isEmpty() ? result.error
                                           : tr("矢量导出：成功 %1 项，问题 %2 项\n%3\n%4")
                                                 .arg(result.succeeded)
                                                 .arg(result.failures.size())
                                                 .arg(result.directory, result.failures.join("\n")));
        },
        &error);
    notify(task ? tr("矢量成果正在后台导出，可在任务中心查看进度。") : error);
  } else if (action == "exportMap") {
    if (m_workbench->hasUnpublishedEdits(id)) {
      notify(tr("请先保存编辑，再输出图件。"));
      return true;
    }
    if (!prepareMap(id, &error)) {
      notify(error);
      return true;
    }
    if (auto *compose = findChild<ComposePage *>())
      emit compose->exportPdfRequested();
  } else
    return false;
  return true;
}
