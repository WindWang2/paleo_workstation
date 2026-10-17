// 层：视图
// 综合图面板·道编排与会话（编辑会话同步、相工作流、道操作槽、spec 增量、会话记忆）——自 wellcompositepanel.cpp 拆出（方向 66，行为零变更）
#include "wellcompositepanel.h"
#include "../../workflow/wellfaciesworkflow.h"
#include "editsession.h"
#include "wellpositionlegendwidget.h"
#include "trackconfigdialog.h"
#include "hiddentrackbar.h"
#include "trackops.h"
#include "trackregistry.h"
#include <QDialog>
#include <QFileDialog>
#include <QMap>
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
  connect(workflow, &WellFaciesWorkflow::planChanged, this, [this](const QString &summary) { m_faciesPlan = summary; });
  connect(workflow, &WellFaciesWorkflow::availabilityChanged, this, [this](bool ready, const QString &reason) {
    m_faciesReady = ready;
    if (!m_btnCancelFacies->isEnabled())
      m_btnPredictFacies->setEnabled(true);
    m_faciesModel->setToolTip(tr("%1\n预测按地层组逐组进行；同一地层组有多个模型版本时使用此处所选版本")
                                  .arg(m_faciesModel->currentData(Qt::ToolTipRole).toString()));
    m_btnPredictFacies->setToolTip(ready ? tr("提交当前井的对应井段预测相（按地层组逐组，逐组返回）：%1").arg(m_faciesPlan) : reason);
    m_faciesStatus->setText(ready ? tr("可预测：%1").arg(m_faciesPlan) : reason);
  });
  connect(workflow, &WellFaciesWorkflow::busyChanged, this, [this](bool busy) {
    m_btnCancelFacies->setEnabled(busy);
    m_btnPredictFacies->setEnabled(!busy);
    m_btnCancelFacies->setToolTip(busy ? tr("停止本地等待（所有地层），服务端已受理任务继续执行") : tr("没有正在等待的预测"));
    m_faciesModel->setEnabled(!busy); m_btnFaciesService->setEnabled(!busy); m_btnRefreshFacies->setEnabled(!busy);
  });
  connect(workflow, &WellFaciesWorkflow::statusChanged, m_faciesStatus, &QLabel::setText);
  connect(workflow, &WellFaciesWorkflow::resultReady, this, &WellCompositePanel::showFaciesPrediction);
  connect(workflow, &WellFaciesWorkflow::resultCleared, this, &WellCompositePanel::clearFaciesPrediction);
  connect(workflow, &WellFaciesWorkflow::attributeAvailabilityChanged, this, [this](bool available, const QString &reason) {
    for (const auto &name : {"btnWellAttributes", "btnWellFormations", "btnWellFactors"}) if (auto *button = findChild<QToolButton *>(name)) {
      button->setEnabled(available); button->setToolTip(reason);
    }
  });
  connect(workflow, &WellFaciesWorkflow::attributesReady, this, &WellCompositePanel::showWellAttributes);
  for (const auto &entry : {qMakePair(QStringLiteral("btnWellAttributes"), false), qMakePair(QStringLiteral("btnWellFactors"), true)})
    if (auto *button = findChild<QToolButton *>(entry.first))
      connect(button, &QToolButton::clicked, workflow, [workflow, factors = entry.second] { workflow->requestAttributeTable(factors); });
  if (auto *button = findChild<QToolButton *>(QStringLiteral("btnWellFormations")))
    connect(button, &QToolButton::clicked, workflow, &WellFaciesWorkflow::requestFormationTable);
  workflow->setData(m_data);
}
void WellCompositePanel::showWellAttributes(const ComprehensiveWellData &data) {
  m_data.lithologyIntervals = data.lithologyIntervals;
  m_data.faciesIntervals = data.faciesIntervals;
  bool hasLithology = false, hasFacies = false;
  for (int i = m_canvas->trackCount() - 1; i >= 0; --i) {
    const auto track = m_canvas->tracks().at(i);
    if (track->type() == TrackType::Lithology) {
      if (data.lithologyIntervals.isEmpty()) m_canvas->removeTrack(i);
      else { std::static_pointer_cast<LithologyTrack>(track)->setIntervals(data.lithologyIntervals); hasLithology = true; }
    } else if (track->type() == TrackType::FaciesCompound) {
      if (data.faciesIntervals.isEmpty()) m_canvas->removeTrack(i);
      else { std::static_pointer_cast<FaciesCompoundTrack>(track)->setIntervals(data.faciesIntervals); hasFacies = true; }
    }
  }
  if (!hasLithology && !data.lithologyIntervals.isEmpty()) {
    auto track = std::make_shared<LithologyTrack>(tr("岩性分析"), 80.0);
    track->setIntervals(data.lithologyIntervals); m_canvas->addTrack(track);
  }
  if (!hasFacies && !data.faciesIntervals.isEmpty()) {
    auto track = std::make_shared<FaciesCompoundTrack>(tr("维护相"), 180.0);
    track->setIntervals(data.faciesIntervals); m_canvas->addTrack(track);
  }
  if (m_legendWidget) m_legendWidget->setWellData(m_data);
  m_canvas->updateAll();
}
void WellCompositePanel::clearFaciesPrediction() {
  for (int i=m_canvas->trackCount()-1; i>=0; --i) {
    const auto track = m_canvas->tracks().at(i);
    if (track == m_predictionTrack || track == m_confidenceTrack || m_dominantTracks.contains(track)) m_canvas->removeTrack(i);
  }
  m_predictionTrack.reset(); m_confidenceTrack.reset(); m_dominantTracks.clear(); m_btnShowFacies->setEnabled(false);
  if (m_legendWidget) m_legendWidget->setWellData(m_data);
  m_btnShowFacies->setToolTip(tr("先运行测井相预测"));
}
void WellCompositePanel::showFaciesPrediction(const WellFaciesResult &result) {
  clearFaciesPrediction();
  auto legendData = m_data;
  legendData.faciesIntervals += result.hierarchyIntervals;
  if (m_legendWidget) m_legendWidget->setWellData(legendData);
  std::shared_ptr<WellTrack> labels;
  if (!result.hierarchyIntervals.isEmpty()) {
    auto track = std::make_shared<FaciesCompoundTrack>(tr("预测相"), 180.0);
    track->setIntervals(result.hierarchyIntervals);
    labels = track;
  } else {
    auto legacy = std::make_shared<TextTrack>(tr("预测相"), 120.0);
    legacy->setIntervals(result.intervals);
    legacy->setKeepTextVisible(true);
    labels = legacy;
  }
  auto confidence = std::make_shared<CurveTrack>(tr("预测置信度"), 140.0);
  confidence->addCurve(result.confidence);
  m_predictionTrack = labels; m_confidenceTrack = confidence;
  labels->setVisible(m_btnShowFacies->isChecked()); confidence->setVisible(m_btnShowFacies->isChecked());
  m_canvas->addTrack(labels); m_canvas->addTrack(confidence);
  // 优势相道：每个地层单位一块，文字 = 优势微相 + 占比；组、段两级都有时各一道。
  QMap<QString, QVector<TextInterval>> byLevel;
  for (const auto &d : result.dominant) {
    TextInterval interval;
    interval.topDepth = d.top; interval.bottomDepth = d.bottom; interval.category = d.formation;
    interval.text = (d.tie ? tr("%1 %2%（并列）") : tr("%1 %2%")).arg(d.dominant).arg(qRound(d.ratio * 100));
    byLevel[d.unitType] << interval;
  }
  QStringList levels{QStringLiteral("组"), QStringLiteral("段")};
  for (const auto &d : result.dominant) if (!levels.contains(d.unitType)) levels << d.unitType;
  for (const auto &level : levels) {
    if (!byLevel.contains(level)) continue;
    auto dominant = std::make_shared<TextTrack>(byLevel.size() > 1 ? tr("优势相（%1）").arg(level) : tr("优势相"), 120.0);
    dominant->setIntervals(byLevel.value(level)); dominant->setKeepTextVisible(true);
    dominant->setVisible(m_btnShowFacies->isChecked());
    m_dominantTracks << dominant; m_canvas->addTrack(dominant);
  }
  m_btnShowFacies->setEnabled(true);
  m_btnShowFacies->setToolTip(tr("显示或隐藏预测相、置信度和优势相；模型 %1 %2；任务 %3")
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
          // 曲线集/量程同样是会话记忆（TrackConfig 写进 spec.params）：
          // 重开/数据重载后恢复用户配置的曲线，而非回退到按助记名分组的
          // 默认道。restoreSessionState 在 m_data 填充后被调用，名字可解析；
          // 数据未到的名字静默跳过（与 onTrackConfigRequested 同语义）。
          if (!spec.curveNames().isEmpty())
            TrackOps::injectCurvesFromWellData(*ct, spec, m_data);
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
