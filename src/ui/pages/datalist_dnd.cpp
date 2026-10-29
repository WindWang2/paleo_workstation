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
  const CatalogEntity target = m_ctx.cat->entityById(entityId);
  int attached = 0, transferred = 0, skipped = 0;
  QVector<paleo::dataops::DataOpCommand *> cmds;
  for (const QString &id : assetIds)
  {
    const QVector<EntityAssetLink> links = m_ctx.cat->linksForAsset(id);
    if (links.isEmpty())
    {
      // 无链接资产：直接建链接挂到目标实体（addLink——不可撤销路径，
      // D5.5 语义由确认对话框承担；这里作为拖放主路径不弹额外确认，
      // 失败明细走状态栏）。
      EntityAssetLink l;
      l.entityType = target.entityType;
      l.entityId = entityId;
      l.assetId = id;
      l.role = QStringLiteral("reference");
      QString err;
      if (!m_ctx.cat->addLink(l, &err))
      {
        emit statusMessage(tr("挂接失败：%1").arg(err));
        continue;
      }
      ++attached;
      continue;
    }
    bool acted = false;
    for (const EntityAssetLink &l : links)
    {
      if (l.unresolved)
      {
        cmds.append(new paleo::dataops::AttachLinkCmd(m_ctx, id, l.role, entityId,
                                                      target.entityType));
        ++attached;
        acted = true;
        break; // 每资产处理第一条未决链接（与既有行内控件口径一致）
      }
    }
    if (acted)
      continue;
    for (const EntityAssetLink &l : links)
    {
      if (!l.unresolved && l.entityId != entityId)
      {
        cmds.append(new paleo::dataops::TransferLinkCmd(m_ctx, id, l.role, l.entityId,
                                                        entityId));
        ++transferred;
        acted = true;
        break;
      }
    }
    if (!acted)
      ++skipped; // 已挂到目标实体的资产
  }
  // N 条命令一次收口刷新（拖多选资产不再 N 次整表重建）。
  pushCommands(cmds);
  if (m_history)
    m_history->push(tr("批量挂接 %1（挂 %2 / 转移 %3 / 跳过 %4）")
                        .arg(entityId).arg(attached).arg(transferred).arg(skipped));
  // D3.1 确认 toast（非模态反馈 + 可撤销提示）。
  emit statusMessage(tr("挂接到「%1」：新挂 %2、转移 %3、跳过 %4（可撤销）")
                         .arg(target.name.isEmpty() ? entityId : target.name)
                         .arg(attached).arg(transferred).arg(skipped));
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
