// 层：功能
// workflow/dataopscommandservice — P1（data-commands 2026-11-08）：数据操作
// 编排服务化与可信执行回执。原先散在视图层（ui/pages/dataopsundo.h 与
// ui/pages/dataops/dataopscommands.h）的可执行核心下沉功能层：命令对象、
// 共享撤销栈、侧车存储绑定与会话守卫在此编排；视图只收集意图并展示回执。
// 旧头经薄转发保持全部公开类型/入口可用（兼容声明，见两个 ui/ 头）。
//
// 关键语义（对应任务 W2–W5）：
//   · 回执（BatchReceipt）区分 成功变更/无变化/拒绝/失败已恢复/失败留有部分
//     变化，请求数、去重数与各结果集合可相互核对；不编造源 API 没有的错误码。
//   · 目录侧多步操作（转移、目录族宏）用 DataCatalog::BatchSave 形成真实
//     事务边界：abort 或落盘失败都会把内存滚回批次前状态（#169），不留
//     「已解挂未挂目标」的半途中间态。
//   · 侧车侧批量操作一次落盘（不再 N 次序列化整份文件）；save 失败先恢复
//     内存前态再如实上报失败阶段。目录事务与多份 JSON 侧车不是原子存储，
//     不宣称跨存储 ACID——宏按目录/侧车两段补偿并核对每次补偿结果。
//   · 会话守卫：QPointer 生命周期 + catalogPath 绑定值 + 会话代数
//     （DataOpsSession）三重校验，同代才执行；旧命令不写入新会话。
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include "../catalog/datacatalog.h"
#include "../services/dataopsstores.h"

namespace paleo::dataops
{

// ---- P1-W5：结构化执行回执 --------------------------------------------------
// 状态命名：Changed=成功且实际变更；NoChange=成功但幂等命中（无变更）；
// Rejected=准备/守卫阶段拒绝（零变更）；FailedRecovered=执行失败但已完整
// 恢复（目录整批回滚或补偿成功/侧车内存已还原）；FailedPartial=失败且留有
// 部分变化（状态不确定，需重新读取）。
enum class OpOutcome
{
  Changed,
  NoChange,
  Rejected,
  FailedRecovered,
  FailedPartial
};

struct ReceiptItem
{
  QString targetId;               // 资产/实体稳定 ID
  QString op;                     // attach/detach/transfer/primary/tag/type/softdel/restore/rename/addlink/entity
  OpOutcome outcome = OpOutcome::Rejected;
  QString reason;                 // 源 API 如实给出的原因（可空）
  bool undoable = false;          // 该项是否落在一条可撤销历史里
};

struct BatchReceipt
{
  QString operation;              // 操作名（tag-add / attach-drop / …）
  int requested = 0;              // 请求数
  int merged = 0;                 // 准备阶段去重合并数
  QList<ReceiptItem> items;       // 每条请求的去向（含拒绝项——请求→计划映射）
  bool undoable = false;          // 一条历史覆盖全部 Changed 项且可整组撤销
  bool historyInvalidated = false;// 失败留有部分变化 → 撤销栈已整栈作废
  QString failureStage;           // guard/prepare/catalog-commit/catalog-abort/sidecar-write/compensation/…

