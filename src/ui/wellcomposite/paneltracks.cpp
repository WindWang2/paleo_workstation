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
#include <QSaveFile>
#include <QSet>
#include <algorithm>
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
      // 层序/砂层组同为 FormationTrack 但数据源不同——按数据源身份路由，
      // 不能按标题（用户可经道配置改名，改名层序道曾被分层区间覆盖）。
      switch (ft->source())
      {
        case FormationTrack::FormationSource::Sequence:
          ft->setIntervals(doc.sequenceIntervals);
          break;
        case FormationTrack::FormationSource::SandGroup:
          break; // 砂层组不受分层编辑会话同步（与旧标题判据同语义）
        case FormationTrack::FormationSource::Layers:
          if (!doc.formationIntervals.isEmpty())
            ft->setIntervals(doc.formationIntervals);
          break;
      }
    }
    else if (t->type() == TrackType::Lithology)
    {
      auto lt = std::static_pointer_cast<LithologyTrack>(t);
      lt->setIntervals(doc.lithologyIntervals);
    }
    else if (t->type() == TrackType::FaciesCompound && t != m_predictionTrack)
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
      button->setEnabled(available);
      button->setToolTip(available && QString::fromLatin1(name) == QStringLiteral("btnWellAttributes")
          ? tr("查看或编辑井段岩性与相解释；沉积相道含源文件解释和预测回写值，可在图道配置中调整")
          : reason);
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
    } else if (track->type() == TrackType::FaciesCompound && track != m_predictionTrack) {
      if (data.faciesIntervals.isEmpty()) m_canvas->removeTrack(i);
      else { std::static_pointer_cast<FaciesCompoundTrack>(track)->setIntervals(data.faciesIntervals); hasFacies = true; }
    }
  }
  if (!hasLithology && !data.lithologyIntervals.isEmpty()) {
    auto track = std::make_shared<LithologyTrack>(tr("岩性"), 90.0);
    track->setIntervals(data.lithologyIntervals); m_canvas->addTrack(track);
  }
  if (!hasFacies && !data.faciesIntervals.isEmpty()) {
    auto track = std::make_shared<FaciesCompoundTrack>(tr("沉积相"), 180.0);
    track->setIntervals(data.faciesIntervals); m_canvas->addTrack(track);
  }
  if (m_legendWidget) m_legendWidget->setWellData(m_data);
  m_hiddenBar->setTracks(currentSpecs());
  updateDefaultLayoutButton();
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
  confidence->setParticipatesInStandardLayout(false);
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

  // 原子写 + 写满校验：旧实现 QFile Truncate 直写且丢弃 write() 返回值，
  // 磁盘满/路径失效时产出截断文件却仍提示「已导出」。
  QSaveFile f(path);
  const QByteArray payload = QByteArray("\xEF\xBB\xBF") + csv.toUtf8(); // UTF-8 BOM（Excel 中文兼容）
  if (!f.open(QIODevice::WriteOnly) || f.write(payload) != payload.size() || !f.commit())
  {
    const QString detail = f.errorString();
    f.cancelWriting();
    PaleoNotify::warning(this, tr("导出失败"),
                         tr("无法写入文件: %1%2")
                             .arg(path, detail.isEmpty() ? QString() : QStringLiteral("（") + detail + QStringLiteral("）")));
    return;
  }
  m_lblStatus->setText(tr("已导出: %1").arg(path));
}

