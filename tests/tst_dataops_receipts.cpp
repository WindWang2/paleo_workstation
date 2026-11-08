// 层：测试壳
// P1（data-commands 2026-11-08）W7：可信执行回执 + 故障注入矩阵。
// 每个关键失败场景检查：内存对象、独立重开后的文件状态、回执、栈深与
// history，并验证失败后的第二次合法操作（无残留失败态）。
//
// 注入点登记（夹具如实记录阶段，全部穿过生产执行入口）：
//   · 目录提交失败：DataCatalog::setLockedReadOnly(true)——save 恒败（既有
//     可控失败点；mutator/BatchSave flush 失败 → 单行/整批回滚）。
//   · 侧车写失败：把 .paleo/<name>.json 换成同名目录——QSaveFile 打不开
//     （writeJsonFile 既有失败路径，不依赖 chmod 权限环境）。
//   · 转移第二阶段失败：空实体 id 是 attachLink 的天然拒绝
//     （tst_dataops_commands 侧钉事务边界）。
//   · 宏补偿/补偿失败：最小测试注入 Probe 命令（记录命中阶段与次数）包住
//     真实命令——宏/栈/回执的消费逻辑是生产代码，Probe 只替换单次结果。
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/services/jsonsidecar.h"
#include "../src/workflow/dataopscommandservice.h"

using namespace paleo::dataops;

namespace
{
struct ReceiptFixture
{
  QTemporaryDir dir;
  DataCatalog cat;
  DataOpsUndoStack stack;
  DataOpsSession session;
  TagStore tags;
  AssetOverrideStore typeOv;
  EntityOverrideStore entityOv;
  RecycleBin recycle;
  DataOpsCommandService svc;

  QString well1 = QStringLiteral("well-1"), well2 = QStringLiteral("well-2");
  QString a1 = QStringLiteral("ast-1"), a2 = QStringLiteral("ast-2"),
          a3 = QStringLiteral("ast-3");
  QString role = QStringLiteral("reference");

  int m_case = 0; // 每用例独立工程目录

  bool setup()
  {
    ++m_case;
    cat.setLockedReadOnly(false); // 上一用例失败提前返回时锁态不外溢
    if (!cat.open(dir.filePath(QStringLiteral("case%1").arg(m_case)), nullptr))
      return false;
    CatalogEntity w;
    w.entityType = QStringLiteral("well");
    for (int i = 1; i <= 2; ++i)
    {
      w.id = i == 1 ? well1 : well2;
      w.name = i == 1 ? QStringLiteral("W1") : QStringLiteral("W2");
      if (!cat.addEntity(w, nullptr))
        return false;
    }
    for (int i = 1; i <= 3; ++i)
    {
      CatalogAsset a;
      a.id = QStringLiteral("ast-%1").arg(i);
      a.type = QStringLiteral("well_log");
      a.displayName = QStringLiteral("log%1.las").arg(i);
      if (!cat.addAsset(a, nullptr))
        return false;
    }
    EntityAssetLink l1;
    l1.entityType = QStringLiteral("well");
    l1.entityId = well1;
    l1.assetId = a1;
    l1.role = role;
    if (!cat.addLink(l1, nullptr))
      return false;
    EntityAssetLink l2;
    l2.entityType = QStringLiteral("well");
    l2.entityId = well2;
    l2.assetId = a2;
    l2.role = role;
    if (!cat.addLink(l2, nullptr))
      return false;
    rebind();
    return true;
  }

  QString projectDir() const
  {
    return dir.filePath(QStringLiteral("case%1").arg(m_case));
  }

  void rebind()
  {
    tags.load(&cat);
    typeOv.load(&cat);
    entityOv.load(&cat);
    recycle.load(&cat);
    svc.bindSession(&cat, &tags, &typeOv, &entityOv, &recycle, &stack, &session);
  }

