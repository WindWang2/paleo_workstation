#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/welltopsedit.h"
#include "../src/io/wellfileparsers.h"
#include "../src/workflow/welltopseditorworkflow.h"

#include <QFile>
#include <QDir>

// 方向 32：井分层编辑与质量管理——domain 纯函数 + workflow 版本化落库。
//   · 写侧序列化 round-trip（parse → write → parse 记录级恒等；自适应精度）
//   · 校验召回：叠置/同深/倒置/空洞/悬空/域冲突逐条行级检出（Oracle #2）
//   · 差异摘要 / 批量变换（重命名/位移/删除）
//   · 合并 diff + 取舍应用（Oracle #3 前半）
//   · 编辑提交 round-trip：新 DERIVED 版本 + 重开逐字段一致（Oracle #1）
//   · 下游失效：编辑提交 ⇒ 以旧版本为父的 DERIVED 产物落 stale（Oracle #4）
//   · 回滚 = 新版本指向旧内容；未变化 no-op 不发版本
class TestWellTopsEdit : public QObject
{
  Q_OBJECT

private slots:
  void writerRoundTrip();
  void writerPrecisionAdaptive();
  void validatorRecall();
  void validatorSkipsFrameworkChecksWhenAbsent();
  void diffSummary();
  void batchTransforms();
  void mergeDiffAndApply();
  void commitRoundTrip();
  void commitUnchangedIsNoop();
  void rollbackCreatesNewVersionWithOldContent();
  void downstreamStaleAfterEdit();
  void mergeViaWorkflow();
  void batchCommitAllRows();
  void contextForUsesWellTd();
  void sentinelValuesAreValidatorErrors();
  void wellKeyMirrorsCatalogUnderscoreRule();
  void validateAllWellsAggregatesPerWell();
  void mergeCommitCarriesProvenance();
  void sameDepthChainWithinTolerance();
  void halfCoordinateGroupPreservesZ();

private:
  struct Fixture
  {
    QTemporaryDir dir;
    DataCatalog cat;
    QString assetId;
    QString rawVersionId;
    QString a1Id, a2Id;

    static WellTopRecord rec(const QString &well, const QString &top, double md, double tvd)
    {
      WellTopRecord r;
      r.wellName = well;
      r.topName = top;
      r.md = md;
      r.hasMd = true;
      r.tvd = tvd;
      r.hasTvd = true;
      r.x = 5288.67;
      r.y = 8219.94;
      r.hasX = true;
      r.hasY = true;
      r.z = -tvd;
      r.timeMs = 0;
      r.hasTime = false;
      return r;
    }

    bool build()
    {
      const QString proj = dir.path();
      if (!cat.open(proj))
        return false;
      QVector<WellTopRecord> rows;
      rows << rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0)
           << rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5)
           << rec(QStringLiteral("A1"), QStringLiteral("B"), 1146.0, 1146.0)
           << rec(QStringLiteral("A2"), QStringLiteral("X"), 800.0, 800.0)
           << rec(QStringLiteral("A2"), QStringLiteral("A"), 900.0, 900.0);

      for (const QString &name : {QStringLiteral("A1"), QStringLiteral("A2")})
      {
        CatalogEntity e;
        e.id = cat.nextEntityId(QStringLiteral("well"));
        e.entityType = QStringLiteral("well");
        e.name = name;
        e.td = 2500.0;
        e.hasSurface = true;
        e.surfaceX = 5288.67;
        e.surfaceY = 8219.94;
        if (!cat.addEntity(e))
          return false;
        if (name == QLatin1String("A1"))
          a1Id = e.id;
        else
          a2Id = e.id;
      }

      CatalogAsset a;
      a.id = cat.nextAssetId();
      a.type = QStringLiteral("well_stratification");
      a.format = QStringLiteral("dat");
      a.displayName = QStringLiteral("DC.dat");
      if (!cat.addAsset(a))
        return false;
      assetId = a.id;

      CatalogVersion v;
      v.id = cat.nextVersionId();
      v.assetId = a.id;
      v.stage = QStringLiteral("RAW");
      v.versionNumber = 1;
      v.managed = true;
      v.fileName = QStringLiteral("DC.dat");
      v.path = QStringLiteral("artifacts/") +
               DataCatalog::managedPath(QStringLiteral("raw"), a.id, v.id,
                                        QStringLiteral("DC.dat"));
      v.sourceUri = QStringLiteral("fixture");
      const QString abs = QDir(proj).absoluteFilePath(v.path);
      if (!QDir().mkpath(QFileInfo(abs).absolutePath()))
        return false;
      QFile f(abs);
      if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
      f.write(writeWellTopsText(rows));
      f.close();
      v.sha256 = DataCatalog::sha256FileHex(abs);
      if (!cat.addVersion(v))
        return false;
      rawVersionId = v.id;

