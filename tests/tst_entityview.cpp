#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>

#include "../src/catalog/datacatalog.h"
#include "../src/catalog/entityview.h"

// B 包（docs/DATA_FABRIC_ADOPTION.md）：EntityAssetLink.ordinal 落盘 +
// 同 (entity,role) ordinal 排序、entityDataView 角色槽视图、
// downstreamClosure 下游闭包、staleness-lite（supersede / sha 失配标记）。
class TestEntityView : public QObject
{
  Q_OBJECT

private slots:
  void ordinalRoundTripsAndOrdersMembers();
  void legacyLinkWithoutOrdinalReadsZero();
  void slotsEnumerateRegistryRolesIncludingEmpty();
  void unresolvedLinksLandInSlotUnresolved();
  void derivedProductsTraceParentVersionIds();
  void missingSourcesDetectDanglingParents();
  void downstreamClosureReturnsTransitiveSet();
  void staleMarksOnParentSupersede();
  void markDownstreamStaleApi();
  void unknownEntityYieldsEmptyView();
};

namespace
{
  CatalogEntity makeWell(const QString &id, const QString &name)
  {
    CatalogEntity e;
    e.id = id;
    e.entityType = QStringLiteral("well");
    e.name = name;
    return e;
  }

  CatalogAsset makeAsset(const QString &id, const QString &type = QStringLiteral("well_log"))
  {
    CatalogAsset a;
    a.id = id;
    a.type = type;
    a.format = QStringLiteral("dat");
    a.displayName = id + QStringLiteral(".dat");
    return a;
  }

  CatalogVersion makeVersion(const QString &id, const QString &assetId, int number,
                             const QString &stage = QStringLiteral("RAW"),
                             const QStringList &parents = {})
  {
    CatalogVersion v;
    v.id = id;
    v.assetId = assetId;
    v.stage = stage;
    v.versionNumber = number;
    v.parentVersionIds = parents;
    return v;
  }

  EntityAssetLink makeLink(const QString &entityId, const QString &assetId,
                           const QString &role, int ordinal = 0,
                           bool primary = true, bool unresolved = false)
  {
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = entityId;
    l.assetId = assetId;
    l.role = role;
    l.isPrimary = primary;
    l.unresolved = unresolved;
    l.ordinal = ordinal;
    return l;
  }

  const RoleSlot *slotFor(const EntityView &view, const QString &role)
  {
    for (const RoleSlot &s : view.roleSlots)
      if (s.def.role == role)
        return &s;
    return nullptr;
  }

  QStringList versionIds(const QVector<CatalogVersion> &versions)
  {
    QStringList ids;
    for (const CatalogVersion &v : versions)
      ids.append(v.id);
    return ids;
  }
} // namespace

// ordinal：catalog.json 恒写、旧文档缺省读 0、linksForEntity 同角色成员按
// ordinal 升序（全 0 旧数据保持入库序）。
void TestEntityView::ordinalRoundTripsAndOrdersMembers()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A1"), QStringLiteral("A1"))));
    for (const QString &id : {QStringLiteral("ast-1"), QStringLiteral("ast-2"),
                              QStringLiteral("ast-3"), QStringLiteral("ast-4")})
      QVERIFY(cat.addAsset(makeAsset(id)));

    // 乱序 ordinal 入库：同角色成员须按 ordinal 而非入库序展示。
    QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-1"),
                                 QStringLiteral("well_log"), 0, true)));
    QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-2"),
                                 QStringLiteral("well_log"), 5, false)));
    QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-3"),
                                 QStringLiteral("well_log"), 1, false)));
    // 不同角色的链接按 ordinal 一起稳定排序（ordinal 相同保持入库序）。
    QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-4"),
                                 QStringLiteral("tops"), 3, true)));
  }

  // catalog.json：每条链接恒写 "ordinal" 键。
  QFile f(dir.filePath(QStringLiteral("artifacts/metadata/catalog.json")));
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QJsonArray links =
      QJsonDocument::fromJson(f.readAll())
          .object()
          .value(QStringLiteral("entity_asset_links"))
          .toArray();
  f.close();
  QCOMPARE(links.size(), 4);
  for (const auto &v : links)
    QVERIFY(v.toObject().contains(QStringLiteral("ordinal")));

  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  const auto entityLinks = reloaded.linksForEntity(QStringLiteral("well-A1"));
  QCOMPARE(entityLinks.size(), 4);
  // 全局 ordinal 升序：0,1,3,5。
  QCOMPARE(entityLinks.at(0).assetId, QStringLiteral("ast-1"));
  QCOMPARE(entityLinks.at(1).assetId, QStringLiteral("ast-3"));
  QCOMPARE(entityLinks.at(2).assetId, QStringLiteral("ast-4"));
  QCOMPARE(entityLinks.at(3).assetId, QStringLiteral("ast-2"));
  QCOMPARE(entityLinks.at(3).ordinal, 5);

  // 实体视图成员桶继承同一 ordinal 序（primary 单独成桶）。
  const EntityView view = entityDataView(reloaded, QStringLiteral("well-A1"));
  const RoleSlot *log = slotFor(view, QStringLiteral("well_log"));
  QVERIFY(log != nullptr);
  QCOMPARE(log->primary.assetId, QStringLiteral("ast-1"));
  QCOMPARE(log->members.size(), 2);
  QCOMPARE(log->members.at(0).assetId, QStringLiteral("ast-3"));
  QCOMPARE(log->members.at(1).assetId, QStringLiteral("ast-2"));
}