  // 侧车写失败注入：同名目录占位（QSaveFile 无法打开目录）。
  bool blockSidecar(const char *name)
  {
    const QString p = paleo::dataopsSidecarPath(&cat, QString::fromLatin1(name));
    QFile::remove(p);
    QDir().mkpath(QFileInfo(p).absolutePath()); // .paleo 可能尚未创建
    return QDir().mkdir(p);
  }
  void unblockSidecar(const char *name)
  {
    const QString p = paleo::dataopsSidecarPath(&cat, QString::fromLatin1(name));
    QDir(p).removeRecursively();
  }

  QJsonObject rawSidecar(const char *name) const
  {
    return paleo::dataopsSidecarLoad(const_cast<DataCatalog *>(&cat),
                                     QString::fromLatin1(name));
  }
};

// 最小测试注入：包装真实命令，在指定阶段强制失败并记录命中。
// receiptFromInner 保留被包装命令的真实回执（阶段证据）。
class ProbeCommand : public DataOpCommand
{
public:
  enum class Fail
  {
    None,
    Redo,     // redo() 强制失败（storeIntact 由 inner 成功与否决定）
    Undo      // undo() 强制失败（补偿失败注入）
  };
  ProbeCommand(DataOpCommand *inner, Fail fail, int *hits)
    : m_inner(inner), m_fail(fail), m_hits(hits)
  {
  }
  ~ProbeCommand() override { delete m_inner; }
  bool redo() override
  {
    ++(*m_hits);
    m_probeReceipt = BatchReceipt();
    if (m_fail == Fail::Redo)
    {
      m_error = QStringLiteral("probe: redo forced fail");
      m_storeIntact = true; // 未执行任何变更 = 干净失败
      return false;
    }
    const bool ok = m_inner->redo();
    m_error = m_inner->errorText();
    m_storeIntact = m_inner->storeIntact();
    m_probeReceipt = m_inner->receipt();
    return ok;
  }
  bool undo() override
  {
    if (m_fail == Fail::Undo)
    {
      m_error = QStringLiteral("probe: undo forced fail");
      m_storeIntact = false;
      m_probeReceipt = m_inner->receipt();
      return false;
    }
    const bool ok = m_inner->undo();
    m_error = m_inner->errorText();
    m_storeIntact = m_inner->storeIntact();
    m_probeReceipt = m_inner->receipt();
    return ok;
  }
  QString text() const override { return m_inner->text(); }
  const BatchReceipt &receipt() const override { return m_probeReceipt; }

private:
  DataOpCommand *m_inner = nullptr;
  Fail m_fail = Fail::None;
  int *m_hits = nullptr;
  BatchReceipt m_probeReceipt;
};
} // namespace

class TestDataOpsReceipts : public QObject
{
  Q_OBJECT
private slots:
  void receiptCountsReconcileAcrossOutcomes();
  void sidecarWriteFailureRecoversMemoryAndDisclosesStage();
  void sidecarWriteFailureOnUndoInvalidatesHistoryHonestly();
  void catalogBatchCommitFailureMarksWholeSegmentRecovered();
  void dropMixedBatchSeparatesUndoableScope();
  void macroFailureWithSuccessfulCompensationRecovers();
  void macroFailureWithFailingCompensationKeepsBothEvidences();
  void noChangeBatchAddsNeitherHistoryNorFailure();
  void droppedTargetEntityGoneAfterDialog();
  void typeOverrideSidecarFailureRecovers();
  void failureThenSecondLegalOperationSucceeds();

private:
  ReceiptFixture m_f;
};

// 请求数/去重数/各结果集合可解释且相互核对。
void TestDataOpsReceipts::receiptCountsReconcileAcrossOutcomes()
{
  QVERIFY(m_f.setup());
  // a1 已有标签 exist → NoChange；ast-missing 不存在 → Rejected；a2 正常。
  QVERIFY(m_f.tags.addTag(m_f.a1, QStringLiteral("tagX")));
  QVERIFY(m_f.tags.save());
  m_f.rebind();
  BatchReceipt r =
      m_f.svc.addTags({m_f.a1, QStringLiteral("ast-missing"), m_f.a2, m_f.a1},
                      QStringLiteral("tagX"));
  QCOMPARE(r.requested, 4);
  QCOMPARE(r.merged, 1); // a1 重复请求去重
  QCOMPARE(r.changedCount(), 1);         // a2
  QCOMPARE(r.noChangeCount(), 1);        // a1 已存在
  QCOMPARE(r.rejectedCount(), 1);        // ast-missing
  QCOMPARE(r.items.size(), r.requested - r.merged); // 每条去重后请求有去向
  QVERIFY(r.undoable);
}

