#include "domain/wellsection.h"
#include <QSet>
#include <QtTest>
#include <cmath>

using namespace wellsection;

class TestWellSection : public QObject {
  Q_OBJECT
private slots:
  void zonesRules() {
    Well w;
    w.tops = {{"A", 100}, {"B", 100}, {"C", 200}, {"D", 300}};
    // A 零厚度（A 底 = B 顶 = 100）跳过；末段 [300,400]；首顶以上不出段。
    const auto z = zones(w, 400.0);
    QCOMPARE(z.size(), 3);
    QCOMPARE(z[0].name, QString("B"));
    QCOMPARE(z[0].topMd, 100.0);
    QCOMPARE(z[0].baseMd, 200.0);
    QCOMPARE(z[1].name, QString("C"));
    QCOMPARE(z[1].topMd, 200.0);
    QCOMPARE(z[1].baseMd, 300.0);
    QCOMPARE(z[2].name, QString("D"));
    QCOMPARE(z[2].topMd, 300.0);
    QCOMPARE(z[2].baseMd, 400.0);
    // bottomMd 不超过末顶 → 无末段（中间段底仍取下一顶）。
    const auto z2 = zones(w, 250.0);
    QCOMPARE(z2.size(), 2);
    QCOMPARE(z2.back().name, QString("C"));
    QCOMPARE(z2.back().baseMd, 300.0);
    QCOMPARE(zones(w, 300.0).size(), 2); // bottomMd == 末顶同样不出末段
    Well empty;
    QVERIFY(zones(empty, 500.0).isEmpty());
  }
  void linksOrderAndMissing() {
    Well l, r;
    l.tops = {{"A", 100}, {"B", 200}, {"C", 300}, {"A", 350}};
    r.tops = {{"C", 310}, {"A", 90}};
    const auto ls = links(l, r);
    QCOMPARE(ls.size(), 2); // 左井重名 A 只出一条
    QCOMPARE(ls[0].name, QString("A"));
    QCOMPARE(ls[0].leftMd, 100.0);
    QCOMPARE(ls[0].rightMd, 90.0);
    QCOMPARE(ls[1].name, QString("C"));
    QCOMPARE(ls[1].leftMd, 300.0);
    QCOMPARE(ls[1].rightMd, 310.0);
  }
  void orderedTopNamesMerge() {
    Well w1, w2, w3;
    w1.tops = {{"A", 100}, {"B", 200}, {"C", 300}};
    // X 仅见于 w2，夹在两共有名之间；Z 比共有名都深。
    w2.tops = {{"A", 110}, {"X", 150}, {"B", 210}, {"C", 290}, {"Z", 320}};
    // W 比 w2 全部已知名都浅 → 插最前。
    w3.tops = {{"W", 50}, {"B", 205}};
    QCOMPARE(orderedTopNames({w1, w2}),
             QStringList({"A", "X", "B", "C", "Z"}));
    QCOMPARE(orderedTopNames({w1, w2, w3}),
             QStringList({"W", "A", "X", "B", "C", "Z"}));
    QVERIFY(orderedTopNames({}).isEmpty());
  }
  void flattenOffsetAndAccessors() {
    Well w;
    w.x = 1;
    w.y = 2;
    w.tops = {{"A", 150}};
    Curve gr;
    gr.mnemonic = "NGR";
    w.curves = {gr};
    QVERIFY(w.hasCoordinates());
    QCOMPARE(flattenOffset(w, "A"), 150.0);
    QCOMPARE(flattenOffset(w, "Z"), 0.0);
    QCOMPARE(flattenOffset(w, ""), 0.0);
    QCOMPARE(w.topMd("A"), 150.0);
    QVERIFY(std::isnan(w.topMd("a"))); // 精确匹配
    QCOMPARE(w.curve("ngr"), &w.curves.first()); // 大小写不敏感
    QVERIFY(!w.curve("GRX"));
    Well bare;
    QVERIFY(!bare.hasCoordinates());
  }
  void depthWindowVariants() {
    Well w1, w2;
    w1.tops = {{"A", 100}, {"B", 200}};
    w2.tops = {{"A", 80}, {"B", 180}};
    // 无拉平：并集 [80,200]，span 120 → pad max(5,4.8)=5 → {75,205}。
    const auto win = depthWindow({w1, w2}, QString());
    QCOMPARE(win.top, 75.0);
    QCOMPARE(win.base, 205.0);
    // 拉平 B：两井 B 顶都落在显示深 0。
    QCOMPARE(w1.topMd("B") - flattenOffset(w1, "B"), 0.0);
    QCOMPARE(w2.topMd("B") - flattenOffset(w2, "B"), 0.0);
    const auto flat = depthWindow({w1, w2}, "B");
    QCOMPARE(flat.top, -105.0); // 并集 [-100,0] → pad 5
    QCOMPARE(flat.base, 5.0);
    // 全井无顶 → 有限曲线深度并集 [500,700]，span 200 → pad 8。
    Well c1, c2;
    Curve c;
    c.depths = {500, 550, std::numeric_limits<float>::quiet_NaN(), 700};
    c.values = {1, 2, 3, 4};
    c1.curves = {c};
    const auto cw = depthWindow({c1, c2}, QString());
    QCOMPARE(cw.top, 492.0);
    QCOMPARE(cw.base, 708.0);
    // 什么都没有 → {0,100}。
    const auto none = depthWindow({Well{}, Well{}}, QString());
    QCOMPARE(none.top, 0.0);
    QCOMPARE(none.base, 100.0);
    // 单顶：span 0 → 最小 1 m 后 pad → {494.5,505.5}。
    Well s;
    s.tops = {{"A", 500}};
    const auto sw = depthWindow({s}, QString());
    QCOMPARE(sw.top, 494.5);
    QCOMPARE(sw.base, 505.5);
  }
  void formationIntervalCases() {
    Well w;
    w.tops = {{"A", 100}, {"B", 200}};
    const auto ab = formationInterval(w, "A", "B");
    QVERIFY(ab.valid());
    QVERIFY(ab.hasBase());
    QCOMPARE(ab.topMd, 100.0);
    QCOMPARE(ab.baseMd, 200.0);
    const auto noBase = formationInterval(w, "A", "Z");
    QVERIFY(noBase.valid());
    QVERIFY(!noBase.hasBase());
    QVERIFY(std::isnan(noBase.baseMd));
    const auto shallower = formationInterval(w, "B", "A");
    QVERIFY(shallower.valid());
    QVERIFY(!shallower.hasBase());
    QVERIFY(!formationInterval(w, "Z", "B").valid());
    QVERIFY(!formationInterval(w, "", "B").valid());
  }
  void inferSandShaleRules() {
    Curve gr;
    gr.unit = "GAPI";
    // 基础分段：砂/泥/砂，异类相邻取中点，端点取样点深。
    gr.depths = {100, 101, 102, 103, 104, 105, 106, 107};
    gr.values = {30, 30, 80, 80, 80, 30, 30, 30};
    const auto base = inferSandShale(gr, 50.0);
    QCOMPARE(base.size(), 3);
    QVERIFY(base[0].sand);
    QCOMPARE(base[0].topMd, 100.0);
    QCOMPARE(base[0].baseMd, 101.5);
    QVERIFY(!base[1].sand);
    QCOMPARE(base[1].topMd, 101.5);
    QCOMPARE(base[1].baseMd, 104.5);
    QVERIFY(base[2].sand);
    QCOMPARE(base[2].topMd, 104.5);
    QCOMPARE(base[2].baseMd, 107.0);
    // 薄尖峰（<0.5 m）并入前段，相邻同类合并 → 单段砂。
    Curve spike;
    spike.depths = {100, 102, 102.4f, 102.8f, 105};
    spike.values = {30, 30, 80, 30, 30};
    const auto sp = inferSandShale(spike, 50.0, 0.5);
    QCOMPARE(sp.size(), 1);
    QVERIFY(sp[0].sand);
    QCOMPARE(sp[0].topMd, 100.0);
    QCOMPARE(sp[0].baseMd, 105.0);
    // 薄段在块首 → 并入后段。
    Curve head;
    head.depths = {100, 100.4f, 102};
    head.values = {80, 30, 30};
    const auto hd = inferSandShale(head, 50.0, 0.5);
    QCOMPARE(hd.size(), 1);
    QVERIFY(hd[0].sand);
    QCOMPARE(hd[0].topMd, 100.0);
    QCOMPARE(hd[0].baseMd, 102.0);
    // NaN 断块：跨缺段不并。
    Curve gap;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    gap.depths = {100, 101, 102, 103, 104};
    gap.values = {30, 30, nan, 30, 30};
    const auto gp = inferSandShale(gap, 50.0);
    QCOMPARE(gp.size(), 2);
    QCOMPARE(gp[0].baseMd, 101.0);
    QCOMPARE(gp[1].topMd, 103.0);
    // 降序输入反转为升序返回。
    Curve desc;
    desc.depths = {105, 104, 103, 102, 101, 100};
    desc.values = {30, 80, 80, 80, 30, 30};
    const auto dc = inferSandShale(desc, 50.0);
    QCOMPARE(dc.size(), 3);
    QVERIFY(dc[0].sand);
    QCOMPARE(dc[0].topMd, 100.0);
    QCOMPARE(dc[0].baseMd, 101.5);
    QVERIFY(!dc[1].sand);
    QCOMPARE(dc[1].topMd, 101.5);
    QCOMPARE(dc[1].baseMd, 104.5);
    QVERIFY(dc[2].sand);
    QCOMPARE(dc[2].topMd, 104.5);
    QCOMPARE(dc[2].baseMd, 105.0);
  }
  void seismicGapSampling() {
    SeismicGap g;
    QVERIFY(!g.valid());
    QVERIFY(std::isnan(g.sampleAt(0, 0)));
    g.columns = 2;
    g.samples = 3;
    g.startMs = 100;
    g.stepMs = 4;
    g.values = {1, 2, 3, 4, 5, 6}; // [sample*columns+col]
    QVERIFY(g.valid());
    QCOMPARE(g.sampleAt(0.0, 100.0), 1.0f);
    QCOMPARE(g.sampleAt(1.0, 100.0), 2.0f);
    QCOMPARE(g.sampleAt(0.0, 104.0), 3.0f);
    QCOMPARE(g.sampleAt(0.0, 102.0), 2.0f); // 样点间线性
    QCOMPARE(g.sampleAt(0.0, 106.0), 4.0f);
    QCOMPARE(g.sampleAt(0.0, 108.0), 5.0f); // 末样点
    QVERIFY(std::isnan(g.sampleAt(0.0, 110.0)));
    QVERIFY(std::isnan(g.sampleAt(0.0, 99.0)));
    QVERIFY(std::isnan(g.sampleAt(-0.1, 100.0)));
    QVERIFY(std::isnan(g.sampleAt(1.1, 100.0)));
    g.values[3] = std::numeric_limits<float>::quiet_NaN(); // sample1,col1
    QVERIFY(std::isnan(g.sampleAt(1.0, 104.0)));
    QVERIFY(std::isnan(g.sampleAt(1.0, 103.0))); // 插值越过无效
    SeismicStrip strip;
    QVERIFY(!strip.anyValid());
    strip.gaps.push_back(g);
    QVERIFY(strip.anyValid());
  }
  void adaptiveClipValues() {
    SeismicGap g;
    g.columns = 10;
    g.samples = 10;
    g.stepMs = 1;
    for (int i = 0; i < 100; ++i)
      g.values.push_back(float(i + 1)); // |v| = 1..100
    QCOMPARE(adaptiveClip({g}, 0.99), 99.0f); // round(0.99*99)=98 → 第99小
    QCOMPARE(adaptiveClip({g}, 1.0), 100.0f);
    SeismicGap zeros;
    zeros.columns = 2;
    zeros.samples = 2;
    zeros.stepMs = 1;
    zeros.values = {0, 0, 0, 0};
    QCOMPARE(adaptiveClip({zeros}), 1.0f);
    QCOMPARE(adaptiveClip({}), 1.0f);
    SeismicGap bad; // 无效缝不计
    bad.reason = "x";
    QCOMPARE(adaptiveClip({bad, g}, 0.99), 99.0f);
  }
  void timeDepthAndGapTwt() {
    seismic::TimeDepthModel ma, mb;
    QVERIFY(ma.setCheckshots({{1000, 100}, {2000, 200}}));
    QVERIFY(mb.setCheckshots({{1000, 200}, {2000, 300}}));
    Well a, b, bare;
    a.timeDepth = TimeDepth{ma, 0.0, QString("时深表")};
    b.timeDepth = TimeDepth{mb, 20.0, QString("常速校正")};
    QCOMPARE(a.timeDepth->twtAt(1500), 150.0);
    QCOMPARE(b.timeDepth->twtAt(1500), 250.0 + 20.0);
    QVERIFY(std::isnan(a.timeDepth->twtAt(2500))); // 不外推
    QVERIFY(std::isnan(a.timeDepth->twtAt(qQNaN())));
    // gapTwtMs：f=0/1 端点 + f=0.5 中点；offA/offB 为拉平偏移。
    QCOMPARE(gapTwtMs(a, 100, b, 100, 0.0, 1400), 150.0);
    QCOMPARE(gapTwtMs(a, 100, b, 100, 1.0, 1400), 270.0);
    QCOMPARE(gapTwtMs(a, 100, b, 100, 0.5, 1400), 210.0);
    QVERIFY(std::isnan(gapTwtMs(a, 0, bare, 0, 0.5, 1400)));
    QVERIFY(std::isnan(gapTwtMs(a, 0, b, 0, 0.5, 4000))); // 超表外推
  }
  void metatypesRegistered() {
    // 信号槽排队传递需要可拷贝元类型（Q_DECLARE_METATYPE 生效）。
    QVERIFY(qMetaTypeId<QVector<wellsection::Well>>() >= 0);
    QVERIFY(qMetaTypeId<wellsection::SeismicStrip>() >= 0);
  }

