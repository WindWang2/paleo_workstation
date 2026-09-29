#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "ui/wellcomposite/editstack.h"
#include "ui/wellcomposite/editsession.h"
#include "ui/wellcomposite/topseditor.h"
#include "ui/wellcomposite/intervaleditor.h"
#include "ui/wellcomposite/stratassignment.h"
#include "ui/wellcomposite/wellcompositestore.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "domain/wellcompositemodel.h"
#include "io/wellcompositexml.h"
#include "qgis/qgisruntime.h"

#include <cmath>

using namespace WellComposite;

namespace {

ComprehensiveWellData makeWellData()
{
  ComprehensiveWellData d;
  d.wellName = QStringLiteral("TEST-1");
  d.minDepth = 1000.0;
  d.maxDepth = 2000.0;
  d.standardHorizons = {{1500.0, QStringLiteral("T35")}, {1700.0, QStringLiteral("T40")}};

  LithologyInterval l1;
  l1.topDepth = 1000.0f;
  l1.bottomDepth = 1200.0f;
  l1.lithoName = QStringLiteral("泥岩");
  LithologyInterval l2;
  l2.topDepth = 1200.0f;
  l2.bottomDepth = 1300.0f;
  l2.lithoName = QStringLiteral("砂岩");
  LithologyInterval l3;
  l3.topDepth = 1300.0f;
  l3.bottomDepth = 1400.0f;
  l3.lithoName = QStringLiteral("砂岩");
  d.lithologyIntervals = {l1, l2, l3};

  FaciesInterval f1;
  f1.topDepth = 1000.0f;
  f1.bottomDepth = 2000.0f;
  f1.majorFacies = QStringLiteral("三角洲相");
  f1.subFacies = QStringLiteral("三角洲前缘");
  f1.microFacies = QStringLiteral("河口坝");
  d.faciesIntervals = {f1};

  StratigraphyInterval s1;
  s1.topDepth = 1000.0f;
  s1.bottomDepth = 2000.0f;
  s1.formation = QStringLiteral("A"); // 未识别层名（D3.6）
  d.stratigraphyIntervals = {s1};

  CurveData gr;
  gr.name = QStringLiteral("GR");
  gr.depths = {1000.0f, 1500.0f, 2000.0f};
  gr.values = {40.0f, 60.0f, 50.0f};
  d.continuousCurves = {gr};
  return d;
}

} // namespace

// wave/wellcomposite-deep — D3.x 编辑套件测试（含 D8.3 全链路）
class TestWellCompositeEditing : public QObject
{
  Q_OBJECT

private slots:
  // ---- D3.8 undo 栈 ----
  void testEditStackDepth50()
  {
    EditStack stack;
    int v = 0;
    for (int i = 0; i < 60; ++i)
    {
      const int old = v;
      stack.push(std::make_unique<EditCommand>(
          QStringLiteral("op%1").arg(i),
          [&v, old]() { v = old; },
          [&v, i]() { v = i; }));
    }
    QCOMPARE(stack.count(), 50); // 超深丢弃最旧
    // 撤销 50 次 → 回到第 10 步的值
    for (int i = 0; i < 50; ++i)
      stack.undo();
    QVERIFY(!stack.canUndo());
    QCOMPARE(v, 9);
  }

  void testEditStackDirty()
  {
    EditStack stack;
    QVERIFY(!stack.isDirty()); // 初始干净
    int v = 0;
    const int old = v;
    stack.push(std::make_unique<EditCommand>(QStringLiteral("set"),
                                             [&v, old]() { v = old; },
                                             [&v]() { v = 42; }));
    QVERIFY(stack.isDirty()); // D3.9 编辑后脏
    stack.markSaved();
    QVERIFY(!stack.isDirty());
    // 再编辑 → 脏；撤销回到保存点 → 干净
    stack.push(std::make_unique<EditCommand>(QStringLiteral("set2"),
                                             [&v]() { v = 42; },
                                             [&v]() { v = 43; }));
    QVERIFY(stack.isDirty());
    stack.undo();
    QVERIFY(!stack.isDirty());
    // 操作名（栈位置语义；干净点下方仍有历史命令可继续撤销）
    QCOMPARE(stack.redoText(), QStringLiteral("set2"));
    stack.redo();
    QCOMPARE(stack.undoText(), QStringLiteral("set2"));
    QCOMPARE(stack.redoText(), QString());
    // 全量清单
    QCOMPARE(stack.undoTexts().size(), 2);
  }

