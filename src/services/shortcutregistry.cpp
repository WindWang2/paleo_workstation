// 层：数据
#include "shortcutregistry.h"

namespace paleo::shortcuts
{

namespace
{
QString bindingName(Binding b)
{
  return b == Binding::Shortcut ? QStringLiteral("shortcut") : QStringLiteral("key-handler");
}

QString kindName(ShortcutConflict::Kind k)
{
  switch (k)
  {
  case ShortcutConflict::Kind::Ambiguous:
    return QStringLiteral("ambiguous");
  case ShortcutConflict::Kind::Duplicate:
    return QStringLiteral("duplicate");
  case ShortcutConflict::Kind::Shadowed:
    return QStringLiteral("shadowed");
  }
  return QString();
}

// 条目在 ctx 处是否激活：应用级快捷键处处激活，其余看作用域路径包含。
bool entryCovers(const ShortcutEntry &e, const QString &ctx)
{
  if (e.binding == Binding::Shortcut && e.qtContext == Qt::ApplicationShortcut)
    return true;
  return ShortcutRegistry::contextCovers(e.context, ctx);
}
} // namespace

QString ShortcutConflict::describe() const
{
  return QStringLiteral("%1: %2 [%3 @ %4 (%5)] <-> [%6 @ %7 (%8)]")
      .arg(kindName(kind), first.key.toString(QKeySequence::PortableText), first.id,
           first.context, bindingName(first.binding), second.id, second.context,
           bindingName(second.binding));
}

bool ShortcutRegistry::registerEntry(const ShortcutEntry &entry, QString *error)
{
  const auto fail = [error](const QString &why) {
    if (error)
      *error = why;
    return false;
  };
  if (entry.id.trimmed().isEmpty())
    return fail(QStringLiteral("empty id"));
  if (contains(entry.id))
    return fail(QStringLiteral("duplicate id: %1").arg(entry.id));
  if (entry.key.isEmpty())
    return fail(QStringLiteral("empty key sequence: %1").arg(entry.id));
  if (entry.context.trimmed().isEmpty())
    return fail(QStringLiteral("empty context: %1").arg(entry.id));
  if (entry.group.trimmed().isEmpty())
    return fail(QStringLiteral("empty group: %1").arg(entry.id));
  if (entry.description.trimmed().isEmpty())
    return fail(QStringLiteral("empty description: %1").arg(entry.id));
  m_entries.append(entry);
  return true;
}

bool ShortcutRegistry::contains(const QString &id) const
{
  for (const ShortcutEntry &e : m_entries)
    if (e.id == id)
      return true;
  return false;
}

ShortcutEntry ShortcutRegistry::entry(const QString &id) const
{
  for (const ShortcutEntry &e : m_entries)
    if (e.id == id)
      return e;
  return {};
}

QKeySequence ShortcutRegistry::key(const QString &id) const
{
  return entry(id).key;
}

QList<ShortcutEntry> ShortcutRegistry::entriesIn(Binding binding) const
{
  QList<ShortcutEntry> out;
  for (const ShortcutEntry &e : m_entries)
    if (e.binding == binding)
      out.append(e);
  return out;
}

QStringList ShortcutRegistry::groups() const
{
  QStringList out;
  for (const ShortcutEntry &e : m_entries)
    if (!out.contains(e.group))
      out.append(e.group);
  return out;
}

QList<ShortcutEntry> ShortcutRegistry::entriesInGroup(const QString &group) const
{
  QList<ShortcutEntry> out;
  for (const ShortcutEntry &e : m_entries)
    if (e.group == group)
      out.append(e);
  return out;
}

QList<ShortcutConflict> ShortcutRegistry::conflicts() const
{
  QList<ShortcutConflict> out;
  for (int i = 0; i < m_entries.size(); ++i)
  {
    const ShortcutEntry &a = m_entries.at(i);
    for (int j = i + 1; j < m_entries.size(); ++j)
    {
      const ShortcutEntry &b = m_entries.at(j);
      if (a.key != b.key)
        continue;
      if (a.binding == Binding::Shortcut && b.binding == Binding::Shortcut)
      {
        // 两条 Qt 快捷键：作用域重叠即同时激活 → Qt 判歧义。
        if (entryCovers(a, b.context) || entryCovers(b, a.context))
          out.append({ShortcutConflict::Kind::Ambiguous, a, b});
      }
      else if (a.context == b.context)
      {
        out.append({ShortcutConflict::Kind::Duplicate, a, b});
      }
    }
  }
  return out;
}

QList<ShortcutConflict> ShortcutRegistry::shadows() const
{
  QList<ShortcutConflict> out;
  for (const ShortcutEntry &s : m_entries)
  {
    if (s.binding != Binding::Shortcut)
      continue;
    for (const ShortcutEntry &h : m_entries)
    {
      if (h.binding != Binding::KeyHandler || h.key != s.key)
        continue;
      // 同上下文已在 conflicts() 计为 Duplicate；这里只收严格外层遮蔽。
      if (h.context != s.context && entryCovers(s, h.context))
        out.append({ShortcutConflict::Kind::Shadowed, s, h});
    }
  }
  return out;
}

bool ShortcutRegistry::contextCovers(const QString &outer, const QString &inner)
{
  if (outer.isEmpty() || inner.isEmpty())
    return false;
  if (outer == inner)
    return true;
  return inner.startsWith(outer) && inner.at(outer.size()) == QLatin1Char('/');
}

bool ShortcutRegistry::contextsRelated(const QString &a, const QString &b)
{
  return contextCovers(a, b) || contextCovers(b, a);
}

int ShortcutRegistry::contextDepth(const QString &context)
{
  if (context.isEmpty())
    return 0;
  return int(context.count(QLatin1Char('/'))) + 1;
}

std::optional<ShortcutEntry> ShortcutRegistry::resolve(const QKeySequence &key,
                                                        const QString &activeContext) const
{
  QList<const ShortcutEntry *> shortcuts;
  QList<const ShortcutEntry *> handlers;
  for (const ShortcutEntry &e : m_entries)
  {
    if (e.key != key || !entryCovers(e, activeContext))
      continue;
    (e.binding == Binding::Shortcut ? shortcuts : handlers).append(&e);
  }
  if (shortcuts.size() == 1)
    return *shortcuts.front();
  if (shortcuts.size() > 1)
    return std::nullopt; // Qt 歧义：一条都不触发
  const ShortcutEntry *best = nullptr;
  bool tie = false;
  for (const ShortcutEntry *h : handlers)
  {
    const int depth = contextDepth(h->context);
    const int bestDepth = best ? contextDepth(best->context) : -1;
    if (depth > bestDepth)
    {
      best = h;
      tie = false;
    }
    else if (depth == bestDepth)
    {
      tie = true;
    }
  }
  if (!best || tie)
    return std::nullopt;
  return *best;
}

} // namespace paleo::shortcuts