      for (const QString &eid : {a1Id, a2Id})
      {
        EntityAssetLink l;
        l.entityType = QStringLiteral("well");
        l.entityId = eid;
        l.assetId = a.id;
        l.role = QStringLiteral("tops");
        l.isPrimary = true;
        if (!cat.addLink(l))
          return false;
      }
      return true;
    }
  };

  static bool sameRecord(const WellTopRecord &a, const WellTopRecord &b)
  {
    return WellTopsEdit::sameValues(a, b);
  }

  static bool sameRecords(const QVector<WellTopRecord> &a, const QVector<WellTopRecord> &b)
  {
    if (a.size() != b.size())
      return false;
    for (int i = 0; i < a.size(); ++i)
      if (!sameRecord(a.at(i), b.at(i)))
        return false;
    return true;
  }

  static QVector<WellTopsEdit::Issue> findIssues(const QVector<WellTopsEdit::Issue> &issues,
                                                 WellTopsEdit::IssueKind kind)
  {
    QVector<WellTopsEdit::Issue> out;
    for (const WellTopsEdit::Issue &i : issues)
      if (i.kind == kind)
        out.append(i);
    return out;
  }
};

// ---- 写侧 round-trip ------------------------------------------------------

void TestWellTopsEdit::writerRoundTrip()
{
  const QByteArray original =
      "#WellTops File From SMI\r\n"
      "#WellName    Name         MD           X            Y            Z            TVD          Time(ms)\r\n"
      "A1           X            850.000      5288.670     8219.940     -850.000     850.000      -99999.000\r\n"
      "A2           A            900.5        -99999.000   -99999.000   -99999.000   -99999.000   1234.500\r\n"
      "A2           B            950.125      1.0          2.0          -950.125     950.125      -99999.000\r\n";
  const QVector<WellTopRecord> first = parseWellTopsText(original);
  QCOMPARE(first.size(), 3);
  const QByteArray written = writeWellTopsText(first);
  QVERIFY(written.contains("#WellTops"));
  QVERIFY(written.contains("-99999.000"));
  const QVector<WellTopRecord> second = parseWellTopsText(written);
  QVERIFY2(sameRecords(first, second), "write → parse 必须记录级恒等");
  QCOMPARE(second.at(1).hasX, false); // 哨兵回读仍为空
  QCOMPARE(second.at(2).x, 1.0);
}

void TestWellTopsEdit::writerPrecisionAdaptive()
{
  QVector<WellTopRecord> rows;
  WellTopRecord r;
  r.wellName = QStringLiteral("W9");
  r.topName = QStringLiteral("P");
  r.md = 1234.5678901; // 7 位小数——3 位定点会丢
  r.hasMd = true;
  rows.append(r);
  const QVector<WellTopRecord> back = parseWellTopsText(writeWellTopsText(rows));
  QCOMPARE(back.size(), 1);
  QCOMPARE(back.front().md, r.md); // 自适应精度保住往返
}

// ---- 校验召回 --------------------------------------------------------------

