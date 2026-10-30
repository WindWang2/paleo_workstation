// 层：视图
// 预测页地震相 DS 发布（自 attachPredictPage 拆出，控 file_budget 函数体规模）。
// 本 TU 禁止直连 qgs* / Qgs 符号（qgis-spread / qgs-audit 只缩不涨）；
// MessageLog 仍由 attach_predict.cpp（已在基线）承接。
#include "paleomainwindow.h"

#include "../qgis/qgisprojectservice.h"
#include "../services/subintervalfactors.h"
#include <paleo_data/service.h>
#include "../workflow/workflows.h"
#include "pages/pagepanels.h"
#include "../linkage/unitselectionbridge.h"

#include <QLabel>
#include <QStatusBar>
#include "../services/pythonenv.h"
#include "../workflow/mamcltool.h"
#include <QStandardPaths>
#include <QVariantMap>

namespace {

QVector<paleo::strat::SeismicSliceVote> votesFromParams(const QVariantMap &params)
{
  QVector<paleo::strat::SeismicSliceVote> votes;
  for (const auto &v : params.value(QStringLiteral("seismicSliceVotes")).toList())
  {
    const auto m = v.toMap();
    paleo::strat::SeismicSliceVote vote;
    vote.classCode = m.value(QStringLiteral("classCode")).toInt();
    vote.weight = m.value(QStringLiteral("weight")).toDouble();
    votes.append(vote);
  }
  return votes;
}

} // namespace

void PaleoMainWindow::publishSeismicMaskAggregate(PredictPage *predictPage, PredictionWorkflow *pred,
                                                  const QVariantMap &params)
{
  auto *status = predictPage ? predictPage->findChild<QLabel *>(QStringLiteral("statusLabel")) : nullptr;
  const auto setStatus = [status, this](const QString &msg, bool ok = false) {
    if (status)
      status->setText(msg);
    if (ok)
      statusBar()->showMessage(msg, 8000);
  };
  const auto bind = paleo::strat::bindAnalysis(m_selection, QString());
  if (!bind.hasUnit || !bind.unit.isValid() || bind.unit.aggregate)
  {
    setStatus(tr("请先在画布层段分析条选择具体子层段（S1–S4），再发布层段综合"));
    return;
  }
  const QString projectDir = pred ? pred->projectDir() : QString();
  paleo::data::DataService ds;
  QString openErr;
  if (!paleo::strat::openProjectDataService(projectDir, ds, &openErr))
  {
    setStatus(openErr.isEmpty() ? tr("工程尚无 Data Service 工区，无法发布地震相绑定") : openErr);
    return;
  }
  const auto votes = votesFromParams(params);
  if (votes.isEmpty())
  {
    setStatus(tr("请填写至少一行切片投票（相码,权重）"));
    return;
  }
  const double minCov = params.value(QStringLiteral("seismicMinCoverage"), 0.5).toDouble();
  const bool also = params.value(QStringLiteral("seismicAlsoBindSlices")).toBool();
  const QString asset = QStringLiteral("seismic_facies.%1.interval").arg(bind.unit.subIntervalId);
  const auto report = paleo::strat::publishSeismicSliceAggregate(
      ds, bind.unit, votes, asset, minCov, also,
      QStringLiteral("seismic_facies.%1.slice").arg(bind.unit.subIntervalId));
  if (!report.ok)
  {
    setStatus(tr("层段综合发布失败：%1").arg(report.error.isEmpty() ? tr("未知原因") : report.error));
    return;
  }
  setStatus(tr("已发布层段综合：相码 %1，覆盖率 %2（%3）%4")
                .arg(report.aggregate.classCode)
                .arg(report.aggregate.coverage, 0, 'f', 2)
                .arg(report.aggregate.strategyId)
                .arg(also ? tr("；并已登记切片绑定") : QString()),
            true);
}

void PaleoMainWindow::bindSeismicSliceAfterPublish(PredictionWorkflow *pred, const QVariantMap &jobParams,
                                                   const QString &layerId, const QString &outputPath,
                                                   QLabel *status)
{
  const QString pType = jobParams.value(QStringLiteral("predictType")).toString();
  const QString scope = jobParams.value(QStringLiteral("seismicScope")).toString();
  if (pType != QLatin1String("seismic_facies") || scope != QLatin1String("slice") || !pred)
    return;
  const auto unit = paleo::strat::fromMap(jobParams.value(QStringLiteral("unitSelection")).toMap());
  if (!unit.isValid() || unit.aggregate)
    return;
  paleo::data::DataService ds;
  QString openErr;
  if (!paleo::strat::openProjectDataService(pred->projectDir(), ds, &openErr))
    return;
  const int classCode = jobParams.value(QStringLiteral("seismicClassCode"), 1).toInt();
  const auto br = paleo::strat::bindSeismicFaciesResult(
      ds, unit, paleo::strat::SeismicFaciesScope::Slice, classCode, 0, {},
      layerId.isEmpty() ? outputPath : layerId);
  if (status)
    status->setText(br.ok ? tr("预测已发布；地震相切片已绑定 Data Service")
                          : tr("预测已发布；切片 DS 绑定跳过：%1").arg(br.error));
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
