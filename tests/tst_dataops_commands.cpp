// 层：测试壳
// P1（data-commands 2026-11-08）W7：功能层数据命令在真实存储（临时工程目录
// + DataCatalog + .paleo 侧车文件）上的行为回归。全部用例不建任何 QWidget，
// 穿过生产执行入口（DataOpsCommandService / 命令对象 / 共享撤销栈）。
// 故障注入与回执矩阵见 tst_dataops_receipts.cpp。
#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/services/jsonsidecar.h"
#include "../src/workflow/dataopscommandservice.h"

using namespace paleo::dataops;

namespace
{
// 真实工程夹具：开目录 → 两井实体 + 三资产 + 已决/未决链接 + 四个侧车 store。
struct ProjectFixture
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

  int m_case = 0; // 每用例独立工程目录（成员级 QTemporaryDir 复用）

  bool setup()
  {
    ++m_case;
    cat.setLockedReadOnly(false); // 上一用例失败提前返回时锁态不外溢
    QString err;
    const QString sub = dir.filePath(QStringLiteral("case%1").arg(m_case));
    if (!cat.open(sub, &err))
    {
      qWarning("setup: open failed: %s", qPrintable(err));
      return false;
    }
    CatalogEntity w1;
    w1.id = well1;
    w1.entityType = QStringLiteral("well");
    w1.name = QStringLiteral("W1");
    if (!cat.addEntity(w1, &err))
    {
      qWarning("setup: addEntity w1 failed: %s", qPrintable(err));
      return false;
    }
    CatalogEntity w2 = w1;
    w2.id = well2;
    w2.name = QStringLiteral("W2");
    if (!cat.addEntity(w2, &err))
    {
      qWarning("setup: addEntity w2 failed: %s", qPrintable(err));
      return false;
    }
    for (int i = 1; i <= 3; ++i)
    {
      CatalogAsset a;
      a.id = QStringLiteral("ast-%1").arg(i);
      a.type = QStringLiteral("well_log");
      a.format = QStringLiteral("las");
      a.displayName = QStringLiteral("log%1.las").arg(i);
      if (!cat.addAsset(a, &err))
      {
        qWarning("setup: addAsset %s failed: %s", qPrintable(a.id), qPrintable(err));
        return false;
      }
    }
    // a1 → well1 已决；a2 → well2 已决；a3 未决（note 记候选）。
    if (!addResolvedLink(a1, well1) || !addResolvedLink(a2, well2))
      return false;
    EntityAssetLink l3;
    l3.entityType = QStringLiteral("well");
    l3.assetId = a3;
    l3.role = role;
    l3.unresolved = true;
    l3.note = QStringLiteral("W?");
    if (!cat.addLink(l3, nullptr))
      return false;
    rebind();
    return true;
  }

  bool addResolvedLink(const QString &assetId, const QString &entityId)
  {
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = entityId;
    l.assetId = assetId;
    l.role = role;
    return cat.addLink(l, nullptr);
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

  // 独立重开核对：新 DataCatalog + 新 store 实例（执行器与检查器分离）。
  struct Reopened
  {
    std::unique_ptr<DataCatalog> cat;
    TagStore tags;
    AssetOverrideStore typeOv;
    RecycleBin recycle;
  };
  static Reopened reopen(const QString &projectDir)
  {
    Reopened r;
    r.cat = std::make_unique<DataCatalog>();
    r.cat->open(projectDir, nullptr);
    r.tags.load(r.cat.get());
    r.typeOv.load(r.cat.get());
    r.recycle.load(r.cat.get());
    return r;
  }

  // 原始侧车内容（不经 store 的磁盘事实）。
  static QJsonObject rawSidecar(DataCatalog *cat, const char *name)
  {
    return paleo::dataopsSidecarLoad(cat, QString::fromLatin1(name));
  }
};
} // namespace