void WellCompositePanel::onTrackConfigRequested(int trackIndex)
{
  if (trackIndex < 0 || trackIndex >= m_canvas->trackCount())
    return;

  const auto track = m_canvas->tracks().at(trackIndex); // 拷贝 shared_ptr：后续跨 exec()
  TrackSpec initial = TrackRegistry::instance().captureSpec(track);
  if (initial.typeId.isEmpty())
    initial.typeId = TrackRegistry::typeIdForEnum(track->type());

  // 曲线族道：曲线池按「道内有效值优先」装配（同名曲线用道上的当前值替换池值）。
  // 旧实现直接传裸池值——对话框按池值预填、按池值做差，用户此前（或 sidecar
  // 覆盖层）设过的量程/单位/颜色/对数态在往返中被静默清空（D3.12 覆盖层
  // 从未被任何代码写入 sidecar）。
  QVector<CurveData> curvePool = TrackOps::combinedCurvePool(m_data.continuousCurves, m_data.discreteCurves);
  if (auto ct = std::dynamic_pointer_cast<CurveTrack>(track))
  {
    for (const auto &eff : ct->curves())
    {
      for (auto &pooled : curvePool)
      {
        if (pooled.name.compare(eff.name, Qt::CaseInsensitive) == 0)
        {
          pooled.minScale = eff.minScale;
          pooled.maxScale = eff.maxScale;
          pooled.isLogarithmic = eff.isLogarithmic;
          pooled.unit = eff.unit;
          pooled.color = eff.color;
          break;
        }
      }
    }
  }

  TrackConfigDialog dlg(initial, curvePool, this);
  if (dlg.exec() != QDialog::Accepted)
    return;

  const TrackSpec spec = dlg.resultSpec();
  track->setTitle(spec.title);
  track->setWidth(spec.width);
  track->setVisible(spec.visible);
  track->setPrintIncluded(spec.printIncluded);

  if (auto lt = std::dynamic_pointer_cast<LithologyTrack>(track))
  {
    lt->setGrainQuantum(spec.params.value(QStringLiteral("grainQuantum"), lt->grainQuantum()).toDouble());
    lt->setWidthEncoding(spec.params.value(QStringLiteral("widthEncoding"), lt->widthEncoding()).toBool());
  }
  if (auto ct = std::dynamic_pointer_cast<CurveTrack>(track))
  {
    ct->setShowGrid(spec.showGrid());
    ct->setGridDensity(spec.gridDensity());
    if (!spec.curveNames().isEmpty())
      TrackOps::injectCurvesFromWellData(*ct, spec, m_data);
    // D3.12：曲线覆盖层落 sidecar（此前只读不写，重开即丢）。
    // 合并口径：sidecar 已有条目保留，本次对话框的差量叠加进去——曲线未改时
    // 不产生差量，原有覆盖层不会被清空。
    if (m_store && !spec.curveNames().isEmpty())
    {
      QVariantMap merged = m_store->curveOverrides();
      const QVariantMap edited = spec.curveOverrides();
      for (const QString &name : spec.curveNames())
      {
        const QVariantMap diff = edited.value(name).toMap();
        if (diff.isEmpty())
          continue;
        QVariantMap one = merged.value(name).toMap();
        for (auto it = diff.constBegin(); it != diff.constEnd(); ++it)
          one.insert(it.key(), it.value());
        merged.insert(name, one);
      }
      m_store->setCurveOverrides(merged);
      persistSidecar();
    }
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
  updateDefaultLayoutButton();
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
  // 打印开关按「类型+标题」匹配）；旧自定义曲线道按名字从完整曲线池重建。
  if (specs.isEmpty())
    return;

  QList<std::shared_ptr<WellTrack>> reordered;
  QSet<QString> restoredCurves;
  for (const auto &spec : specs)
  {
    std::shared_ptr<WellTrack> match;
    for (const auto &track : m_canvas->tracks())
    {
      if (!track || reordered.contains(track)) continue;
      const bool legacyInterpretation = track != m_predictionTrack &&
          track->type() == TrackType::FaciesCompound &&
          (spec.title == tr("维护相") || spec.title == tr("沉积相") || spec.title == tr("井段相解释"));
      if (TrackRegistry::typeIdForEnum(track->type()) == spec.typeId &&
          (track->title() == spec.title || legacyInterpretation))
      { match = track; break; }
    }
    // 默认组合变更后，旧会话的自定义曲线道仍按原配置恢复。
    if (!match && spec.typeId == QStringLiteral("curve") && !spec.curveNames().isEmpty())
      match = TrackRegistry::instance().createTrack(spec);
    if (!match) continue;
    match->setWidth(spec.width);
    match->setVisible(spec.visible);
    match->setPrintIncluded(spec.printIncluded);
    if (auto litho = std::dynamic_pointer_cast<LithologyTrack>(match))
    {
      litho->setGrainQuantum(spec.params.value(QStringLiteral("grainQuantum"), litho->grainQuantum()).toDouble());
      litho->setWidthEncoding(spec.params.value(QStringLiteral("widthEncoding"), litho->widthEncoding()).toBool());
    }
    if (auto curve = std::dynamic_pointer_cast<CurveTrack>(match))
    {
      curve->setShowGrid(spec.showGrid());
      curve->setGridDensity(spec.gridDensity());
      if (!spec.curveNames().isEmpty())
      {
        const auto original = curve->curves();
        if (!TrackOps::injectCurvesFromWellData(*curve, spec, m_data))
        { curve->setCurves(original); continue; }
      }
      for (const auto &item : curve->curves()) restoredCurves.insert(item.name);
    }
    reordered << match;
  }
  // 新数据中的曲线保留默认可见性；已由旧配置恢复的曲线不再重复附加。
  for (const auto &track : m_canvas->tracks())
  {
    if (!track || reordered.contains(track)) continue;
    if (auto curve = std::dynamic_pointer_cast<CurveTrack>(track))
    {
      auto remaining = curve->curves();
      remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&restoredCurves](const CurveData &c) {
        return restoredCurves.contains(c.name);
      }), remaining.end());
      if (remaining.isEmpty()) continue;
      curve->setCurves(remaining);
    }
    reordered << track;
  }
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

