// 层：功能
// workflow/dataopscommandservice — 实现见头文件注释（P1 data-commands）。
#include "dataopscommandservice.h"

#include <QHash>

namespace paleo::dataops
{

// ---- DataOpsContext --------------------------------------------------------
bool DataOpsContext::alive(QString *why) const
{
  if (!valid())
    return opFail(why, ctxInvalidText()), false;
  if (!cat->isOpen())
    return opFail(why, QObject::tr("数据目录未打开")), false;
  // 会话身份：绑定时记下 catalog 路径；对象被复用重开别的目录时路径漂移，
  // 旧命令拒绝执行（不静默写新会话）。
  if (!boundCatalogPath.isEmpty() && cat->catalogPath() != boundCatalogPath)
    return opFail(why, QObject::tr("工程会话已变更（数据目录已切换），请重新操作")), false;
  if (session && session->generation() != sessionGeneration)
    return opFail(why, QObject::tr("工程会话已变更，请重新操作")), false;
  return true;
}

// ---- DataOpsUndoStack ------------------------------------------------------
bool DataOpsUndoStack::push(DataOpCommand *cmd)
{
  if (!cmd)
    return false;
  if (!cmd->redo())
  {
    m_lastError = cmd->errorText().isEmpty() ? tr("操作未能完成") : cmd->errorText();
    m_lastReceipt = cmd->receipt();
    const bool noChangeOnly = m_lastReceipt.allNoChange();
    const bool intact = cmd->storeIntact();
    delete cmd;
    if (noChangeOnly)
    {
      // 全部条目幂等命中：不算失败、不追加虚假历史、不清 redo。
      m_lastError.clear();
      return true;
    }
    if (!intact)
    {
      // 留有部分变化（补偿失败/状态不确定）：两支栈整栈作废——历史不再
      // 描述真实状态，宁可丢历史也不让后续撤销在错误基线上操作。
      qDeleteAll(m_undo);
      qDeleteAll(m_redo);
      m_undo.clear();
      m_redo.clear();
      m_lastReceipt.historyInvalidated = true;
      emit operationFailed(m_lastError);
      emit stackChanged();
      return false;
    }
    emit operationFailed(m_lastError);
    return false;
  }
  if (cmd->receipt().allNoChange())
  {
    // 成功但零变化：不入栈、不清 redo（无变化不添加虚假命令）。
    m_lastReceipt = cmd->receipt();
    delete cmd;
    return true;
  }
  m_lastError.clear();
  m_lastReceipt = cmd->receipt();
  // 相邻同类合并（D5.3）：栈顶同 id 且 mergeWith 接受 → 不再压新条目。
  if (!m_undo.isEmpty() && !cmd->id().isEmpty() &&
      m_undo.last()->id() == cmd->id() && m_undo.last()->mergeWith(cmd))
  {
    delete cmd;
    clearRedo(); // 分支点作废（指针元素须 delete，不能只 clear）
    emit stackChanged();
    return true;
  }
  m_undo.append(cmd);
  if (m_undo.size() > kMaxDepth)
    delete m_undo.takeFirst(); // 弹底：最老命令的效果成为既成事实
  clearRedo();
  emit stackChanged();
  return true;
}

QString DataOpsUndoStack::undo()
{
  if (m_undo.isEmpty())
    return QString();
  DataOpCommand *cmd = m_undo.takeLast();
  if (!cmd->undo())
  {
    failAndReset(cmd);
    return QString();
  }
  m_lastError.clear();
  m_lastReceipt = cmd->receipt();
  m_redo.append(cmd);
  emit stackChanged();
  return cmd->text();
}

QString DataOpsUndoStack::redo()
{
  if (m_redo.isEmpty())
    return QString();
  DataOpCommand *cmd = m_redo.takeLast();
  if (!cmd->redo())
  {
    failAndReset(cmd);
    return QString();
  }
  m_lastError.clear();
  m_lastReceipt = cmd->receipt();
  m_undo.append(cmd);
  if (m_undo.size() > kMaxDepth)
    delete m_undo.takeFirst();
  emit stackChanged();
  return cmd->text();
}

void DataOpsUndoStack::clear()
{
  if (m_undo.isEmpty() && m_redo.isEmpty())
    return;
  qDeleteAll(m_undo);
  qDeleteAll(m_redo);
  m_undo.clear();
  m_redo.clear();
  emit stackChanged();
}

void DataOpsUndoStack::failAndReset(DataOpCommand *cmd)
{
  m_lastError = cmd->errorText().isEmpty() ? tr("操作未能完成") : cmd->errorText();
  m_lastReceipt = cmd->receipt();
  m_lastReceipt.historyInvalidated = true;
  delete cmd;
  qDeleteAll(m_undo);
  qDeleteAll(m_redo);
  m_undo.clear();
  m_redo.clear();
  emit operationFailed(m_lastError);
  emit stackChanged();
}

// ---- DataOpMacro -----------------------------------------------------------
DataOpMacro::DataOpMacro(QString text, QList<DataOpCommand *> children)
  : m_text(std::move(text)), m_children(std::move(children))
{
}

DataOpMacro::~DataOpMacro()
{
  qDeleteAll(m_children);
}

QString DataOpMacro::text() const
{
  return m_text;
}

namespace
{
// 聚合 child 回执到宏回执（保留每条的最终结局）。
void mergeReceiptItems(BatchReceipt *agg, const BatchReceipt &part)
{
  for (const ReceiptItem &it : part.items)
    agg->items.append(it);
  agg->requested += part.requested;
  if (part.failureStage.isEmpty() && agg->failureStage.isEmpty())
    agg->failureStage = part.failureStage;
}
} // namespace

bool DataOpMacro::run(bool forward)
{
  m_error.clear();
  m_storeIntact = true;
  BatchReceipt agg;
  agg.operation = m_text;
  DataCatalog::BatchSave batch(m_batchCat); // null 目录 → flush 恒 true，无副作用
  const int n = m_children.size();
  int done = 0;
  bool failed = false;
  QString failError;
  bool failIntact = true;

  const auto runChild = [&](DataOpCommand *c, bool fwd) -> bool {
    return fwd ? c->redo() : c->undo();
  };
  for (int step = 0; step < n; ++step)
  {
    const int idx = forward ? step : n - 1 - step;
    DataOpCommand *c = m_children.at(idx);
    if (!runChild(c, forward))
    {
      failError = c->errorText();
      failIntact = c->storeIntact();
      failed = true;
      // 失败 child 的回执同样并入——它是「实际发生了什么」的证据（可能已有
      // 部分条目被自身回滚，也可能留有 NoChange/Rejected 分类）。
      mergeReceiptItems(&agg, c->receipt());
      break;
    }
    ++done;
    mergeReceiptItems(&agg, c->receipt());
  }

  if (!failed)
  {
    QString err;
    if (!batch.flush(&err))
    {
      // 目录族宏整体提交失败：#169 已把内存滚回批次前——不留半截状态，
      // 已成功 children 的目录条目如实标「失败已恢复」。
      m_error = QObject::tr("批量提交失败（已整体回滚）：%1").arg(err);
      for (ReceiptItem &it : agg.items)
        if (it.outcome == OpOutcome::Changed)
        {
          it.outcome = OpOutcome::FailedRecovered;
          it.reason = m_error;
          it.undoable = false;
        }
      agg.failureStage = QStringLiteral("catalog-commit");
      m_receipt = agg;
      return false; // 内存已恢复 → storeIntact 保持 true
    }
    m_receipt = agg;
    return true;
  }

  // 有 child 失败。
  m_error = failError;
  m_storeIntact = failIntact;
  if (m_batchCat)
  {
    // 目录族宏：整批 abort——目录内存回到批次前（含已成功 children 的
    // 目录变更），不留半途中间态；侧车不在本模式保护面（见类注释）。
    batch.abort();
    for (ReceiptItem &it : agg.items)
      if (it.outcome == OpOutcome::Changed)
      {
        it.outcome = OpOutcome::FailedRecovered;
        it.reason = QObject::tr("批量失败已整体回滚：%1").arg(m_error);
        it.undoable = false;
      }
    agg.failureStage = QStringLiteral("catalog-abort");
  }
  else
  {
    // 混合宏：child 各自原子；对已完成 children 逆序补偿（相对执行方向），
    // 核对每次补偿结果——补偿失败保留原失败与补偿失败两条证据。
    bool compensationOk = true;
    for (int k = 0; k < done; ++k)
    {
      const int idx = forward ? done - 1 - k : n - done + k;
      DataOpCommand *c = m_children.at(idx);
      if (!runChild(c, !forward))
      {
        compensationOk = false;
        const QString why = c->errorText();
        m_error += QStringLiteral("；补偿失败：%1").arg(why);
        const BatchReceipt cr = c->receipt();
        for (ReceiptItem &it : agg.items)
          for (const ReceiptItem &ci : cr.items)
            if (it.targetId == ci.targetId && it.op == ci.op &&
                it.outcome == OpOutcome::Changed)
            {
              it.outcome = OpOutcome::FailedPartial;
              it.reason = QObject::tr("补偿失败：%1").arg(why);
              it.undoable = false;
            }
      }
      else
      {
        // 补偿成功：该 child 的已变更条目如实标「失败已恢复」。
        const BatchReceipt cr = c->receipt();
        for (ReceiptItem &it : agg.items)
          for (const ReceiptItem &ci : cr.items)
            if (it.targetId == ci.targetId && it.op == ci.op &&
                it.outcome == OpOutcome::Changed)
            {
              it.outcome = OpOutcome::FailedRecovered;
              it.reason = QObject::tr("批量失败已补偿恢复");
              it.undoable = false;
            }
      }
    }
    agg.failureStage = compensationOk ? QStringLiteral("compensated")
                                      : QStringLiteral("compensation");
    if (!compensationOk)
      m_storeIntact = false; // 状态不确定 → 栈须整栈作废
  }
  m_receipt = agg;
  return false;
}

bool DataOpMacro::redo()
{
  return run(true);
}

bool DataOpMacro::undo()
{
  return run(false);
}

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
    m_receipt.failureStage = QStringLiteral("catalog-commit");
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

// ---- 服务 ------------------------------------------------------------------
void DataOpsCommandService::bindSession(DataCatalog *cat, TagStore *tags,
                                        AssetOverrideStore *assetOverrides,
                                        EntityOverrideStore *entityOverrides,
                                        RecycleBin *recycle, DataOpsUndoStack *stack,
                                        DataOpsSession *session)
{
  const QString path = cat ? cat->catalogPath() : QString();
  // 会话变化判定：曾绑定过，且 cat 对象或路径换人（旧对象已销毁 = QPointer
  // 归零，同样算换人——不能依赖旧绑定的 valid()，那会把「旧工程关闭」误判
  // 成「无变化」，让旧历史漏进新会话）。首次绑定不 bump（generation 从 1
  // 起）。同 cat 同路径的重复绑定（refreshAssetTable 每次刷新都会走到）零
  // 副作用。
  const bool sessionChange =
      m_everBound && (cat != m_prevCat.data() || path != m_ctx.boundCatalogPath);
  m_ctx.cat = cat;
  m_ctx.tags = tags;
  m_ctx.assetOverrides = assetOverrides;
  m_ctx.entityOverrides = entityOverrides;
  m_ctx.recycle = recycle;
  m_ctx.session = session; // 命令侧会话守卫的代数来源（P1-W2）
  m_ctx.boundCatalogPath = path;
  m_everBound = true;
  m_prevCat = cat;
  m_stack = stack;
  if (session)
  {
    if (sessionChange)
      session->bump();
    m_ctx.sessionGeneration = session->generation();
  }
  // 旧会话的命令对新 catalog 无意义（D5.6 同口径）：整栈清空。
  if (sessionChange && stack)
    stack->clear();
}

QString DataOpsCommandService::guardFailure(Guard kind) const
{
  Q_UNUSED(kind);
  if (m_ctx.cat.isNull() || !m_ctx.valid())
    return QObject::tr("数据目录不可用（工程已关闭或切换）");
  if (!m_ctx.cat->isOpen())
    return QObject::tr("数据目录未打开");
  if (m_ctx.cat->refusesWrites())
    return QObject::tr("工程为只读，数据操作不可用");
  if (!m_stack)
    return QObject::tr("撤销栈不可用");
  if (m_ctx.session && m_ctx.session->generation() != m_ctx.sessionGeneration)
    return QObject::tr("工程会话已变更，请重新操作");
  return QString();
}

bool DataOpsCommandService::guardBatch(Guard kind, const QString &operation,
                                       const QStringList &requests, BatchReceipt *out)
{
  out->operation = operation;
  out->requested = requests.size();
  const QString guard = guardFailure(kind);
  if (!guard.isEmpty())
  {
    for (const QString &id : requests)
      out->addItem(id, operation.section(QLatin1Char('-'), 0, 0),
                   OpOutcome::Rejected, guard);
    return false;
  }
  return true;
}

void DataOpsCommandService::finishWithStack(DataOpCommand *cmd, const QString &op,
                                            BatchReceipt *prepare)
{
  Q_UNUSED(op);
  if (!m_stack)
  {
    delete cmd; // 命令是裸指针：栈不在场时别泄漏
    prepare->failureStage = QStringLiteral("no-stack");
    return;
  }
  m_stack->push(cmd);
  const BatchReceipt &exec = m_stack->lastReceipt();
  if (!exec.failureStage.isEmpty())
    prepare->failureStage = exec.failureStage;
  if (exec.historyInvalidated)
    prepare->historyInvalidated = true;
  // 执行条目（Changed/Failed*/NoChange）从命令回执并入外层回执——
  // 命令回执覆盖的正是准备段判定的待执行集合。
  for (const ReceiptItem &it : exec.items)
    prepare->items.append(it);
}

BatchReceipt DataOpsCommandService::addTags(const QStringList &assetIds,
                                            const QString &tag)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Sidecar, QStringLiteral("tag-add"), assetIds, &r))
    return r;
  // 标签值全批校验一次（TagStore::addTag 的规则：非空、≤40、无控制字符）。
  const QString trimmed = tag.trimmed();
  QString tagInvalid;
  if (trimmed.isEmpty())
    tagInvalid = QObject::tr("标签名不能为空");
  else if (trimmed.size() > 40)
    tagInvalid = QObject::tr("标签名过长（至多 40 字符）");
  else
    for (const QChar &ch : trimmed)
      if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format)
      {
        tagInvalid = QObject::tr("标签名含非法字符");
        break;
      }
  if (!tagInvalid.isEmpty())
  {
    for (const QString &id : assetIds)
      r.addItem(id, QStringLiteral("tag"), OpOutcome::Rejected, tagInvalid);
    return r;
  }
  QStringList uniq;
  QSet<QString> seen;
  for (const QString &id : assetIds)
  {
    if (seen.contains(id))
    {
      ++r.merged;
      continue;
    }
    seen.insert(id);
    uniq << id;
  }
  QStringList toTag;
  for (const QString &id : uniq)
  {
    if (m_ctx.cat->assetById(id).id.isEmpty())
    {
      r.addItem(id, QStringLiteral("tag"), OpOutcome::Rejected,
                QObject::tr("资产不存在"));
      continue;
    }
    if (m_ctx.recycle->isRemoved(id))
    {
      r.addItem(id, QStringLiteral("tag"), OpOutcome::Rejected,
                QObject::tr("资产已在可回收清单"));
      continue;
    }
    if (m_ctx.tags->tagsFor(id).contains(trimmed, Qt::CaseInsensitive))
    {
      r.addItem(id, QStringLiteral("tag"), OpOutcome::NoChange,
                QObject::tr("标签已存在"));
      continue;
    }
    toTag << id;
  }
  if (!toTag.isEmpty())
  {
    finishWithStack(new BatchTagCmd(m_ctx, trimmed, true, toTag),
                    QStringLiteral("tag"), &r);
  }
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

