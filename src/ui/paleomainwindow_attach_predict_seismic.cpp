// 层：视图
// 预测页地震相 DS 发布（自 attachPredictPage 拆出，控 file_budget 函数体规模）。
// 本 TU 禁止直连 qgs* / Qgs 符号（qgis-spread / qgs-audit 只缩不涨）；
// MessageLog 仍由 attach_predict.cpp（已在基线）承接。
#include "paleomainwindow.h"

#include "../qgis/qgisprojectservice.h"
#include "../workflow/seismicresultpublisher.h"

#include "../workflow/workflows.h"
#include "pages/pagepanels.h"
#include "../linkage/unitselectionbridge.h"

#include <QLabel>
#include <QStatusBar>
#include "../services/pythonenv.h"
#include "../workflow/mamcltool.h"
#include <QStandardPaths>
#include <QVariantMap>

void PaleoMainWindow::publishSeismicMaskAggregate(PredictPage *page, PredictionWorkflow *workflow,
                                                  const QVariantMap &params)
{
  const auto binding = paleo::strat::bindAnalysis(m_selection);
  const auto feedback = publishSeismicAggregate(workflow ? workflow->projectDir() : QString(), binding.unit, params);
  if (auto *status = page ? page->findChild<QLabel *>(QStringLiteral("statusLabel")) : nullptr)
    status->setText(feedback.message);
  if (feedback.ok) statusBar()->showMessage(feedback.message, 8000);
}
void PaleoMainWindow::attachPredictPageTools(PredictPage *predictPage, PredictionWorkflow *pred)
{
  if (!predictPage)
    return;
  auto *status = predictPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
  const QString envRoot =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
      QStringLiteral("/external/mamcl");
  auto *pyenv = new PythonEnvService(envRoot, this);
  auto *mamcl = new MamclTool(pyenv, this);
  connect(predictPage, &PredictPage::mamclLaunchRequested, mamcl, &MamclTool::open);
  connect(predictPage, &PredictPage::wellFaciesPanelRequested, this, [this, status] {
    showPage(QStringLiteral("data"));
    const QString hint = tr("已切到「数据管理」：在数据列表中打开一口井的综合显示，"
                            "在测井相面板选择模型、检查输入并运行预测");
    if (status)
      status->setText(hint);
    statusBar()->showMessage(hint, 10000);
  });
  connect(predictPage, &PredictPage::seismicMaskPublishRequested, this,
          [this, pred, predictPage](const QVariantMap &params) {
            publishSeismicMaskAggregate(predictPage, pred, params);
          });
  connect(mamcl, &MamclTool::busyChanged, predictPage, &PredictPage::setMamclBusy);
  connect(mamcl, &MamclTool::statusMessage, predictPage, [status](const QString &msg) {
    if (status)
      status->setText(msg);
  });
  // QgsMessageLog 接线留在 attach_predict.cpp（基线内 TU），避免本文件新增 Qgs 散布。
  connect(mamcl, &MamclTool::launchFinished, predictPage, [status](bool ok, const QString &msg) {
    if (status)
      status->setText(msg);
    Q_UNUSED(ok);
  });

}