void TestWellTopsEdit::validatorRecall()
{
  using namespace WellTopsEdit;
  auto rec = Fixture::rec;
  QVector<WellTopRecord> rows;
  rows << rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0)      // 0 格架 idx0
       << rec(QStringLiteral("A1"), QStringLiteral("C6"), 1300.0, 1300.0)   // 1 格架 idx1——与行 2 倒置
       << rec(QStringLiteral("A1"), QStringLiteral("C3"), 1200.0, 1200.0)   // 2 格架 idx2（浅于行 1 却更浅? C6 浅层深 MD）
       << rec(QStringLiteral("A1"), QStringLiteral("C3"), 1350.0, 900.0)    // 3 与行 2 层名重复（tvd<md 合法）
       << rec(QStringLiteral("A1"), QStringLiteral("DUP"), 1400.0, 1500.0)  // 4 TvdOverMd
       << rec(QStringLiteral("A1"), QStringLiteral("NEG"), -5.0, -5.0)      // 5 MD 为负
       << rec(QStringLiteral("A1"), QStringLiteral("OVER"), 3000.0, 3000.0) // 6 MD 超 TD（ctx.td=2500）
       << rec(QStringLiteral("A1"), QStringLiteral("ZZ"), 900.0, 900.0)     // 7 悬空层名
       << WellTopRecord();                                                  // 8 层名空（md 空）
  rows.last().wellName = QStringLiteral("A1");
  rows.last().topName = QString();
  // 同深叠置：再放一行 MD=900（与 ZZ 同深）
  rows << rec(QStringLiteral("A1"), QStringLiteral("SAME"), 900.0, 900.0);  // 9 SameDepth（与行 7）

  ValidationContext ctx;
  ctx.hasTd = true;
  ctx.td = 2500.0;
  ctx.framework = {QStringLiteral("X"), QStringLiteral("C6"), QStringLiteral("C3"),
                   QStringLiteral("D53")};
  const QVector<Issue> issues = validate(rows, ctx);

  // 倒置：C6(浅)@1000 vs C3(深)@1200 → 两行各一条
  const auto inversions = findIssues(issues, IssueKind::Inversion);
  QCOMPARE(inversions.size(), 2);
  QVERIFY(inversions.at(0).rowIndex == 1 || inversions.at(0).rowIndex == 2);
  QVERIFY(inversions.at(1).rowIndex == 1 || inversions.at(1).rowIndex == 2);

  // 层名重复：C3 两行（行 2、3）各一条
  const auto dups = findIssues(issues, IssueKind::DuplicateName);
  QCOMPARE(dups.size(), 2);
  QVERIFY(dups.at(0).rowIndex == 2 || dups.at(0).rowIndex == 3);

  // 同深：900 出现两行（7、9）各一条
  const auto sameDepth = findIssues(issues, IssueKind::SameDepth);
  QCOMPARE(sameDepth.size(), 2);

  // TVD > MD
  const auto tvdOver = findIssues(issues, IssueKind::TvdOverMd);
  QCOMPARE(tvdOver.size(), 1);
  QCOMPARE(tvdOver.front().rowIndex, 4);

  // MD 负值 + 超 TD
  const auto range = findIssues(issues, IssueKind::MdOutOfRange);
  QCOMPARE(range.size(), 2);
  QVERIFY(range.at(0).rowIndex == 5 || range.at(0).rowIndex == 6);

  // 悬空层名：名单外的 5 行（4 DUP / 5 NEG / 6 OVER / 7 ZZ / 9 SAME；空名行不计）
  const auto dangling = findIssues(issues, IssueKind::DanglingName);
  QCOMPARE(dangling.size(), 5);
  QVERIFY(std::any_of(dangling.begin(), dangling.end(),
                      [](const Issue &i) { return i.rowIndex == 7; }));

  // 空层名
  const auto empty = findIssues(issues, IssueKind::EmptyName);
  QCOMPARE(empty.size(), 1);
  QCOMPARE(empty.front().rowIndex, 8);

  // 层名含空白（格式契约：写盘即损坏——错误级）
  {
    QVector<WellTopRecord> wsRows;
    wsRows << Fixture::rec(QStringLiteral("A1"), QStringLiteral("A B"), 900.0, 900.0);
    const auto ws = validate(wsRows, ctx);
    QCOMPARE(findIssues(ws, IssueKind::WhitespaceName).size(), 1);
    QVERIFY(findIssues(ws, IssueKind::WhitespaceName).front().isError());
  }

  // 空洞：词表 D53 缺失——锚 = 最深的既有浅侧行（首个 C3 行 2，格架序 2）
  const auto missing = findIssues(issues, IssueKind::MissingTop);
  QCOMPARE(missing.size(), 1);
  QCOMPARE(missing.front().rowIndex, 2);
  QVERIFY(missing.front().message.contains(QStringLiteral("D53")));

  // 消息必须含行号（行级定位诚实面）
  for (const Issue &i : issues)
    if (i.rowIndex >= 0)
      QVERIFY(i.message.contains(QString::number(i.rowIndex + 1)) || i.message.contains(QStringLiteral("缺失")));
}

void TestWellTopsEdit::validatorSkipsFrameworkChecksWhenAbsent()
{
  using namespace WellTopsEdit;
  QVector<WellTopRecord> rows;
  rows << Fixture::rec(QStringLiteral("A1"), QStringLiteral("ZZ"), 850.0, 850.0)
       << Fixture::rec(QStringLiteral("A1"), QStringLiteral("YY"), 900.0, 900.0);
  ValidationContext ctx; // framework 空
  const QVector<Issue> issues = validate(rows, ctx);
  QCOMPARE(findIssues(issues, IssueKind::DanglingName).size(), 0);
  QCOMPARE(findIssues(issues, IssueKind::MissingTop).size(), 0);
  QCOMPARE(findIssues(issues, IssueKind::Inversion).size(), 0);
}