BatchReceipt DataOpsCommandService::removeTags(const QStringList &assetIds,
                                               const QString &tag)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Sidecar, QStringLiteral("tag-remove"), assetIds, &r))
    return r;
  const QString trimmed = tag.trimmed();
  QStringList uniq;
  QSet<QString> seen;
  for (const QString &id : assetIds)
  {
    if (seen.contains(id))
    {
      ++r.merged;
      continue;
    }
    seen.insert(id);
    uniq << id;
  }
  QStringList toRemove;
  for (const QString &id : uniq)
  {
    if (m_ctx.cat->assetById(id).id.isEmpty())
    {
      r.addItem(id, QStringLiteral("tag"), OpOutcome::Rejected,
                QObject::tr("资产不存在"));
      continue;
    }
    if (!m_ctx.tags->tagsFor(id).contains(trimmed, Qt::CaseInsensitive))
    {
      r.addItem(id, QStringLiteral("tag"), OpOutcome::NoChange,
                QObject::tr("标签不存在"));
      continue;
    }
    toRemove << id;
  }
  if (!toRemove.isEmpty())
  {
    finishWithStack(new BatchTagCmd(m_ctx, trimmed, false, toRemove),
                    QStringLiteral("tag"), &r);
  }
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

BatchReceipt DataOpsCommandService::setTypeOverrides(const QStringList &assetIds,
                                                     const QString &newType)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Sidecar, QStringLiteral("type"), assetIds, &r))
    return r;
  if (newType.trimmed().isEmpty())
  {
    for (const QString &id : assetIds)
      r.addItem(id, QStringLiteral("type"), OpOutcome::Rejected,
                QObject::tr("类型不能为空"));
    return r;
  }
  QStringList uniq;
  QSet<QString> seen;
  for (const QString &id : assetIds)
  {
    if (seen.contains(id))
    {
      ++r.merged;
      continue;
    }
    seen.insert(id);
    uniq << id;
  }
  QList<QPair<QString, QString>> toChange; // (assetId, prevType)
  for (const QString &id : uniq)
  {
    if (m_ctx.cat->assetById(id).id.isEmpty())
    {
      r.addItem(id, QStringLiteral("type"), OpOutcome::Rejected,
                QObject::tr("资产不存在"));
      continue;
    }
    if (m_ctx.recycle->isRemoved(id))
    {
      r.addItem(id, QStringLiteral("type"), OpOutcome::Rejected,
                QObject::tr("资产已在可回收清单"));
      continue;
    }
    const QString prev = m_ctx.assetOverrides->overriddenType(id);
    if (prev == newType)
    {
      r.addItem(id, QStringLiteral("type"), OpOutcome::NoChange,
                QObject::tr("类型已是 %1").arg(newType));
      continue;
    }
    toChange.append({id, prev});
  }
  if (!toChange.isEmpty())
  {
    finishWithStack(new BatchTypeOverrideCmd(m_ctx, newType, toChange),
                    QStringLiteral("type"), &r);
  }
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