class TestDataOpsCommands : public QObject
{
  Q_OBJECT
private slots:
  void attachTransferSetPrimaryRenameRoundTrip();
  void transferSecondPhaseFailureLeavesNoPendingState();
  void transferCatalogCommitFailureRollsBackAndSecondOpSucceeds();
  void tagBatchSingleUndoableEntryAndPreciseUndo();
  void tagAlreadyExistsNoHistoryNoRedoClear();
  void setPrimaryUndoRestoresPrevPrimaryAcrossAssets();
  void renameGuardsAndRoundTrip();
  void sessionGuardRejectsStaleCommandsAfterRebind();
  void readonlyGuardRejectsWithoutChanges();
  void softRemoveAndRestoreRoundTripWithReopen();

private:
  int linkCount(const QString &assetId, const QString &entityId)
  {
    int n = 0;
    for (const EntityAssetLink &l : m_f.cat.linksForAsset(assetId))
      if (!l.unresolved && l.entityId == entityId)
        ++n;
    return n;
  }
  ProjectFixture m_f;
};

// 场景矩阵：设置主关联、转移、重命名成功 → 真实目录状态、回执、历史和一次
// 撤销/重做一致；独立重开核验（新实例读盘）。
void TestDataOpsCommands::attachTransferSetPrimaryRenameRoundTrip()
{
  QVERIFY(m_f.setup());
  // a3 未决 → 挂到 well1
  BatchReceipt r1 = m_f.svc.attachOrTransfer({m_f.a3}, m_f.well1, m_f.role);
  QCOMPARE(r1.changedCount(), 1);
  QVERIFY(r1.undoable);
  QCOMPARE(linkCount(m_f.a3, m_f.well1), 1);
  QCOMPARE(m_f.stack.depth(), 1);

  // a1 转移 well1 → well2
  BatchReceipt r2 = m_f.svc.attachOrTransfer({m_f.a1}, m_f.well2, m_f.role);
  QCOMPARE(r2.changedCount(), 1);
  QVERIFY(r2.undoable);
  QCOMPARE(linkCount(m_f.a1, m_f.well2), 1);
  QCOMPARE(linkCount(m_f.a1, m_f.well1), 0);

  // a2 设为主关联：well2·reference 现任主是 a2（a1 转入后为成员——attachLink
  // 不夺主，方向 44 契约）；提升 a2 之前先提升 a1 再撤销，验证前主跨资产恢复。
  bool a2IsPrimaryNow = false;
  for (const EntityAssetLink &l : m_f.cat.linksForEntity(m_f.well2))
    if (l.assetId == m_f.a2 && l.isPrimary && !l.unresolved)
      a2IsPrimaryNow = true;
  QVERIFY(a2IsPrimaryNow); // a2 仍为首任主（转移不夺主）
  BatchReceipt r3 = m_f.svc.setPrimary({m_f.a1}); // 提升 a1（现任主 a2 被冻结为前主）
  QCOMPARE(r3.changedCount(), 1);
  QVERIFY(r3.undoable);
  bool a1Primary = false, a2Primary = false;
  for (const EntityAssetLink &l : m_f.cat.linksForEntity(m_f.well2))
  {
    if (l.assetId == m_f.a1 && l.isPrimary)
      a1Primary = true;
    if (l.assetId == m_f.a2 && l.isPrimary)
      a2Primary = true;
  }
  QVERIFY(a1Primary);
  QVERIFY(!a2Primary); // 同角色至多一条主（§3 不变量）
  // 撤销设主：前主 a2 精确恢复（跨资产）。
  QVERIFY(!m_f.stack.undo().isEmpty());
  a1Primary = false, a2Primary = false;
  for (const EntityAssetLink &l : m_f.cat.linksForEntity(m_f.well2))
  {
    if (l.assetId == m_f.a1 && l.isPrimary)
      a1Primary = true;
    if (l.assetId == m_f.a2 && l.isPrimary)
      a2Primary = true;
  }
  QVERIFY(a2Primary);
  QVERIFY(!a1Primary);
  // 重新执行设主（后续 undo/redo 一致性段落保持 4 条历史的假设改为 3+重做）。
  QVERIFY(!m_f.stack.redo().isEmpty());

  // 重命名 a1
  BatchReceipt r4 = m_f.svc.renameAsset(m_f.a1, QStringLiteral("renamed.las"));
  QCOMPARE(r4.changedCount(), 1);
  QVERIFY(r4.undoable);
  QCOMPARE(m_f.cat.assetById(m_f.a1).displayName, QStringLiteral("renamed.las"));

  QCOMPARE(m_f.stack.depth(), 4); // 四条独立历史（挂接/转移/设主/重命名）
  // 撤销链按 LIFO：先重命名 → 再设主 → 再转移；每次核对真实目录状态。
  QVERIFY(!m_f.stack.undo().isEmpty());
  QCOMPARE(m_f.cat.assetById(m_f.a1).displayName, QStringLiteral("log1.las"));
  QVERIFY(!m_f.stack.undo().isEmpty()); // 设主撤销 → 前主 a2 恢复
  {
    bool a2p = false;
    for (const EntityAssetLink &l : m_f.cat.linksForEntity(m_f.well2))
      if (l.assetId == m_f.a2 && l.isPrimary)
        a2p = true;
    QVERIFY(a2p);
  }
  QVERIFY(!m_f.stack.undo().isEmpty()); // 转移撤销 → a1 回 well1
  QCOMPARE(linkCount(m_f.a1, m_f.well1), 1);
  QCOMPARE(linkCount(m_f.a1, m_f.well2), 0);
  const QString redone = m_f.stack.redo();
  QVERIFY(!redone.isEmpty()); // 重做转移 → a1 再挂 well2
  QCOMPARE(linkCount(m_f.a1, m_f.well2), 1);

  // 独立重开：盘上事实与内存一致（a3→well1、a1→well2、a2 主、改名）。
  {
    auto ro = ProjectFixture::reopen(m_f.projectDir());
    QVERIFY(ro.cat->isOpen());
    int a3w1 = 0, a1w2 = 0;
    bool a1p = false; // 此处断言的是 well2 的现任主（a2）
    for (const EntityAssetLink &l : ro.cat->links())
    {
      if (l.assetId == m_f.a3 && !l.unresolved && l.entityId == m_f.well1)
        ++a3w1;
      if (l.assetId == m_f.a1 && !l.unresolved && l.entityId == m_f.well2)
        ++a1w2;
      if (l.assetId == m_f.a2 && l.isPrimary && !l.unresolved)
        a1p = true;
    }
    QCOMPARE(a3w1, 1);
    QCOMPARE(a1w2, 1);
    QVERIFY(a1p); // 末态：设主已撤销（redo 只重做转移）→ 现任主仍是 a2
    // 重命名在撤销链中被撤销后未重做——盘上为原名（LIFO 语义如实）。
    QCOMPARE(ro.cat->assetById(m_f.a1).displayName, QStringLiteral("log1.las"));
  }
}