// 场景矩阵：侧车写失败 → 内存与持久状态重新一致；失败阶段披露；独立重开核验。
void TestDataOpsReceipts::sidecarWriteFailureRecoversMemoryAndDisclosesStage()
{
  QVERIFY(m_f.setup());
  QVERIFY(m_f.blockSidecar("asset_tags.json"));
  BatchReceipt r = m_f.svc.addTags({m_f.a1, m_f.a2}, QStringLiteral("blocked"));
  QCOMPARE(r.count(OpOutcome::FailedRecovered), 2);
  QCOMPARE(r.failureStage, QStringLiteral("sidecar-write"));
  QVERIFY(!r.failureLines().isEmpty());
  QVERIFY(!r.undoable);
  QCOMPARE(m_f.stack.depth(), 0); // 提交失败且全部恢复 → 不产生成功历史
  // 内存回到可信状态：与磁盘（重开的新 store 实例）一致——同一对象不再
  // 同时扮演执行器与检查器。
  {
    TagStore fresh;
    fresh.load(&m_f.cat);
    QVERIFY(fresh.tagsFor(m_f.a1).isEmpty());
    QVERIFY(fresh.tagsFor(m_f.a2).isEmpty());
  }
  QVERIFY(m_f.tags.tagsFor(m_f.a1).isEmpty()); // 执行侧内存也已还原
  m_f.unblockSidecar("asset_tags.json");
  // 失败后第二次合法操作成功。
  BatchReceipt r2 = m_f.svc.addTags({m_f.a1}, QStringLiteral("ok"));
  QCOMPARE(r2.changedCount(), 1);
}

// 场景矩阵：宏执行（这里是单命令 undo）失败且恢复失败 → 原失败与补偿失败
// 均保留；不可信历史失效。
void TestDataOpsReceipts::sidecarWriteFailureOnUndoInvalidatesHistoryHonestly()
{
  QVERIFY(m_f.setup());
  QVERIFY(!m_f.svc.addTags({m_f.a1, m_f.a2}, QStringLiteral("u1")).anyFailure());
  QCOMPARE(m_f.stack.depth(), 1);
  QVERIFY(!m_f.stack.undo().isEmpty()); // 先成功撤销一次（ redo 支路 1 条）
  QVERIFY(!m_f.stack.redo().isEmpty()); // 重做回来
  // 撤销路径的侧车失败：先成功写入，再堵住标签侧车。
  QVERIFY(!m_f.svc.softRemove({m_f.a1}, QStringLiteral("清理")).anyFailure());
  QCOMPARE(m_f.stack.depth(), 2);
  QVERIFY(m_f.blockSidecar("asset_tags.json"));
  // 第一条撤销（软删恢复，recycle 侧车未堵）成功。
  QVERIFY(!m_f.stack.undo().isEmpty());
  QVERIFY(!m_f.recycle.isRemoved(m_f.a1));
  // 第二条撤销（BatchTagCmd::undo → tags 侧车写失败）失败。
  const QString undone = m_f.stack.undo();
  QVERIFY(undone.isEmpty()); // 失败如实返回空
  QVERIFY(!m_f.stack.lastError().isEmpty());
  QVERIFY(m_f.stack.lastReceipt().historyInvalidated);
  QCOMPARE(m_f.stack.depth(), 0); // 不可信历史整栈作废
  QCOMPARE(m_f.stack.redoDepth(), 0);
  // 撤销未生效：执行侧内存仍带 u1（save 失败已回滚内存变更）。
  QVERIFY(m_f.tags.tagsFor(m_f.a1).contains(QStringLiteral("u1")));
  QVERIFY(m_f.tags.tagsFor(m_f.a2).contains(QStringLiteral("u1")));
  m_f.unblockSidecar("asset_tags.json");
  // 堵塞注入本身销毁了盘上文件（QFile::remove 后以目录占位）：解除后经一次
  // 合法操作重新落盘，内存（含未生效撤销所保留的 u1）与盘恢复一致。
  QVERIFY(!m_f.svc.addTags({m_f.a1}, QStringLiteral("after")).anyFailure());
  {
    TagStore fresh;
    fresh.load(&m_f.cat);
    QVERIFY(fresh.tagsFor(m_f.a1).contains(QStringLiteral("u1")));
    QVERIFY(fresh.tagsFor(m_f.a2).contains(QStringLiteral("u1")));
    QVERIFY(fresh.tagsFor(m_f.a1).contains(QStringLiteral("after")));
  }
}