  // ---- 基准面三模式（Oracle #1：拉平不变量）----
  void datumModes() {
    Well w1, w2;
    w1.kb = 100.0;
    w1.tops = {{"A", 1000}, {"B", 2000}};
    w2.kb = 80.0;
    w2.tops = {{"A", 950}, {"B", 1950}};
    const QVector<Well> ws = {w1, w2};

    // 井深：零偏移；海拔：各自 kb；拉平：各自基准顶 MD（缺顶/空名 → 0）。
    for (const Well &w : ws) {
      QCOMPARE(datumOffset(w, Datum{DatumMode::Depth, QString()}), 0.0);
      QCOMPARE(datumOffset(w, Datum{DatumMode::Elevation, QString()}), w.kb);
      QCOMPARE(datumOffset(w, Datum{DatumMode::Flatten, "A"}),
               flattenOffset(w, "A"));
      QCOMPARE(datumOffset(w, Datum{DatumMode::Flatten, "Z"}), 0.0);
      QCOMPARE(datumOffset(w, Datum{DatumMode::Flatten, QString()}), 0.0);
    }
    // 拉平 A：两井 A 顶显示深同为 0；并集 [0,1000] span 1000 → pad 40。
    const auto flat = depthWindow(ws, Datum{DatumMode::Flatten, "A"});
    QCOMPARE(flat.top, -40.0);
    QCOMPARE(flat.base, 1040.0);
    // 海拔：显示并集 [870,1900] span 1030 → pad 41.2。
    const auto elev = depthWindow(ws, Datum{DatumMode::Elevation, QString()});
    QCOMPARE(elev.top, 870.0 - 41.2);
    QCOMPARE(elev.base, 1900.0 + 41.2);
    // 旧 QString 重载委托新口径（Flatten）。
    QCOMPARE(depthWindow(ws, QString("A")).top, flat.top);
    QCOMPARE(datumLabel(DatumMode::Depth), QStringLiteral("井深 m"));
    QCOMPARE(datumLabel(DatumMode::Elevation), QStringLiteral("海拔 m"));
    QCOMPARE(datumLabel(DatumMode::Flatten), QStringLiteral("拉平 m"));
  }