// 旧 catalog 的链接没有 "ordinal" 键 → 读 0（前向兼容，与 note 键同一约定）。
void TestEntityView::legacyLinkWithoutOrdinalReadsZero()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("artifacts/metadata"))));
  QFile f(dir.filePath(QStringLiteral("artifacts/metadata/catalog.json")));
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArrayLiteral(
      "{\"schema_version\":1,\"catalog_revision\":1,"
      "\"entities\":[{\"id\":\"well-A1\",\"entity_type\":\"well\",\"name\":\"A1\"}],"
      "\"entity_asset_links\":[{\"entity_type\":\"well\",\"entity_id\":\"well-A1\","
      "\"asset_id\":\"ast-1\",\"role\":\"well_log\",\"is_primary\":true}]}"));
  f.close();
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  const auto links = cat.linksForEntity(QStringLiteral("well-A1"));
  QCOMPARE(links.size(), 1);
  QCOMPARE(links.front().ordinal, 0);
}

// 词表驱动：well 实体的视图展开全部 9 个内置角色槽——空角色也占位，
// 词表序保持（well_head 打头、other 收尾）。
void TestEntityView::slotsEnumerateRegistryRolesIncludingEmpty()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A1"), QStringLiteral("A1"))));

  const EntityView view = entityDataView(cat, QStringLiteral("well-A1"));
  QCOMPARE(view.entity.id, QStringLiteral("well-A1"));
  QCOMPARE(view.roleSlots.size(), 9);
  QCOMPARE(view.roleSlots.first().def.role, QStringLiteral("well_head"));
  QCOMPARE(view.roleSlots.last().def.role, QStringLiteral("other"));
  for (const RoleSlot &s : view.roleSlots)
  {
    QVERIFY(s.primary.assetId.isEmpty());
    QVERIFY(s.members.isEmpty());
    QVERIFY(s.unresolved.isEmpty());
  }

  // 无词表的实体类型：视图非空但槽为空（如实——词表没有可枚举的角色）。
  QVERIFY(cat.addEntity(CatalogEntity{QStringLiteral("aux-1"),
                                      QStringLiteral("auxiliary"),
                                      QStringLiteral("扫描图")}));
  const EntityView auxView = entityDataView(cat, QStringLiteral("aux-1"));
  QCOMPARE(auxView.entity.id, QStringLiteral("aux-1"));
  QVERIFY(auxView.roleSlots.isEmpty());
}

// 未决链接按 entityId 归属落槽（上游约定：歧义链接携带候选实体 id）；
// entityId 为空的未决链接不属于任何实体，不凭空出现在槽里。
void TestEntityView::unresolvedLinksLandInSlotUnresolved()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A1"), QStringLiteral("A1"))));
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A2"), QStringLiteral("A2"))));
  for (const QString &id : {QStringLiteral("ast-1"), QStringLiteral("ast-2"),
                            QStringLiteral("ast-3")})
    QVERIFY(cat.addAsset(makeAsset(id)));

  // 双候选歧义：同一资产对两口井各落一条 entityId 未决链接（上游语义）。
  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-1"),
                               QStringLiteral("well_log"), 0, false, true)));
  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A2"), QStringLiteral("ast-1"),
                               QStringLiteral("well_log"), 0, false, true)));
  // 全局未决：零匹配，实体留空——不进任何实体的槽。
  QVERIFY(cat.addLink(makeLink(QString(), QStringLiteral("ast-2"),
                               QStringLiteral("well_log"), 0, false, true)));
  // 已决成员对照。
  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-3"),
                               QStringLiteral("well_log"), 1, true)));

  const EntityView view = entityDataView(cat, QStringLiteral("well-A1"));
  const RoleSlot *log = slotFor(view, QStringLiteral("well_log"));
  QVERIFY(log != nullptr);
  QCOMPARE(log->unresolved.size(), 1);
  QCOMPARE(log->unresolved.front().assetId, QStringLiteral("ast-1"));
  QVERIFY(log->unresolved.front().unresolved);
  QCOMPARE(log->primary.assetId, QStringLiteral("ast-3"));
  QVERIFY(log->members.isEmpty());

  // 未决资产不作为本实体数据源——不参与 derivedProducts/missingSources 的种子。
  QVERIFY(view.derivedProducts.isEmpty());
  QVERIFY(view.missingSources.isEmpty());
}