  int count(OpOutcome o) const
  {
    int n = 0;
    for (const ReceiptItem &i : items)
      n += int(i.outcome == o);
    return n;
  }
  int changedCount() const { return count(OpOutcome::Changed); }
  int noChangeCount() const { return count(OpOutcome::NoChange); }
  int rejectedCount() const { return count(OpOutcome::Rejected); }
  int failedCount() const
  {
    return count(OpOutcome::FailedRecovered) + count(OpOutcome::FailedPartial);
  }
  bool anyPartial() const { return count(OpOutcome::FailedPartial) > 0; }
  bool anyFailure() const { return failedCount() > 0 || historyInvalidated; }
  // 全部条目幂等命中且无失败——不追加历史、不失败。
  bool allNoChange() const
  {
    return !items.isEmpty() && noChangeCount() == items.size();
  }
  // 失败明细行（视图失败明细对话框/状态栏共用同一事实）。
  QStringList failureLines() const
  {
    QStringList out;
    for (const ReceiptItem &i : items)
      if (i.outcome == OpOutcome::FailedRecovered || i.outcome == OpOutcome::FailedPartial)
        out << QStringLiteral("%1: %2").arg(i.targetId, i.reason);
    return out;
  }
  void addItem(QString targetId, QString op, OpOutcome o, QString reason = QString(),
               bool undoable = false)
  {
    ReceiptItem it;
    it.targetId = std::move(targetId);
    it.op = std::move(op);
    it.outcome = o;
    it.reason = std::move(reason);
    it.undoable = undoable;
    items.append(std::move(it));
  }
};

// ---- 会话代数（P1-W2）-------------------------------------------------------
// 面板在 catalog 会话变化（换对象/换路径）时 bump；命令执行/撤销前核对代数。
// 只读判断归 catalog（refusesWrites），不加 catalog 内核状态。
class DataOpsSession : public QObject
{
  Q_OBJECT
public:
  explicit DataOpsSession(QObject *parent = nullptr) : QObject(parent) {}
  quint64 generation() const { return m_generation; }
  void bump() { ++m_generation; }

private:
  quint64 m_generation = 1;
};

// ---- 命令执行环境 -----------------------------------------------------------
// 迁移自 ui/pages/dataops/dataopscommands.h（兼容面不变），新增会话三件套。
struct DataOpsContext
{
  QPointer<DataCatalog> cat;
  TagStore *tags = nullptr;
  AssetOverrideStore *assetOverrides = nullptr;
  EntityOverrideStore *entityOverrides = nullptr;
  RecycleBin *recycle = nullptr;
  QPointer<DataOpsSession> session;   // 可空：无会话源时退化为指针+isOpen 守卫
  quint64 sessionGeneration = 0;      // 绑定时的会话代
  QString boundCatalogPath;           // 绑定时的 catalog 路径（会话身份）

