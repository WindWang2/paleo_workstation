// 层：视图
// ui/locator/commandlocatorfilter — Ctrl+K 命令搜索（UI 重设计 U1）。
//
// 列出 CommandRegistry 里带 Locator 入口的全部命令：可用的按匹配度排前，禁用的
// 照样列出并在说明里写原因（§35 禁用必带原因）；选中即经 CommandDispatcher 触发。
// 前缀 "c"（与 w 井位 / h 层位 同族）；不带前缀时也参与搜索。
#pragma once

#include <qgslocatorfilter.h>

#include <QVector>

#include <functional>

class QgsFeedback;

class CommandLocatorFilter : public QgsLocatorFilter
{
  Q_OBJECT
public:
  struct Entry
  {
    QString commandId;
    QString title;
    QString tooltip;
    QString group;   // ribbonGroup 或阶段名
    bool enabled = false;
    QString reason;  // 禁用原因（enabled 时为空）
  };
  using ListFn = std::function<QVector<Entry>()>;
  using TriggerFn = std::function<void(const QString &commandId)>;

  CommandLocatorFilter(ListFn list, TriggerFn trigger, QObject *parent = nullptr);
  QString name() const override { return QStringLiteral("paleo_commands"); }
  QString displayName() const override { return tr("命令"); }
  Priority priority() const override { return High; }
  QString prefix() const override { return QStringLiteral("c"); }
  QgsLocatorFilter::Flags flags() const override { return FlagFast; }
  CommandLocatorFilter *clone() const override { return new CommandLocatorFilter(m_list, m_trigger); }
  void fetchResults(const QString &string, const QgsLocatorContext &context, QgsFeedback *feedback) override;
  void triggerResult(const QgsLocatorResult &result) override;

  // 纯函数（测试用）：按查询串过滤并打分，空串 = 全部（按标题序）。
  static QVector<QPair<Entry, double>> match(const QVector<Entry> &entries, const QString &query);

private:
  ListFn m_list;
  TriggerFn m_trigger;
};