// 场景矩阵：目录提交失败（注入：setLockedReadOnly 使 save 恒败——命令/
// 宏不经服务守卫直驱，穿过真实 mutator 与 BatchSave flush）。#169 把内存滚
// 回批次前 → 回执 FailedRecovered + catalog-commit；服务层入口下的同注入
// 在准备阶段整批 Rejected（guard）——两条路径分开钉。
void TestDataOpsReceipts::catalogBatchCommitFailureMarksWholeSegmentRecovered()
{
  QVERIFY(m_f.setup());
  // 目录族宏（真实 children）：挂未决 a3 + 转移 a1，一个提交边界。
  EntityAssetLink l3;
  l3.entityType = QStringLiteral("well");
  l3.entityId = m_f.well1;
  l3.assetId = m_f.a3;
  l3.role = m_f.role;
  l3.unresolved = true;
  QVERIFY(m_f.cat.addLink(l3, nullptr)); // 只读前布景
  m_f.rebind(); // 布景后重绑（会话上下文一致）
  m_f.cat.setLockedReadOnly(true);
  QList<DataOpCommand *> children;
  children.append(new AttachLinkCmd(m_f.svc.context(), m_f.a3, m_f.role, m_f.well2,
                                    QStringLiteral("well")));
  children.append(new TransferLinkCmd(m_f.svc.context(), m_f.a1, m_f.role, m_f.well1,
                                      m_f.well2));
  auto *macro = new DataOpMacro(QStringLiteral("提交失败宏"), children);
  macro->setBatchCatalog(&m_f.cat);
  QVERIFY(!m_f.stack.push(macro));
  const BatchReceipt &r = m_f.stack.lastReceipt();
  // locked 注入 = save() 在批次挂起前拒绝：首个 child 的 mutator 即失败
  //（attachLink 单行回滚），回执如实记 1 条 FailedRecovered，宏整批 abort。
  QCOMPARE(r.count(OpOutcome::FailedRecovered), 1);
  QCOMPARE(r.failureStage, QStringLiteral("catalog-abort"));
  QVERIFY(!r.undoable);
  QCOMPARE(m_f.stack.depth(), 0);
  QCOMPARE(m_f.stack.redoDepth(), 0);
  // 目录未被写入：a3 的未决链接原样保留（未被挂到 well2）、a1 仍在 well1。
  bool a3Unresolved = false;
  for (const EntityAssetLink &l : m_f.cat.linksForAsset(m_f.a3))
    a3Unresolved = l.unresolved && l.entityId == m_f.well1; // 未决候选原样保留
  QVERIFY(a3Unresolved);
  int a1w1 = 0;
  for (const EntityAssetLink &l : m_f.cat.linksForAsset(m_f.a1))
    if (!l.unresolved && l.entityId == m_f.well1)
      ++a1w1;
  QCOMPARE(a1w1, 1);
  // 独立重开核验。
  {
    DataCatalog ro;
    QVERIFY(ro.open(m_f.projectDir(), nullptr));
    bool a3u = false;
    for (const EntityAssetLink &l : ro.linksForAsset(m_f.a3))
      a3u = l.unresolved && l.entityId == m_f.well1;
    QVERIFY(a3u);
  }
  m_f.cat.setLockedReadOnly(false); // 解锁（后续用例不被锁态外溢拖累）
}

