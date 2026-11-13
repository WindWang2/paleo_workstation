// 层：功能
#include "dataopscommandservice.h"

#include <QHash>

namespace paleo::dataops
{

// ---- 实体族 ----------------------------------------------------------------
QString EntityEditCmd::text() const
{
  if (!m_next.note.isEmpty() && m_next.note != m_prev.note)
    return QObject::tr("改备注 %1").arg(m_entityId);
  if (m_next.hasCoords && (!m_prev.hasCoords || m_next.surfaceX != m_prev.surfaceX ||
                           m_next.surfaceY != m_prev.surfaceY))
    return QObject::tr("改坐标 %1").arg(m_entityId);
  return QObject::tr("重命名实体 %1→%2").arg(m_entityId, m_next.name);
}
bool EntityEditCmd::run(const EntityOverride &o)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("entity");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_entityId, QStringLiteral("entity"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const QHash<QString, EntityOverride> snap = m_ctx.entityOverrides->snapshot();
  const EntityOverride before = m_ctx.entityOverrides->overrideFor(m_entityId);
  m_ctx.entityOverrides->setOverride(m_entityId, o);
  if (before == o) // 值未变（结构体成员均有 ==；空 override 等值）
  {
    m_receipt.addItem(m_entityId, QStringLiteral("entity"), OpOutcome::NoChange,
                      QObject::tr("实体改写未变化"));
    return true;
  }
  if (!m_ctx.entityOverrides->save())
  {
    m_ctx.entityOverrides->restoreSnapshot(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    m_receipt.addItem(m_entityId, QStringLiteral("entity"),
                      OpOutcome::FailedRecovered, m_error);
    return false;
  }
  m_receipt.addItem(m_entityId, QStringLiteral("entity"), OpOutcome::Changed,
                    QString(), true);
  return true;
}
bool EntityEditCmd::redo()
{
  return run(m_next);
}
bool EntityEditCmd::undo()
{
  return run(m_prev);
}

QString EntityCreateCmd::text() const
{
  return QObject::tr("新建实体 %1").arg(m_e.name.isEmpty() ? m_e.id : m_e.name);
}
bool EntityCreateCmd::redo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("entitynew");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_e.id, QStringLiteral("entitynew"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  // addEntity 幂等保护：已存在（undo 后 redo 的 catalog 状态没有实体删除，
  // undo 只做 sidecar 软删）时跳过。
  QString err;
  if (!m_ctx.cat->hasEntity(m_e.id) && !m_ctx.cat->addEntity(m_e, &err))
  {
    m_receipt.addItem(m_e.id, QStringLiteral("entitynew"),
                      OpOutcome::FailedRecovered, err);
    return opFail(&m_error, err);
  }
  // 可见性恢复仅在实际有软删记录时要落盘（全新实体无记录 → 不空写侧车，
  // 也避免把无关的侧车写失败误报成本命令失败）。
  const RecycleBin::StateSnapshot snap = m_ctx.recycle->captureState();
  if (m_ctx.recycle->restore(m_e.id))
  {
    if (!m_ctx.recycle->save())
    {
      m_ctx.recycle->restoreState(snap); // 回到软删态：实体入库但隐藏（=撤销后状态）
      m_error = saveFailedText();
      m_receipt.failureStage = QStringLiteral("sidecar-write");
      m_receipt.addItem(m_e.id, QStringLiteral("entitynew"),
                        OpOutcome::FailedRecovered,
                        QObject::tr("%1（实体已写入目录但可见性未恢复）").arg(m_error));
      return false;
    }
  }
  m_receipt.addItem(m_e.id, QStringLiteral("entitynew"), OpOutcome::Changed,
                    QString(), true);
  return true;
}
bool EntityCreateCmd::undo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("entitynew");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_e.id, QStringLiteral("entitynew"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  // catalog 无 removeEntity API（GAPS）：undo = sidecar 软删（视图隐藏），
  // 实体记录留库，redo 时恢复可见。
  const RecycleBin::StateSnapshot snap = m_ctx.recycle->captureState();
  m_ctx.recycle->remove(m_e.id, m_e.entityType, m_e.name,
                        QObject::tr("新建实体撤销"));
  if (!m_ctx.recycle->save())
  {
    m_ctx.recycle->restoreState(snap);
    m_error = saveFailedText();
    m_receipt.failureStage = QStringLiteral("sidecar-write");
    m_receipt.addItem(m_e.id, QStringLiteral("entitynew"),
                      OpOutcome::FailedRecovered, m_error);
    return false;
  }
  m_receipt.addItem(m_e.id, QStringLiteral("entitynew"), OpOutcome::Changed,
                    QString(), true);
  return true;
}

} // namespace paleo::dataops