// derivedProducts：实体已决资产版本为种子的下游闭包 ∩ DERIVED——
// 多跳穿透 INTERMEDIATE；未挂接/其他实体的 DERIVED 不混入。
void TestEntityView::derivedProductsTraceParentVersionIds()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A1"), QStringLiteral("A1"))));
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-B1"), QStringLiteral("B1"))));
  for (const QString &id : {QStringLiteral("ast-raw"), QStringLiteral("ast-d1"),
                            QStringLiteral("ast-d2"), QStringLiteral("ast-i"),
                            QStringLiteral("ast-b"), QStringLiteral("ast-x")})
    QVERIFY(cat.addAsset(makeAsset(id, QStringLiteral("generic"))));

  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-raw"),
                               QStringLiteral("well_log"))));
  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-B1"), QStringLiteral("ast-b"),
                               QStringLiteral("well_log"))));

  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-r1"),
                                     QStringLiteral("ast-raw"), 1)));
  // r1 → i1（INTERMEDIATE，穿透不收录）→ d2；r1 → d1。
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-i1"),
                                     QStringLiteral("ast-i"), 1,
                                     QStringLiteral("INTERMEDIATE"),
                                     {QStringLiteral("ver-r1")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d1"),
                                     QStringLiteral("ast-d1"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-r1")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d2"),
                                     QStringLiteral("ast-d2"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-i1")})));
  // 别的井的下游不混入。
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-b1"),
                                     QStringLiteral("ast-b"), 1)));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-x1"),
                                     QStringLiteral("ast-x"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-b1")})));

  const EntityView view = entityDataView(cat, QStringLiteral("well-A1"));
  const QStringList derived = versionIds(view.derivedProducts);
  QCOMPARE(derived.size(), 2);
  QVERIFY(derived.contains(QStringLiteral("ver-d1")));
  QVERIFY(derived.contains(QStringLiteral("ver-d2")));
  QVERIFY(!derived.contains(QStringLiteral("ver-i1"))); // 非 DERIVED 不收
  QVERIFY(!derived.contains(QStringLiteral("ver-x1"))); // 别的实体的下游
  QVERIFY(view.missingSources.isEmpty());
}

// missingSources：视图内版本（实体资产版本 ∪ 下游闭包）的 parentVersionIds
// 指向不存在版本 → 悬空引用如实列出、去重、有序。
void TestEntityView::missingSourcesDetectDanglingParents()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A1"), QStringLiteral("A1"))));
  for (const QString &id : {QStringLiteral("ast-raw"), QStringLiteral("ast-d"),
                            QStringLiteral("ast-g")})
    QVERIFY(cat.addAsset(makeAsset(id, QStringLiteral("generic"))));
  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-raw"),
                               QStringLiteral("well_log"))));
  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-g"),
                               QStringLiteral("interpretation"))));

  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-r1"),
                                     QStringLiteral("ast-raw"), 1)));
  // 下游产物引用一个存在 + 一个不存在（同一悬空 id 出现两次 → 去重）。
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d1"),
                                     QStringLiteral("ast-d"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-r1"),
                                      QStringLiteral("ver-ghost")})));
  // 实体自身资产的版本直接引用悬空父版本也被发现（实体内子集口径）。
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-g1"),
                                     QStringLiteral("ast-g"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-ghost"),
                                      QStringLiteral("ver-missing")})));

  const EntityView view = entityDataView(cat, QStringLiteral("well-A1"));
  QCOMPARE(view.missingSources,
           QStringList({QStringLiteral("ver-ghost"), QStringLiteral("ver-missing")}));
}