// ---- 差异 / 批量 -----------------------------------------------------------

void TestWellTopsEdit::diffSummary()
{
  using namespace WellTopsEdit;
  auto rec = Fixture::rec;
  const QVector<WellTopRecord> oldRows = {
      rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0),
      rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5),
      rec(QStringLiteral("A2"), QStringLiteral("X"), 800.0, 800.0),
  };
  const QVector<WellTopRecord> newRows = {
      rec(QStringLiteral("A1"), QStringLiteral("X"), 860.0, 860.0), // changed
      rec(QStringLiteral("A2"), QStringLiteral("X"), 800.0, 800.0), // 同
      rec(QStringLiteral("A2"), QStringLiteral("B"), 950.0, 950.0), // added
  };
  const DiffSummary d = diff(oldRows, newRows);
  QCOMPARE(d.added, 1);
  QCOMPARE(d.removed, 1);
  QCOMPARE(d.changed, 1);
}

void TestWellTopsEdit::batchTransforms()
{
  using namespace WellTopsEdit;
  auto rec = Fixture::rec;
  QVector<WellTopRecord> rows;
  rows << rec(QStringLiteral("A1"), QStringLiteral("c3"), 850.0, 850.0) // 小写变体
       << rec(QStringLiteral("A1"), QStringLiteral("C3"), 900.0, 900.0)
       << rec(QStringLiteral("A2"), QStringLiteral("C3"), 800.0, 800.0)
       << rec(QStringLiteral("A2"), QStringLiteral("D53"), 950.0, 950.0)
       << WellTopRecord(); // 只有层名的行
  rows.last().wellName = QStringLiteral("A3");
  rows.last().topName = QStringLiteral("X");

  QVector<WellTopRecord> copy = rows;
  QCOMPARE(applyRename(&copy, QStringLiteral("c3 "), QStringLiteral("C3")), 3); // 规范化匹配 + 统一
  QCOMPARE(copy.at(0).topName, QStringLiteral("C3"));
  QCOMPARE(copy.at(2).topName, QStringLiteral("C3"));

  copy = rows;
  QCOMPARE(applyShift(&copy, 10.0), 4); // 4 行有深度列；纯层名行不动
  QCOMPARE(copy.at(0).md, 860.0);
  QCOMPARE(copy.at(0).tvd, 860.0);
  QCOMPARE(copy.at(0).z, -850.0 + 10.0);
  QCOMPARE(copy.at(4).hasMd, false);   // 空行不动
  QCOMPARE(applyShift(&copy, 0.0), 0); // 0 位移 no-op

  copy = rows;
  QCOMPARE(applyDelete(&copy, QStringLiteral("C3")), 3);
  QCOMPARE(copy.size(), 2);
}

// ---- 合并 ------------------------------------------------------------------

void TestWellTopsEdit::mergeDiffAndApply()
{
  using namespace WellTopsEdit;
  auto rec = Fixture::rec;
  const QVector<WellTopRecord> oldRows = {
      rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0),
      rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5),
      rec(QStringLiteral("A1"), QStringLiteral("OLD"), 700.0, 700.0),
  };
  const QVector<WellTopRecord> incoming = {
      rec(QStringLiteral("A1"), QStringLiteral("X"), 900.0, 900.0), // 冲突
      rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5), // 一致
      rec(QStringLiteral("A1"), QStringLiteral("NEW"), 1000.0, 1000.0),
  };
  QVector<MergeRow> m = mergeDiff(oldRows, incoming);
  QCOMPARE(m.size(), 4);
  QCOMPARE(m.at(0).conflicts(), true);
  QCOMPARE(m.at(1).conflicts(), false);
  QVERIFY(!m.at(2).inNew); // OLD 仅旧
  QVERIFY(!m.at(3).inOld); // NEW 仅新
  QCOMPARE(m.at(3).resolution, MergeRow::Resolution::TakeNew); // 新行默认新增

  // 冲突默认保留旧值 → 结果 = 旧集 + NEW。
  QVector<WellTopRecord> applied = applyMerge(m);
  QCOMPARE(applied.size(), 4);
  QCOMPARE(applied.at(0).md, 850.0);
  QCOMPARE(applied.at(3).topName, QStringLiteral("NEW"));

  // 逐行取舍：冲突采用新值；OLD 删除；NEW 跳过。
  m[0].resolution = MergeRow::Resolution::TakeNew;
  m[2].resolution = MergeRow::Resolution::RemoveOld;
  m[3].resolution = MergeRow::Resolution::KeepOld;
  applied = applyMerge(m);
  QCOMPARE(applied.size(), 2);
  QCOMPARE(applied.at(0).md, 900.0);
  QCOMPARE(applied.at(1).topName, QStringLiteral("A"));
}

