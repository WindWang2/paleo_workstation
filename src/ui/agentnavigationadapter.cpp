// 层：视图
#include "agentnavigationadapter.h"

#include "../linkage/selectioncontext.h"
#include "../linkage/wellmaplink.h"
#include "datapreview/datapreviewtabs.h"

#include <QCryptographicHash>
#include <QJsonArray>

// ui/agentnavigationadapter.cpp — I4：SelectionAccess + PreviewAccess 实现。
//
// 选择走 SelectionContext 的既有广播（origin=agent_navigation 是可信井来源：
// WellMapLink 复用业务 ID 索引 + QSignalBlocker 防回声；SeismicMapLink 侧
// 已按井域 origin 隔离，同字串 line_id 不被误选/清空）。回读以真实选择
// 总线和原生 QgsVectorLayer 选中集为准，不以信号发射次数充当结果。
// 预览委托 DataPreviewTabs typed 接缝；per-request 只读策略每次打开单独
// 传入，不改共享服务状态。

namespace {
constexpr int kSnapshotWellsCap = 32;
} // namespace

AgentNavigationAdapter::AgentNavigationAdapter(DataPreviewTabs *preview,
                                               SelectionContext *selection,
                                               WellMapLink *wellLink, QObject *parent)
  : QObject(parent), m_preview(preview), m_selection(selection), m_wellLink(wellLink)
{
}

QString AgentNavigationAdapter::unavailabilityReason() const
{
  if (m_selection.isNull())
    return QStringLiteral("selection_provider_unavailable");
  return QString();
}

QString AgentNavigationAdapter::selectionToken(const QStringList &ids,
                                               const QString &origin) const
{
  // 宿主生成的选择修订条件：内容 + 来源的稳定摘要。模型只能从
  // navigation_state 原样回传；自造 token 必然失配（stale_selection）。
  const QByteArray source = (ids.join(QLatin1Char('|')) + QLatin1Char('#') + origin).toUtf8();
  return QString::fromLatin1(
      QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex().left(32));
}

QJsonObject AgentNavigationAdapter::snapshot() const
{
  QJsonObject facts;
  if (m_selection.isNull()) {
    facts.insert(QStringLiteral("available"), false);
    return facts;
  }
  const QStringList ids = m_selection->selectedIds();
  QJsonArray wells;
  for (int i = 0; i < ids.size() && i < kSnapshotWellsCap; ++i)
    wells.append(ids.at(i));
  if (ids.size() > kSnapshotWellsCap)
    wells.append(QStringLiteral("…(%1)").arg(ids.size() - kSnapshotWellsCap));
  facts.insert(QStringLiteral("available"), true);
  facts.insert(QStringLiteral("wells"), wells);
  facts.insert(QStringLiteral("empty"), ids.isEmpty());
  facts.insert(QStringLiteral("origin"), m_selection->origin());
  facts.insert(QStringLiteral("token"), selectionToken(ids, m_selection->origin()));
  return facts;
}

QJsonObject AgentNavigationAdapter::select(const QStringList &wellIds)
{
  QJsonObject facts;
  if (m_selection.isNull()) {
    facts.insert(QStringLiteral("ok"), false);
    facts.insert(QStringLiteral("code"), QStringLiteral("selection_provider_unavailable"));
    return facts;
  }
  // replace 语义：整批（已去重保序）一次性广播；WellMapLink 在同一调用栈
  // 内按业务 ID 索引落到井层（QSignalBlocker 防回声——不依赖信号次数）。
  m_selection->setSelection(wellIds, QStringLiteral("agent_navigation"));
  // 真实回读：业务选择总线 + 原生 QgsVectorLayer 选中集。
  const QStringList applied = m_selection->selectedIds();
  QJsonArray selected;
  for (int i = 0; i < applied.size() && i < kSnapshotWellsCap; ++i)
    selected.append(applied.at(i));
  // 原生 QgsVectorLayer 选中集经 WellMapLink 只读出口回读（ui 不直连 qgs*
  // 头——qgs-audit/qgis-spread 闸；-1 = 井层未实例化/未绑定，如实报告）。
  const int mappedFeatures =
      m_wellLink ? m_wellLink->selectedWellFeatureCount() : -1;
  QJsonArray unmapped;
  for (const QString &id : wellIds)
    if (!applied.contains(id))
      unmapped.append(id);
  facts.insert(QStringLiteral("requested_count"), wellIds.size());
  facts.insert(QStringLiteral("selected_count"), applied.size());
  facts.insert(QStringLiteral("selected_wells"), selected);
  facts.insert(QStringLiteral("mapped_feature_count"), mappedFeatures);
  if (!unmapped.isEmpty())
    facts.insert(QStringLiteral("unmapped_wells"), unmapped);
  facts.insert(QStringLiteral("token"),
               selectionToken(applied, m_selection->origin()));
  const bool ok = applied.size() == wellIds.size() && unmapped.isEmpty();
  facts.insert(QStringLiteral("ok"), ok);
  if (!ok)
    facts.insert(QStringLiteral("code"), QStringLiteral("selection_readback_mismatch"));
  return facts;
}