// downstreamClosure：parentVersionIds 反查的传递闭包——多父去重、环安全、
// 种子自身不入结果。
void TestEntityView::downstreamClosureReturnsTransitiveSet()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  for (const QString &id : {QStringLiteral("ast-1"), QStringLiteral("ast-2"),
                            QStringLiteral("ast-3"), QStringLiteral("ast-4")})
    QVERIFY(cat.addAsset(makeAsset(id, QStringLiteral("generic"))));

  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-a"),
                                     QStringLiteral("ast-1"), 1)));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-b"),
                                     QStringLiteral("ast-2"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-a")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-c"),
                                     QStringLiteral("ast-3"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-b"),
                                      QStringLiteral("ver-a")}))); // 多父
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-x"),
                                     QStringLiteral("ast-4"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-a")})));

  const QStringList closure = versionIds(cat.downstreamClosure(QStringLiteral("ver-a")));
  QCOMPARE(closure.size(), 3);
  QVERIFY(closure.contains(QStringLiteral("ver-b")));
  QVERIFY(closure.contains(QStringLiteral("ver-c")));
  QVERIFY(closure.contains(QStringLiteral("ver-x")));
  QVERIFY(!closure.contains(QStringLiteral("ver-a"))); // 种子不入结果
  // BFS：ver-c 经 b 与 a 两条路可达，只出现一次。
  QCOMPARE(closure.count(QStringLiteral("ver-c")), 1);

  // 环：p ↔ q 互相引用——遍历终止且种子不被当成自己的下游。
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-p"),
                                     QStringLiteral("ast-1"), 2,
                                     QStringLiteral("RAW"),
                                     {QStringLiteral("ver-q")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-q"),
                                     QStringLiteral("ast-2"), 2,
                                     QStringLiteral("RAW"),
                                     {QStringLiteral("ver-p")})));
  QCOMPARE(versionIds(cat.downstreamClosure(QStringLiteral("ver-p"))),
           QStringList{QStringLiteral("ver-q")});

  QVERIFY(cat.downstreamClosure(QString()).isEmpty());
  QVERIFY(cat.downstreamClosure(QStringLiteral("ver-nope")).isEmpty());
}

// staleness-lite：addVersion 入库一个更高的 versionNumber ⇒ 同资产旧版本
// 被取代，其下游闭包里的 DERIVED 版本记 extra["stale"]+["staleReason"]，
// 与 addVersion 同一原子写落盘。
void TestEntityView::staleMarksOnParentSupersede()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  for (const QString &id : {QStringLiteral("ast-raw"), QStringLiteral("ast-d1"),
                            QStringLiteral("ast-d2"), QStringLiteral("ast-i")})
    QVERIFY(cat.addAsset(makeAsset(id, QStringLiteral("generic"))));

  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-r1"),
                                     QStringLiteral("ast-raw"), 1)));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d1"),
                                     QStringLiteral("ast-d1"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-r1")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d2"),
                                     QStringLiteral("ast-d2"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-d1")})));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-i1"),
                                     QStringLiteral("ast-i"), 1,
                                     QStringLiteral("INTERMEDIATE"),
                                     {QStringLiteral("ver-r1")})));

  // 同资产更高 versionNumber → ver-r1 被取代 → 下游 DERIVED（含多跳 d2）标 stale。
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-r2"),
                                     QStringLiteral("ast-raw"), 2)));
  const CatalogVersion d1 = cat.versionById(QStringLiteral("ver-d1"));
  QVERIFY(d1.extra.value(QStringLiteral("stale")).toBool());
  QVERIFY(!d1.extra.value(QStringLiteral("staleReason")).toString().isEmpty());
  QVERIFY(d1.extra.value(QStringLiteral("staleReason")).toString()
              .contains(QStringLiteral("ver-r2")));
  QVERIFY(cat.versionById(QStringLiteral("ver-d2"))
              .extra.value(QStringLiteral("stale"))
              .toBool());
  // INTERMEDIATE 在闭包里穿透但不记 stale（staleness-lite 只标 DERIVED）。
  QVERIFY(!cat.versionById(QStringLiteral("ver-i1"))
               .extra.value(QStringLiteral("stale"))
               .toBool());
  // 被取代者自身与新版本不标。
  QVERIFY(!cat.versionById(QStringLiteral("ver-r1"))
               .extra.value(QStringLiteral("stale"))
               .toBool());
  QVERIFY(!cat.versionById(QStringLiteral("ver-r2"))
               .extra.value(QStringLiteral("stale"))
               .toBool());

  // 标记随 catalog.json 落盘（同一原子写）。
  DataCatalog reloaded;
  QVERIFY(reloaded.open(dir.path()));
  QVERIFY(reloaded.versionById(QStringLiteral("ver-d1"))
              .extra.value(QStringLiteral("stale"))
              .toBool());

  // 实体视图能把 stale 派生产物如实呈现（下游闭包照常含 stale 版本）。
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A1"), QStringLiteral("A1"))));
  QVERIFY(cat.addLink(makeLink(QStringLiteral("well-A1"), QStringLiteral("ast-raw"),
                               QStringLiteral("well_log"))));
  const EntityView view = entityDataView(cat, QStringLiteral("well-A1"));
  QVERIFY(versionIds(view.derivedProducts)
              .contains(QStringLiteral("ver-d1")));
}

