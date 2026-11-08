// 层：视图
#include "datalist.h"
#include "../../catalog/datacatalog.h"
#include "dataops/dataopscommands.h"
#include "dataopsimportui.h"
#include "dataops/dataopsimportlogic.h"

#include <QFileInfo>

void DataListPanel::applyEntityDrop(const QStringList &assetIds, const QString &entityId)
{
  using namespace paleo::dataops;
  if (!m_ctx.valid() || entityId.isEmpty())
    return;
  // P1-W3/W6：编排走命令服务——准备阶段冻结每项去向，addLink 段与可撤销段
  // 分别落提交边界；回执按 新挂（不可撤销）/转移（可撤销）/跳过/失败 如实
  // 汇报，不再整批误标「可撤销」。
  const CatalogEntity target = m_ctx.cat->entityById(entityId);
  const BatchReceipt r =
      m_cmdSvc.attachOrTransfer(assetIds, entityId, QStringLiteral("reference"));
  refreshAssetTable(); // 批次一次收口刷新
  emit entityRefreshRequested();
  int attached = 0, transferred = 0, skipped = 0;
  for (const ReceiptItem &it : r.items)
  {
    if (it.outcome == OpOutcome::Changed && it.op == QLatin1String("addlink"))
      ++attached;
    else if (it.outcome == OpOutcome::Changed &&
             (it.op == QLatin1String("attach") || it.op == QLatin1String("transfer")))
      ++transferred;
    else if (it.outcome == OpOutcome::NoChange)
      ++skipped;
  }
  if (m_history && (attached + transferred) > 0)
    m_history->push(tr("批量挂接 %1（挂 %2 / 转移 %3 / 跳过 %4）")
                        .arg(entityId).arg(attached).arg(transferred).arg(skipped));
  const QString name = target.name.isEmpty() ? entityId : target.name;
  if (r.anyFailure() || r.historyInvalidated)
  {
    // 失败概要 + 明细（同一回执事实源）。
    PaleoNotify::warning(this, tr("挂接结果"), receiptStatusText(r) +
                                       QLatin1Char('\n') +
                                       r.failureLines().join(QLatin1Char('\n')));
    return;
  }
  // 可撤销范围只覆盖转移/挂未决段；纯 addLink 批次明示不可撤销。
  const QString undoNote = transferred > 0
                               ? (attached > 0 ? tr("；转移可撤销，新挂关联不可撤销")
                                               : tr("（可撤销）"))
                               : (attached > 0 ? tr("（新挂关联不可撤销）") : QString());
  emit statusMessage(tr("挂接到「%1」：新挂 %2、转移 %3、跳过 %4%5")
                         .arg(name)
                         .arg(attached)
                         .arg(transferred)
                         .arg(skipped)
                         .arg(undoNote));
}

void DataListPanel::handleExternalFiles(const QStringList &paths)
{
  using namespace paleo::dataops;
  if (paths.isEmpty())
    return;
  // 与五个导入入口同一道门：无工程（或目录不可用）时入队必然整条失败
  //（runner 报 project dir is not set）——拖入同样先写原因不干活，
  // 免得先弹预估对话框再整批报错。工程态由壳注入探针（工程关后 catalog
  // 对象可能仍然开着，isOpen 分辨不了「工程已关」）。
  const bool projectOpen = !m_projectOpen || m_projectOpen();
  if (!projectOpen || !m_ctx.valid() || !m_ctx.cat->isOpen())
  {
    emit statusMessage(!projectOpen
                           ? tr("先新建或打开工程")
                           : (m_ctx.cat && !m_ctx.cat->openError().isEmpty()
                                  ? tr("数据目录不可用，导入暂不可用：%1")
                                        .arg(m_ctx.cat->openError())
                                  : tr("数据目录尚未就绪，导入暂不可用")));
    return;
  }
  // D8.6：目录/大量文件 → 预估 + 分批确认；单文件直接走壳导入流。
  QStringList dirs;
  int fileCount = 0;
  for (const QString &p : paths)
  {
    const QFileInfo fi(p);
    if (fi.isDir())
      dirs << p;
    else if (fi.isFile())
      ++fileCount;
  }
  QStringList toImport = paths;
  if (!dirs.isEmpty() || paths.size() > 20)
  {
    ImportEstimate estimate;
    if (!dirs.isEmpty())
    {
      // finding-123：多目录拖入逐目录累计预估（旧实现只 estimateDirectory(最后一
      // 个目录)，其余目录的文件数/字节被漏计）；byExtension 按扩展名合并计数。
      QHash<QString, int> extCount;
      for (const QString &d : dirs)
      {
        const ImportEstimate e = estimateDirectory(d);
        estimate.fileCount += e.fileCount;
        estimate.totalBytes += e.totalBytes;
        estimate.largeFileCount += e.largeFileCount;
        estimate.canceled = estimate.canceled || e.canceled;
        for (const QString &part : e.byExtension)
        {
          const int colon = part.indexOf(QLatin1Char(':'));
          if (colon > 0)
            extCount[part.left(colon)] += part.mid(colon + 1).toInt();
        }
      }
      QStringList parts;
      for (auto e = extCount.constBegin(); e != extCount.constEnd(); ++e)
        parts << QStringLiteral("%1:%2").arg(e.key()).arg(e.value());
      parts.sort();
      estimate.byExtension = parts;
    }
    else
    {
      estimate.fileCount = fileCount;
      for (const QString &p : paths)
        if (QFileInfo(p).isFile())
          estimate.totalBytes += QFileInfo(p).size();
    }
    ImportEstimateDialog dlg(this);
    dlg.setEstimate(estimate);
    if (dlg.exec() != QDialog::Accepted)
      return;
    // finding-123：分批选择落到实际入队/导入路径（旧实现仅改状态栏文案，仍全量
    // 开跑）——分批确认后按 batchPaths 截首批，整批确认才全量。
    const bool batched = dlg.batchChosen();
    if (batched)
      toImport = batchPaths(paths, 200);
    emit statusMessage(batched
                           ? tr("分批导入已确认（首批 %1 项）").arg(toImport.size())
                           : tr("整批导入已确认（%1 项）").arg(toImport.size()));
  }
  // 视图只发意图：实际导入由壳的 FolderImport 流执行（T22 确认表复用）。
  emit externalImportRequested(toImport);
  // 导入队列（D8.1）：未注入 runner 时仅登记（生产接线见 docs/dataops/GAPS.md）。
  if (m_importQueue)
  {
    QHash<QString, QString> typeByExt;
    for (const ImportPreset &p : importPresets())
      typeByExt = p.typeMap; // 预设的类型映射（最后一个命中的预设）
    m_importQueue->enqueuePaths(toImport, typeByExt);
  }
}
