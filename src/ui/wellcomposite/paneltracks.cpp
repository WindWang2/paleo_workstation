// 层：视图
// 综合图面板·道编排与会话（编辑会话同步、相工作流、道操作槽、spec 增量、会话记忆）——自 wellcompositepanel.cpp 拆出（方向 66，行为零变更）
#include "wellcompositepanel.h"
#include "../../workflow/wellfaciesworkflow.h"
#include "editsession.h"
#include "trackconfigdialog.h"
#include "hiddentrackbar.h"
#include "trackops.h"
#include "trackregistry.h"
#include <QDialog>
#include <QFileDialog>
#include "../notifications/paleonotify.h"
#include <QSignalBlocker>

namespace WellComposite
{

// 编辑会话数据 → 画布道重同步（undo/redo/编辑后统一走这里）
void WellCompositePanel::syncSessionToTracks()
{
  if (!m_editSession)
    return;
  const auto &doc = m_editSession->document();
  m_canvas->setMarkerLines(doc.standardHorizons);

  for (const auto &t : m_canvas->tracks())
  {
    if (!t)
      continue;
    if (t->type() == TrackType::Formation)
    {
      auto ft = std::static_pointer_cast<FormationTrack>(t);
      if (!doc.formationIntervals.isEmpty())
        ft->setIntervals(doc.formationIntervals);
    }
    else if (t->type() == TrackType::Lithology)
    {
      auto lt = std::static_pointer_cast<LithologyTrack>(t);
      lt->setIntervals(doc.lithologyIntervals);
    }
    else if (t->type() == TrackType::FaciesCompound)
    {
      auto fc = std::static_pointer_cast<FaciesCompoundTrack>(t);
      fc->setIntervals(doc.faciesIntervals);
    }
    else if (t->type() == TrackType::StratigraphyCompound)
    {
      auto sc = std::static_pointer_cast<StratigraphyCompoundTrack>(t);
      if (!doc.stratigraphyIntervals.isEmpty())
        sc->setIntervals(doc.stratigraphyIntervals);
    }
  }
  m_canvas->updateAll();
  emit faciesDataChanged(doc);
}

void WellCompositePanel::bindFaciesWorkflow(WellFaciesWorkflow *workflow) {
  if (!workflow || m_faciesWorkflow) return;
  m_faciesWorkflow = workflow;
  connect(this, &WellCompositePanel::faciesDataChanged, workflow, &WellFaciesWorkflow::setData);
  connect(this, &WellCompositePanel::faciesPredictionRequested, workflow, &WellFaciesWorkflow::run);
  connect(this, &WellCompositePanel::faciesCancelRequested, workflow, &WellFaciesWorkflow::cancel);
  connect(this, &WellCompositePanel::faciesModelsRequested, workflow, &WellFaciesWorkflow::refreshModels);
  connect(this, &WellCompositePanel::faciesModelSelected, workflow, &WellFaciesWorkflow::selectModel);
  connect(this, &WellCompositePanel::faciesConfigurationRequested, workflow, [workflow](const QString &url, const QString &key, bool allowInsecureHttp) {
    WellFaciesConfig config; config.baseUrl = QUrl(url); config.apiKey = key.toUtf8(); config.allowInsecureHttp = allowInsecureHttp; workflow->configure(config);
  });
  connect(workflow, &WellFaciesWorkflow::modelsChanged, this, [this](const QVariantList &models) {
    QSignalBlocker block(m_faciesModel); m_faciesModel->clear();
    for (const auto &v : models) {
      const auto m = v.toMap(); m_faciesModel->addItem(m.value("name").toString(), m.value("id"));
      m_faciesModel->setItemData(m_faciesModel->count()-1, m.value("requirements"), Qt::ToolTipRole);
      if (m.value("selected").toBool()) m_faciesModel->setCurrentIndex(m_faciesModel->count()-1);
    }
  });
  connect(workflow, &WellFaciesWorkflow::availabilityChanged, this, [this](bool ready, const QString &reason) {
    m_btnPredictFacies->setEnabled(ready);
    const QString requirements = m_faciesModel->currentData(Qt::ToolTipRole).toString();
    m_faciesModel->setToolTip(requirements);
    m_btnPredictFacies->setToolTip(ready ? tr("提交当前井的对应井段预测相：%1").arg(requirements) : reason);
    m_faciesStatus->setText(ready ? tr("可预测：%1").arg(requirements) : reason);
  });
  connect(workflow, &WellFaciesWorkflow::busyChanged, this, [this](bool busy) {
    m_btnCancelFacies->setEnabled(busy);
    m_btnCancelFacies->setToolTip(busy ? tr("停止本地等待，服务端已受理任务继续执行") : tr("没有正在等待的预测"));
    m_faciesModel->setEnabled(!busy); m_btnFaciesService->setEnabled(!busy); m_btnRefreshFacies->setEnabled(!busy);
  });
  connect(workflow, &WellFaciesWorkflow::statusChanged, m_faciesStatus, &QLabel::setText);
  connect(workflow, &WellFaciesWorkflow::resultReady, this, &WellCompositePanel::showFaciesPrediction);
  connect(workflow, &WellFaciesWorkflow::resultCleared, this, &WellCompositePanel::clearFaciesPrediction);
  workflow->setData(m_data);
}
void WellCompositePanel::clearFaciesPrediction() {
  for (int i=m_canvas->trackCount()-1; i>=0; --i) {
    const auto track = m_canvas->tracks().at(i);
    if (track == m_predictionTrack || track == m_confidenceTrack) m_canvas->removeTrack(i);
  }
  m_predictionTrack.reset(); m_confidenceTrack.reset(); m_btnShowFacies->setEnabled(false);
  m_btnShowFacies->setToolTip(tr("先运行测井相预测"));
}
void WellCompositePanel::showFaciesPrediction(const WellFaciesResult &result) {
  clearFaciesPrediction();
  auto labels = std::make_shared<TextTrack>(tr("预测相"), 120.0);
  labels->setIntervals(result.intervals);
  labels->setKeepTextVisible(true);
  auto confidence = std::make_shared<CurveTrack>(tr("预测置信度"), 140.0);
  confidence->addCurve(result.confidence);
  m_predictionTrack = labels; m_confidenceTrack = confidence;
  labels->setVisible(m_btnShowFacies->isChecked()); confidence->setVisible(m_btnShowFacies->isChecked());
  m_canvas->addTrack(labels); m_canvas->addTrack(confidence);
  m_btnShowFacies->setEnabled(true);
  m_btnShowFacies->setToolTip(tr("显示或隐藏预测相和置信度；模型 %1 %2；任务 %3")
                            .arg(result.modelName, result.modelVersion, result.jobId));
  m_canvas->updateAll();
}

// ----------------------------------------------------------------------------
// D1.5/D1.6/D1.10 道操作槽
// ----------------------------------------------------------------------------
void WellCompositePanel::onTrackCsvRequested(int trackIndex)
{
  const QString csv = m_canvas->trackCsvAt(trackIndex);
  if (csv.isEmpty())
    return;

  const QString suggested = QStringLiteral("%1_%2.csv").arg(m_wellName.isEmpty() ? QStringLiteral("well") : m_wellName,
                                                            m_canvas->tracks().at(trackIndex)->title());
  QString path = QFileDialog::getSaveFileName(this, tr("导出该道 CSV"), suggested,
                                              QStringLiteral("CSV (*.csv)"));
  if (path.isEmpty())
    return;
  if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
    path += QStringLiteral(".csv");

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    PaleoNotify::warning(this, tr("导出失败"), tr("无法写入文件: %1").arg(path));
    return;
  }
  f.write("\xEF\xBB\xBF"); // UTF-8 BOM（Excel 中文兼容）
  f.write(csv.toUtf8());
  m_lblStatus->setText(tr("已导出: %1").arg(path));
}