  // 旧语义兼容：指针非空（既有调用方/测试沿用）。
  bool valid() const
  {
    return cat && tags && assetOverrides && entityOverrides && recycle;
  }
  // P1-W2 完整守卫：对象生命周期 + isOpen + 会话代 + 路径一致。
  // why 非空时写入人可读原因。
  bool alive(QString *why = nullptr) const;
};

// PW-015: shared failure helpers — every redo()/undo() reports whether the
// catalog/sidecar actually reached the intended state.
inline bool opFail(QString *err, const QString &why)
{
  if (err)
    *err = why;
  return false;
}
inline QString ctxInvalidText() { return QObject::tr("数据目录不可用（工程已关闭或切换）"); }
inline QString linkMissingText(const QString &assetId)
{
  return QObject::tr("找不到 %1 的对应关联（目录已在别处被改动）").arg(assetId);
}
inline QString saveFailedText() { return QObject::tr("写入侧车文件失败"); }

// links() 中按 (assetId, role[, entityId]) 找链接（T28 身份寻址：动作时刻
// 重扫，不持行下标——刷新后同一行上的其他对象不会被误动）。
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

// ---- 命令基类（迁移自 ui/pages/dataopsundo.h）-------------------------------
class DataOpCommand
{
public:
  virtual ~DataOpCommand() = default;
  // PW-015：返回 false = 未能把 catalog/sidecar 带到目标状态（原因见
  // errorText()）。栈据此不再搬运该命令，避免历史与真实状态脱节。
  virtual bool redo() = 0;
  virtual bool undo() = 0;
  QString errorText() const { return m_error; }
  // 本次 redo/undo 失败后存储是否仍处于可信已知状态。false = 已留部分变化
  // （补偿失败/状态不确定）——栈须整栈作废并如实披露。
  bool storeIntact() const { return m_storeIntact; }
  // 执行回执：redo/undo 后可读，栈与服务据此组装面向用户的真实结果。
  virtual const BatchReceipt &receipt() const { return m_receipt; }
  // 菜单/状态栏文案（D5.2）。
  virtual QString text() const = 0;
  // 同类合并键：相邻栈顶命令 id 相同且 mergeWith 接受 → 合并为一条。
  virtual QByteArray id() const { return QByteArray(); }
  // 返回 true = 吞并 other（other 的效果已被本命令覆盖）。
  virtual bool mergeWith(const DataOpCommand *other)
  {
    Q_UNUSED(other);
    return false;
  }

protected:
  QString m_error;
  bool m_storeIntact = true;
  BatchReceipt m_receipt;
};

// PW-049：宏命令——批量操作作为一条撤销记录。两种模式：
//   · 目录族宏（setBatchCatalog）：children 全部为目录操作，整批一个
//     BatchSave 提交边界；任一 child 失败 → batch.abort() 把目录整体滚回
//     批次前状态（不留半途态，也无需逐 child 补偿）。
//   · 混合宏（无批次目录，兼容旧行为）：child 各自原子（单 mutator 自带
//     save 失败回滚 / 侧车命令自带内存还原），失败时对已完成 children
//     逆序补偿 undo() 并核对每次结果；补偿失败 → storeIntact=false。
class DataOpMacro : public DataOpCommand
{
public:
  DataOpMacro(QString text, QList<DataOpCommand *> children);
  ~DataOpMacro() override;
  // 仅当 children 全部是目录（DataCatalog）操作时调用——abort 回滚的是目录
  // 内存，侧车 child 不在此保护面内。
  void setBatchCatalog(DataCatalog *cat) { m_batchCat = cat; }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  int childCount() const { return m_children.size(); }

private:
  // forward/backward 任一方向的执行体。redo: 正序 child->redo()；
  // undo: 逆序 child->undo()。
  bool run(bool forward);
  QString m_text;
  QList<DataOpCommand *> m_children;
  DataCatalog *m_batchCat = nullptr;
};

class DataOpsUndoStack : public QObject
{
  Q_OBJECT

public:
  static constexpr int kMaxDepth = 50; // D5.3

  explicit DataOpsUndoStack(QObject *parent = nullptr)
    : QObject(parent)
  {
  }

  ~DataOpsUndoStack() override { qDeleteAll(m_undo); qDeleteAll(m_redo); }

  // 执行命令并入栈（redo 在此处调用一次）。空指针忽略。
  // PW-015：redo 失败 → 命令不入栈（删除），返回 false，原因见 lastError()。
  // P1-W5：redo 失败且 storeIntact()==false（留有部分变化）→ 两支栈整栈
  // 作废（历史不再描述真实状态）；全部条目无变化 → 不入栈、不清 redo、
  // 返回 true（无变化不添加虚假命令）。
  bool push(DataOpCommand *cmd);

  // 最近一次失败的原因（push/undo/redo 失败时设置；成功时清空）。
  QString lastError() const { return m_lastError; }
  // 最近一次 push/undo/redo 的命令回执（失败命令的回执在删除前留存）。
  const BatchReceipt &lastReceipt() const { return m_lastReceipt; }

  bool canUndo() const { return !m_undo.isEmpty(); }
  bool canRedo() const { return !m_redo.isEmpty(); }
  QString undoText() const { return m_undo.isEmpty() ? QString() : m_undo.last()->text(); }
  QString redoText() const { return m_redo.isEmpty() ? QString() : m_redo.last()->text(); }
  int depth() const { return m_undo.size(); }
  int redoDepth() const { return m_redo.size(); }

  // 撤销/重做。返回执行的命令文案（D5.4 状态栏反馈），空 = 栈空未动或失败。
  QString undo();
  QString redo();

  // D5.6：项目会话清空策略——open()/切换工程时调用；redo 支路一并清。
  void clear();