  void topsTableInvariant() {
    Well w1, w2;
    w1.name = "W1";
    w1.kb = 100.0;
    w1.tops = {{"A", 1000.25}, {"B", 2000}};
    w2.name = "W2";
    w2.tops = {{"A", 950}, {"B", 1950}};
    const QVector<Well> ws = {w1, w2};
    QVector<TopsTable> tables;
    tables << topsTable(ws, Datum{DatumMode::Depth, QString()})
           << topsTable(ws, Datum{DatumMode::Elevation, QString()})
           << topsTable(ws, Datum{DatumMode::Flatten, "A"});
    // 行数一致（2 井 × 2 顶），MD 列逐行相等——模式切换不改井深表数值
    //（拉平不变量：仅视图基准变化）。
    for (const TopsTable &t : tables)
      QCOMPARE(t.rows.size(), 4);
    for (int r = 0; r < 4; ++r)
      for (int i = 1; i < tables.size(); ++i) {
        QCOMPARE(tables[i].rows[r][0], tables[0].rows[r][0]); // 井名
        QCOMPARE(tables[i].rows[r][1], tables[0].rows[r][1]); // 顶名
        QCOMPARE(tables[i].rows[r][2], tables[0].rows[r][2]); // MD
      }
    // 表头标记随模式；拉平带层名。
    QCOMPARE(tables[0].header.at(3), QStringLiteral("基准面"));
    QCOMPARE(tables[0].header.at(4), QStringLiteral("井深 m"));
    QCOMPARE(tables[1].header.at(4), QStringLiteral("海拔 m"));
    QCOMPARE(tables[2].header.at(4), QStringLiteral("A"));
    // CSV：表头 + 4 行 + 末换行；数值 f2。
    const QString csv = tables[0].csv();
    QVERIFY(csv.endsWith(QLatin1Char('\n')));
    QCOMPARE(csv.count(QLatin1Char('\n')), 5);
    QVERIFY(csv.contains(QStringLiteral("W1,A,1000.25")));
  }