// 场景矩阵：无链接 addLink 与可撤销转移混合 → 成功/失败/跳过与可撤销范围
// 分别说明，撤销只回滚可撤销部分。
void TestDataOpsReceipts::dropMixedBatchSeparatesUndoableScope()
{
  QVERIFY(m_f.setup());
  // a3 无链接（addLink，不可撤销）；a1 已决 well1（转移，可撤销）；
  // a2 已决 well2（已挂目标 → 跳过）。
  BatchReceipt r = m_f.svc.attachOrTransfer({m_f.a3, m_f.a1, m_f.a2}, m_f.well2,
                                            m_f.role);
  QCOMPARE(r.changedCount(), 2);
  bool addlinkChangedUndoable = true, transferChangedUndoable = false;
  for (const ReceiptItem &it : r.items)
  {
    if (it.op == QStringLiteral("addlink") && it.outcome == OpOutcome::Changed)
      addlinkChangedUndoable = it.undoable;
    if (it.op == QStringLiteral("transfer") && it.outcome == OpOutcome::Changed)
      transferChangedUndoable = it.undoable;
    if (it.op == QStringLiteral("attach") && it.outcome == OpOutcome::NoChange)
      QVERIFY(!it.undoable);
  }
  QCOMPARE(addlinkChangedUndoable, false); // addLink 明示不可撤销
  QVERIFY(transferChangedUndoable);        // 转移在可撤销范围
  QVERIFY(r.undoable);                     // 批次含可撤销部分
  QCOMPARE(m_f.stack.depth(), 1);
  // 撤销一条历史：只有转移回滚；addLink 留存（真实不可撤销语义）。
  QVERIFY(!m_f.stack.undo().isEmpty());
  int a1w1 = 0, a1w2 = 0, a3w2 = 0;
  for (const EntityAssetLink &l : m_f.cat.links())
  {
    if (l.assetId == m_f.a1 && !l.unresolved && l.entityId == m_f.well1)
      ++a1w1;
    if (l.assetId == m_f.a1 && !l.unresolved && l.entityId == m_f.well2)
      ++a1w2;
    if (l.assetId == m_f.a3 && !l.unresolved && l.entityId == m_f.well2)
      ++a3w2;
  }
  QCOMPARE(a1w1, 1);
  QCOMPARE(a1w2, 0);
  QCOMPARE(a3w2, 1); // addLink 不随撤销消失
  // 独立重开核验。
  {
    DataCatalog ro;
    QVERIFY(ro.open(m_f.projectDir(), nullptr));
    int a3w2ro = 0;
    for (const EntityAssetLink &l : ro.links())
      if (l.assetId == m_f.a3 && !l.unresolved && l.entityId == m_f.well2)
        ++a3w2ro;
    QCOMPARE(a3w2ro, 1);
  }
}

// 混合宏失败 + 补偿成功 → FailedRecovered（全部恢复，历史不失效）。
// 注入登记：ProbeCommand::Redo 在第二个 child 强制失败（命中次数=2——
// 宏的顺序执行穿过 child1 与 child2）；child1 是真实 BatchTagCmd，补偿
// undo 走生产路径。
void TestDataOpsReceipts::macroFailureWithSuccessfulCompensationRecovers()
{
  QVERIFY(m_f.setup());
  int probeHits = 0;
  QList<DataOpCommand *> children;
  children.append(new BatchTagCmd(m_f.svc.context(), QStringLiteral("m1"), true,
                                  QStringList{m_f.a1}));
  children.append(new ProbeCommand(
      new TagCmd(m_f.svc.context(), m_f.a2, QStringLiteral("m2"), true),
      ProbeCommand::Fail::Redo, &probeHits));
  DataOpMacro macro(QStringLiteral("混合宏"), children);
  QVERIFY(!macro.redo());
  QCOMPARE(probeHits, 1); // 第二 child 命中注入（第一 child 是真实命令）
  const BatchReceipt &r = macro.receipt();
  QCOMPARE(r.count(OpOutcome::FailedRecovered), 1); // child1 已补偿恢复
  QVERIFY(r.failureStage == QStringLiteral("compensated"));
  QVERIFY(macro.storeIntact()); // 补偿成功 → 可信
  // 真实存储：child1 的标签被补偿撤销（内存与盘一致）。
  QVERIFY(m_f.tags.tagsFor(m_f.a1).isEmpty());
  {
    TagStore fresh;
    fresh.load(&m_f.cat);
    QVERIFY(fresh.tagsFor(m_f.a1).isEmpty());
  }
}