// 场景矩阵：转移后一步失败 → 不静默遗留未决中间态。
// 注入方式（夹具记录）：toEntity 为空串是 attachLink 的天然第二阶段拒绝
//（datacatalog_links.cpp「attach needs an entity id」）——第一阶段
// setLinkUnresolved 已执行，事务必须把它滚回。服务层不会产生空 to（守卫
// 拒绝），本用例直接驱动命令对象验证事务边界本身。
void TestDataOpsCommands::transferSecondPhaseFailureLeavesNoPendingState()
{
  QVERIFY(m_f.setup());
  DataOpsContext ctx = m_f.svc.context();
  TransferLinkCmd cmd(ctx, m_f.a1, m_f.role, m_f.well1, QString());
  QVERIFY(!cmd.redo());
  QCOMPARE(cmd.receipt().failureStage, QStringLiteral("transfer-phase2"));
  QCOMPARE(cmd.receipt().count(OpOutcome::FailedRecovered), 1);
  // 关键断言：a1 仍是 well1 的已决链接——没有被留在「已解挂」半途态。
  QCOMPARE(linkCount(m_f.a1, m_f.well1), 1);
  bool anyUnresolvedForA1 = false;
  for (const EntityAssetLink &l : m_f.cat.linksForAsset(m_f.a1))
    if (l.unresolved)
      anyUnresolvedForA1 = true;
  QVERIFY(!anyUnresolvedForA1);
  // 独立重开：盘上同样没有半途态。
  {
    auto ro = ProjectFixture::reopen(m_f.projectDir());
    int resolved = 0;
    for (const EntityAssetLink &l : ro.cat->linksForAsset(m_f.a1))
      if (!l.unresolved && l.entityId == m_f.well1)
        ++resolved;
    QCOMPARE(resolved, 1);
  }
  // 失败后第二次合法操作成功（无残留失败态）。
  BatchReceipt r = m_f.svc.attachOrTransfer({m_f.a1}, m_f.well2, m_f.role);
  QCOMPARE(r.changedCount(), 1);
  QVERIFY(r.undoable);
}

