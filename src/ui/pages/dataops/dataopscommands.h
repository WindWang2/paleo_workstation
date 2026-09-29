// 层：视图
// ui/pages/dataops/dataopscommands — D5 命令对象的具体实现。
// 全部操作组合 DataCatalog 既有 mutator（attachLink/setLinkUnresolved/
// setLinkPrimary/addLink/addEntity）与 sidecar store（标签/改型/软删/实体
// override）——视图层零服务写路径。身份寻址沿 T28 惯例：动作时刻重扫
// links()，不持行下标。
//
// 不可撤销面（D5.5）：addLink（角色变更走它）与 addEntity 无删除/更新
// 对偶 API——RoleChangeCmd 不入栈，确认对话框明示不可撤销；EntityCreate
// 的「删除」是 sidecar 软删（可撤销）。
#pragma once

#include <QPointer>

#include "../../../catalog/datacatalog.h"
#include "dataopsmodel.h"
#include "../dataopsundo.h"

namespace paleo::dataops
{

// 命令执行环境（DataListPanel 装配一次，命令栈共享）。
struct DataOpsContext
{
  QPointer<DataCatalog> cat;
  TagStore *tags = nullptr;
  AssetOverrideStore *assetOverrides = nullptr;
  EntityOverrideStore *entityOverrides = nullptr;
  RecycleBin *recycle = nullptr;

  bool valid() const { return cat && tags && assetOverrides && entityOverrides && recycle; }
};

// links() 中按 (assetId, role[, entityId]) 找链接（T28 身份寻址复刻）。
inline int indexOfLink(DataCatalog *cat, const QString &assetId, const QString &role,
                       const QString &entityId)
{
  const QVector<EntityAssetLink> ls = cat->links();
  for (int i = 0; i < ls.size(); ++i)
  {
    const EntityAssetLink &l = ls.at(i);
    if (l.assetId != assetId || l.role != role)
      continue;
    if (entityId.isEmpty())
    {
      if (l.unresolved)
        return i;
    }
    else if (!l.unresolved && l.entityId == entityId)
      return i;
  }
  return -1;
}

// ---- 挂接（未决 → 已决）----------------------------------------------------
class AttachLinkCmd : public DataOpCommand
{
public:
  AttachLinkCmd(DataOpsContext ctx, QString assetId, QString role, QString entityId,
                QString entityType)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_role(std::move(role))
    , m_entityId(std::move(entityId)), m_entityType(std::move(entityType))
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, QString());
    if (idx < 0)
      return;
    m_ctx.cat->attachLink(idx, m_entityId);
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, m_entityId);
    if (idx >= 0)
      m_ctx.cat->setLinkUnresolved(idx);
  }
  QString text() const override
  {
    return QObject::tr("挂接 %1→%2").arg(m_assetId, m_entityId);
  }
  QByteArray id() const override { return "attach"; }

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_role, m_entityId, m_entityType;
};

// ---- 解挂（已决 → 未决）----------------------------------------------------
class DetachLinkCmd : public DataOpCommand
{
public:
  DetachLinkCmd(DataOpsContext ctx, QString assetId, QString role, QString entityId)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_role(std::move(role))
    , m_entityId(std::move(entityId))
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, m_entityId);
    if (idx >= 0)
      m_ctx.cat->setLinkUnresolved(idx);
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, QString());
    if (idx >= 0)
      m_ctx.cat->attachLink(idx, m_entityId);
  }
  QString text() const override
  {
    return QObject::tr("解挂 %1→%2").arg(m_assetId, m_entityId);
  }
  QByteArray id() const override { return "detach"; }

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_role, m_entityId;
};