BatchReceipt DataOpsCommandService::attachOrTransfer(const QStringList &assetIds,
                                                     const QString &entityId,
                                                     const QString &fallbackRole)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Catalog, QStringLiteral("attach-drop"), assetIds, &r))
    return r;
  // 模态对话框/拖放返回后重新验证冻结目标：目标实体必须仍在当前会话目录里。
  const CatalogEntity target = m_ctx.cat->entityById(entityId);
  if (target.id.isEmpty())
  {
    for (const QString &id : assetIds)
      r.addItem(id, QStringLiteral("attach"), OpOutcome::Rejected,
                QObject::tr("目标实体不存在（工程已变化，请重新选择）"));
    return r;
  }
  if (entityId.isEmpty() || fallbackRole.isEmpty())
  {
    for (const QString &id : assetIds)
      r.addItem(id, QStringLiteral("attach"), OpOutcome::Rejected,
                QObject::tr("挂接目标不完整"));
    return r;
  }
  QStringList uniq;
  QSet<QString> seen;
  for (const QString &id : assetIds)
  {
    if (seen.contains(id))
    {
      ++r.merged;
      continue;
    }
    seen.insert(id);
    uniq << id;
  }

  struct Plan
  {
    QString assetId;
    int kind = 0; // 0=addLink(不可撤销) 1=attach 2=transfer
    QString role, fromEntity;
  };
  QList<Plan> plans;
  QStringList addLinkIds;
  for (const QString &id : uniq)
  {
    if (m_ctx.cat->assetById(id).id.isEmpty())
    {
      r.addItem(id, QStringLiteral("attach"), OpOutcome::Rejected,
                QObject::tr("资产不存在"));
      continue;
    }
    if (m_ctx.recycle->isRemoved(id))
    {
      r.addItem(id, QStringLiteral("attach"), OpOutcome::Rejected,
                QObject::tr("资产已在可回收清单"));
      continue;
    }
    const QVector<EntityAssetLink> links = m_ctx.cat->linksForAsset(id);
    if (links.isEmpty())
    {
      Plan p;
      p.assetId = id;
      p.kind = 0;
      p.role = fallbackRole;
      plans.append(p);
      addLinkIds << id;
      continue;
    }
    bool acted = false;
    for (const EntityAssetLink &l : links)
      if (l.unresolved)
      {
        Plan p;
        p.assetId = id;
        p.kind = 1;
        p.role = l.role;
        plans.append(p);
        acted = true;
        break; // 每资产处理第一条未决链接（与既有行内控件口径一致）
      }
    if (acted)
      continue;
    for (const EntityAssetLink &l : links)
      if (!l.unresolved && l.entityId != entityId)
      {
        Plan p;
        p.assetId = id;
        p.kind = 2;
        p.role = l.role;
        p.fromEntity = l.entityId;
        plans.append(p);
        acted = true;
        break;
      }
    if (!acted)
      r.addItem(id, QStringLiteral("attach"), OpOutcome::NoChange,
                QObject::tr("已挂到目标实体"));
  }

  // 执行段 1：无链接资产 addLink（真实不可撤销——目录无 removeLink API）。
  // 一个 BatchSave 一个提交边界：任一失败整段回滚（FailedRecovered）。
  if (!addLinkIds.isEmpty())
  {
    DataCatalog::BatchSave batch(m_ctx.cat);
    bool ok = true;
    QString err;
    for (const QString &id : addLinkIds)
    {
      EntityAssetLink l;
      l.entityType = target.entityType;
      l.entityId = entityId;
      l.assetId = id;
      l.role = fallbackRole;
      if (!m_ctx.cat->addLink(l, &err))
      {
        ok = false;
        break;
      }
    }
    if (ok)
      ok = batch.flush(&err);
    else
      batch.abort();
    if (!ok)
    {
      r.failureStage = QStringLiteral("catalog-commit");
      for (const QString &id : addLinkIds)
        r.addItem(id, QStringLiteral("addlink"), OpOutcome::FailedRecovered,
                  QObject::tr("挂接失败（已回滚）：%1").arg(err));
    }
    else
    {
      for (const QString &id : addLinkIds)
        r.addItem(id, QStringLiteral("addlink"), OpOutcome::Changed,
                  QObject::tr("新建关联（不可撤销）"), false);
    }
  }

  // 执行段 2：可撤销族（挂未决/转移）一条宏历史（目录族宏单提交边界）。
  QList<DataOpCommand *> cmds;
  QString firstText;
  for (const Plan &p : plans)
  {
    if (p.kind == 1)
    {
      auto *c = new AttachLinkCmd(m_ctx, p.assetId, p.role, entityId,
                                  target.entityType);
      if (firstText.isEmpty())
        firstText = c->text();
      cmds.append(c);
    }
    else if (p.kind == 2)
    {
      auto *c = new TransferLinkCmd(m_ctx, p.assetId, p.role, p.fromEntity, entityId);
      if (firstText.isEmpty())
        firstText = c->text();
      cmds.append(c);
    }
  }
  if (!cmds.isEmpty())
  {
    DataOpCommand *entry = cmds.size() == 1
                               ? cmds.first()
                               : [&]() {
                                   auto *m = new DataOpMacro(
                                       QObject::tr("批量挂接 %1 项：%2")
                                           .arg(cmds.size())
                                           .arg(firstText),
                                       cmds);
                                   m->setBatchCatalog(m_ctx.cat);
                                   return m;
                                 }();
    finishWithStack(entry, QStringLiteral("attach"), &r);
  }
  // 可撤销范围只覆盖宏段：addLink 段永不计入。
  bool macroFailed = false;
  for (const ReceiptItem &it : r.items)
    if ((it.op == QStringLiteral("attach") || it.op == QStringLiteral("transfer")) &&
        (it.outcome == OpOutcome::FailedRecovered ||
         it.outcome == OpOutcome::FailedPartial))
      macroFailed = true;
  r.undoable = !macroFailed && r.changedCount() > 0 &&
               [&]() {
                 for (const ReceiptItem &it : r.items)
                   if (it.outcome == OpOutcome::Changed &&
                       (it.op == QStringLiteral("attach") ||
                        it.op == QStringLiteral("transfer")))
                     return true;
                 return false;
               }();
  return r;
}