void WellCompositePanel::onTrackConfigRequested(int trackIndex)
{
  if (trackIndex < 0 || trackIndex >= m_canvas->trackCount())
    return;

  const auto track = m_canvas->tracks().at(trackIndex); // 拷贝 shared_ptr：tracks() 按值返回，后续跨 exec()
  TrackSpec initial = TrackRegistry::instance().captureSpec(track);
  if (initial.typeId.isEmpty())
    initial.typeId = TrackRegistry::typeIdForEnum(track->type());

  TrackConfigDialog dlg(initial, TrackOps::combinedCurvePool(m_data.continuousCurves, m_data.discreteCurves), this);
  if (dlg.exec() != QDialog::Accepted)
    return;

  const TrackSpec spec = dlg.resultSpec();
  track->setTitle(spec.title);
  track->setWidth(spec.width);
  track->setVisible(spec.visible);
  track->setPrintIncluded(spec.printIncluded);

  if (auto ct = std::dynamic_pointer_cast<CurveTrack>(track))
  {
    ct->setShowGrid(spec.showGrid());
    ct->setGridDensity(spec.gridDensity());
    if (!spec.curveNames().isEmpty())
      TrackOps::injectCurvesFromWellData(*ct, spec, m_data);
  }
  m_canvas->updateAll();
  onTrackOrderOrWidthChanged();
}