// ---- 转移挂接（实体 A → 实体 B，D3.4）--------------------------------------
// redo：本链接解挂（保留链接行）→ 同一链接行 attach 到目标实体。
// catalog 不增不删链接行，全程可逆。
class TransferLinkCmd : public DataOpCommand
{
public:
  TransferLinkCmd(DataOpsContext ctx, QString assetId, QString role, QString fromEntity,
                  QString toEntity)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_role(std::move(role))
    , m_from(std::move(fromEntity)), m_to(std::move(toEntity))
  {
  }
  void redo() override { apply(m_from, m_to); }
  void undo() override { apply(m_to, m_from); }
  QString text() const override
  {
    return QObject::tr("转移挂接 %1：%2→%3").arg(m_assetId, m_from, m_to);
  }
  QByteArray id() const override { return "transfer"; }

private:
  void apply(const QString &from, const QString &to)
  {
    if (!m_ctx.valid() || from == to)
      return;
    int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, from);
    if (idx < 0)
      idx = indexOfLink(m_ctx.cat, m_assetId, m_role, QString()); // 已被解挂的半途态
    if (idx < 0)
      return;
    m_ctx.cat->setLinkUnresolved(idx);   // 同一行：先置未决（幂等——本就未决也成功？不，已决才成功）
    m_ctx.cat->attachLink(idx, to);      // 再挂到目标
  }
  DataOpsContext m_ctx;
  QString m_assetId, m_role, m_from, m_to;
};

// ---- 设为主关联 -------------------------------------------------------------
class SetPrimaryCmd : public DataOpCommand
{
public:
  SetPrimaryCmd(DataOpsContext ctx, QString assetId, QString role, QString entityId,
                QString prevPrimaryAssetId)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_role(std::move(role))
    , m_entityId(std::move(entityId))
    , m_prevPrimary(std::move(prevPrimaryAssetId))
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    const int idx = indexOfLink(m_ctx.cat, m_assetId, m_role, m_entityId);
    if (idx >= 0)
      m_ctx.cat->setLinkPrimary(idx);
  }
  void undo() override
  {
    if (!m_ctx.valid() || m_prevPrimary.isEmpty())
      return;
    const int idx = indexOfLink(m_ctx.cat, m_prevPrimary, m_role, m_entityId);
    if (idx >= 0)
      m_ctx.cat->setLinkPrimary(idx);
  }
  QString text() const override
  {
    return QObject::tr("设为主版本 %1（%2·%3）").arg(m_assetId, m_entityId, m_role);
  }
  QByteArray id() const override { return "primary"; }

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_role, m_entityId, m_prevPrimary;
};

// ---- 打/去标签（D5.3 合并示例：同资产连续操作并栈）-------------------------
class TagCmd : public DataOpCommand
{
public:
  TagCmd(DataOpsContext ctx, QString assetId, QString tag, bool add)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_tag(std::move(tag)), m_add(add)
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    if (m_add)
      m_ctx.tags->addTag(m_assetId, m_tag);
    else
      m_ctx.tags->removeTag(m_assetId, m_tag);
    m_ctx.tags->save();
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    if (m_add)
      m_ctx.tags->removeTag(m_assetId, m_tag);
    else
      m_ctx.tags->addTag(m_assetId, m_tag);
    m_ctx.tags->save();
  }
  QString text() const override
  {
    return m_add ? QObject::tr("打标签 %1「%2」").arg(m_assetId, m_tag)
                 : QObject::tr("去标签 %1「%2」").arg(m_assetId, m_tag);
  }
  QByteArray id() const override { return "tag"; }
  // 合并：同资产同标签的相邻 add/remove 互相抵消？——不抵消，语义保留；
  // 只合并同向连续（同资产同标签重复 add 幂等，无需合并）。这里取「同资产
  // 相邻标签操作合并为一条文案」的保守策略：仅当完全同向同键时吞并。
  bool mergeWith(const DataOpCommand *other) override
  {
    const auto *o = dynamic_cast<const TagCmd *>(other);
    return o && o->m_assetId == m_assetId && o->m_tag == m_tag && o->m_add == m_add;
  }

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_tag;
  bool m_add = true;
};

// ---- 类型改写（D1.5）--------------------------------------------------------
class TypeOverrideCmd : public DataOpCommand
{
public:
  TypeOverrideCmd(DataOpsContext ctx, QString assetId, QString newType, QString prevType)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_new(std::move(newType))
    , m_prev(std::move(prevType))
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    m_ctx.assetOverrides->setType(m_assetId, m_new);
    m_ctx.assetOverrides->save();
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    // prev 空 = 原无改写：撤销 = 清除改写回 catalog 原型。
    if (m_prev.isEmpty())
      m_ctx.assetOverrides->clearType(m_assetId);
    else
      m_ctx.assetOverrides->setType(m_assetId, m_prev);
    m_ctx.assetOverrides->save();
  }
  QString text() const override
  {
    return QObject::tr("改类型 %1→%2").arg(m_assetId, m_new);
  }
  QByteArray id() const override { return "type"; }

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_new, m_prev;
};