// 场景矩阵：目录拒绝写入 → 源失败原因可追溯；失败后第二次合法操作成功。
// 注入：setLockedReadOnly(true)（save 恒败——既有可控失败点）。两条路径分开钉：
// 服务入口 → 准备守卫整批 Rejected；命令直驱（无服务守卫）→ mutator/BatchSave
// 提交边界失败 → #169 回滚 → FailedRecovered。
void TestDataOpsCommands::transferCatalogCommitFailureRollsBackAndSecondOpSucceeds()
{
  QVERIFY(m_f.setup());
  // 路径 1：服务入口守卫（准备阶段拒绝，零变更）。
  m_f.cat.setLockedReadOnly(true);
  BatchReceipt guarded = m_f.svc.attachOrTransfer({m_f.a1}, m_f.well2, m_f.role);
  QCOMPARE(guarded.count(OpOutcome::Rejected), 1);
  QVERIFY(!guarded.items.first().reason.isEmpty()); // 源原因可追溯
  QCOMPARE(m_f.stack.depth(), 0);
  // 路径 2：命令直驱到提交边界（setLinkUnresolved 内存成功 → flush 落盘失败）。
  DataOpsContext ctx = m_f.svc.context();
  TransferLinkCmd cmd(ctx, m_f.a1, m_f.role, m_f.well1, m_f.well2);
  QVERIFY(!cmd.redo());
  QCOMPARE(cmd.receipt().count(OpOutcome::FailedRecovered), 1);
  // 阶段一 mutator 写拒绝（尚无提交）：catalog-write（与 flush 失败的
  // catalog-commit 区分——两条路径都整批回滚）。
  QCOMPARE(cmd.receipt().failureStage, QStringLiteral("catalog-write"));
  QCOMPARE(linkCount(m_f.a1, m_f.well1), 1); // 内存已滚回（未留半途态）
  QCOMPARE(m_f.stack.depth(), 0);            // 不追加成功历史
  // 独立重开：盘上同样未动。
  {
    auto ro = ProjectFixture::reopen(m_f.projectDir());
    int n = 0;
    for (const EntityAssetLink &l : ro.cat->linksForAsset(m_f.a1))
      if (!l.unresolved && l.entityId == m_f.well1)
        ++n;
    QCOMPARE(n, 1);
  }
  m_f.cat.setLockedReadOnly(false);
  // 失败后第二次合法操作成功。
  BatchReceipt r2 = m_f.svc.attachOrTransfer({m_f.a1}, m_f.well2, m_f.role);
  QCOMPARE(r2.changedCount(), 1);
  QCOMPARE(m_f.stack.depth(), 1);
}