  // ---- D3.1/D3.2 标志层编辑 ----
  void testMarkerMoveUndoRedo()
  {
    EditSession session(makeWellData());
    QVERIFY(session.moveMarker(QStringLiteral("T35"), 1520.0));
    QCOMPARE(session.document().standardHorizons.at(0).first, 1520.0);

    session.stack()->undo();
    QCOMPARE(session.document().standardHorizons.at(0).first, 1500.0);
    session.stack()->redo();
    QCOMPARE(session.document().standardHorizons.at(0).first, 1520.0);

    // 不存在的标志层
    QVERIFY(!session.moveMarker(QStringLiteral("NOPE"), 100.0));
    // 零位移幂等
    QVERIFY(session.moveMarker(QStringLiteral("T35"), 1520.0));
  }

  void testMarkerRenameInsertRemove()
  {
    EditSession session(makeWellData());

    QVERIFY(session.renameMarker(QStringLiteral("T35"), QStringLiteral("T35x")));
    QCOMPARE(session.document().standardHorizons.at(0).second, QStringLiteral("T35x"));
    // 重名拒绝
    QVERIFY(!session.renameMarker(QStringLiteral("T35x"), QStringLiteral("T40")));
    session.stack()->undo();
    QCOMPARE(session.document().standardHorizons.at(0).second, QStringLiteral("T35"));

    QVERIFY(session.insertMarker(QStringLiteral("T50"), 1900.0));
    QCOMPARE(session.document().standardHorizons.size(), 3);
    // 同名插入拒绝
    QVERIFY(!session.insertMarker(QStringLiteral("T50"), 1910.0));
    session.stack()->undo();
    QCOMPARE(session.document().standardHorizons.size(), 2);

    QVERIFY(session.removeMarker(QStringLiteral("T40")));
    QCOMPARE(session.document().standardHorizons.size(), 1);
    session.stack()->undo();
    QCOMPARE(session.document().standardHorizons.size(), 2);
  }

  // ---- D3.4 岩性编辑 ----
  void testLithoEditUndo()
  {
    EditSession session(makeWellData());
    LithologyInterval changed;
    changed.topDepth = 1000.0f;
    changed.bottomDepth = 1250.0f;
    changed.lithoName = QStringLiteral("粉砂岩");
    QVERIFY(session.editLithoInterval(0, changed));
    QCOMPARE(session.document().lithologyIntervals.at(0).lithoName, QStringLiteral("粉砂岩"));
    QCOMPARE(session.document().lithologyIntervals.at(0).bottomDepth, 1250.0f);

    session.stack()->undo();
    QCOMPARE(session.document().lithologyIntervals.at(0).lithoName, QStringLiteral("泥岩"));

    // 底 ≤ 顶拒绝
    changed.bottomDepth = 1000.0f;
    QVERIFY(!session.editLithoInterval(0, changed));

    // 删除/追加
    QVERIFY(session.removeLithoInterval(2));
    QCOMPARE(session.document().lithologyIntervals.size(), 2);
    session.stack()->undo();
    QCOMPARE(session.document().lithologyIntervals.size(), 3);

    LithologyInterval extra;
    extra.topDepth = 1400.0f;
    extra.bottomDepth = 1500.0f;
    extra.lithoName = QStringLiteral("灰岩");
    QVERIFY(session.appendLithoInterval(extra));
    QCOMPARE(session.document().lithologyIntervals.size(), 4);
  }

  // ---- D3.5 相编辑 ----
  void testFaciesEditUndo()
  {
    EditSession session(makeWellData());
    FaciesInterval fi = session.document().faciesIntervals.at(0);
    fi.microFacies = QStringLiteral("远砂坝");
    QVERIFY(session.editFaciesInterval(0, fi));
    QCOMPARE(session.document().faciesIntervals.at(0).microFacies, QStringLiteral("远砂坝"));
    session.stack()->undo();
    QCOMPARE(session.document().faciesIntervals.at(0).microFacies, QStringLiteral("河口坝"));
    // 非法区间拒绝
    FaciesInterval bad = fi;
    bad.bottomDepth = bad.topDepth;
    QVERIFY(!session.editFaciesInterval(0, bad));
  }