// 场景矩阵：宏执行失败且补偿也失败 → 原失败、补偿失败均保留；不能报
//「整批未变化」；不可信历史失效（storeIntact=false → 栈整栈作废）。
// 注入登记：ProbeCommand::Undo 使第一个 child 的补偿 undo 强制失败
//（命中阶段=补偿；child1 的真实变更留在存储里）。
void TestDataOpsReceipts::macroFailureWithFailingCompensationKeepsBothEvidences()
{
  QVERIFY(m_f.setup());
  const QJsonObject before = m_f.rawSidecar("asset_tags.json");
  int probeHits = 0;
  QList<DataOpCommand *> children;
  children.append(new ProbeCommand(
      new TagCmd(m_f.svc.context(), m_f.a1, QStringLiteral("partial"), true),
      ProbeCommand::Fail::Undo, &probeHits));
  children.append(new ProbeCommand(
      new TagCmd(m_f.svc.context(), m_f.a2, QStringLiteral("m2"), true),
      ProbeCommand::Fail::Redo, &probeHits));
  DataOpMacro macro(QStringLiteral("补偿失败宏"), children);
  QVERIFY(!macro.redo());
  QCOMPARE(probeHits, 2); // child2 redo 失败 + child1 补偿 undo 失败（双命中）
  const BatchReceipt &r = macro.receipt();
  QCOMPARE(r.count(OpOutcome::FailedPartial), 1); // child1 留有部分变化
  QVERIFY(r.failureStage == QStringLiteral("compensation"));
  QVERIFY(!macro.storeIntact());
  // 双证据：原失败 + 补偿失败都在错误文案里。
  QVERIFY(macro.errorText().contains(QStringLiteral("补偿失败")));
  // 部分变化真实存在：a1 的标签已落盘（不能报「整批未变化」）。
  {
    TagStore fresh;
    fresh.load(&m_f.cat);
    QVERIFY(fresh.tagsFor(m_f.a1).contains(QStringLiteral("partial")));
  }
  const QJsonObject after = m_f.rawSidecar("asset_tags.json");
  QVERIFY(after != before);
  // 栈消费：push 失败宏（补偿失败 → storeIntact=false）→ 两支栈整栈作废。
  QList<DataOpCommand *> children2;
  children2.append(new ProbeCommand(
      new TagCmd(m_f.svc.context(), m_f.a1, QStringLiteral("partial2"), true),
      ProbeCommand::Fail::Undo, &probeHits));
  children2.append(new ProbeCommand(
      new TagCmd(m_f.svc.context(), m_f.a2, QStringLiteral("later2"), true),
      ProbeCommand::Fail::Redo, &probeHits));
  auto *macro2 = new DataOpMacro(QStringLiteral("补偿失败宏2"), children2);
  // 先放一条干净历史，再 push 失败宏 → 历史必须被作废。
  QVERIFY(m_f.stack.push(new TagCmd(m_f.svc.context(), m_f.a1,
                                    QStringLiteral("clean"), true)));
  QCOMPARE(m_f.stack.depth(), 1);
  QVERIFY(!m_f.stack.push(macro2));
  QCOMPARE(m_f.stack.depth(), 0);
  QVERIFY(m_f.stack.lastReceipt().historyInvalidated);
}

// 全无变化批次：不失败、不追加历史、不清 redo。
void TestDataOpsReceipts::noChangeBatchAddsNeitherHistoryNorFailure()
{
  QVERIFY(m_f.setup());
  // a2 预置既有标签 seed（幂等命中的来源）。
  QVERIFY(m_f.tags.addTag(m_f.a2, QStringLiteral("seed")));
  QVERIFY(m_f.tags.save());
  m_f.rebind();
  QVERIFY(!m_f.svc.addTags({m_f.a1}, QStringLiteral("n1")).anyFailure());
  QVERIFY(!m_f.stack.undo().isEmpty());
  QCOMPARE(m_f.stack.redoDepth(), 1);
  BatchReceipt r = m_f.svc.addTags({m_f.a2}, QStringLiteral("seed"));
  QVERIFY(r.allNoChange());
  QCOMPARE(m_f.stack.depth(), 0);
  QCOMPARE(m_f.stack.redoDepth(), 1); // 无端清空 redo = 红线
}