// 场景矩阵：多资产同类标签变更——无入栈前预写；一条可撤销批次；撤销不删
// 除原有标签。单次提交属性由「一条历史 + 原子失败测试（receipts 侧车写
// 失败整批还原）」结构性钉死。
void TestDataOpsCommands::tagBatchSingleUndoableEntryAndPreciseUndo()
{
  QVERIFY(m_f.setup());
  // a1 已有既有标签 keep（构造前态）。
  QVERIFY(m_f.tags.addTag(m_f.a1, QStringLiteral("keep")));
  QVERIFY(m_f.tags.save());
  m_f.rebind();

  BatchReceipt r = m_f.svc.addTags({m_f.a1, m_f.a2, m_f.a3}, QStringLiteral("批A"));
  QCOMPARE(r.changedCount(), 3);
  QVERIFY(r.undoable);
  QCOMPARE(m_f.stack.depth(), 1); // 一条可撤销批次
  // 原始侧车内容：三个资产都在 tags 表里（真实落盘，非内存假象）。
  {
    const QJsonObject root =
        ProjectFixture::rawSidecar(&m_f.cat, "asset_tags.json");
    const QJsonObject tagsObj = root.value(QStringLiteral("tags")).toObject();
    QCOMPARE(tagsObj.size(), 3); // 三个资产各一个键（真实落盘，非内存假象）
    QVERIFY(tagsObj.value(m_f.a1).toArray().toVariantList().contains(
        QVariant(QStringLiteral("批A"))));
    QVERIFY(tagsObj.value(m_f.a2).toArray().toVariantList().contains(
        QVariant(QStringLiteral("批A"))));
  }
  // 撤销：只移除本批加的标签，a1 的既有标签 keep 保留。
  const QString undone = m_f.stack.undo();
  QVERIFY(!undone.isEmpty());
  QVERIFY(!m_f.tags.tagsFor(m_f.a1).contains(QStringLiteral("批A")));
  QVERIFY(m_f.tags.tagsFor(m_f.a1).contains(QStringLiteral("keep")));
  QVERIFY(m_f.tags.tagsFor(m_f.a2).isEmpty());
  // 独立重开核对（执行器与检查器分离）。
  {
    auto ro = ProjectFixture::reopen(m_f.projectDir());
    QVERIFY(ro.tags.tagsFor(m_f.a1).contains(QStringLiteral("keep")));
    QVERIFY(!ro.tags.tagsFor(m_f.a1).contains(QStringLiteral("批A")));
    QVERIFY(ro.tags.tagsFor(m_f.a2).isEmpty());
  }
  // 重做恢复。
  QVERIFY(!m_f.stack.redo().isEmpty());
  QVERIFY(m_f.tags.tagsFor(m_f.a2).contains(QStringLiteral("批A")));
}

// 场景矩阵：已存在标签 → 准确分类 NoChange；无变化不入栈、不清 redo。
void TestDataOpsCommands::tagAlreadyExistsNoHistoryNoRedoClear()
{
  QVERIFY(m_f.setup());
  BatchReceipt first = m_f.svc.addTags({m_f.a1}, QStringLiteral("t1"));
  QCOMPARE(first.changedCount(), 1);
  QVERIFY(!m_f.svc.addTags({m_f.a2}, QStringLiteral("t1")).anyFailure());
  QCOMPARE(m_f.stack.depth(), 2);
  QVERIFY(!m_f.stack.undo().isEmpty()); // 弹出 a2 的批次（LIFO）→ redo 支路 1 条
  QCOMPARE(m_f.stack.redoDepth(), 1);

  BatchReceipt again = m_f.svc.addTags({m_f.a1}, QStringLiteral("t1"));
  QCOMPARE(again.count(OpOutcome::NoChange), 1);
  QCOMPARE(again.changedCount(), 0);
  QVERIFY(!again.undoable);
  QCOMPARE(m_f.stack.depth(), 1);      // 无变化不添加虚假命令（深度不变）
  QCOMPARE(m_f.stack.redoDepth(), 1);  // 无端清空 redo = 红线
  QVERIFY(!m_f.stack.redo().isEmpty()); // redo 仍可用（恢复 a2 的批次）
}