  // ---- D3.13 合并/拆分 ----
  void testMergeSplit()
  {
    EditSession session(makeWellData());
    // 砂岩两段相邻（1200-1300、1300-1400）→ 1 对合并
    QCOMPARE(session.mergeAdjacentLithoIntervals(), 1);
    QCOMPARE(session.document().lithologyIntervals.size(), 2);
    QCOMPARE(session.document().lithologyIntervals.at(1).bottomDepth, 1400.0f);
    session.stack()->undo();
    QCOMPARE(session.document().lithologyIntervals.size(), 3);

    // 拆分：段 0 (1000~1200) 在 1100 处一分为二
    QVERIFY(session.splitLithoInterval(0, 1100.0));
    QCOMPARE(session.document().lithologyIntervals.size(), 4);
    QCOMPARE(session.document().lithologyIntervals.at(0).bottomDepth, 1100.0f);
    QCOMPARE(session.document().lithologyIntervals.at(1).topDepth, 1100.0f);
    // 界外拆分拒绝
    QVERIFY(!session.splitLithoInterval(0, 999.0));
    QVERIFY(!session.splitLithoInterval(0, 1200.0));

    // 无可合并时幂等 0
    EditSession clean(makeWellData());
    clean.mergeAdjacentLithoIntervals();
    QCOMPARE(clean.mergeAdjacentLithoIntervals(), 0);
  }

  // ---- D3.7 批量导入 ----
  void testBatchApply()
  {
    EditSession session(makeWellData());
    QVector<QPair<QString, double>> rows = {
        {QStringLiteral("T35"), 1550.0},  // 既有同名不同深 → moved
        {QStringLiteral("T60"), 1950.0},  // 新增
        {QStringLiteral(""), 123.0},      // 拒绝
        {QStringLiteral("BAD"), -1.0}};   // 拒绝
    const auto res = session.applyBatchMarkers(rows);
    QCOMPARE(res.inserted, 1);
    QCOMPARE(res.moved, 1);
    QCOMPARE(res.rejected.size(), 2);
    QCOMPARE(session.document().standardHorizons.size(), 3);
    QCOMPARE(session.document().standardHorizons.at(0).first, 1550.0);

    // undo 全量回滚
    session.stack()->undo();
    QCOMPARE(session.document().standardHorizons.size(), 2);
    QCOMPARE(session.document().standardHorizons.at(0).first, 1500.0);
  }

  void testTopsParse()
  {
    // CSV + 表头 + 坏行 + base 列
    const QString text = QStringLiteral(
        "name,top,base\n"
        "T50,1900,1950\n"
        "T60,2000\n"
        "坏行\n"
        "T70,abc\n"
        "T71,2100,2050\n");
    const auto rep = TopsEditor::parseImportText(text);
    QCOMPARE(rep.parsedRows.size(), 3);
    QCOMPARE(rep.parsedRows.at(0).name, QStringLiteral("T50"));
    QCOMPARE(rep.parsedRows.at(0).base, 1950.0);
    QCOMPARE(rep.parsedRows.at(2).base, -1.0); // 底<顶行保留但忽略底深
    QCOMPARE(rep.errors.size(), 3);            // 坏行 + 顶深无效 + 底<顶

    // TSV 且无表头
    const auto rep2 = TopsEditor::parseImportText(QStringLiteral("T1\t1000\nT2\t1100"));
    QCOMPARE(rep2.parsedRows.size(), 2);
    QVERIFY(!rep2.hasErrors());
  }

  void testTopsValidateConflicts()
  {
    const auto parsed = TopsEditor::parseImportText(QStringLiteral("T35,1560\nT40,1700"));
    QVector<QPair<QString, double>> existing = {{QStringLiteral("T35"), 1500.0},
                                                {QStringLiteral("T40"), 1700.0}};
    const auto rep = TopsEditor::validateAgainst(parsed, existing);
    QCOMPARE(rep.conflicts.size(), 1);
    QVERIFY(rep.conflicts.first().contains(QStringLiteral("T35")));
    QVERIFY(rep.conflicts.first().contains(QStringLiteral("1500")));
    // T40 同深无冲突

    // 导入内同名不同深
    const auto dup = TopsEditor::parseImportText(QStringLiteral("T9,100\nT9,200"));
    const auto rep2 = TopsEditor::validateAgainst(dup, {});
    QVERIFY(rep2.hasErrors());

    // 摘要文本
    const QString summary = TopsEditor::conflictSummary(rep);
    QVERIFY(summary.contains(QStringLiteral("冲突 1 项")));
  }