  // 测试/审计面：当前栈内命令文案（栈底→栈顶）。
  QStringList undoTexts() const
  {
    QStringList out;
    for (const DataOpCommand *c : m_undo)
      out << c->text();
    return out;
  }

signals:
  // 任何栈变化（push/undo/redo/clear）——订阅方刷新按钮态/菜单文案/面板。
  void stackChanged();
  // PW-015：命令执行失败（原因文案）。
  void operationFailed(const QString &error); // NOLINT(readability-inconsistent-declaration-parameter-name)

private:
  // undo/redo 失败后历史已不再描述真实状态——整栈作废（失败命令一并删除），
  // 宁可丢历史也不让后续撤销在错误基线上操作。
  void failAndReset(DataOpCommand *cmd);

  // 作废 redo 支路：QList 的 clear 只销毁指针元素，指向对象要 qDeleteAll。
  void clearRedo()
  {
    qDeleteAll(m_redo);
    m_redo.clear();
  }

  QList<DataOpCommand *> m_undo;
  QList<DataOpCommand *> m_redo;
  QString m_lastError;
  BatchReceipt m_lastReceipt;
};

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
  bool redo() override;
  bool undo() override;
  QString text() const override;
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
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "detach"; }

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_role, m_entityId;
};

// ---- 转移挂接（实体 A → 实体 B，D3.4）--------------------------------------
// P1-W3：两步（先置未决再挂目标）包进一个 BatchSave——attach 失败时 abort
// 把目录滚回批次前，不再遗留「已解挂未挂目标」的半途态；落盘失败同样由
// #169 回滚（内存与盘一致）。
class TransferLinkCmd : public DataOpCommand
{
public:
  TransferLinkCmd(DataOpsContext ctx, QString assetId, QString role, QString fromEntity,
                  QString toEntity)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_role(std::move(role))
    , m_from(std::move(fromEntity)), m_to(std::move(toEntity))
  {
  }
  bool redo() override { return apply(m_from, m_to); }
  bool undo() override { return apply(m_to, m_from); }
  QString text() const override;
  QByteArray id() const override { return "transfer"; }

private:
  bool apply(const QString &from, const QString &to);
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
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "primary"; }

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_role, m_entityId, m_prevPrimary;
};

// ---- 重命名数据资产（catalog renameAsset 写路径，单 mutator 原子）-----------
class RenameAssetCmd : public DataOpCommand
{
public:
  RenameAssetCmd(DataOpsContext ctx, QString assetId, QString prevName, QString nextName)
    : m_ctx(ctx), m_assetId(std::move(assetId))
    , m_prev(std::move(prevName)), m_next(std::move(nextName))
  {
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return QByteArray(); } // 不并栈

private:
  bool runRename(const QString &name);
  DataOpsContext m_ctx;
  QString m_assetId, m_prev, m_next;
};

// ---- 打/去标签（单条兼容面；批量走 BatchTagCmd——一次落盘）------------------
class TagCmd : public DataOpCommand
{
public:
  TagCmd(DataOpsContext ctx, QString assetId, QString tag, bool add)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_tag(std::move(tag)), m_add(add)
  {
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "tag"; }
  bool mergeWith(const DataOpCommand *other) override;

private:
  bool run(bool add);
  DataOpsContext m_ctx;
  QString m_assetId, m_tag;
  bool m_add = true;
};

// ---- 批量打/去标签（P1-W4：一次内存变更 + 单次侧车提交，整组可撤销）--------
class BatchTagCmd : public DataOpCommand
{
public:
  BatchTagCmd(DataOpsContext ctx, QString tag, bool add, QStringList assetIds)
    : m_ctx(ctx), m_tag(std::move(tag)), m_add(add), m_ids(std::move(assetIds))
  {
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return m_add ? "batchtag+" : "batchtag-"; }

private:
  // 通用执行：applyFn 做内存变更（返回是否有变化），失败时按前态恢复并
  // 如实报告（save 失败 → FailedRecovered；恢复失败 → FailedPartial）。
  bool run(bool add);
  DataOpsContext m_ctx;
  QString m_tag;
  bool m_add = true;
  QStringList m_ids;
  QHash<QString, QStringList> m_prev; // 实际变更项的前态（undo 精确恢复）
};