  // ---- 连线改接（井对无序键；缺省连接）----
  void linkOverrideRules() {
    const auto a = makeLinkOverride("B", "A", "T1", false); // 归一键序
    QCOMPARE(a.leftWellId, QStringLiteral("A"));
    QCOMPARE(a.rightWellId, QStringLiteral("B"));
    const auto b = makeLinkOverride("A", "B", "T1", false);
    QCOMPARE(a, b);
    QVector<LinkOverride> o = {a};
    QVERIFY(!linkConnected(o, "A", "B", "T1"));
    QVERIFY(!linkConnected(o, "B", "A", "T1")); // 无序查询
    QVERIFY(linkConnected(o, "A", "B", "T2"));  // 其它顶缺省连接
    QVERIFY(linkConnected(o, "A", "C", "T1"));  // 其它井对缺省连接
    QVERIFY(linkConnected({}, "A", "B", "T1")); // 空表全连接
    // 重连：同键 upsert 后恢复连接。
    o[0].connected = true;
    QVERIFY(linkConnected(o, "A", "B", "T1"));
  }

  // ---- 井距模式（等距 / 按井口距离比例）----
  void spacingGapWidths() {
    Well w1, w2, w3, w4;
    w1.x = 0;   w1.y = 0;
    w2.x = 100; w2.y = 0;
    w3.x = 300; w3.y = 0; // 距 w2 = 200（两倍于 w1-w2）
    w4.x = 300; w4.y = 0; // 与 w3 同点 → 距离 0（夹 minGap 保护）
    const QVector<Well> ws3 = {w1, w2, w3};
    const QVector<Well> ws4 = {w1, w2, w3, w4};

    // 等距：均分。
    const auto eq = gapWidthsFor(ws3, SpacingMode::Equal, 300, 48, 600);
    QCOMPARE(eq, QVector<double>({150.0, 150.0}));
    // 比例：100:200 → 100:200。
    const auto pr = gapWidthsFor(ws3, SpacingMode::Proportional, 300, 48, 600);
    QCOMPARE(pr.size(), 2);
    QVERIFY(std::fabs(pr[0] - 100.0) < 1e-9);
    QVERIFY(std::fabs(pr[1] - 200.0) < 1e-9);
    // 零距段夹 minGap；总宽超上限夹 maxGap。
    const auto cl = gapWidthsFor(ws4, SpacingMode::Proportional, 3000, 48, 600);
    QCOMPARE(cl, QVector<double>({600.0, 600.0, 48.0}));
    // 全缺坐标 → 等距退化。
    Well n1, n2;
    const auto de = gapWidthsFor({n1, n2}, SpacingMode::Proportional, 200, 48,
                                 600);
    QCOMPARE(de, QVector<double>({200.0}));
    // 缺坐标段用中位距离补：前两段 100/200，后两段缺 → 中位=200，
    // 距离 [100,200,200,200]，预算 350 → 50/100/100/100。
    Well nox; // 无坐标
    const auto mix = gapWidthsFor({w1, w2, w3, nox, w4},
                                  SpacingMode::Proportional, 350, 48, 600);
    QCOMPARE(mix.size(), 4);
    QVERIFY(std::fabs(mix[0] - 50.0) < 1e-9);
    QVERIFY(std::fabs(mix[1] - 100.0) < 1e-9);
    QVERIFY(std::fabs(mix[2] - 100.0) < 1e-9);
    QVERIFY(std::fabs(mix[3] - 100.0) < 1e-9);
    // 单井 → 空。
    QVERIFY(gapWidthsFor({w1}, SpacingMode::Proportional, 300, 48, 600)
                .isEmpty());
    // 全零距离（同平台井）：退化等距，防 NaN 毒化布局。
    Well z1, z2, z3;
    z1.x = 50; z1.y = 50;
    z2.x = 50; z2.y = 50;
    z3.x = 50; z3.y = 50;
    const auto zeros =
        gapWidthsFor({z1, z2, z3}, SpacingMode::Proportional, 300, 48, 600);
    QCOMPARE(zeros, QVector<double>({150.0, 150.0}));
    for (double g : zeros)
      QVERIFY(std::isfinite(g));
  }

