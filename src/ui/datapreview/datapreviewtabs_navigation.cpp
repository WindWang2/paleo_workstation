// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

#include "../../catalog/datacatalog.h"
#include "../../services/agentnavigationaccess.h" // sanitizeModelText（detail 路径净化）
#include "../../services/sectiondoc.h" // PreviewDocService::SectionDoc 完整定义（结果信号成员访问）

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>

// datapreviewtabs_navigation.cpp — I4：typed 导航接缝实现（Agent 语义导航
// 专用；普通 UI 流程不走）。三个职责（任务书 W5/W6/W8）：
//   1. 固定版本井资料的严格打开 + 三事实回读（页面创建/聚焦/正文显示）；
//   2. 固定版本地震测线的真实请求与显示完成（拒绝越界/钳位；结果按目标
//      (axis,line) 认领，用户换线/关页/重建 → superseded/cancelled，迟到
//      回执不产生第二终态）；
//   3. per-request SHA 策略转发（m_navigationPolicyDepth 只在同步 typed 调用
//      栈内非零——不是共享全局开关，模型不可设）。

using namespace paleo::datapreview_detail;

namespace {
// navigation_state 快照的预览目标条目上限。
constexpr int kSnapshotPreviewsCap = 8;

bool seismicAxisIsInline(const QString &axis) {
  return axis == QLatin1String("inline");
}
} // namespace

PreviewDocService::ShaGatePolicy DataPreviewTabs::navigationShaPolicy() const
{
  return m_navigationPolicyDepth > 0 ? PreviewDocService::ShaGatePolicy::ReportOnly
                                    : PreviewDocService::ShaGatePolicy::MarkDownstreamStale;
}

// 已知边界（审查 I4-R7 备注）：深度标记覆盖整个同步 typed 调用栈，含
// rebuildAssetTab 尾部的 drainDeferredRebuilds()——期间顺延重建的「其他
// 资产」也会按 ReportOnly 过闸（少一次 stale 标记，不会误写）。GUI 线程
// 同栈语义下窗口极窄；按资产作用域化留待后续接缝收敛。

// ---- 井资料：固定版本 + 指定井 -------------------------------------------

void DataPreviewTabs::openWellViewVersionAsync(
    const QString &versionId, const QString &wellId, PreviewDocService::ShaGatePolicy policy,
    std::function<void(const WellViewFacts &)> done)
{
  const auto deliver = [done](const WellViewFacts &facts) {
    if (done)
      done(facts);
  };
  if (!m_doc || versionId.isEmpty() || wellId.isEmpty()) {
    WellViewFacts facts;
    facts.state = QStringLiteral("error");
    facts.detail = tr("预览门面未接线或目标不完整");
    deliver(facts);
    return;
  }
  const CatalogVersion v = m_doc->versionForPreview(versionId);
  if (v.id.isEmpty()) {
    WellViewFacts facts;
    facts.state = QStringLiteral("version_unknown");
    facts.versionId = versionId;
    deliver(facts);
    return;
  }
  // 外链版本先过版本精确 SHA 闸（Agent 只读导航按 policy——失配只回报，
  // 不写 Catalog）；已验（本会话同 asset+version+指纹）则直接打开。
  if (!m_doc->externalShaVerified(v.assetId, v)) {
    const QString abs = m_doc->absolutePathForVersion(v);
    if (abs.isEmpty() || !QFile::exists(abs)) {
      WellViewFacts facts;
      facts.state = QStringLiteral("source_missing");
      facts.assetId = v.assetId;
      facts.versionId = v.id;
      deliver(facts);
      return;
    }
    QPointer<DataPreviewTabs> guard(this);
    m_doc->verifyExternalShaAsync(
        v.assetId, v, abs, this,
        [this, guard, versionId, wellId, policy, done](bool ok, const QString &error) {
          if (!guard)
            return; // 标签宿主已销毁：无人可回执（编排层按截止收口）
          if (!ok) {
            WellViewFacts facts;
            facts.state = QStringLiteral("sha_mismatch");
            facts.versionId = versionId;
            facts.detail = error;
            if (done)
              done(facts);
            return;
          }
          if (done)
            done(openWellViewVersion(versionId, wellId, policy));
        },
        policy);
    return;
  }
  deliver(openWellViewVersion(versionId, wellId, policy));
}

