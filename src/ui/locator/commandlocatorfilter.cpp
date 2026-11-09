// 层：视图
// ui/locator/commandlocatorfilter.cpp — 见 commandlocatorfilter.h。
#include "commandlocatorfilter.h"

#include <qgsfeedback.h>

#include <algorithm>

CommandLocatorFilter::CommandLocatorFilter(ListFn list, TriggerFn trigger, QObject *parent)
  : QgsLocatorFilter(parent), m_list(std::move(list)), m_trigger(std::move(trigger))
{
}

QVector<QPair<CommandLocatorFilter::Entry, double>> CommandLocatorFilter::match(const QVector<Entry> &entries,
                                                                                const QString &query)
{
  const QString q = query.trimmed();
  QVector<QPair<Entry, double>> out;
  for (const Entry &e : entries)
  {
    double score = 0;
    if (q.isEmpty())
      score = 0.5;
    else if (e.title.compare(q, Qt::CaseInsensitive) == 0 || e.commandId.compare(q, Qt::CaseInsensitive) == 0)
      score = 1.0;
    else if (e.title.startsWith(q, Qt::CaseInsensitive) || e.commandId.startsWith(q, Qt::CaseInsensitive))
      score = 0.9;
    else if (e.title.contains(q, Qt::CaseInsensitive) || e.commandId.contains(q, Qt::CaseInsensitive))
      score = 0.8;
    else if (e.tooltip.contains(q, Qt::CaseInsensitive) || e.group.contains(q, Qt::CaseInsensitive))
      score = 0.6;
    if (score <= 0)
      continue;
    if (!e.enabled)
      score -= 0.3; // 禁用的仍列出（带原因），排在可用的后面
    out.append({e, score});
  }
  std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
  return out;
}

void CommandLocatorFilter::fetchResults(const QString &string, const QgsLocatorContext &context,
                                        QgsFeedback *feedback)
{
  Q_UNUSED(context)
  if (!m_list)
    return;
  for (const auto &[e, score] : match(m_list(), string))
  {
    if (feedback && feedback->isCanceled())
      return;
    QgsLocatorResult result;
    result.filter = this;
    result.displayString = e.title;
    result.description = e.enabled ? (e.tooltip.isEmpty() ? e.group : e.tooltip)
                                   : tr("不可用：%1").arg(e.reason);
    result.group = displayName();
    result.groupScore = 0.9;
    result.score = score;
    result.setUserData(e.commandId);
    emit resultFetched(result);
  }
}

void CommandLocatorFilter::triggerResult(const QgsLocatorResult &result)
{
  if (m_trigger)
    m_trigger(result.userData().toString());
}
