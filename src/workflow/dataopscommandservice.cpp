// 层：功能
#include "dataopscommandservice.h"

namespace paleo::dataops
{

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
        r.requested = 1; // 单操作语义：实际意图只有这一条（计数与条目互核）
        return r; // 首个选中资产的第一条已决链接
      }
  }
  r.requested = 1; // 单操作语义：无可操作项 = 这一条意图被拒
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
        r.requested = 1; // 单操作语义：实际意图只有这一条
        return r;
      }
  }
  r.requested = 1; // 单操作语义：无可操作项 = 这一条意图被拒
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