// ---- workflow：版本化提交 ---------------------------------------------------

void TestWellTopsEdit::commitRoundTrip()
{
  Fixture fx;
  QVERIFY(fx.build());
  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());

  QVector<WellTopRecord> all;
  QString err;
  QVERIFY(wf.loadAllRows(fx.assetId, &all, &err));
  QCOMPARE(all.size(), 5);
  const QVector<WellTopRecord> a2Before = WellTopsEditorWorkflow::rowsForWell(
      all, QStringLiteral("A2"));
  QCOMPARE(a2Before.size(), 2);

  // 编辑 A1：改 X 深度、删 B、增 NEW。
  QVector<WellTopRecord> edited;
  edited << Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 855.0, 855.0)
         << Fixture::rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5)
         << Fixture::rec(QStringLiteral("A1"), QStringLiteral("NEW"), 1500.0, 1500.0);
  const auto out = wf.commitWellRows(fx.assetId, QStringLiteral("A1"), edited, QString());
  QVERIFY2(out.ok, qPrintable(out.error));
  QVERIFY(!out.unchanged);
  QCOMPARE(out.newVersionNumber, 2);
  QCOMPARE(out.diff.added, 1);
  QCOMPARE(out.diff.removed, 1);
  QCOMPARE(out.diff.changed, 1);

  const CatalogVersion v2 = fx.cat.currentVersion(fx.assetId);
  QCOMPARE(v2.id, out.newVersionId);
  QCOMPARE(v2.stage, QStringLiteral("DERIVED"));
  QVERIFY(!v2.sha256.isEmpty());
  QCOMPARE(v2.parentVersionIds.size(), 0); // 血缘在 extra，不在 parent（防自我 stale）
  QCOMPARE(v2.extra.value(QStringLiteral("editKind")).toString(), QStringLiteral("edit"));
  QCOMPARE(v2.extra.value(QStringLiteral("editWell")).toString(), QStringLiteral("A1"));
  QCOMPARE(v2.extra.value(QStringLiteral("editBaseline")).toString(), fx.rawVersionId);
  QCOMPARE(v2.extra.value(QStringLiteral("rowsChanged")).toInt(), 1);
  QVERIFY(!v2.extra.contains(QStringLiteral("stale"))); // 编辑版本自身不得被 supersede 标 stale

  // 重开逐字段一致 + 其他井不受扰动。
  QVector<WellTopRecord> reloaded;
  QVERIFY(wf.loadAllRows(fx.assetId, &reloaded, &err));
  const QVector<WellTopRecord> a1After = WellTopsEditorWorkflow::rowsForWell(
      reloaded, QStringLiteral("A1"));
  QVERIFY(sameRecords(a1After, edited));
  QVERIFY(sameRecords(WellTopsEditorWorkflow::rowsForWell(reloaded, QStringLiteral("A2")),
                      a2Before));

  // 重开 catalog（盘上持久）。
  DataCatalog reopened;
  QVERIFY(reopened.open(fx.dir.path()));
  WellTopsEditorWorkflow wf2(&reopened, fx.dir.path());
  QVector<WellTopRecord> persisted;
  QVERIFY(wf2.loadAllRows(fx.assetId, &persisted, &err));
  QVERIFY(sameRecords(WellTopsEditorWorkflow::rowsForWell(persisted, QStringLiteral("A1")),
                      edited));
}

void TestWellTopsEdit::commitUnchangedIsNoop()
{
  Fixture fx;
  QVERIFY(fx.build());
  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());
  QVector<WellTopRecord> all;
  QString err;
  QVERIFY(wf.loadAllRows(fx.assetId, &all, &err));
  const QVector<WellTopRecord> a1 =
      WellTopsEditorWorkflow::rowsForWell(all, QStringLiteral("A1"));
  const auto out = wf.commitWellRows(fx.assetId, QStringLiteral("A1"), a1, QString());
  QVERIFY(out.ok);
  QVERIFY(out.unchanged);
  QCOMPARE(fx.cat.versionsForAsset(fx.assetId).size(), 1); // 未发新版本
}

