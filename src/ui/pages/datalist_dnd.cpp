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
        pushCommand(new AttachLinkCmd(m_ctx, id, l.role, entityId, target.entityType));
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
        pushCommand(new TransferLinkCmd(m_ctx, id, l.role, l.entityId, entityId));
        ++transferred;
        acted = true;
        break;
      }
    }
    if (!acted)
      ++skipped; // 已挂到目标实体的资产
  }
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
  // D8.6：目录/大量文件 → 预估 + 分批确认；单文件直接走壳导入流。
  QString dir;
  int fileCount = 0;
  for (const QString &p : paths)
  {
    const QFileInfo fi(p);
    if (fi.isDir())
      dir = p;
    else if (fi.isFile())
      ++fileCount;
  }
  if (!dir.isEmpty() || paths.size() > 20)
  {
    ImportEstimate estimate;
    if (!dir.isEmpty())
      estimate = estimateDirectory(dir);
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
    emit statusMessage(dlg.batchChosen()
                           ? tr("分批导入已确认（首批 %1 项）").arg(batchPaths(paths, 200).size())
                           : tr("整批导入已确认（%1 项）").arg(paths.size()));
  }
  // 视图只发意图：实际导入由壳的 FolderImport 流执行（T22 确认表复用）。
  emit externalImportRequested(paths);
  // 导入队列（D8.1）：未注入 runner 时仅登记（生产接线见 docs/dataops/GAPS.md）。
  if (m_importQueue)
  {
    QHash<QString, QString> typeByExt;
    for (const ImportPreset &p : importPresets())
      typeByExt = p.typeMap; // 预设的类型映射（最后一个命中的预设）
    m_importQueue->enqueuePaths(paths, typeByExt);
  }
}