// ---- 类型改写（单条兼容面）--------------------------------------------------
class TypeOverrideCmd : public DataOpCommand
{
public:
  TypeOverrideCmd(DataOpsContext ctx, QString assetId, QString newType, QString prevType)
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_new(std::move(newType))
    , m_prev(std::move(prevType))
  {
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "type"; }

private:
  bool run(const QString &type);
  DataOpsContext m_ctx;
  QString m_assetId, m_new, m_prev;
};

// ---- 批量类型改写（P1-W4：一次内存变更 + 单次侧车提交，整组可撤销）--------
class BatchTypeOverrideCmd : public DataOpCommand
{
public:
  BatchTypeOverrideCmd(DataOpsContext ctx, QString newType,
                       const QList<QPair<QString, QString>> &idPrevPairs)
    : m_ctx(ctx), m_new(std::move(newType))
  {
    for (const auto &kv : idPrevPairs)
    {
      m_ids.append(kv.first);
      m_prev.insert(kv.first, kv.second);
    }
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "batchtype"; }

private:
  bool run(bool forward);
  DataOpsContext m_ctx;
  QString m_new;
  QStringList m_ids;
  QHash<QString, QString> m_prev; // 改写前类型（空 = 原无改写）
};

// ---- 软删 / 恢复（单条兼容面）----------------------------------------------
class SoftDeleteCmd : public DataOpCommand
{
public:
  SoftDeleteCmd(DataOpsContext ctx, QString assetId, QString displayName, QString type,
                bool remove) // remove=false → 恢复
    : m_ctx(ctx), m_assetId(std::move(assetId)), m_name(std::move(displayName))
    , m_type(std::move(type)), m_remove(remove)
  {
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "softdel"; }

private:
  // 单命令面：save 失败先恢复 recycle 前态再报失败。
  bool run(bool remove);
  QString tr_reason() const;
  DataOpsContext m_ctx;
  QString m_assetId, m_name, m_type;
  bool m_remove = true;
};

// ---- 方向 30：批量恢复（一条命令一次落盘，整组可撤销）-----------------------
class BatchRestoreCmd : public DataOpCommand
{
public:
  BatchRestoreCmd(DataOpsContext ctx, QStringList assetIds,
                  QVector<std::pair<QString, RecycleEntry>> entries)
    : m_ctx(ctx), m_ids(std::move(assetIds)), m_entries(std::move(entries))
  {
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "batchrestore"; }

private:
  DataOpsContext m_ctx;
  QStringList m_ids;
  QVector<std::pair<QString, RecycleEntry>> m_entries; // undo 需要的原条目（含原因）
};

// ---- 方向 30：批量软删（一条命令一次落盘，整组可撤销）-----------------------
class BatchRemoveCmd : public DataOpCommand
{
public:
  BatchRemoveCmd(DataOpsContext ctx, QStringList assetIds,
                 QVector<std::pair<QString, RecycleEntry>> entries, QString reason)
    : m_ctx(ctx), m_ids(std::move(assetIds)), m_entries(std::move(entries))
    , m_reason(std::move(reason))
  {
  }
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "batchremove"; }

private:
  DataOpsContext m_ctx;
  QStringList m_ids;
  QVector<std::pair<QString, RecycleEntry>> m_entries;
  QString m_reason;
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
  bool redo() override;
  bool undo() override;
  QString text() const override;
  // 两侧同为 QByteArray：const char[] 与 QByteArray 的三目公共类型在 MSVC
  // 下有歧义（C2445，GCC 放行）。
  QByteArray id() const override
  { return m_mergeable ? QByteArrayLiteral("entityedit") : QByteArray(); }

private:
  bool run(const EntityOverride &o);
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
  bool redo() override;
  bool undo() override;
  QString text() const override;
  QByteArray id() const override { return "entitynew"; }

private:
  DataOpsContext m_ctx;
  CatalogEntity m_e;
};

