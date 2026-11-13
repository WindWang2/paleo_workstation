// 层：功能
#include "dataopscommandservice.h"

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
  if (agg->failureStage.isEmpty() && !part.failureStage.isEmpty())
    agg->failureStage = part.failureStage; // 子回执的失败阶段向聚合面传播
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

} // namespace paleo::dataops