// ---- 软删 / 恢复（D1.6）----------------------------------------------------
class SoftDeleteCmd : public DataOpCommand
{
public:
  SoftDeleteCmd(DataOpsContext ctx, QString assetId, QString displayName, QString type,
                bool remove) // remove=false → 恢复
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_name(std::move(displayName))
    , m_type(std::move(type)), m_remove(remove)
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    if (m_remove)
      m_ctx.recycle->remove(m_assetId, m_type, m_name, tr_reason());
    else
      m_ctx.recycle->restore(m_assetId);
    m_ctx.recycle->save();
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    if (m_remove)
      m_ctx.recycle->restore(m_assetId);
    else
      m_ctx.recycle->remove(m_assetId, m_type, m_name, tr_reason());
    m_ctx.recycle->save();
  }
  QString text() const override
  {
    return m_remove ? QObject::tr("移除 %1（进可回收清单）").arg(m_name)
                    : QObject::tr("恢复 %1").arg(m_name);
  }
  QByteArray id() const override { return "softdel"; }

private:
  QString tr_reason() const
  {
    return m_remove ? QObject::tr("批量移除") : QObject::tr("撤销恢复");
  }
  DataOpsContext m_ctx;
  QString m_assetId, m_name, m_type;
  bool m_remove = true;
};

// ---- 实体改名 / 坐标注释（D4.1/D4.2，override 落盘）------------------------
class EntityEditCmd : public DataOpCommand
{
public:
  EntityEditCmd(DataOpsContext ctx, QString entityId, EntityOverride next,
                EntityOverride prev, bool mergeable)
    : m_ctx(ctx), m_entityId(std::move(entityId)), m_next(std::move(next))
    , m_prev(std::move(prev)), m_mergeable(mergeable)
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    m_ctx.entityOverrides->setOverride(m_entityId, m_next);
    m_ctx.entityOverrides->save();
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    m_ctx.entityOverrides->setOverride(m_entityId, m_prev);
    m_ctx.entityOverrides->save();
  }
  QString text() const override
  {
    if (!m_next.note.isEmpty() && m_next.note != m_prev.note)
      return QObject::tr("改备注 %1").arg(m_entityId);
    if (m_next.hasCoords && (!m_prev.hasCoords || m_next.surfaceX != m_prev.surfaceX ||
                             m_next.surfaceY != m_prev.surfaceY))
      return QObject::tr("改坐标 %1").arg(m_entityId);
    return QObject::tr("重命名实体 %1→%2").arg(m_entityId, m_next.name);
  }
  QByteArray id() const override { return m_mergeable ? "entityedit" : QByteArray(); }

private:
  DataOpsContext m_ctx;
  QString m_entityId;
  EntityOverride m_next, m_prev;
  bool m_mergeable = false;
};

// ---- 新建实体（addEntity + sidecar 计数；删除走软删面）---------------------
class EntityCreateCmd : public DataOpCommand
{
public:
  EntityCreateCmd(DataOpsContext ctx, CatalogEntity e)
    : m_ctx(ctx), m_e(std::move(e))
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    // addEntity 幂等保护：已存在（undo 后 redo 的 catalog 状态没有实体删除，
    // undo 只做 sidecar 软删）时跳过。
    if (!m_ctx.cat->hasEntity(m_e.id))
      m_ctx.cat->addEntity(m_e);
    m_ctx.recycle->restore(m_e.id); // undo 做的是软删——恢复可见性
    m_ctx.recycle->save();
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    // catalog 无 removeEntity API（GAPS）：undo = sidecar 软删（视图隐藏），
    // 实体记录留库，redo 时恢复可见。
    m_ctx.recycle->remove(m_e.id, m_e.entityType, m_e.name,
                          QObject::tr("新建实体撤销"));
    m_ctx.recycle->save();
  }
  QString text() const override
  {
    return QObject::tr("新建实体 %1").arg(m_e.name.isEmpty() ? m_e.id : m_e.name);
  }
  QByteArray id() const override { return "entitynew"; }

private:
  DataOpsContext m_ctx;
  CatalogEntity m_e;
};

} // namespace paleo::dataops