// 场景矩阵：设主撤销后前主关联可正确恢复（跨资产）。
void TestDataOpsCommands::setPrimaryUndoRestoresPrevPrimaryAcrossAssets()
{
  QVERIFY(m_f.setup());
  // 布局：well2·reference 现任主 = a2（把 a1 转走后 a2 为主）。
  QVERIFY(!m_f.svc.attachOrTransfer({m_f.a1}, m_f.well2, m_f.role).anyFailure());
  m_f.stack.clear();
  // 再挂 a3 到 well2（成员）→ 提升 a3 为主。
  QVERIFY(!m_f.svc.attachOrTransfer({m_f.a3}, m_f.well2, m_f.role).anyFailure());
  m_f.stack.clear();
  BatchReceipt promote = m_f.svc.setPrimary({m_f.a3});
  QCOMPARE(promote.changedCount(), 1);
  QVERIFY(!m_f.stack.undo().isEmpty());
  // 撤销后：前主 a2 恢复为主（跨资产恢复），a3 回成员。
  bool a2p = false, a3p = false;
  for (const EntityAssetLink &l : m_f.cat.linksForEntity(m_f.well2))
  {
    if (l.assetId == m_f.a2 && l.isPrimary)
      a2p = true;
    if (l.assetId == m_f.a3 && l.isPrimary)
      a3p = true;
  }
  QVERIFY(a2p);
  QVERIFY(!a3p);
}

// 重命名守卫：同名 NoChange；缺失资产 Rejected；成功往返。
void TestDataOpsCommands::renameGuardsAndRoundTrip()
{
  QVERIFY(m_f.setup());
  const QString prev = m_f.cat.assetById(m_f.a1).displayName;
  BatchReceipt same = m_f.svc.renameAsset(m_f.a1, prev);
  QCOMPARE(same.count(OpOutcome::NoChange), 1);
  QCOMPARE(m_f.stack.depth(), 0);
  BatchReceipt missing = m_f.svc.renameAsset(QStringLiteral("ast-none"), QStringLiteral("x"));
  QCOMPARE(missing.count(OpOutcome::Rejected), 1);
  BatchReceipt empty = m_f.svc.renameAsset(m_f.a1, QStringLiteral("  "));
  QCOMPARE(empty.count(OpOutcome::Rejected), 1);
  BatchReceipt ok = m_f.svc.renameAsset(m_f.a1, QStringLiteral("new-name"));
  QCOMPARE(ok.changedCount(), 1);
  QVERIFY(!m_f.stack.undo().isEmpty());
  QCOMPARE(m_f.cat.assetById(m_f.a1).displayName, prev);
}

// 场景矩阵：同目录重开（新 catalog 对象）→ 旧命令不写入新会话。
// 壳层路径（prepareOpen 每次新建对象）→ 面板把同一 service/stack/session
// 重绑到新对象：bindSession 判定会话变化 → bump 代数 + 清栈；绑定前捕获的
// 旧 ctx 命令在代数校验下拒绝执行。
void TestDataOpsCommands::sessionGuardRejectsStaleCommandsAfterRebind()
{
  QVERIFY(m_f.setup());
  QVERIFY(!m_f.svc.addTags({m_f.a1}, QStringLiteral("s1")).anyFailure());
  QCOMPARE(m_f.stack.depth(), 1);
  const DataOpsContext staleCtx = m_f.svc.context(); // 旧会话上下文（命令侧）

  // 同目录重开 = 新 DataCatalog 对象（壳层 prepareOpen 同构）+ 面板重绑。
  DataCatalog cat2;
  QVERIFY(cat2.open(m_f.projectDir(), nullptr));
  TagStore t2;
  t2.load(&cat2);
  AssetOverrideStore ao2;
  ao2.load(&cat2);
  EntityOverrideStore eo2;
  eo2.load(&cat2);
  RecycleBin rb2;
  rb2.load(&cat2);
  m_f.svc.bindSession(&cat2, &t2, &ao2, &eo2, &rb2, &m_f.stack, &m_f.session);
  QCOMPARE(m_f.stack.depth(), 0); // 旧会话历史整栈作废（不写入新会话）

  // 旧 ctx 的命令在重绑后的世界里执行 → 会话代数校验拒绝（零变更）。
  TagCmd stale(staleCtx, m_f.a1, QStringLiteral("stale"), true);
  QVERIFY(!stale.redo());
  QVERIFY(stale.errorText().contains(QStringLiteral("会话")));
  QVERIFY(t2.tagsFor(m_f.a1).contains(QStringLiteral("s1"))); // 旧会话的合法写入仍在
  QVERIFY(!t2.tagsFor(m_f.a1).contains(QStringLiteral("stale"))); // 旧命令未写入新会话

  // 新会话上第二次合法操作成功。
  BatchReceipt fresh = m_f.svc.addTags({m_f.a1}, QStringLiteral("s2"));
  QCOMPARE(fresh.changedCount(), 1);
  QCOMPARE(m_f.stack.depth(), 1);
}

