// 层：QGIS 封装
// qgis/commandlocatorfilter.cpp — 见 commandlocatorfilter.h。
#include "commandlocatorfilter.h"

#include "../domain/operationsearch.h"

#include <qgsfeedback.h>

#include <algorithm>

CommandLocatorFilter::CommandLocatorFilter(ListFn list, TriggerFn trigger, QObject *parent)
  : QgsLocatorFilter(parent), m_list(std::move(list)), m_trigger(std::move(trigger))
{
}

QVector<QPair<CommandLocatorFilter::Entry, double>> CommandLocatorFilter::match(const QVector<Entry> &entries,
                                                                                const QString &query)
{
  const paleo::OperationSearch search(query);
  QVector<QPair<Entry, double>> out;
  for (const Entry &e : entries)
  {
    // 标题类候选：显示标题 + 别名（中文源串 / 英文标题）+ 命令 id，取最强匹配档。
    QStringList names{e.title, e.commandId};
    names += e.aliases;
    double score = search.score(names, {e.tooltip, e.group});
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
  if (!m_list)
    return;
  // 空闲定位器（无前缀、查询为空）不倾倒全部命令——否则结果列表常驻，
  // Tab 被 QGIS 当成「下一条结果」，键盘焦点出不去（tst_shell_chrome）。
  // 用户键入前缀 "cmd" 时 usingPrefix=true，空余串仍列出全部命令。
  if (string.trimmed().isEmpty() && !context.usingPrefix)
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