BatchReceipt DataOpsCommandService::detachLinks(const QStringList &assetIds)
{
  BatchReceipt r;
  QStringList ordered = assetIds;
  ordered.sort(); // QSet 迭代无序 → 冻结确定序（单操作语义取首个）
  if (!guardBatch(Guard::Catalog, QStringLiteral("detach"), ordered, &r))
    return r;
  for (const QString &id : ordered)
  {
    for (const EntityAssetLink &l : m_ctx.cat->linksForAsset(id))
      if (!l.unresolved)
      {
        finishWithStack(new DetachLinkCmd(m_ctx, id, l.role, l.entityId),
                        QStringLiteral("detach"), &r);
        r.undoable = r.changedCount() > 0 && !r.anyFailure();
        return r; // 单操作语义：首个选中资产的第一条已决链接
      }
  }
  r.addItem(ordered.isEmpty() ? QString() : ordered.first(),
            QStringLiteral("detach"), OpOutcome::Rejected,
            QObject::tr("选中资产没有已决关联"));
  return r;
}

BatchReceipt DataOpsCommandService::setPrimary(const QStringList &assetIds)
{
  BatchReceipt r;
  QStringList ordered = assetIds;
  ordered.sort();
  if (!guardBatch(Guard::Catalog, QStringLiteral("primary"), ordered, &r))
    return r;
  for (const QString &id : ordered)
  {
    const QVector<EntityAssetLink> links = m_ctx.cat->linksForAsset(id);
    for (const EntityAssetLink &l : links)
      if (!l.unresolved && !l.isPrimary)
      {
        // 前主关联可能在另一个资产上，不能只在本资产的链接里找。
        QString prevPrimary;
        for (const EntityAssetLink &o : m_ctx.cat->linksForEntity(l.entityId))
          if (!o.unresolved && o.isPrimary && o.role == l.role)
            prevPrimary = o.assetId;
        finishWithStack(new SetPrimaryCmd(m_ctx, id, l.role, l.entityId, prevPrimary),
                        QStringLiteral("primary"), &r);
        r.undoable = r.changedCount() > 0 && !r.anyFailure();
        return r;
      }
  }
  r.addItem(ordered.isEmpty() ? QString() : ordered.first(),
            QStringLiteral("primary"), OpOutcome::Rejected,
            QObject::tr("选中资产没有可提升的非主关联"));
  return r;
}