void TestWellTopsEdit::rollbackCreatesNewVersionWithOldContent()
{
  Fixture fx;
  QVERIFY(fx.build());
  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());

  QVector<WellTopRecord> all;
  QString err;
  QVERIFY(wf.loadAllRows(fx.assetId, &all, &err));
  QVector<WellTopRecord> edited;
  edited << Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 855.0, 855.0);
  const auto editOut = wf.commitWellRows(fx.assetId, QStringLiteral("A1"), edited, QString());
  QVERIFY(editOut.ok);
  QCOMPARE(editOut.newVersionNumber, 2);

  const auto back = wf.rollbackTo(fx.assetId, fx.rawVersionId);
  QVERIFY2(back.ok, qPrintable(back.error));
  QCOMPARE(back.newVersionNumber, 3);
  QCOMPARE(fx.cat.currentVersion(fx.assetId).versionNumber, 3);

  QVector<WellTopRecord> rolled;
  QVERIFY(wf.loadAllRows(fx.assetId, &rolled, &err));
  QVERIFY(sameRecords(rolled, all)); // 内容回到 RAW v1 的行集

  const CatalogVersion v3 = fx.cat.currentVersion(fx.assetId);
  QCOMPARE(v3.extra.value(QStringLiteral("editKind")).toString(), QStringLiteral("rollback"));
  QCOMPARE(v3.extra.value(QStringLiteral("rollbackTo")).toString(), fx.rawVersionId);

  // 回滚到当前版本 → 拒绝。
  const auto noop = wf.rollbackTo(fx.assetId, v3.id);
  QVERIFY(!noop.ok);
}

void TestWellTopsEdit::downstreamStaleAfterEdit()
{
  Fixture fx;
  QVERIFY(fx.build());

  // 下游产物：以 RAW v1 为父的 DERIVED 版本（模拟剖面保存/厚度井控等的记账）。
  CatalogAsset d;
  d.id = fx.cat.nextAssetId();
  d.type = QStringLiteral("section");
  d.format = QStringLiteral("json");
  d.displayName = QStringLiteral("section.json");
  QVERIFY(fx.cat.addAsset(d));
  CatalogVersion dv;
  dv.id = fx.cat.nextVersionId();
  dv.assetId = d.id;
  dv.stage = QStringLiteral("DERIVED");
  dv.versionNumber = 1;
  dv.managed = false;
  dv.fileName = QStringLiteral("section.json");
  dv.parentVersionIds = {fx.rawVersionId};
  QVERIFY(fx.cat.addVersion(dv));

  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());
  QVector<WellTopRecord> all;
  QString err;
  QVERIFY(wf.loadAllRows(fx.assetId, &all, &err));
  QVector<WellTopRecord> edited;
  edited << Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 855.0, 855.0)
         << Fixture::rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5)
         << Fixture::rec(QStringLiteral("A1"), QStringLiteral("B"), 1146.0, 1146.0);
  const auto out = wf.commitWellRows(fx.assetId, QStringLiteral("A1"), edited, QString());
  QVERIFY(out.ok);

  const CatalogVersion downstream = fx.cat.versionById(dv.id);
  QVERIFY(downstream.extra.value(QStringLiteral("stale")).toBool());
  QVERIFY(downstream.extra.value(QStringLiteral("staleReason")).toString().contains(
      QStringLiteral("取代")));
  // 编辑新版本自身保持干净。
  QVERIFY(!fx.cat.versionById(out.newVersionId).extra.contains(QStringLiteral("stale")));
}

