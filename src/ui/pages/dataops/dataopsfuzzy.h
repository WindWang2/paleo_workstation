// 层：视图
// ui/pages/dataops/dataopsfuzzy — D6.1/D6.2：模糊匹配器 + 命令注册表。
// 命令注册表是面板（Ctrl+Shift+P 命令面板）/快捷键表（? 键）/右键菜单的统一动作登记处
//（D6.2：所有可命令化操作统一登记，供面板与菜单共用）。
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace paleo::dataops
{

// ---- 模糊匹配（subsequence 打分；空查询 = 全命中）---------------------------
// 打分：连续命中 +8、词首命中 +6、其余命中 +2；大小写不敏感。分数 > 0 才算
// 命中。简单可控（无第三方依赖），10k 条目 × 短查询在毫秒级。
inline int fuzzyScore(const QString &candidate, const QString &query)
{
  if (query.isEmpty())
    return 1;
  if (candidate.size() < query.size())
    return 0;
  const QString c = candidate.toLower();
  const QString q = query.toLower();
  int score = 0, ci = 0, streak = 0;
  for (int qi = 0; qi < q.size(); ++qi)
  {
    const QChar ch = q.at(qi);
    if (ch.isSpace())
      continue;
    int found = -1;
    for (int k = ci; k < c.size(); ++k)
      if (c.at(k) == ch)
      {
        found = k;
        break;
      }
    if (found < 0)
      return 0;
    streak = (found == ci && qi > 0) ? streak + 1 : 0;
    score += 2 + streak * 8;
    if (found == 0 || c.at(found - 1).isSpace() || c.at(found - 1) == QLatin1Char('_') ||
        c.at(found - 1) == QLatin1Char('-') || c.at(found - 1) == QLatin1Char('.'))
      score += 6; // 词首
    ci = found + 1;
  }
  return score;
}

// 对候选集模糊排序（稳定：同分保原序）。返回 (index, score) 对。
inline QList<QPair<int, int>> fuzzyRank(const QStringList &candidates,
                                        const QString &query)
{
  QList<QPair<int, int>> out;
  for (int i = 0; i < candidates.size(); ++i)
  {
    const int s = fuzzyScore(candidates.at(i), query);
    if (s > 0)
      out.append({i, s});
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const auto &a, const auto &b) { return a.second > b.second; });
  return out;
}

// ---- D6.2 命令注册表 --------------------------------------------------------
// 一条命令 = id + 标题 + 分类 + 关键词 + 快捷键串 + 触发器 + 使能谓词。
// 注册表只存数据与 std::function——面板渲染它、快捷键表渲染它、菜单查它。
struct CommandEntry
{
  QString id;        // "dataops.selectAll"
  QString title;     // 「全选可见项」
  QString category;  // 「选择 / 批量 / 视图 / 过滤 / 导入 …」
  QStringList keywords;
  QString shortcut;  // "Ctrl+A"（显示用；实际快捷键由部件安装）
  int weight = 0;    // 同分排序加权（常用命令排前）
  std::function<bool()> enabled;      // 空恒 true
  std::function<void()> trigger;      // 回调（面板回车/菜单触发）

  bool isEnabled() const { return !enabled || enabled(); }
};

class CommandRegistry
{
public:
  // 登记命令（id 重复 → 替换；面板与菜单共用同一份登记）。
  void registerCommand(const CommandEntry &e) { m_byId.insert(e.id, e); }
  void unregister(const QString &id) { m_byId.remove(id); }
  void clear() { m_byId.clear(); }

  QList<CommandEntry> all() const
  {
    QList<CommandEntry> out;
    for (auto it = m_byId.constBegin(); it != m_byId.constEnd(); ++it)
      out.append(it.value());
    return out;
  }
  bool has(const QString &id) const { return m_byId.contains(id); }
  CommandEntry byId(const QString &id) const { return m_byId.value(id); }

  // D6.1 检索：标题 + 关键词 + 分类都参与模糊打分（取最高分）。
  struct Match
  {
    CommandEntry entry;
    int score = 0;
  };
  QList<Match> search(const QString &query) const
  {
    QList<Match> out;
    for (const CommandEntry &e : all())
    {
      int best = fuzzyScore(e.title, query);
      for (const QString &k : e.keywords)
        best = qMax(best, fuzzyScore(k, query));
      best = qMax(best, fuzzyScore(e.category, query));
      if (best <= 0)
        continue;
      out.append({e, best + e.weight});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Match &a, const Match &b) { return a.score > b.score; });
    return out;
  }

  // D6.6 快捷键冲突检测：同一快捷键串被多条命令登记 → 冲突对。
  struct Conflict
  {
    QString shortcut;
    QStringList commandIds;
  };
  QList<Conflict> shortcutConflicts() const
  {
    QHash<QString, QStringList> byKey;
    for (const CommandEntry &e : all())
      if (!e.shortcut.isEmpty())
        byKey[e.shortcut].append(e.id);
    QList<Conflict> out;
    for (auto it = byKey.constBegin(); it != byKey.constEnd(); ++it)
      if (it.value().size() > 1)
      {
        Conflict c;
        c.shortcut = it.key();
        c.commandIds = it.value();
        out.append(c);
      }
    return out;
  }

private:
  QHash<QString, CommandEntry> m_byId;
};

} // namespace paleo::dataops