BatchReceipt DataOpsCommandService::renameAsset(const QString &assetId,
                                                const QString &nextName)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Catalog, QStringLiteral("rename"),
                  QStringList{assetId}, &r))
    return r;
  const CatalogAsset a = m_ctx.cat->assetById(assetId);
  if (a.id.isEmpty())
  {
    r.addItem(assetId, QStringLiteral("rename"), OpOutcome::Rejected,
              QObject::tr("资产不存在"));
    return r;
  }
  const QString next = nextName.trimmed();
  if (next.isEmpty())
  {
    r.addItem(assetId, QStringLiteral("rename"), OpOutcome::Rejected,
              QObject::tr("名称不能为空"));
    return r;
  }
  if (next == a.displayName)
  {
    r.addItem(assetId, QStringLiteral("rename"), OpOutcome::NoChange,
              QObject::tr("名称未变化"));
    return r;
  }
  finishWithStack(new RenameAssetCmd(m_ctx, assetId, a.displayName, next),
                  QStringLiteral("rename"), &r);
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

BatchReceipt DataOpsCommandService::softRemove(const QStringList &assetIds,
                                               const QString &reason)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Sidecar, QStringLiteral("softdel"), assetIds, &r))
    return r;
  QStringList uniq;
  QSet<QString> seen;
  for (const QString &id : assetIds)
  {
    if (seen.contains(id))
    {
      ++r.merged;
      continue;
    }
    seen.insert(id);
    uniq << id;
  }
  QVector<std::pair<QString, RecycleEntry>> entries;
  for (const QString &id : uniq)
  {
    const CatalogAsset a = m_ctx.cat->assetById(id);
    if (a.id.isEmpty())
    {
      r.addItem(id, QStringLiteral("softdel"), OpOutcome::Rejected,
                QObject::tr("资产不存在"));
      continue;
    }
    if (m_ctx.recycle->isRemoved(id))
    {
      r.addItem(id, QStringLiteral("softdel"), OpOutcome::NoChange,
                QObject::tr("已在可回收清单"));
      continue;
    }
    RecycleEntry e;
    e.assetId = id;
    e.type = a.type;
    e.displayName = a.displayName;
    entries.append({id, e});
  }
  if (!entries.isEmpty())
  {
    QStringList ids;
    for (const auto &kv : entries)
      ids << kv.first;
    finishWithStack(new BatchRemoveCmd(m_ctx, ids, entries, reason),
                    QStringLiteral("softdel"), &r);
  }
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