void TestWellTopsEdit::mergeViaWorkflow()
{
  Fixture fx;
  QVERIFY(fx.build());
  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());

  // 再导入文件：A1 的 X 改深、NEW 新增。
  const QString external = fx.dir.filePath(QStringLiteral("DC2.dat"));
  {
    QVector<WellTopRecord> incoming;
    incoming << Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 870.0, 870.0)
             << Fixture::rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5)
             << Fixture::rec(QStringLiteral("A1"), QStringLiteral("NEW"), 1600.0, 1600.0)
             << Fixture::rec(QStringLiteral("A9"), QStringLiteral("X"), 100.0, 100.0);
    QFile f(external);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(writeWellTopsText(incoming));
    f.close();
  }

  QVector<WellTopsEdit::MergeRow> rows;
  QString err;
  QVERIFY(wf.loadMergeRows(fx.assetId, QStringLiteral("A1"), external, &rows, &err));
  QCOMPARE(rows.size(), 4); // X 冲突、A 一致、B 仅旧、NEW 仅新
  QVERIFY(!rows.at(2).inNew); // B 不受「按井过滤」外行影响
  rows[0].resolution = WellTopsEdit::MergeRow::Resolution::TakeNew; // X 采用新值
  rows[2].resolution = WellTopsEdit::MergeRow::Resolution::RemoveOld; // B 删除
  const QVector<WellTopRecord> merged = WellTopsEdit::applyMerge(rows);

  const auto out = wf.commitWellRows(fx.assetId, QStringLiteral("A1"), merged, QString());
  QVERIFY2(out.ok, qPrintable(out.error));
  QCOMPARE(out.diff.added, 1);
  QCOMPARE(out.diff.removed, 1);
  QCOMPARE(out.diff.changed, 1);

  QVector<WellTopRecord> reloaded;
  QVERIFY(wf.loadAllRows(fx.assetId, &reloaded, &err));
  const QVector<WellTopRecord> a1 =
      WellTopsEditorWorkflow::rowsForWell(reloaded, QStringLiteral("A1"));
  QCOMPARE(a1.size(), 3);
  QCOMPARE(a1.at(0).topName, QStringLiteral("X"));
  QCOMPARE(a1.at(0).md, 870.0);
  QVERIFY(std::none_of(a1.begin(), a1.end(),
                       [](const WellTopRecord &r) { return r.topName == QLatin1String("B"); }));
  // A9 行不进库（井外行被过滤）。
  QVERIFY(WellTopsEditorWorkflow::rowsForWell(reloaded, QStringLiteral("A9")).isEmpty());

  // 合并落库版本记 editKind=merge（UI 侧合并走同一提交面时由 note 区分）。
  QVERIFY(rows.at(1).inOld && rows.at(1).inNew && !rows.at(1).conflicts());
}

void TestWellTopsEdit::batchCommitAllRows()
{
  Fixture fx;
  QVERIFY(fx.build());
  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());
  QVector<WellTopRecord> all;
  QString err;
  QVERIFY(wf.loadAllRows(fx.assetId, &all, &err));
  const int shifted = WellTopsEdit::applyShift(&all, 5.0);
  QCOMPARE(shifted, 5);

  const auto out = wf.commitAllRows(fx.assetId, all, QStringLiteral("batch-shift"), QString());
  QVERIFY2(out.ok, qPrintable(out.error));
  QCOMPARE(fx.cat.currentVersion(fx.assetId).extra.value(QStringLiteral("editKind")).toString(),
           QStringLiteral("batch-shift"));

  QVector<WellTopRecord> reloaded;
  QVERIFY(wf.loadAllRows(fx.assetId, &reloaded, &err));
  QCOMPARE(reloaded.at(0).md, 855.0); // A1 X 850 → 855
  QCOMPARE(WellTopsEditorWorkflow::rowsForWell(reloaded, QStringLiteral("A2"))
                .at(0)
                .md,
           805.0);
}

// M2：X/Y/TVD/Time 命中 -99999 哨兵域 = 错误级（缺失应清空单元格）。
void TestWellTopsEdit::sentinelValuesAreValidatorErrors()
{
  using namespace WellTopsEdit;
  WellTopRecord r = Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0);
  r.tvd = -100000.0; // 命中哨兵域
  r.x = -99999.5;
  const QVector<Issue> issues = validate({r}, ValidationContext());
  const auto sentinels = findIssues(issues, IssueKind::SentinelValue);
  QCOMPARE(sentinels.size(), 2);
  QVERIFY(sentinels.front().isError());
  QVERIFY(sentinels.at(0).message.contains(QStringLiteral("TVD")) ||
          sentinels.at(0).message.contains(QStringLiteral("X")));
}

// M5：井名键镜像 catalog 规则——下划线在两端口径一致（A_1 ≡ A1）。
void TestWellTopsEdit::wellKeyMirrorsCatalogUnderscoreRule()
{
  using namespace WellTopsEdit;
  const QVector<WellTopRecord> oldRows = {
      Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0),
      Fixture::rec(QStringLiteral("A_1"), QStringLiteral("X"), 850.0, 850.0),
  };
  const DiffSummary d = diff(oldRows,
                             {Fixture::rec(QStringLiteral("a-1"), QStringLiteral("X"), 850.0,
                                           850.0)});
  QVERIFY(d.isEmpty()); // 三种拼写 = 同一 (井,层) 键，值未变
}

// M3：批量校验面——逐井跑校验器，错误带井名前缀。
void TestWellTopsEdit::validateAllWellsAggregatesPerWell()
{
  Fixture fx;
  QVERIFY(fx.build());
  QVector<WellTopRecord> all;
  QString err;
  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());
  QVERIFY(wf.loadAllRows(fx.assetId, &all, &err));
  all.append(Fixture::rec(QStringLiteral("A1"), QStringLiteral("OVER"), 3000.0, 3000.0)); // 超 TD（悬空名只警告）
  const QStringList errors = WellTopsEditorWorkflow::validateAllWells(&fx.cat, all);
  QCOMPARE(errors.size(), 1);
  QVERIFY(errors.front().contains(QStringLiteral("[A1]")));
  QVERIFY(errors.front().contains(QStringLiteral("TD")));
}

