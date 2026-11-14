// 层：功能
#include "dataopscommandservice.h"

#include <QHash>

namespace paleo::dataops
{

// ---- 标签族 ----------------------------------------------------------------
QString TagCmd::text() const
{
  return m_add ? QObject::tr("打标签 %1「%2」").arg(m_assetId, m_tag)
               : QObject::tr("去标签 %1「%2」").arg(m_assetId, m_tag);
}
bool TagCmd::run(bool add)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = add ? QStringLiteral("tag-add") : QStringLiteral("tag-remove");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("tag"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const QHash<QString, QStringList> snap = m_ctx.tags->snapshot();
  const bool changed = add ? m_ctx.tags->addTag(m_assetId, m_tag)
                           : m_ctx.tags->removeTag(m_assetId, m_tag);
  if (!changed)
  {
    m_receipt.addItem(m_assetId, QStringLiteral("tag"), OpOutcome::NoChange,
                      QObject::tr("标签未变化（已存在或不存在）"));
    return true;
  }
  if (!m_ctx.tags->save())
  {
    m_ctx.tags->restoreSnapshot(snap); // 内存回到可信前态
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    m_receipt.addItem(m_assetId, QStringLiteral("tag"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("tag"), OpOutcome::Changed, QString(),
                    true);
  return true;
}
bool TagCmd::redo()
{
  return run(m_add);
}
bool TagCmd::undo()
{
  return run(!m_add);
}
bool TagCmd::mergeWith(const DataOpCommand *other)
{
  const auto *o = dynamic_cast<const TagCmd *>(other);
  return o && o->m_assetId == m_assetId && o->m_tag == m_tag && o->m_add == m_add;
}

QString BatchTagCmd::text() const
{
  return m_add ? QObject::tr("打标签「%1」（%2 项）").arg(m_tag).arg(m_ids.size())
               : QObject::tr("去标签「%1」（%2 项）").arg(m_tag).arg(m_ids.size());
}
bool BatchTagCmd::run(bool add)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = add ? QStringLiteral("tag-add") : QStringLiteral("tag-remove");
  m_receipt.requested = m_ids.size();
  QString why;
  if (!m_ctx.alive(&why))
  {
    for (const QString &id : m_ids)
      m_receipt.addItem(id, QStringLiteral("tag"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const QHash<QString, QStringList> snap = m_ctx.tags->snapshot();
  QStringList applied;
  for (const QString &id : m_ids)
  {
    const QStringList prev = m_ctx.tags->tagsFor(id);
    const bool changed = add ? m_ctx.tags->addTag(id, m_tag)
                             : m_ctx.tags->removeTag(id, m_tag);
    if (!changed)
    {
      m_receipt.addItem(id, QStringLiteral("tag"), OpOutcome::NoChange,
                        QObject::tr("标签未变化（已存在或不存在）"));
      continue;
    }
    m_prev.insert(id, prev); // 前态（含 redo→undo→redo 循环的重捕获）
    applied << id;
  }
  if (applied.isEmpty())
    return true; // 全幂等：栈丢弃（无虚假历史）
  if (!m_ctx.tags->save())
  {
    m_ctx.tags->restoreSnapshot(snap); // 一次提交失败 → 整批内存还原
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    for (const QString &id : applied)
      m_receipt.addItem(id, QStringLiteral("tag"), OpOutcome::FailedRecovered, m_error);
    return false; // 已恢复 → storeIntact=true
  }
  for (const QString &id : applied)
    m_receipt.addItem(id, QStringLiteral("tag"), OpOutcome::Changed, QString(), true);
  return true;
}
bool BatchTagCmd::redo()
{
  return run(m_add);
}
bool BatchTagCmd::undo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation =
      m_add ? QStringLiteral("tag-add") : QStringLiteral("tag-remove");
  QString why;
  if (!m_ctx.alive(&why))
  {
    for (auto it = m_prev.constBegin(); it != m_prev.constEnd(); ++it)
      m_receipt.addItem(it.key(), QStringLiteral("tag"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  if (m_prev.isEmpty())
    return true;
  const QHash<QString, QStringList> snap = m_ctx.tags->snapshot();
  for (auto it = m_prev.constBegin(); it != m_prev.constEnd(); ++it)
    m_ctx.tags->setTagsExact(it.key(), it.value()); // 精确恢复前态（不误删已有标签）
  if (!m_ctx.tags->save())
  {
    m_ctx.tags->restoreSnapshot(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    for (auto it = m_prev.constBegin(); it != m_prev.constEnd(); ++it)
      m_receipt.addItem(it.key(), QStringLiteral("tag"), OpOutcome::FailedRecovered,
                        m_error);
    return false;
  }
  for (auto it = m_prev.constBegin(); it != m_prev.constEnd(); ++it)
    m_receipt.addItem(it.key(), QStringLiteral("tag"), OpOutcome::Changed, QString(),
                      true);
  return true;
}

// ---- 类型改写族 ------------------------------------------------------------
QString TypeOverrideCmd::text() const
{
  return QObject::tr("改类型 %1→%2").arg(m_assetId, m_new);
}
bool TypeOverrideCmd::run(const QString &type)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("type");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("type"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const QHash<QString, QString> snap = m_ctx.assetOverrides->snapshot();
  m_ctx.assetOverrides->setType(m_assetId, type); // 空 type = 清除改写
  if (!m_ctx.assetOverrides->save())
  {
    m_ctx.assetOverrides->restoreSnapshot(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    m_receipt.addItem(m_assetId, QStringLiteral("type"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("type"), OpOutcome::Changed, QString(),
                    true);
  return true;
}
bool TypeOverrideCmd::redo()
{
  return run(m_new);
}
bool TypeOverrideCmd::undo()
{
  // prev 空 = 原无改写：撤销 = 清除改写回 catalog 原型。
  return run(m_prev);
}

QString BatchTypeOverrideCmd::text() const
{
  return QObject::tr("批量改类型 %1 项→%2").arg(m_ids.size()).arg(m_new);
}
bool BatchTypeOverrideCmd::run(bool forward)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("type");
  m_receipt.requested = m_ids.size();
  QString why;
  if (!m_ctx.alive(&why))
  {
    for (const QString &id : m_ids)
      m_receipt.addItem(id, QStringLiteral("type"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const QHash<QString, QString> snap = m_ctx.assetOverrides->snapshot();
  QStringList applied;
  for (const QString &id : m_ids)
  {
    const QString target = forward ? m_new : m_prev.value(id);
    if (!m_ctx.assetOverrides->setType(id, target))
    {
      m_receipt.addItem(id, QStringLiteral("type"), OpOutcome::NoChange,
                        QObject::tr("类型已是目标值"));
      continue;
    }
    applied << id;
  }
  if (applied.isEmpty())
    return true;
  if (!m_ctx.assetOverrides->save())
  {
    m_ctx.assetOverrides->restoreSnapshot(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    for (const QString &id : applied)
      m_receipt.addItem(id, QStringLiteral("type"), OpOutcome::FailedRecovered,
                        m_error);
    return false;
  }
  for (const QString &id : applied)
    m_receipt.addItem(id, QStringLiteral("type"), OpOutcome::Changed, QString(), true);
  return true;
}
bool BatchTypeOverrideCmd::redo()
{
  return run(true);
}
bool BatchTypeOverrideCmd::undo()
{
  return run(false);
}

// ---- 软删/恢复族 -----------------------------------------------------------
QString SoftDeleteCmd::tr_reason() const
{
  return m_remove ? QObject::tr("批量移除") : QObject::tr("撤销恢复");
}
QString SoftDeleteCmd::text() const
{
  return m_remove ? QObject::tr("移除 %1（进可回收清单）").arg(m_name)
                  : QObject::tr("恢复 %1").arg(m_name);
}
bool SoftDeleteCmd::run(bool remove)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("softdel");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("softdel"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const RecycleBin::StateSnapshot snap = m_ctx.recycle->captureState();
  bool changed = false;
  if (remove)
  {
    const bool wasRemoved = m_ctx.recycle->isRemoved(m_assetId);
    m_ctx.recycle->remove(m_assetId, m_type, m_name, tr_reason());
    changed = !wasRemoved;
  }
  else
  {
    changed = m_ctx.recycle->restore(m_assetId);
  }
  if (!changed)
  {
    m_receipt.addItem(m_assetId, QStringLiteral("softdel"), OpOutcome::NoChange,
                      QObject::tr("回收状态未变化"));
    return true;
  }
  if (!m_ctx.recycle->save())
  {
    m_ctx.recycle->restoreState(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    m_receipt.addItem(m_assetId, QStringLiteral("softdel"),
                      OpOutcome::FailedRecovered, m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("softdel"), OpOutcome::Changed,
                    QString(), true);
  return true;
}
bool SoftDeleteCmd::redo()
{
  return run(m_remove);
}
bool SoftDeleteCmd::undo()
{
  return run(!m_remove);
}

QString BatchRestoreCmd::text() const
{
  return QObject::tr("批量恢复 %1 项").arg(m_ids.size());
}
bool BatchRestoreCmd::redo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("restore");
  m_receipt.requested = m_ids.size();
  QString why;
  if (!m_ctx.alive(&why))
  {
    for (const QString &id : m_ids)
      m_receipt.addItem(id, QStringLiteral("restore"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const RecycleBin::StateSnapshot snap = m_ctx.recycle->captureState();
  QStringList applied;
  for (const QString &id : m_ids)
  {
    if (!m_ctx.recycle->restore(id))
    {
      m_receipt.addItem(id, QStringLiteral("restore"), OpOutcome::NoChange,
                        QObject::tr("不在可回收清单"));
      continue;
    }
    applied << id;
  }
  if (applied.isEmpty())
    return true;
  if (!m_ctx.recycle->save())
  {
    m_ctx.recycle->restoreState(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    for (const QString &id : applied)
      m_receipt.addItem(id, QStringLiteral("restore"), OpOutcome::FailedRecovered,
                        m_error);
    return false;
  }
  for (const QString &id : applied)
    m_receipt.addItem(id, QStringLiteral("restore"), OpOutcome::Changed, QString(),
                      true);
  return true;
}
bool BatchRestoreCmd::undo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("restore");
  QString why;
  if (!m_ctx.alive(&why))
  {
    for (const auto &kv : m_entries)
      m_receipt.addItem(kv.first, QStringLiteral("restore"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  if (m_entries.isEmpty())
    return true;
  const RecycleBin::StateSnapshot snap = m_ctx.recycle->captureState();
  for (const auto &kv : m_entries)
    m_ctx.recycle->remove(kv.first, kv.second.type, kv.second.displayName,
                          kv.second.reason);
  if (!m_ctx.recycle->save())
  {
    m_ctx.recycle->restoreState(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    for (const auto &kv : m_entries)
      m_receipt.addItem(kv.first, QStringLiteral("restore"),
                        OpOutcome::FailedRecovered, m_error);
    return false;
  }
  for (const auto &kv : m_entries)
    m_receipt.addItem(kv.first, QStringLiteral("restore"), OpOutcome::Changed,
                      QString(), true);
  return true;
}

QString BatchRemoveCmd::text() const
{
  return QObject::tr("批量移除 %1 项（软删）").arg(m_ids.size());
}
bool BatchRemoveCmd::redo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("softdel");
  m_receipt.requested = m_entries.size();
  QString why;
  if (!m_ctx.alive(&why))
  {
    for (const auto &kv : m_entries)
      m_receipt.addItem(kv.first, QStringLiteral("softdel"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const RecycleBin::StateSnapshot snap = m_ctx.recycle->captureState();
  QStringList applied;
  for (const auto &kv : m_entries)
  {
    if (m_ctx.recycle->isRemoved(kv.first))
    {
      m_receipt.addItem(kv.first, QStringLiteral("softdel"), OpOutcome::NoChange,
                        QObject::tr("已在可回收清单"));
      continue;
    }
    m_ctx.recycle->remove(kv.first, kv.second.type, kv.second.displayName, m_reason);
    applied << kv.first;
  }
  if (applied.isEmpty())
    return true;
  if (!m_ctx.recycle->save())
  {
    m_ctx.recycle->restoreState(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    for (const QString &id : applied)
      m_receipt.addItem(id, QStringLiteral("softdel"), OpOutcome::FailedRecovered,
                        m_error);
    return false;
  }
  for (const QString &id : applied)
    m_receipt.addItem(id, QStringLiteral("softdel"), OpOutcome::Changed, QString(),
                      true);
  return true;
}
bool BatchRemoveCmd::undo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("softdel");
  QString why;
  if (!m_ctx.alive(&why))
  {
    for (const QString &id : m_ids)
      m_receipt.addItem(id, QStringLiteral("softdel"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  if (m_ids.isEmpty())
    return true;
  const RecycleBin::StateSnapshot snap = m_ctx.recycle->captureState();
  QStringList applied;
  for (const QString &id : m_ids)
  {
    if (!m_ctx.recycle->restore(id))
    {
      m_receipt.addItem(id, QStringLiteral("softdel"), OpOutcome::NoChange,
                        QObject::tr("不在可回收清单"));
      continue;
    }
    applied << id;
  }
  if (applied.isEmpty())
    return true;
  if (!m_ctx.recycle->save())
  {
    m_ctx.recycle->restoreState(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    for (const QString &id : applied)
      m_receipt.addItem(id, QStringLiteral("softdel"), OpOutcome::FailedRecovered,
                        m_error);
    return false;
  }
  for (const QString &id : applied)
    m_receipt.addItem(id, QStringLiteral("softdel"), OpOutcome::Changed, QString(),
                      true);
  return true;
}

} // namespace paleo::dataops