  // ---- D3.5 三级联动校验 ----
  void testFaciesValidation()
  {
    // 合法：微⊂亚⊂相
    QVector<FaciesInterval> ok = {
        {1000.0f, 2000.0f, QStringLiteral("三角洲相"), QStringLiteral("前缘"), QStringLiteral("河口坝")},
        {1000.0f, 1500.0f, QStringLiteral("三角洲相"), QStringLiteral("前缘"), QStringLiteral("河口坝")},
    };
    QVERIFY(IntervalEditor::validateFaciesIntervals(ok).isEmpty());

    // 微相缺亚相归属
    QVector<FaciesInterval> bad1 = {
        {1000.0f, 1100.0f, QStringLiteral("三角洲相"), QString(), QStringLiteral("河口坝")}};
    QVERIFY(!IntervalEditor::validateFaciesIntervals(bad1).isEmpty());

    // 微相越出亚相区间
    QVector<FaciesInterval> bad2 = {
        {1000.0f, 1200.0f, QStringLiteral("相"), QStringLiteral("亚"), QStringLiteral("微1")},
        {1100.0f, 1500.0f, QString(), QStringLiteral("亚"), QStringLiteral("微2")}}; // 微2 [1100,1500] 越出亚 [1000,1200]
    const auto issues = IntervalEditor::validateFaciesIntervals(bad2);
    QVERIFY(!issues.isEmpty());

    // 底 ≤ 顶
    QVector<FaciesInterval> bad3 = {
        {1000.0f, 1000.0f, QStringLiteral("相"), QStringLiteral("亚"), QStringLiteral("微")}};
    QVERIFY(!IntervalEditor::validateFaciesIntervals(bad3).isEmpty());
  }

  void testLithoValidation()
  {
    const QVector<LithologyInterval> ok = {
        {1000.0f, 1100.0f, QStringLiteral("泥岩")}};
    QVERIFY(IntervalEditor::validateLithoIntervals(ok).isEmpty());

    const QVector<LithologyInterval> bad = {
        {1100.0f, 1000.0f, QStringLiteral("倒置")}, // 底≤顶
        {1050.0f, 1200.0f, QStringLiteral("重叠")}}; // 与上重叠（排序后）
    const auto issues = IntervalEditor::validateLithoIntervals(bad);
    QCOMPARE(issues.size(), 2);
  }

  // ---- D3.6 地层指派 ----
  void testStratAssignApply()
  {
    // 未识别层名清单
    QVector<StratigraphyInterval> intervals = {
        {1000.0f, 1500.0f, QString(), QString(), QStringLiteral("A")},
        {1500.0f, 2000.0f, QStringLiteral("新近系"), QStringLiteral("中新统"), QStringLiteral("韩江组")}};
    const QStringList unknown = StratAssign::unrecognizedLayerNames(intervals);
    QCOMPARE(unknown, QStringList{QStringLiteral("A")});

    // 用户显式指派（程序不猜）
    QList<StratAssignment> assigns;
    StratAssignment a;
    a.layerName = QStringLiteral("A");
    a.system = QStringLiteral("古近系");
    a.series = QStringLiteral("渐新统");
    a.formation = QStringLiteral("珠海组");
    assigns << a;
    const auto applied = StratAssign::applyAssignments(intervals, assigns);
    QCOMPARE(applied.at(0).system, QStringLiteral("古近系"));
    QCOMPARE(applied.at(0).series, QStringLiteral("渐新统"));
    // 已识别层不被覆盖
    QCOMPARE(applied.at(1).system, QStringLiteral("新近系"));
    // 原始数据不被污染
    QVERIFY(intervals.at(0).system.isEmpty());

    // 会话侧：applyStratAssignments 可撤销
    EditSession session(makeWellData());
    QVERIFY(session.applyStratAssignments(assigns));
    QCOMPARE(session.document().stratigraphyIntervals.at(0).system, QStringLiteral("古近系"));
    session.stack()->undo();
    QVERIFY(session.document().stratigraphyIntervals.at(0).system.isEmpty());
  }