QJsonObject AgentNavigationAdapter::clear()
{
  QJsonObject facts;
  if (m_selection.isNull()) {
    facts.insert(QStringLiteral("ok"), false);
    facts.insert(QStringLiteral("code"), QStringLiteral("selection_provider_unavailable"));
    return facts;
  }
  // 只清井选择域：origin=agent_navigation 已被 SeismicMapLink 按井域隔离，
  // 同字串地震线选择不受影响；解释/编辑目标不经本总线，天然不动。
  m_selection->setSelection({}, QStringLiteral("agent_navigation"));
  const QStringList applied = m_selection->selectedIds();
  facts.insert(QStringLiteral("status"),
               applied.isEmpty() ? QStringLiteral("cleared") : QStringLiteral("partial"));
  facts.insert(QStringLiteral("ok"), applied.isEmpty());
  if (!applied.isEmpty())
    facts.insert(QStringLiteral("code"), QStringLiteral("selection_readback_mismatch"));
  return facts;
}

QJsonObject AgentNavigationAdapter::previewSnapshot() const
{
  if (m_preview.isNull())
    return QJsonObject{{QStringLiteral("items"), QJsonArray()}};
  return m_preview->navigationPreviewSnapshot();
}

QJsonObject AgentNavigationAdapter::openWellView(
    const QString &versionId, const QString &wellId,
    const paleo::agent::navigation::PreviewAccess::Completion &done)
{
  if (m_preview.isNull()) {
    QJsonObject rejected{{QStringLiteral("status"), QStringLiteral("rejected")},
                         {QStringLiteral("code"), QStringLiteral("preview_provider_unavailable")}};
    return rejected;
  }
  // Agent 井资料导航的 Read 请求：per-request 只读 SHA 策略（失配只回报，
  // 不 markDownstreamStale——普通 UI 打开保持缺省写策略，互不影响）。
  m_preview->openWellViewVersionAsync(
      versionId, wellId, PreviewDocService::ShaGatePolicy::ReportOnly,
      [done](const DataPreviewTabs::WellViewFacts &facts) {
        if (!done)
          return;
        QJsonObject payload;
        payload.insert(QStringLiteral("status"), facts.state);
        payload.insert(QStringLiteral("page_open"), facts.pageOpen);
        payload.insert(QStringLiteral("page_focused"), facts.pageFocused);
        if (!facts.assetId.isEmpty())
          payload.insert(QStringLiteral("asset_id"), facts.assetId);
        if (!facts.versionId.isEmpty())
          payload.insert(QStringLiteral("version_id"), facts.versionId);
        if (!facts.wellId.isEmpty())
          payload.insert(QStringLiteral("well_id"), facts.wellId);
        if (!facts.detail.isEmpty())
          payload.insert(QStringLiteral("detail"),
                         paleo::agent::navigation::sanitizeModelText(facts.detail));
        done(payload);
      });
  return {}; // 已受理异步（回执经 done 恰一次）
}

QJsonObject AgentNavigationAdapter::openSeismicLine(
    const QString &versionId, const QString &axis, int lineNumber,
    const paleo::agent::navigation::PreviewAccess::Completion &done)
{
  if (m_preview.isNull()) {
    QJsonObject rejected{{QStringLiteral("status"), QStringLiteral("rejected")},
                         {QStringLiteral("code"), QStringLiteral("preview_provider_unavailable")}};
    return rejected;
  }
  m_preview->openSeismicLineVersion(
      versionId, axis, lineNumber, PreviewDocService::ShaGatePolicy::ReportOnly,
      [done](const QJsonObject &outcome) {
        if (done)
          done(outcome);
      });
  return {}; // 已受理异步（回执经 done 恰一次；被拒也在 done 内回报）
}
