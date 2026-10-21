// 层：视图
// DataListPanel 部分类（datalist_* 拆分先例）：数据对象重命名与索引级删除的
// 处理器。命令走共享 undo 栈；索引级删除不可撤销（确认文案明示）。
#include "datalist.h"
#include "dataops/dataopscommands.h"
#include "../../workflow/assetops.h"
#include "../notifications/paleonotify.h"
#include <QInputDialog>
#include <QLineEdit>

void DataListPanel::renameSelectedAsset()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.size() != 1 || !m_ctx.valid() || m_ctx.cat->refusesWrites())
    return;
  const QString id = *ids.constBegin();
  const QString prev = m_ctx.cat->assetById(id).displayName;
  if (prev.isEmpty())
    return;
  bool ok = false;
  const QString next = QInputDialog::getText(this, tr("重命名数据对象"),
                                             tr("新名称（物理文件名不变）："),
                                             QLineEdit::Normal, prev, &ok).trimmed();
  if (!ok || next.isEmpty() || next == prev)
    return;
  pushCommand(new RenameAssetCmd(m_ctx, id, prev, next));
  if (m_history)
    m_history->push(tr("重命名 %1 → %2").arg(prev, next));
  emit statusMessage(tr("已重命名为「%1」（可撤销）").arg(next));
}

void DataListPanel::removeFromCatalogKeepFiles()
{
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid() || m_ctx.cat->refusesWrites())
    return;
  if (!PaleoNotify::ask(this, tr("从目录移除（保留文件）"),
                        tr("将从目录移除 %1 项数据对象的索引记录（含挂接关系）：\n"
                           "· 所有本地文件一律不动（外链源文件与工程内受管副本均保留）；\n"
                           "· 工程内受管副本将成为孤立文件，可在存储治理中查看与回收；\n"
                           "· 此操作不可撤销——如需可恢复，请改用「删除（可恢复）…」。\n"
                           "确定继续？").arg(ids.size())))
    return;
  QString err;
  const QStringList list(ids.constBegin(), ids.constEnd());
  if (!paleo::assetops::removeAssetsKeepFiles(m_ctx.cat, list, &err))
  {
    PaleoNotify::warning(this, tr("从目录移除失败"), err);
    return;
  }
  if (m_history)
    m_history->push(tr("从目录移除 %1 项（保留文件）").arg(list.size()));
  emit statusMessage(tr("已从目录移除 %1 项索引记录；文件均未改动").arg(list.size()));
}