  // ---- D3.15 只读降级 ----
  void testReadOnlyGuard()
  {
    EditSession session(makeWellData());
    session.setReadOnly(true, QStringLiteral("RAW 资产：源头文件为只读版本"));
    QVERIFY(session.isReadOnly());
    QCOMPARE(session.readOnlyReason(), QStringLiteral("RAW 资产：源头文件为只读版本"));

    // 全部编辑操作被拒绝
    QVERIFY(!session.moveMarker(QStringLiteral("T35"), 1510.0));
    QVERIFY(!session.insertMarker(QStringLiteral("X"), 100.0));
    QVERIFY(!session.removeMarker(QStringLiteral("T35")));
    QVERIFY(!session.renameMarker(QStringLiteral("T35"), QStringLiteral("Y")));
    QVERIFY(!session.editLithoInterval(0, {}));
    QVERIFY(!session.mergeAdjacentLithoIntervals());

    QSignalSpy spy(&session, &EditSession::readOnlyChanged);
    session.setReadOnly(false);
    QCOMPARE(spy.count(), 1);
    QVERIFY(session.moveMarker(QStringLiteral("T35"), 1510.0)); // 解锁后可编辑
  }

  // ---- D3.10 源数据 mtime 冲突 ----
  void testMtimeConflict()
  {
    EditSession session(makeWellData());
    // 未初始化（0）不判冲突
    QVERIFY(!session.sourceChanged(12345));

    session.setSourceMtime(1000);
    QVERIFY(!session.sourceChanged(1000));
    QVERIFY(session.sourceChanged(2000)); // 源被外部改动

    QSignalSpy spy(&session, &EditSession::sourceConflictDetected);
    session.checkSourceConflict(2000);
    QCOMPARE(spy.count(), 1);
    session.checkSourceConflict(2000);
    QCOMPARE(spy.count(), 2); // 持续冲突持续报
  }

  // ---- D3.11 审计 ----
  void testAuditAndDerived()
  {
    EditSession session(makeWellData());
    session.moveMarker(QStringLiteral("T35"), 1520.0);
    session.renameMarker(QStringLiteral("T40"), QStringLiteral("T40x"));
    QCOMPARE(session.auditLines().size(), 2);
    QVERIFY(session.auditLines().at(0).contains(QStringLiteral("marker.move")));
    QVERIFY(session.auditLines().at(0).contains(QStringLiteral("T35")));

    // manifest 风格摘要
    const QString summary = session.buildManifestStyleSummary();
    QVERIFY(summary.contains(QStringLiteral("well=TEST-1")));
    QVERIFY(summary.contains(QStringLiteral("operations=2")));
    QVERIFY(summary.contains(QStringLiteral("marker.rename")));

    // 派生文档数据完整
    const auto derived = session.buildDerivedDocument();
    QCOMPARE(derived.standardHorizons.at(0).first, 1520.0);
    QCOMPARE(derived.lithologyIntervals.size(), 3);
    QCOMPARE(derived.wellName, QStringLiteral("TEST-1"));
  }

  // ---- D8.3 全链路：编辑 → 派生 XML 落盘 → 重解析等价（D3.3）----
  void testIoRoundTripDerivedChain()
  {
    QTemporaryDir dir;
    const QString srcPath = dir.path() + QStringLiteral("/well.xml");

    EditSession session(makeWellData());
    session.moveMarker(QStringLiteral("T35"), 1520.5);
    session.editLithoInterval(0, []() {
      LithologyInterval li;
      li.topDepth = 1000.0f;
      li.bottomDepth = 1200.0f;
      li.lithoName = QStringLiteral("白云岩");
      return li;
    }());

    // D3.3 写回派生版本（io 层序列化 + 审计表）
    const auto derived = session.buildDerivedDocument();
    QVERIFY(writeComprehensiveWellXmlFile(derived, srcPath, session.auditLines()));

    // 重解析：数据等价（标志层移动 + 岩性改名均在）
    ComprehensiveWellData reparsed;
    QString err;
    QVERIFY2(parseComprehensiveWellXml(srcPath, reparsed, &err), qPrintable(err));
    QCOMPARE(reparsed.wellName, QStringLiteral("TEST-1"));

    bool foundMoved = false;
    for (const auto &m : reparsed.standardHorizons)
      if (m.second == QStringLiteral("T35"))
      {
        QCOMPARE(m.first, 1520.5);
        foundMoved = true;
      }
    QVERIFY(foundMoved);

    bool foundLitho = false;
    for (const auto &li : reparsed.lithologyIntervals)
      if (li.lithoName == QStringLiteral("白云岩") && li.topDepth == 1000.0f)
        foundLitho = true;
    QVERIFY(foundLitho);
  }