  // ---- 选井 PCA 序（平面选井一键成剖面的井序）----
  void orderWellsByPositionRules() {
    QVector<Well> pos;
    const double xs[3] = {300.0, 0.0, 100.0};
    const char *names[3] = {"A", "B", "C"};
    for (int i = 0; i < 3; ++i) {
      Well w;
      w.id = QLatin1String(names[i]);
      w.x = xs[i];
      w.y = i * 5.0;
      pos << w;
    }
    // 主轴近 x：按 x 升序。
    QCOMPARE(orderWellsByPosition({"A", "B", "C"}, pos),
             QStringList({"B", "C", "A"}));
    // 子集 + 乱序输入。
    QCOMPARE(orderWellsByPosition({"A", "B"}, pos), QStringList({"B", "A"}));
    // 缺坐标井保原相对序排末。
    Well nox;
    nox.id = QStringLiteral("D");
    pos << nox;
    QCOMPARE(orderWellsByPosition({"D", "A", "B"}, pos),
             QStringList({"B", "A", "D"}));
    // 井位全缺 → 原序。
    QCOMPARE(orderWellsByPosition({"A", "B"}, {nox, nox}),
             QStringList({"A", "B"}));
    // 单井无从定轴 → 原序。
    QCOMPARE(orderWellsByPosition({"A"}, pos), QStringList({"A"}));
  }