// markDownstreamStale：外链 sha 失配等路径的手动标记面——空/未知 id 拒绝；
// 幂等（同一标记不重复落盘不涨 revision）；reason 覆盖。
void TestEntityView::markDownstreamStaleApi()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  for (const QString &id : {QStringLiteral("ast-ext"), QStringLiteral("ast-d")})
    QVERIFY(cat.addAsset(makeAsset(id, QStringLiteral("generic"))));
  CatalogVersion ext = makeVersion(QStringLiteral("ver-ext"),
                                   QStringLiteral("ast-ext"), 1);
  ext.managed = false;
  ext.path = QStringLiteral("/abs/segy.sgy");
  QVERIFY(cat.addVersion(ext));
  QVERIFY(cat.addVersion(makeVersion(QStringLiteral("ver-d1"),
                                     QStringLiteral("ast-d"), 1,
                                     QStringLiteral("DERIVED"),
                                     {QStringLiteral("ver-ext")})));

  QString err;
  QVERIFY(!cat.markDownstreamStale(QString(), QStringLiteral("x"), &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(!cat.markDownstreamStale(QStringLiteral("ver-nope"),
                                   QStringLiteral("x"), &err));
  QVERIFY(!err.isEmpty());

  const int revBefore = cat.catalogRevision();
  err.clear(); // mutator 成功路径不清 *error——成功断言前自己清。
  QVERIFY(cat.markDownstreamStale(QStringLiteral("ver-ext"),
                                  QStringLiteral("源文件 SHA-256 与入库时不一致"),
                                  &err));
  QVERIFY(err.isEmpty());
  const CatalogVersion d1 = cat.versionById(QStringLiteral("ver-d1"));
  QVERIFY(d1.extra.value(QStringLiteral("stale")).toBool());
  QCOMPARE(d1.extra.value(QStringLiteral("staleReason")).toString(),
           QStringLiteral("源文件 SHA-256 与入库时不一致"));

  // 同一标记再来一次：true 但不落盘（revision 不涨）。
  QVERIFY(cat.markDownstreamStale(QStringLiteral("ver-ext"),
                                  QStringLiteral("源文件 SHA-256 与入库时不一致")));
  QCOMPARE(cat.catalogRevision(), revBefore + 1);

  // 新 reason 覆盖旧标记（下游最新的失效原因如实呈现）。
  QVERIFY(cat.markDownstreamStale(QStringLiteral("ver-ext"),
                                  QStringLiteral("external parent missing")));
  QCOMPARE(cat.versionById(QStringLiteral("ver-d1"))
               .extra.value(QStringLiteral("staleReason"))
               .toString(),
           QStringLiteral("external parent missing"));

  // 拒绝写入态（未 open 的 catalog）：mutator 一律 false+error。
  DataCatalog closed;
  QVERIFY(!closed.markDownstreamStale(QStringLiteral("ver-ext"),
                                      QStringLiteral("x"), &err));
  QVERIFY(!err.isEmpty());
}

// 空/未知 entityId → 如实空视图（entity.id 为空、各面皆空），不编造槽位。
void TestEntityView::unknownEntityYieldsEmptyView()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  QVERIFY(cat.addEntity(makeWell(QStringLiteral("well-A1"), QStringLiteral("A1"))));

  for (const QString &id : {QString(), QStringLiteral("well-nope")})
  {
    const EntityView view = entityDataView(cat, id);
    QVERIFY(view.entity.id.isEmpty());
    QVERIFY(view.roleSlots.isEmpty());
    QVERIFY(view.derivedProducts.isEmpty());
    QVERIFY(view.missingSources.isEmpty());
  }
}

QTEST_MAIN(TestEntityView)
#include "tst_entityview.moc"
