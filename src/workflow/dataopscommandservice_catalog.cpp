// 层：功能
#include "dataopscommandservice.h"

namespace paleo::dataops
{

// ---- 目录链接族命令 ---------------------------------------------------------
QString AttachLinkCmd::text() const
{
  return QObject::tr("挂接 %1→%2").arg(m_assetId, m_entityId);
}
bool AttachLinkCmd::redo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("attach");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, QString());
  if (idx < 0)
  {
    m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::Rejected,
                      linkMissingText(m_assetId));
    return opFail(&m_error, linkMissingText(m_assetId));
  }
  if (!m_ctx.cat->attachLink(idx, m_entityId, &m_error))
  {
    // mutator 自带回滚（单行内存变更 + save 失败还原）。
    m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::Changed, QString(),
                    true);
  return true;
}
bool AttachLinkCmd::undo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("attach");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, m_entityId);
  if (idx < 0)
  {
    m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::Rejected,
                      linkMissingText(m_assetId));
    return opFail(&m_error, linkMissingText(m_assetId));
  }
  if (!m_ctx.cat->setLinkUnresolved(idx, &m_error))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("attach"), OpOutcome::Changed, QString(),
                    true);
  return true;
}

QString DetachLinkCmd::text() const
{
  return QObject::tr("解挂 %1→%2").arg(m_assetId, m_entityId);
}
bool DetachLinkCmd::redo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("detach");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, m_entityId);
  if (idx < 0)
  {
    m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::Rejected,
                      linkMissingText(m_assetId));
    return opFail(&m_error, linkMissingText(m_assetId));
  }
  if (!m_ctx.cat->setLinkUnresolved(idx, &m_error))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::Changed, QString(),
                    true);
  return true;
}
bool DetachLinkCmd::undo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("detach");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, QString());
  if (idx < 0)
  {
    m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::Rejected,
                      linkMissingText(m_assetId));
    return opFail(&m_error, linkMissingText(m_assetId));
  }
  if (!m_ctx.cat->attachLink(idx, m_entityId, &m_error))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("detach"), OpOutcome::Changed, QString(),
                    true);
  return true;
}

QString TransferLinkCmd::text() const
{
  return QObject::tr("转移挂接 %1：%2→%3").arg(m_assetId, m_from, m_to);
}
bool TransferLinkCmd::apply(const QString &from, const QString &to)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("transfer");
  const QString op = QStringLiteral("transfer");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, op, OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  if (from == to)
  {
    m_receipt.addItem(m_assetId, op, OpOutcome::NoChange,
                      QObject::tr("目标实体相同"));
    return true;
  }
  int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, from);
  bool resolved = idx >= 0;
  if (idx < 0)
    idx = indexOfLink(m_ctx.cat, m_assetId, m_role, QString()); // 已被解挂的半途态
  if (idx < 0)
  {
    m_receipt.addItem(m_assetId, op, OpOutcome::Rejected, linkMissingText(m_assetId));
    return opFail(&m_error, linkMissingText(m_assetId));
  }
  // P1-W3：两步一个提交边界——第二阶段失败或落盘失败都会整体回到批次前
  // 状态，不再遗留「已解挂、未挂目标」的未决中间态。
  DataCatalog::BatchSave batch(m_ctx.cat);
  if (resolved && !m_ctx.cat->setLinkUnresolved(idx, &m_error))
  {
    batch.abort();
    m_receipt.failureStage = QStringLiteral("catalog-write");
    m_receipt.addItem(m_assetId, op, OpOutcome::FailedRecovered, m_error);
    return false;
  }
  if (!m_ctx.cat->attachLink(idx, to, &m_error))
  {
    batch.abort(); // 把第一阶段（置未决）一并滚回
    m_receipt.failureStage = QStringLiteral("transfer-phase2");
    m_receipt.addItem(m_assetId, op, OpOutcome::FailedRecovered,
                      QObject::tr("转移第二步失败（已回滚）：%1").arg(m_error));
    m_error = QObject::tr("转移第二步失败（已回滚）：%1").arg(m_error);
    return false;
  }
  QString err;
  if (!batch.flush(&err))
  {
    // #169：落盘失败 → 内存滚回批次前（与盘一致）。
    m_receipt.failureStage = QStringLiteral("catalog-commit");
    m_receipt.addItem(m_assetId, op, OpOutcome::FailedRecovered,
                      QObject::tr("转移落盘失败（已回滚）：%1").arg(err));
    m_error = QObject::tr("转移落盘失败（已回滚）：%1").arg(err);
    return false;
  }
  m_receipt.addItem(m_assetId, op, OpOutcome::Changed, QString(), true);
  return true;
}