  // ---- D3.11 派生 XML 审计工作表 ----
  void testDerivedXmlAuditSheet()
  {
    EditSession session(makeWellData());
    session.moveMarker(QStringLiteral("T35"), 1510.0);

    const QByteArray xml = writeComprehensiveWellXml(session.document(), session.auditLines());
    const QString text = QString::fromUtf8(xml);
    QVERIFY(text.contains(QStringLiteral("编辑审计")));
    QVERIFY(text.contains(QStringLiteral("marker.move")));
    QVERIFY(text.contains(QStringLiteral("T35")));
    // SpreadsheetML 骨架合法
    QVERIFY(text.startsWith(QStringLiteral("<?xml")));
    QVERIFY(text.contains(QStringLiteral("<Workbook")));
  }

  // ---- D6.1/D6.5 io 表解析 ----
  void testDeviationAndTimeDepthParse()
  {
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/dev.xml");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"(<?xml version="1.0"?>
<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">
<Worksheet ss:Name="井斜数据"><Table>
<Row><Cell><Data ss:Type="String">MD</Data></Cell><Cell><Data ss:Type="String">INC</Data></Cell><Cell><Data ss:Type="String">AZI</Data></Cell></Row>
<Row><Cell><Data ss:Type="String">1000</Data></Cell><Cell><Data ss:Type="String">0</Data></Cell><Cell><Data ss:Type="String">0</Data></Cell></Row>
<Row><Cell><Data ss:Type="String">1100</Data></Cell><Cell><Data ss:Type="String">15</Data></Cell><Cell><Data ss:Type="String">90</Data></Cell></Row>
</Table></Worksheet>
<Worksheet ss:Name="时深数据"><Table>
<Row><Cell><Data ss:Type="String">1000</Data></Cell><Cell><Data ss:Type="String">850</Data></Cell></Row>
<Row><Cell><Data ss:Type="String">2000</Data></Cell><Cell><Data ss:Type="String">1450</Data></Cell></Row>
</Table></Worksheet>
</Workbook>)");
    f.close();

    QVector<XmlDeviationStation> dev;
    QVERIFY(parseDeviationSurvey(path, dev));
    QCOMPARE(dev.size(), 2); // 表头跳过
    QCOMPARE(dev.at(1).inclinationDeg, 15.0);

    QVector<XmlTimeDepthPair> td;
    QVERIFY(parseTimeDepthTable(path, td));
    QCOMPARE(td.size(), 2);
    QCOMPARE(td.at(1).second, 1450.0);

    // 无该表返回 false
    QVERIFY(!parseDeviationSurvey(QStringLiteral("/nonexistent"), dev));
  }

  // ---- D3.9 面板脏状态与保存派生 ----
  void testPanelSaveDerived()
  {
    WellCompositePanel panel;
    // 未加载井 → 无会话
    QVERIFY(!panel.saveDerived());

    QVector<CurveData> curves;
    CurveData gr;
    gr.name = QStringLiteral("GR");
    gr.depths = {1000.0f, 1200.0f};
    gr.values = {40.0f, 60.0f};
    curves << gr;
    QVERIFY(panel.loadLasCurves(QStringLiteral("W-9"), curves));

    EditSession *session = panel.editSession();
    QVERIFY(session);
    QVERIFY(!session->isDirty());
    QVERIFY(!panel.saveDerived()); // 干净不可保存

    QSignalSpy spy(&panel, &WellCompositePanel::derivedDocumentReady);
    session->insertMarker(QStringLiteral("T90"), 1100.0);
    QVERIFY(session->isDirty());
    QVERIFY(panel.saveDerived());
    QCOMPARE(spy.count(), 1);
    // 信号携带派生文档 + 摘要
    QCOMPARE(spy.first().at(0).value<ComprehensiveWellData>().wellName, QStringLiteral("W-9"));
    QVERIFY(spy.first().at(1).toString().contains(QStringLiteral("marker.insert")));
    // 保存后干净
    QVERIFY(!session->isDirty());
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositeEditing tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_editing.moc"