namespace
{
bool isResultTrack(const std::shared_ptr<WellTrack> &t, const std::shared_ptr<WellTrack> &prediction,
                   const std::shared_ptr<WellTrack> &confidence,
                   const QList<std::shared_ptr<WellTrack>> &dominant)
{
  return t && (t == prediction || t == confidence || dominant.contains(t));
}
} // namespace

bool WellCompositePanel::isDefaultLayout() const
{
  const ComprehensiveWellData doc = m_editSession ? m_editSession->document() : m_data;
  QList<TrackSpec> current;
  for (const auto &t : m_canvas->tracks())
  {
    if (!t || isResultTrack(t, m_predictionTrack, m_confidenceTrack, m_dominantTracks)) continue;
    TrackSpec spec = TrackRegistry::instance().captureSpec(t);
    if (spec.typeId.isEmpty()) spec.typeId = TrackRegistry::typeIdForEnum(t->type());
    current << spec;
  }
  QList<TrackSpec> defaults;
  for (const auto &t : TrackOps::defaultLayoutTracks(doc, m_scaleCombo->currentText()))
  {
    TrackSpec spec = TrackRegistry::instance().captureSpec(t);
    if (spec.typeId.isEmpty()) spec.typeId = TrackRegistry::typeIdForEnum(t->type());
    defaults << spec;
  }
  return TrackOps::layoutSignature(current) == TrackOps::layoutSignature(defaults);
}

void WellCompositePanel::updateDefaultLayoutButton()
{
  if (!m_btnDefaultLayout) return;
  const bool hasData = m_canvas && m_canvas->trackCount() > 0;
  m_btnDefaultLayout->setEnabled(hasData && !isDefaultLayout());
}

void WellCompositePanel::restoreDefaultLayout()
{
  const ComprehensiveWellData doc = m_editSession ? m_editSession->document() : m_data;
  auto tracks = TrackOps::defaultLayoutTracks(doc, m_scaleCombo->currentText());
  for (const auto &t : m_canvas->tracks())
    if (isResultTrack(t, m_predictionTrack, m_confidenceTrack, m_dominantTracks))
      tracks << t; // 预测结果道不属于模板，原样保留
  m_canvas->setTracks(tracks);
  m_canvas->setScaleRatio(m_scaleCombo->currentText());
  m_canvas->setDepthUnitLabel(m_depthFeet ? QStringLiteral("ft") : QString());
  m_canvas->setHighContrast(m_highContrast);
  refreshTwtLabels();
  m_canvas->syncScrollBars();
  m_canvas->updateAll();
  m_hiddenBar->setTracks(currentSpecs());
  onTrackOrderOrWidthChanged(); // 记忆落盘 + 按钮复位
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