DataPreviewTabs::WellViewFacts DataPreviewTabs::openWellViewVersion(
    const QString &versionId, const QString &wellId, PreviewDocService::ShaGatePolicy policy)
{
  WellViewFacts facts;
  facts.versionId = versionId;
  facts.wellId = wellId;
  if (!m_doc) {
    facts.state = QStringLiteral("error");
    facts.detail = tr("预览门面未接线");
    return facts;
  }
  const CatalogVersion v = m_doc->versionForPreview(versionId);
  if (v.id.isEmpty()) {
    facts.state = QStringLiteral("version_unknown");
    return facts;
  }
  facts.assetId = v.assetId;
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat ? cat->assetById(v.assetId) : CatalogAsset();
  if (!asset.id.isEmpty() && asset.type != QLatin1String("well_head") &&
      asset.type != QLatin1String("well_stratification") &&
      asset.type != QLatin1String("time_depth")) {
    facts.state = QStringLiteral("unsupported_well_view_type");
    return facts;
  }

  ++m_navigationPolicyDepth; // 本调用栈内的闸门/解码请求按 policy
  const auto restoreDepth = qScopeGuard([this] { --m_navigationPolicyDepth; });
  Q_UNUSED(restoreDepth);

  const bool existing = m_pageOfAsset.contains(v.assetId);
  if (m_chosenVersionOfAsset.value(v.assetId) != v.id) {
    // 版本切换：取消旧世代并释放旧句柄（openVersion 同口径）。
    m_doc->releaseSection(v.assetId, this);
    m_doc->releaseLas(v.assetId, this);
    m_pendingSection.remove(v.assetId);
    m_pendingLas.remove(v.assetId);
  }
  m_chosenVersionOfAsset[v.assetId] = v.id;
  // 预置井选择：重建时 buildWellFilteredContent 按 prev 恢复 combo（多井），
  // 单井资产直滤。请求井必须是已决关联（编排层已核实）。
  m_wellEntityOfAsset[v.assetId] = wellId;
  if (cat)
    m_titleSuffixOfAsset[v.assetId] = cat->entityById(wellId).name;
  setProperty("paleo.versionNavigation", true);
  openAsset(v.assetId);
  if (existing)
    rebuildAssetTab(v.assetId);
  setProperty("paleo.versionNavigation", false);
  emit versionContextChanged(v.assetId, v.id);

  QWidget *page = m_pageOfAsset.value(v.assetId);
  facts.pageOpen = bool(page);
  if (!page) {
    facts.state = QStringLiteral("error");
    facts.detail = tr("预览页面未能创建");
    return facts;
  }
  facts.pageFocused = m_tabs->currentWidget() == page;
  facts.state = classifyWellBodyState(page, v.assetId, wellId);
  facts.showsWell = facts.state == QLatin1String("shown");
  facts.versionId = page->property("previewVersionId").toString();
  facts.wellId = m_wellEntityOfAsset.value(v.assetId);
  return facts;
}

QString DataPreviewTabs::classifyWellBodyState(QWidget *page, const QString &assetId,
                                               const QString &wellId) const
{
  // 正文状态按本类自设的稳定标记回读（deadEndText/retryBtn 为既有面；
  // wellNotInFileText/noTimeDepthText/timeDepthPanel 为本接缝补钉）——
  // 只读观察，不做对象名驱动的控制自动化。
  if (page->findChild<QWidget *>(QStringLiteral("retryBtn")))
    return QStringLiteral("error");
  if (page->findChild<QWidget *>(QStringLiteral("deadEndText")))
    return QStringLiteral("no_resolved_wells");
  if (page->findChild<QWidget *>(QStringLiteral("wellNotInFileText")) ||
      page->findChild<QWidget *>(QStringLiteral("noTimeDepthText")))
    return QStringLiteral("well_not_in_file");
  if (m_wellEntityOfAsset.value(assetId) != wellId)
    return QStringLiteral("select_well"); // combo 未接受目标井：保真失败
  DataCatalog *cat = m_doc ? m_doc->catalog() : nullptr;
  const QString type = cat ? cat->assetById(assetId).type : QString();
  if (type == QLatin1String("well_stratification")) {
    auto *table = page->findChild<QTableWidget *>(QStringLiteral("topsTable"));
    if (!table)
      return QStringLiteral("error");
    return table->rowCount() > 0 ? QStringLiteral("shown")
                                 : QStringLiteral("well_not_in_file"); // 没有该井样点
  }
  // well_head/time_depth：失败面已在上文排除，buildWellBody 到达此处即成功
  //（well_head：信息条+井位图；time_depth：TIME–TVD 曲线面板）。
  return QStringLiteral("shown");
}