// 场景矩阵：只读工程 → 功能入口拒绝过期意图（准备阶段整批 Rejected）。
void TestDataOpsCommands::readonlyGuardRejectsWithoutChanges()
{
  QVERIFY(m_f.setup());
  m_f.cat.setLockedReadOnly(true);
  BatchReceipt tags = m_f.svc.addTags({m_f.a1}, QStringLiteral("ro"));
  QCOMPARE(tags.count(OpOutcome::Rejected), 1);
  QVERIFY(!tags.items.first().reason.isEmpty());
  BatchReceipt drop = m_f.svc.attachOrTransfer({m_f.a3}, m_f.well1, m_f.role);
  QCOMPARE(drop.count(OpOutcome::Rejected), 1);
  BatchReceipt del = m_f.svc.softRemove({m_f.a1}, QStringLiteral("r"));
  QCOMPARE(del.count(OpOutcome::Rejected), 1);
  QCOMPARE(m_f.stack.depth(), 0);
  m_f.cat.setLockedReadOnly(false);
  // 解锁后第二次合法操作成功。
  QVERIFY(!m_f.svc.addTags({m_f.a1}, QStringLiteral("ro")).anyFailure());
}

// 软删/恢复往返 + 独立重开核对（不可恢复语义不在本任务改动面）。
void TestDataOpsCommands::softRemoveAndRestoreRoundTripWithReopen()
{
  QVERIFY(m_f.setup());
  BatchReceipt rm = m_f.svc.softRemove({m_f.a1, m_f.a2}, QStringLiteral("批量移除"));
  QCOMPARE(rm.changedCount(), 2);
  QVERIFY(rm.undoable);
  QCOMPARE(m_f.stack.depth(), 1);
  QVERIFY(m_f.recycle.isRemoved(m_f.a1));
  QVERIFY(m_f.recycle.isRemoved(m_f.a2));
  // 独立重开：hidden 集合持久化（finding-125 契约不回归）。
  {
    auto ro = ProjectFixture::reopen(m_f.projectDir());
    QVERIFY(ro.recycle.isRemoved(m_f.a1));
    QVERIFY(ro.recycle.isRemoved(m_f.a2));
  }
  // 重复软删 = NoChange，不添加虚假历史。
  BatchReceipt again = m_f.svc.softRemove({m_f.a1}, QStringLiteral("x"));
  QCOMPARE(again.count(OpOutcome::NoChange), 1);
  QCOMPARE(m_f.stack.depth(), 1);
  // 恢复走服务（可撤销）。
  BatchReceipt rs = m_f.svc.restoreFromRecycle({m_f.a1, m_f.a2});
  QCOMPARE(rs.changedCount(), 2);
  QVERIFY(!m_f.recycle.isRemoved(m_f.a1));
  QVERIFY(!m_f.stack.undo().isEmpty());
  QVERIFY(m_f.recycle.isRemoved(m_f.a1)); // 撤销恢复 = 整批回软删
  QVERIFY(m_f.recycle.isRemoved(m_f.a2));
  {
    auto ro = ProjectFixture::reopen(m_f.projectDir());
    QVERIFY(ro.recycle.isRemoved(m_f.a1));
    QVERIFY(ro.recycle.isRemoved(m_f.a2));
  }
}

QTEST_MAIN(TestDataOpsCommands)
#include "tst_dataops_commands.moc"