QString SetPrimaryCmd::text() const
{
  return QObject::tr("设为主版本 %1（%2·%3）").arg(m_assetId, m_entityId, m_role);
}
bool SetPrimaryCmd::redo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("primary");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("primary"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, m_entityId);
  if (idx < 0)
  {
    m_receipt.addItem(m_assetId, QStringLiteral("primary"), OpOutcome::Rejected,
                      linkMissingText(m_assetId));
    return opFail(&m_error, linkMissingText(m_assetId));
  }
  if (!m_ctx.cat->setLinkPrimary(idx, &m_error))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("primary"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("primary"), OpOutcome::Changed,
                    QString(), true);
  return true;
}
bool SetPrimaryCmd::undo()
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("primary");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("primary"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  if (m_prevPrimary.isEmpty())
  {
    const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, m_entityId);
    if (idx < 0)
    {
      m_receipt.addItem(m_assetId, QStringLiteral("primary"), OpOutcome::Rejected,
                        linkMissingText(m_assetId));
      return opFail(&m_error, linkMissingText(m_assetId));
    }
    if (!m_ctx.cat->clearLinkPrimary(idx, &m_error))
    {
      m_receipt.addItem(m_assetId, QStringLiteral("primary"),
                        OpOutcome::FailedRecovered, m_error);
      return false;
    }
  }
  else
  {
    const int idx = indexOfLink(m_ctx.cat, m_prevPrimary, m_role, m_entityId);
    if (idx < 0)
    {
      m_receipt.addItem(m_prevPrimary, QStringLiteral("primary"), OpOutcome::Rejected,
                        linkMissingText(m_prevPrimary));
      return opFail(&m_error, linkMissingText(m_prevPrimary));
    }
    if (!m_ctx.cat->setLinkPrimary(idx, &m_error))
    {
      m_receipt.addItem(m_prevPrimary, QStringLiteral("primary"),
                        OpOutcome::FailedRecovered, m_error);
      return false;
    }
  }
  m_receipt.addItem(m_assetId, QStringLiteral("primary"), OpOutcome::Changed,
                    QString(), true);
  return true;
}

QString RenameAssetCmd::text() const
{
  return QObject::tr("重命名 %1 → %2").arg(m_prev, m_next);
}
bool RenameAssetCmd::redo()
{
  return runRename(m_next);
}
bool RenameAssetCmd::undo()
{
  return runRename(m_prev);
}
// 单 mutator 原子（renameAsset 自带 save 失败回滚 + 同名幂等）。
bool RenameAssetCmd::runRename(const QString &name)
{
  m_receipt = BatchReceipt();
  m_receipt.operation = QStringLiteral("rename");
  QString why;
  if (!m_ctx.alive(&why))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("rename"), OpOutcome::Rejected, why);
    return opFail(&m_error, why);
  }
  if (!m_ctx.cat->renameAsset(m_assetId, name, &m_error))
  {
    m_receipt.addItem(m_assetId, QStringLiteral("rename"), OpOutcome::FailedRecovered,
                      m_error);
    return false;
  }
  m_receipt.addItem(m_assetId, QStringLiteral("rename"), OpOutcome::Changed,
                    QString(), true);
  return true;
}

} // namespace paleo::dataops