// ---- 地震测线：固定版本 + 真实显示完成 ------------------------------------

void DataPreviewTabs::openSeismicLineVersion(
    const QString &versionId, const QString &axis, int lineNumber,
    PreviewDocService::ShaGatePolicy policy, std::function<void(const QJsonObject &)> done)
{
  const auto deliver = [this, &done](const QString &assetId, const QString &version,
                                    const QString &axisName, int line, const QString &status,
                                    const QString &code, const QString &detail) {
    if (!done)
      return;
    QJsonObject outcome;
    outcome.insert(QStringLiteral("status"), status);
    if (!code.isEmpty())
      outcome.insert(QStringLiteral("code"), code);
    outcome.insert(QStringLiteral("asset_id"), assetId);
    outcome.insert(QStringLiteral("version_id"), version);
    outcome.insert(QStringLiteral("axis"), axisName);
    outcome.insert(QStringLiteral("line_number"), line);
    if (!detail.isEmpty())
      outcome.insert(QStringLiteral("detail"), detail);
    done(outcome);
  };
  if (!m_doc || versionId.isEmpty()) {
    deliver(QString(), versionId, axis, lineNumber, QStringLiteral("rejected"),
            QStringLiteral("preview_unavailable"), tr("预览门面未接线或目标不完整"));
    return;
  }
  const CatalogVersion v = m_doc->versionForPreview(versionId);
  if (v.id.isEmpty()) {
    deliver(QString(), versionId, axis, lineNumber, QStringLiteral("rejected"),
            QStringLiteral("version_unknown"), QString());
    return;
  }
  const QString assetId = v.assetId;
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat ? cat->assetById(assetId) : CatalogAsset();
  if (!asset.id.isEmpty() && asset.type != QLatin1String("seismic")) {
    deliver(assetId, v.id, axis, lineNumber, QStringLiteral("rejected"),
            QStringLiteral("not_seismic_asset"), QString());
    return;
  }
  // survey 冻结几何的有界预检（与 applyLineSpinRange 同源；真实存在性由
  // 解码结果判定）。拒绝发生在任何控件写入之前——不吃 SpinBox 钳位。
  QString surveyId;
  if (cat)
    for (const EntityAssetLink &l : cat->linksForAsset(assetId))
      if (l.role == QLatin1String("seismic_volume") && !l.unresolved && !l.entityId.isEmpty())
        surveyId = l.entityId;
  const CatalogEntity survey =
      (cat && !surveyId.isEmpty()) ? cat->entityById(surveyId) : CatalogEntity();
  const bool isInline = seismicAxisIsInline(axis);
  const double lo = isInline ? survey.inlineMin : survey.xlineMin;
  const double hi = isInline ? survey.inlineMax : survey.xlineMax;
  const bool unbounded = lo == 0.0 && hi == 0.0;
  const int fallbackLimit = 1000000;
  if ((!unbounded && (lineNumber < lo || lineNumber > hi)) ||
      (unbounded && (lineNumber < 0 || lineNumber > fallbackLimit))) {
    deliver(assetId, v.id, axis, lineNumber, QStringLiteral("rejected"),
            QStringLiteral("line_out_of_survey_range"), QString());
    return;
  }

  ++m_navigationPolicyDepth;
  const auto restoreDepth = qScopeGuard([this] { --m_navigationPolicyDepth; });
  Q_UNUSED(restoreDepth);

  const bool existing = m_pageOfAsset.contains(assetId);
  if (m_chosenVersionOfAsset.value(assetId) != v.id) {
    m_doc->releaseSection(assetId, this);
    m_pendingSection.remove(assetId);
    m_pendingLas.remove(assetId);
  }
  m_chosenVersionOfAsset[assetId] = v.id;
  // 先登记观察再开页：建页触发的初始解码（初始测线）与随后的目标解码，
  // 其结果都经 wireNavigationOutcomeRoutes 的挂起表认领/顶替。
  {
    NavigationSectionWatch watch;
    watch.versionId = v.id;
    watch.axis = isInline ? QStringLiteral("inline") : QStringLiteral("crossline");
    watch.line = lineNumber;
    watch.inlineAxis = isInline;
    watch.done = done;
    m_navigationSectionWatch.insert(assetId, watch);
  }
  setProperty("paleo.versionNavigation", true);
  openAsset(assetId);
  if (existing)
    rebuildAssetTab(assetId);
  setProperty("paleo.versionNavigation", false);
  emit versionContextChanged(assetId, v.id);

  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page) {
    dropNavigationWatch(assetId, QStringLiteral("rejected"),
                        tr("预览页面未能创建"));
    return;
  }
  auto *mode = page->findChild<QComboBox *>(QStringLiteral("lineMode"));
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  if (!mode || !no) {
    dropNavigationWatch(assetId, QStringLiteral("rejected"),
                        tr("地震正文未建出测线控件"));
    return;
  }
  const int want = mode->findData(isInline ? QStringLiteral("inline")
                                           : QStringLiteral("crossline"));
  if (want < 0) {
    dropNavigationWatch(assetId, QStringLiteral("rejected"), tr("非法测线轴"));
    return;
  }
  if (mode->currentIndex() != want)
    mode->setCurrentIndex(want); // currentIndexChanged → 控件链路 decode
  if (no->value() != lineNumber)
    no->setValue(lineNumber); // valueChanged → decode 目标测线
  if (no->value() != lineNumber) {
    // 防御：范围预检与控件范围同源，正常不可达；到达即如实拒绝，不把
    // 钳位后的其他测线算成功。
    dropNavigationWatch(assetId, QStringLiteral("rejected"),
                        tr("测线号被控件范围改写"));
    return;
  }
  if (auto *modeTabs = page->findChild<QTabWidget *>(QStringLiteral("seismicSubTabs")))
    modeTabs->setCurrentIndex(0); // 聚焦二维测线剖面页签
  // 目标解码已发出——此后非目标结果才是用户换线顶替（superseded）；建页
  // 初始解码的非目标结果在此之前静默忽略。审查修复（I4-R1）：既有页总是
  // 经 rebuildAssetTab 重建并重发解码（重建的初始解码或本处的 setValue 至少
  // 一次），目标匹配的结果信号必然到来——不再用标题后缀做「已显示」短路
  //（后缀与版本无关且重建后已陈旧，会伪造 shown 终态并吞掉后续失败）。
  if (m_navigationSectionWatch.contains(assetId))
    m_navigationSectionWatch[assetId].armed = true;
}