// 模态对话框返回后目标实体已不在 → 冻结目标重新验证，整批拒绝（零变更）。
void TestDataOpsReceipts::droppedTargetEntityGoneAfterDialog()
{
  QVERIFY(m_f.setup());
  // 目标 well2 在对话框后被（真实路径）删除：catalog 无 removeEntity——
  // 用软删语境之外的直接场景：传入不存在实体 id（等价于「对话框后被删」）。
  BatchReceipt r =
      m_f.svc.attachOrTransfer({m_f.a1}, QStringLiteral("well-gone"), m_f.role);
  QCOMPARE(r.count(OpOutcome::Rejected), 1);
  QVERIFY(r.items.first().reason.contains(QStringLiteral("目标实体")));
  QCOMPARE(m_f.stack.depth(), 0);
  int links = 0;
  for (const EntityAssetLink &l : m_f.cat.linksForAsset(m_f.a1))
    if (!l.unresolved)
      ++links;
  QCOMPARE(links, 1); // 原挂接未动
}

// 类型改写侧车失败 → 内存恢复 + 阶段披露（W4 同族核对）。
void TestDataOpsReceipts::typeOverrideSidecarFailureRecovers()
{
  QVERIFY(m_f.setup());
  QVERIFY(m_f.blockSidecar("asset_overrides.json"));
  BatchReceipt r = m_f.svc.setTypeOverrides({m_f.a1, m_f.a2}, QStringLiteral("tops"));
  QCOMPARE(r.count(OpOutcome::FailedRecovered), 2);
  QCOMPARE(r.failureStage, QStringLiteral("sidecar-write"));
  QVERIFY(m_f.typeOv.overriddenType(m_f.a1).isEmpty());
  {
    AssetOverrideStore fresh;
    fresh.load(&m_f.cat);
    QVERIFY(fresh.overriddenType(m_f.a1).isEmpty());
  }
  m_f.unblockSidecar("asset_overrides.json");
  BatchReceipt r2 = m_f.svc.setTypeOverrides({m_f.a1}, QStringLiteral("tops"));
  QCOMPARE(r2.changedCount(), 1);
  QVERIFY(!m_f.stack.undo().isEmpty());
  QVERIFY(m_f.typeOv.overriddenType(m_f.a1).isEmpty());
}

// 通用：失败后第二次合法操作成功（残留失败态会让界面永久不可用 = 红）。
void TestDataOpsReceipts::failureThenSecondLegalOperationSucceeds()
{
  QVERIFY(m_f.setup());
  m_f.cat.setLockedReadOnly(true);
  QCOMPARE(m_f.svc.addTags({m_f.a1}, QStringLiteral("x")).rejectedCount(), 1);
  QCOMPARE(m_f.svc.attachOrTransfer({m_f.a1}, m_f.well2, m_f.role).rejectedCount(), 1);
  QCOMPARE(m_f.svc.softRemove({m_f.a1}, QStringLiteral("r")).rejectedCount(), 1);
  QCOMPARE(m_f.svc.renameAsset(m_f.a1, QStringLiteral("n")).rejectedCount(), 1);
  m_f.cat.setLockedReadOnly(false);
  // 全部入口恢复可用。
  QVERIFY(!m_f.svc.addTags({m_f.a1}, QStringLiteral("x")).anyFailure());
  QVERIFY(!m_f.svc.renameAsset(m_f.a1, QStringLiteral("n")).anyFailure());
  QVERIFY(!m_f.svc.attachOrTransfer({m_f.a1}, m_f.well2, m_f.role).anyFailure());
}

QTEST_MAIN(TestDataOpsReceipts)
#include "tst_dataops_receipts.moc"
