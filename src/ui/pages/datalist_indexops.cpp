// 层：视图
// DataListPanel 部分类（datalist_* 拆分先例）：数据对象重命名与索引级删除的
// 处理器。命令走共享 undo 栈；索引级删除不可撤销（确认文案明示）。
#include "datalist.h"
#include "dataops/dataopscommands.h"
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