void DataPreviewTabs::notifyNavigationSectionOutcome(const QString &assetId,
                                                     const QString &status,
                                                     const QString &detail)
{
  const auto watch = m_navigationSectionWatch.take(assetId);
  if (!watch.done)
    return;
  QJsonObject outcome;
  outcome.insert(QStringLiteral("status"), status);
  outcome.insert(QStringLiteral("asset_id"), assetId);
  outcome.insert(QStringLiteral("version_id"), watch.versionId);
  outcome.insert(QStringLiteral("axis"), watch.axis);
  outcome.insert(QStringLiteral("line_number"), watch.line);
  if (!detail.isEmpty())
    outcome.insert(QStringLiteral("detail"),
                   paleo::agent::navigation::sanitizeModelText(detail));
  watch.done(outcome);
}

void DataPreviewTabs::dropNavigationWatch(const QString &assetId, const QString &status,
                                          const QString &detail)
{
  if (!m_navigationSectionWatch.contains(assetId))
    return;
  notifyNavigationSectionOutcome(assetId, status, detail);
}

void DataPreviewTabs::wireNavigationOutcomeRoutes()
{
  if (!m_doc)
    return;
  const quint64 self = PreviewDocService::consumerTag(this);
  const auto ownResult = [self](quint64 consumer) { return consumer == 0 || consumer == self; };
  // 振幅先到（异步模式的渐进交付）：按目标认领后回执 partial——振幅已
  // 真实显示，附属层位仍在后台读取（诚实区分，不把终态编造成 shown）。
  connect(m_doc, &PreviewDocService::seismicSectionImageReady, this,
          [this, ownResult](const QString &assetId,
                            const PreviewDocService::SectionDoc &doc, quint64 consumer) {
            if (!ownResult(consumer))
              return;
            const auto watch = m_navigationSectionWatch.value(assetId);
            if (!watch.done || doc.isInline != watch.inlineAxis || doc.lineNo != watch.line)
              return;
            notifyNavigationSectionOutcome(assetId, QStringLiteral("partial"),
                                           tr("振幅已显示，地震层位仍在读取"));
          });
  connect(m_doc, &PreviewDocService::seismicSectionReady, this,
          [this, ownResult](const QString &assetId,
                            const PreviewDocService::SectionDoc &doc, quint64 consumer) {
            if (!ownResult(consumer))
              return;
            const auto watch = m_navigationSectionWatch.value(assetId);
            if (!watch.done)
              return;
            if (doc.isInline != watch.inlineAxis || doc.lineNo != watch.line) {
              // 建页初始解码的噪声（目标解码未armed前）不算顶替；armed 后的
              // 非目标结果 = 用户已换线/换轴（旧结果不覆盖新画面）。
              if (watch.armed)
                notifyNavigationSectionOutcome(assetId, QStringLiteral("superseded"));
              return;
            }
            notifyNavigationSectionOutcome(assetId, QStringLiteral("shown"));
          });
  connect(m_doc, &PreviewDocService::seismicSectionFailed, this,
          [this, ownResult](const QString &assetId, const QString &reason,
                            quint64 consumer) {
            if (!ownResult(consumer))
              return;
            const auto watch = m_navigationSectionWatch.value(assetId);
            if (!watch.done)
              return;
            const SectionPending pending = m_pendingSection.value(assetId);
            if (pending.expectedIsInline == watch.inlineAxis &&
                pending.expectedLineNo == watch.line)
              notifyNavigationSectionOutcome(assetId, QStringLiteral("failed"), reason);
            else if (watch.armed)
              notifyNavigationSectionOutcome(assetId, QStringLiteral("superseded"));
          });
  connect(m_doc, &PreviewDocService::seismicSectionCancelled, this,
          [this, ownResult](const QString &assetId, quint64 consumer) {
            if (!ownResult(consumer))
              return;
            dropNavigationWatch(assetId, QStringLiteral("cancelled"));
          });
}