// ---- P1-W2/W6：编排服务 -----------------------------------------------------
// 视图收集意图（多选 ID 集、目标实体、新值），服务负责守卫→准备（去重/分类/
// 冻结前态）→执行（目录 BatchSave / 侧车单次提交 / 经共享栈入历史）→回执。
// 批次使用调用时给定的明确 ID 集；按钮灰态只是引导，功能层守卫是权威。
class DataOpsCommandService
{
public:
  // 绑定会话（面板 loadStoresForCatalog 每次调用；同一 cat+路径重复绑定零
  // 副作用）。cat 对象或路径变化 → bump 会话代并清空撤销栈（旧命令不再
  // 写入新会话）。
  void bindSession(DataCatalog *cat, TagStore *tags, AssetOverrideStore *assetOverrides,
                   EntityOverrideStore *entityOverrides, RecycleBin *recycle,
                   DataOpsUndoStack *stack, DataOpsSession *session);

  const DataOpsContext &context() const { return m_ctx; }
  DataOpsUndoStack *stack() const { return m_stack; }

  // ---- 入口守卫（W2）----
  // kind：Catalog=目录写；Sidecar=侧车写（两者都要求非只读工程）。
  enum class Guard
  {
    Catalog,
    Sidecar
  };
  // 失败原因（空 = 通过）。统一走 alive() 三重校验 + refusesWrites。
  QString guardFailure(Guard kind) const;

  // ---- 批量侧车操作（W4/W6）----
  BatchReceipt addTags(const QStringList &assetIds, const QString &tag);
  BatchReceipt removeTags(const QStringList &assetIds, const QString &tag);
  BatchReceipt setTypeOverrides(const QStringList &assetIds, const QString &newType);

  // ---- 挂接/解挂/转移/设主（W3/W6）----
  // 拖放/批量挂接：无链接资产 addLink（真实不可撤销——无 removeLink API，
  // 回执明示）；未决链接挂目标（可撤销）；已决到其他实体转移（可撤销）；
  // 已挂目标实体 → NoChange。
  BatchReceipt attachOrTransfer(const QStringList &assetIds, const QString &entityId,
                                const QString &fallbackRole);
  // 单操作语义（与旧入口一致）：首个选中资产的第一条已决链接。
  BatchReceipt detachLinks(const QStringList &assetIds);
  // 首个可提升的非主关联（前主关联跨资产冻结，供 undo 精确恢复）。
  BatchReceipt setPrimary(const QStringList &assetIds);

  // ---- 重命名（W6）----
  BatchReceipt renameAsset(const QString &assetId, const QString &nextName);

  // ---- 软删/恢复（W4/W6）----
  BatchReceipt softRemove(const QStringList &assetIds, const QString &reason);
  BatchReceipt restoreFromRecycle(const QStringList &assetIds);

  // ---- 实体命令（W6 薄适配面）----
  BatchReceipt createEntity(const CatalogEntity &e);
  BatchReceipt editEntityOverride(const QString &entityId, const EntityOverride &next,
                                  const EntityOverride &prev, bool mergeable);

private:
  // 会话变化判定状态（见 bindSession）：曾绑定标记 + 上一绑定的 cat 对象。
  bool m_everBound = false;
  QPointer<DataCatalog> m_prevCat;
  // 共享骨架：guard → 冻结去重 → 执行（由 caller 组装命令/回执）。
  // 返回 false = 守卫失败（receipt 已含全部请求的 Rejected 项）。
  bool guardBatch(Guard kind, const QString &operation, const QStringList &requests,
                  BatchReceipt *out);
  // 命令入栈并回填回执（push 失败时保留失败证据；无变化不追加历史）。
  void finishWithStack(DataOpCommand *cmd, const QString &op, BatchReceipt *prepare);

  DataOpsContext m_ctx;
  QPointer<DataOpsUndoStack> m_stack;
  QPointer<DataOpsSession> m_session;
};

} // namespace paleo::dataops