void WellCompositePanel::onTrackDuplicateRequested(int trackIndex)
{
  if (trackIndex < 0 || trackIndex >= m_canvas->trackCount())
    return;
  auto dup = TrackOps::duplicateTrack(m_canvas->tracks().at(trackIndex));
  if (!dup)
    return;
  dup->setTitle(dup->title() + tr(" 副本"));
  m_canvas->insertTrack(trackIndex + 1, dup);
}

void WellCompositePanel::onTrackVisibilityChanged(int trackIndex)
{
  // D1.10 隐藏道管理条刷新
  m_hiddenBar->setTracks(currentSpecs());
  onTrackOrderOrWidthChanged();
}

void WellCompositePanel::onTrackOrderOrWidthChanged()
{
  // D1.8 会话记忆（道序/宽度/显隐/打印开关）
  WellCompositeStore::saveSessionTracks(m_projectName, m_wellName, currentSpecs());

  // D1.4 宽度集合独立记忆（标题键）
  QVariantMap widths;
  for (const auto &t : m_canvas->tracks())
    if (t)
      widths.insert(t->title(), t->width());
  WellCompositeStore::saveWidthSet(m_projectName, m_wellName, widths);
}

QList<TrackSpec> WellCompositePanel::currentSpecs() const
{
  QList<TrackSpec> specs;
  for (const auto &t : m_canvas->tracks())
  {
    TrackSpec spec = TrackRegistry::instance().captureSpec(t);
    if (spec.typeId.isEmpty() && t)
      spec.typeId = TrackRegistry::typeIdForEnum(t->type());
    specs << spec;
  }
  return specs;
}

void WellCompositePanel::applySpecsIncrementally(const QList<TrackSpec> &specs)
{
  // D1.1 道增删改不重建面板：按 spec 列表增量调整既有道（顺序/宽度/可见性/
  // 打印开关按「类型+标题」匹配）；匹配不上的忽略（数据驱动的道可能改名）。
  if (specs.isEmpty())
    return;

  QList<std::shared_ptr<WellTrack>> reordered;
  for (const auto &spec : specs)
  {
    for (const auto &t : m_canvas->tracks())
    {
      if (!t)
        continue;
      const QString typeId = TrackRegistry::typeIdForEnum(t->type());
      if (typeId == spec.typeId && t->title() == spec.title)
      {
        t->setWidth(spec.width);
        t->setVisible(spec.visible);
        t->setPrintIncluded(spec.printIncluded);
        if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
        {
          ct->setShowGrid(spec.showGrid());
          ct->setGridDensity(spec.gridDensity());
        }
        reordered << t;
        break;
      }
    }
  }
  // 记忆外的道（新建/数据新加）保持尾部
  for (const auto &t : m_canvas->tracks())
    if (t && !reordered.contains(t))
      reordered << t;

  if (reordered.size() == m_canvas->trackCount())
    m_canvas->setTracks(reordered);
  m_canvas->syncScrollBars();
  m_canvas->updateAll();
}

void WellCompositePanel::saveSessionState() const
{
  if (m_wellName.isEmpty())
    return;
  WellCompositeStore::saveSessionTracks(m_projectName, m_wellName, currentSpecs());
}

void WellCompositePanel::restoreSessionState()
{
  if (m_wellName.isEmpty())
    return;
  const QList<TrackSpec> remembered = WellCompositeStore::loadSessionTracks(m_projectName, m_wellName);
  applySpecsIncrementally(remembered);
  m_hiddenBar->setTracks(currentSpecs());
}

} // namespace WellComposite