QJsonObject DataPreviewTabs::navigationPreviewSnapshot() const
{
  QJsonArray previews;
  if (!m_doc)
    return QJsonObject{{QStringLiteral("items"), previews}};
  DataCatalog *cat = m_doc->catalog();
  const QStringList assetIds = m_pageOfAsset.keys();
  for (int i = 0; i < assetIds.size() && i < kSnapshotPreviewsCap; ++i) {
    const QString &assetId = assetIds.at(i);
    QWidget *page = m_pageOfAsset.value(assetId);
    if (!page)
      continue;
    const CatalogAsset asset = cat ? cat->assetById(assetId) : CatalogAsset();
    QJsonObject item;
    item.insert(QStringLiteral("asset_id"), assetId);
    if (!asset.type.isEmpty())
      item.insert(QStringLiteral("type"), asset.type);
    const QString versionId = page->property("previewVersionId").toString();
    if (!versionId.isEmpty())
      item.insert(QStringLiteral("version_id"), versionId);
    const QString wellId = m_wellEntityOfAsset.value(assetId);
    if (!wellId.isEmpty())
      item.insert(QStringLiteral("well_id"), wellId);
    const QString suffix = m_titleSuffixOfAsset.value(assetId);
    if (!suffix.isEmpty())
      item.insert(QStringLiteral("display_suffix"), suffix);
    QString state = QStringLiteral("open");
    if (m_shaFailedOfVersion.contains(versionId))
      state = QStringLiteral("sha_failed");
    else if (page->findChild<QWidget *>(QStringLiteral("retryBtn")))
      state = QStringLiteral("error");
    else if (m_pendingSection.contains(assetId))
      state = QStringLiteral("loading");
    item.insert(QStringLiteral("state"), state);
    previews.append(item);
  }
  return QJsonObject{{QStringLiteral("items"), previews}};
}