// M4：合并落库的溯源——editKind=merge 进版本 extra。
void TestWellTopsEdit::mergeCommitCarriesProvenance()
{
  Fixture fx;
  QVERIFY(fx.build());
  WellTopsEditorWorkflow wf(&fx.cat, fx.dir.path());
  QVector<WellTopRecord> all;
  QString err;
  QVERIFY(wf.loadAllRows(fx.assetId, &all, &err));
  QVector<WellTopRecord> merged = WellTopsEditorWorkflow::rowsForWell(
      all, QStringLiteral("A1"));
  merged.removeLast(); // 模拟取舍后少一行
  const auto out = wf.commitWellRows(fx.assetId, QStringLiteral("A1"), merged,
                                     QStringLiteral("合并自 DC2.dat"), QStringLiteral("merge"));
  QVERIFY2(out.ok, qPrintable(out.error));
  const CatalogVersion v = fx.cat.currentVersion(fx.assetId);
  QCOMPARE(v.extra.value(QStringLiteral("editKind")).toString(), QStringLiteral("merge"));
  QCOMPARE(v.extra.value(QStringLiteral("editNote")).toString(), QStringLiteral("合并自 DC2.dat"));
}

// 轮 3：同深链式分组——恰好 1e-6 的链全部检出（量化分桶会漏）。
void TestWellTopsEdit::sameDepthChainWithinTolerance()
{
  using namespace WellTopsEdit;
  QVector<WellTopRecord> rows;
  rows << Fixture::rec(QStringLiteral("A1"), QStringLiteral("A"), 1.0, 1.0)
       << Fixture::rec(QStringLiteral("A1"), QStringLiteral("B"), 1.0 + 0.5e-6, 1.0)
       << Fixture::rec(QStringLiteral("A1"), QStringLiteral("C"), 1.0 + 1.5e-6, 1.0);
  const QVector<Issue> issues = validate(rows, ValidationContext());
  QCOMPARE(findIssues(issues, IssueKind::SameDepth).size(), 3); // 链式三行全报
  // 远离容差的两行不误报。
  QVector<WellTopRecord> apart;
  apart << Fixture::rec(QStringLiteral("A1"), QStringLiteral("A"), 1.0, 1.0)
        << Fixture::rec(QStringLiteral("A1"), QStringLiteral("B"), 1.0 + 1e-5, 1.0);
  QCOMPARE(findIssues(validate(apart, ValidationContext()), IssueKind::SameDepth).size(), 0);
}

// 轮 3 M2：半坐标组（X 有效 Y 哨兵）的 z 不得在写读往返中丢失。
void TestWellTopsEdit::halfCoordinateGroupPreservesZ()
{
  QVector<WellTopRecord> rows;
  WellTopRecord r = Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0);
  r.y = 0.0;
  r.hasY = false; // 半组：X 有效、Y 哨兵
  r.z = -123.456;
  rows.append(r);
  const QVector<WellTopRecord> back = parseWellTopsText(writeWellTopsText(rows));
  QCOMPARE(back.size(), 1);
  QCOMPARE(back.front().hasX, true);
  QCOMPARE(back.front().hasY, false);
  QCOMPARE(back.front().z, -123.456); // 半组的 z 保住了

  const QVector<WellTopsEdit::Issue> issues =
      WellTopsEdit::validate(back, WellTopsEdit::ValidationContext());
  const auto half = findIssues(issues, WellTopsEdit::IssueKind::HalfCoordinateGroup);
  QCOMPARE(half.size(), 1); // 但作为 QA 警告可见
  QVERIFY(!half.front().isError());
}

void TestWellTopsEdit::contextForUsesWellTd()
{
  Fixture fx;
  QVERIFY(fx.build());
  const WellTopsEdit::ValidationContext ctx =
      WellTopsEditorWorkflow::contextFor(&fx.cat, QStringLiteral("A1"));
  QVERIFY(ctx.hasTd);
  QCOMPARE(ctx.td, 2500.0);
  QVERIFY(!ctx.framework.isEmpty()); // AreaRules 默认词表（浅→深）
}

int main(int argc, char *argv[])
{
  QCoreApplication app(argc, argv);
  TestWellTopsEdit tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_welltopsedit.moc"