  // ---- 栅状图自动布点（最小交叉启发式）----
  void fencePlanning() {
    // 3×2 井网（x 步 100 跨 200、y 步 80——PCA 主轴 = x 向）。
    QVector<Well> grid;
    for (int row = 0; row < 2; ++row)
      for (int col = 0; col < 3; ++col) {
        Well w;
        w.id = QStringLiteral("w%1%2").arg(row).arg(col);
        w.x = col * 100.0;
        w.y = row * 80.0;
        grid << w;
      }
    // 单条带：全部井，按 u 单调。
    const auto one = planFence(grid, 1);
    QVERIFY(one.ok());
    QCOMPARE(one.sections.size(), 1);
    QCOMPARE(one.sections[0].wellIds.size(), 6);
    QCOMPARE(one.sections[0].wellIds.front(), QStringLiteral("w00"));
    QCOMPARE(one.sections[0].wellIds.last(), QStringLiteral("w12"));
    // 两条带：各 3 口；井不重复、全覆盖；条带内 u 单调（奇数带倒序——
    // 剪草机端点相接）。
    const auto two = planFence(grid, 2);
    QVERIFY(two.ok());
    QCOMPARE(two.sections.size(), 2);
    QCOMPARE(two.sections[0].wellIds.size(), 3);
    QCOMPARE(two.sections[1].wellIds.size(), 3);
    QStringList all;
    for (const auto &sec : two.sections)
      all << sec.wellIds;
    QCOMPARE(all.size(), 6);
    QSet<QString> uniq(all.begin(), all.end());
    QCOMPARE(uniq.size(), 6); // 无交点（条带互斥）
    QCOMPARE(two.sections[0].wellIds,
             QStringList({"w00", "w01", "w02"})); // v 小的带，u 升序
    QCOMPARE(two.sections[1].wellIds,
             QStringList({"w12", "w11", "w10"})); // 剪草机倒序
    // 退化：缺坐标 / 井数不足。
    Well nox;
    QCOMPARE(planFence({grid[0], nox}, 2).status,
             FencePlan::Status::MissingCoords);
    QCOMPARE(planFence({grid[0]}, 1).status, FencePlan::Status::TooFewWells);
    QCOMPARE(planFence({}, 3).status, FencePlan::Status::TooFewWells);
  }

  // ---- 井路径累计长分数（断层投绘横向映射）----
  void wellPathFractionRules() {
    Well a, b, c;
    a.x = 0;   a.y = 0;
    b.x = 100; b.y = 0;
    c.x = 100; c.y = 100;
    const auto fr = wellPathFractions({a, b, c});
    QCOMPARE(fr, QVector<double>({0.0, 0.5, 1.0})); // 100 + 100 均段
    QCOMPARE(wellPathFractions({a}).size(), 0);     // 单井无路径
    Well nox;                                        // 缺坐标 → 整体退化
    QCOMPARE(wellPathFractions({a, nox}).size(), 0);
    QCOMPARE(wellPathFractions({}).size(), 0);
  }
};

QTEST_APPLESS_MAIN(TestWellSection)
#include "tst_wellsection.moc"