BatchReceipt DataOpsCommandService::restoreFromRecycle(const QStringList &assetIds)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Sidecar, QStringLiteral("restore"), assetIds, &r))
    return r;
  QStringList uniq;
  QSet<QString> seen;
  for (const QString &id : assetIds)
  {
    if (seen.contains(id))
    {
      ++r.merged;
      continue;
    }
    seen.insert(id);
    uniq << id;
  }
  QStringList ids;
  QVector<std::pair<QString, RecycleEntry>> entries;
  for (const QString &id : uniq)
  {
    bool found = false;
    for (const RecycleEntry &x : m_ctx.recycle->entries())
      if (x.assetId == id)
      {
        ids << id;
        entries.append({id, x});
        found = true;
        break;
      }
    if (!found)
    {
      r.addItem(id, QStringLiteral("restore"),
                OpOutcome::Rejected,
                m_ctx.recycle->isRemoved(id)
                    ? QObject::tr("记录已清空，不可恢复")
                    : QObject::tr("不在可回收清单"));
    }
  }
  if (!entries.isEmpty())
  {
    finishWithStack(new BatchRestoreCmd(m_ctx, ids, entries),
                    QStringLiteral("restore"), &r);
  }
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

BatchReceipt DataOpsCommandService::createEntity(const CatalogEntity &e)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Catalog, QStringLiteral("entitynew"),
                  QStringList{e.id}, &r))
    return r;
  finishWithStack(new EntityCreateCmd(m_ctx, e), QStringLiteral("entitynew"), &r);
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

BatchReceipt DataOpsCommandService::editEntityOverride(const QString &entityId,
                                                       const EntityOverride &next,
                                                       const EntityOverride &prev,
                                                       bool mergeable)
{
  BatchReceipt r;
  if (!guardBatch(Guard::Sidecar, QStringLiteral("entity"),
                  QStringList{entityId}, &r))
    return r;
  finishWithStack(new EntityEditCmd(m_ctx, entityId, next, prev, mergeable),
                  QStringLiteral("entity"), &r);
  r.undoable = r.changedCount() > 0 && !r.anyFailure();
  return r;
}

} // namespace paleo::dataops
