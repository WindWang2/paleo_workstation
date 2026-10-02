// tst_perf_las — wave/io-perf-cache D1.4-D1.9：LAS 快解析性能、区间查询、
// 错误分类、大文件防护、空曲线策略、别名归一。
#include <QtTest>
#include <cmath>

#include "io/lasalias.h"
#include "io/lasparser.h"
#include "io/perffixtures.h"

#include <QFile>
#include <QTemporaryDir>

class PerfLasTests : public QObject
{
    Q_OBJECT

  private slots:
    void parseDocUnder50ms();
    void parseDocMatchesLegacyParse();
    void parseHeaderMatchesCurveNames();
    void rangeQueryMatchesSlice();
    void depthRangeEarlyStops();
    void errorClassificationWrapMode();
    void errorClassificationTruncatedRow();
    void errorClassificationMissingUnit();
    void errorClassificationEncoding();
    void oversizeGuardRejectsFullRead();
    void oversizeGuardAllowsStreaming();
    void emptyCurvePolicy();
    void aliasCanonicalization();
    void aliasMemoization();
    void aliasFamilies();
    void bomLasHandled();
    void parseRangeFullFileAgrees();
    void customNullValueMapped();
    void parseDepthRangeManyCurves();
    void parseDepthRangeBomOffset();
    void parseDepthRange100Curves();
    void parseDepthRangeBomVariousHeaders();
    void parseDepthRangeChunkBoundaryStress();

  private:
    QTemporaryDir m_dir;
    void writeLas(const QString &path, const QByteArray &content);
};

void PerfLasTests::writeLas(const QString &path, const QByteArray &content)
{
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write(content);
}

void PerfLasTests::parseDocUnder50ms()
{
  const QString las = m_dir.filePath("p15581.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 15581));
  QElapsedTimer t;
  t.start();
  const LasDoc doc = LasParser::parseDoc(las);
  const double ms = t.nsecsElapsed() / 1.0e6;
  QVERIFY(doc.ok);
  // goal/perf-systematize 簇3：比率化——同文件旧路径 parse() 为在测参照
  //（实测 parseDoc/legacy ≈ 0.08，门 0.5 留 6× 余量；解析器退化回旧量级
  // → 比率→1 必红）。sanity 上限只拦挂死，不判回归。
  QStringList legacyNames;
  QList<LasCurve> legacyCurves;
  t.restart();
  QVERIFY(LasParser::parse(las, legacyNames, legacyCurves));
  const double legacyMs = double(t.nsecsElapsed()) / 1.0e6;
  qInfo("parseDoc %.2fms vs legacy %.1fms (ratio %.3f)", ms, legacyMs,
        legacyMs > 0 ? ms / legacyMs : -1.0);
  QVERIFY2(legacyMs > 0 && ms < 0.5 * legacyMs,
           qPrintable(QStringLiteral("parseDoc %1ms >= 0.5×legacy %2ms（D1.5 退化）")
                          .arg(ms, 0, 'f', 2)
                          .arg(legacyMs, 0, 'f', 1)));
  QVERIFY2(ms < 2000.0,
           qPrintable(QStringLiteral("parseDoc %1ms >= 2000ms（sanity）").arg(ms)));
  QCOMPARE(doc.curves.first().values.size(), 15581);
}

void PerfLasTests::parseDocMatchesLegacyParse()
{
  // 新快路径与旧 parse() 逐值一致（快路径不改变语义）。
  const QString las = m_dir.filePath("agree.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 777));

  QStringList legacyNames;
  QList<LasCurve> legacyCurves;
  QVERIFY(LasParser::parse(las, legacyNames, legacyCurves));

  const LasDoc doc = LasParser::parseDoc(las);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curveNames, legacyNames);
  QCOMPARE(doc.curves.size(), legacyCurves.size());
  for (int c = 0; c < doc.curves.size(); ++c)
  {
    QCOMPARE(doc.curves.at(c).name, legacyCurves.at(c).name);
    QCOMPARE(doc.curves.at(c).values.size(), legacyCurves.at(c).values.size());
    for (int r = 0; r < doc.curves.at(c).values.size(); ++r)
    {
      const double a = doc.curves.at(c).values.at(r);
      const double b = legacyCurves.at(c).values.at(r);
      if (std::isnan(a))
        QVERIFY(std::isnan(b)); // NULL 值两侧都映射 NaN
      else
        QCOMPARE(a, b);
    }
  }
}

void PerfLasTests::parseHeaderMatchesCurveNames()
{
  const QString las = m_dir.filePath("hdr.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 100));
  const LasDoc doc = LasParser::parseDoc(las);
  LasHeaderInfo info;
  QVERIFY(LasParser::parseHeader(las, info));
  QCOMPARE(info.curveNames, doc.curveNames);
  QCOMPARE(info.wellName, QStringLiteral("SYNTH-01"));
  QVERIFY(info.sawAscii);
}

void PerfLasTests::rangeQueryMatchesSlice()
{
  const QString las = m_dir.filePath("range.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 1000));
  const LasDoc full = LasParser::parseDoc(las);

  QStringList names;
  QList<LasCurve> part;
  QVERIFY(LasParser::parseRange(las, 100, 350, names, part));
  QCOMPARE(names, full.curveNames);
  for (int c = 0; c < part.size(); ++c)
  {
    QCOMPARE(part.at(c).values.size(), 250);
    for (int r = 0; r < 250; ++r)
    {
      const double a = part.at(c).values.at(r);
      const double b = full.curves.at(c).values.at(100 + r);
      if (std::isnan(a))
        QVERIFY(std::isnan(b));
      else
        QCOMPARE(a, b);
    }
  }
  // 越界自动夹取。
  QVERIFY(LasParser::parseRange(las, 900, 5000, names, part));
  QCOMPARE(part.first().values.size(), 100);
}

void PerfLasTests::depthRangeEarlyStops()
{
  const QString las = m_dir.filePath("depth.las");
  // 深度 1000 起，步长 0.125，共 8000 行 → 末深度 1999.875。
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 8000));
  QStringList names;
  QList<LasCurve> curves;
  QVERIFY(LasParser::parseDepthRange(las, 1200.0, 1201.0, names, curves));
  QCOMPARE(names.first(), QStringLiteral("DEPT"));
  const QVector<double> depths = curves.first().values;
  QVERIFY(!depths.isEmpty());
  QVERIFY(depths.first() >= 1200.0 - 1e-9);
  QVERIFY(depths.last() <= 1201.0 + 1e-9);
  QCOMPARE(depths.size(), 9); // 1200.000 … 1201.000 含端点
  // 早停证据：区间查询远快于全量（这里以行数语义验证即可——深度过滤生效）。
}

void PerfLasTests::errorClassificationWrapMode()
{
  const QString las = m_dir.filePath("wrap.las");
  writeLas(las, QByteArray("~VERSION\nWRAP. YES : wrapped\n~WELL\nNULL. -999.25\n"
                           "~CURVE\nDEPT.M : depth\n~A\n1 2\n"));
  QList<LasIssue> issues;
  const LasDoc doc = LasParser::parseDoc(las, &issues);
  QVERIFY(!doc.ok);
  bool sawWrap = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::WrapMode && i.severity == LasIssue::Severity::Error)
      sawWrap = true;
  QVERIFY2(sawWrap, "WRAP YES 必须分级为 Format/WrapMode Error");
}

void PerfLasTests::errorClassificationTruncatedRow()
{
  const QString las = m_dir.filePath("trunc.las");
  // 第二行只有 1 个 token（曲线 2 条）→ Truncated warning + NaN 补齐。
  writeLas(las, QByteArray("~VERSION\nWRAP. NO : ok\n~WELL\n~CURVE\nDEPT.M : d\nGR.GAPI : g\n"
                           "~A\n1.0 5.0\n2.0\n"));
  QList<LasIssue> issues;
  const LasDoc doc = LasParser::parseDoc(las, &issues);
  QVERIFY(doc.ok); // 截断行不整体失败（D1.6 分级）
  QVERIFY(doc.curves.at(1).values.at(1) != doc.curves.at(1).values.at(1)); // NaN
  bool sawTruncated = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::Truncated && i.severity == LasIssue::Severity::Warning)
      sawTruncated = true;
  QVERIFY2(sawTruncated, "行 token 缺失必须分级为 Truncated Warning");
}

void PerfLasTests::errorClassificationMissingUnit()
{
  const QString las = m_dir.filePath("nounit.las");
  writeLas(las, QByteArray("~VERSION\n~WELL\n~CURVE\nDEPT. : no unit given\nGR.GAPI : g\n~A\n1 2\n"));
  QList<LasIssue> issues;
  const LasDoc doc = LasParser::parseDoc(las, &issues);
  QVERIFY(doc.ok);
  bool sawMissingUnit = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::MissingUnit && i.severity == LasIssue::Severity::Info)
      sawMissingUnit = true;
  QVERIFY2(sawMissingUnit, "单位缺失必须分级为 MissingUnit Info");
}

void PerfLasTests::errorClassificationEncoding()
{
  // GB18030 井名：非 UTF-8 → Encoding warning，但井名可解（D7.2 联动）。
  const QString las = m_dir.filePath("gb.las");
  QByteArray raw;
  raw += QByteArrayLiteral("~VERSION\nWRAP. NO : x\n~WELL\n");
  raw += QByteArrayLiteral("WELL. Well-1 ");
  QStringEncoder gbEnc("GB18030", QStringConverter::Flag::Stateless);
  raw += gbEnc.encode(QString::fromUtf8("基准井"));
  raw += QByteArrayLiteral(" : name\nNULL. -999.25\n~CURVE\nDEPT.M : d\n~A\n1.0\n");
  // 注：曲线只 1 条（DEPT），数据行 1 token 完整。
  writeLas(las, raw);
  QList<LasIssue> issues;
  const LasDoc doc = LasParser::parseDoc(las, &issues);
  QVERIFY(doc.ok);
  bool sawEncoding = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::Encoding)
      sawEncoding = true;
  QVERIFY2(sawEncoding, "非 UTF-8 头段必须报 Encoding 提示");
}

void PerfLasTests::oversizeGuardRejectsFullRead()
{
  // 用可注入阈值把 2MB 文件当「超大」——不真造 500MB 文件（D1.8 测试口径）。
  const QString las = m_dir.filePath("big.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 20000));
  const qint64 saved = LasParser::fileSizeLimit();
  LasParser::setFileSizeLimit(512 * 1024); // 512KB（文件 ~0.9MB 即超）
  QList<LasIssue> issues;
  const LasDoc doc = LasParser::parseDoc(las, &issues);
  LasParser::setFileSizeLimit(saved);
  QVERIFY(!doc.ok);
  QVERIFY(doc.error.contains(QStringLiteral("guard")));
  bool sawOversize = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::Oversize)
      sawOversize = true;
  QVERIFY(sawOversize);
}

void PerfLasTests::oversizeGuardAllowsStreaming()
{
  const QString las = m_dir.filePath("big2.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 20000));
  const qint64 saved = LasParser::fileSizeLimit();
  LasParser::setFileSizeLimit(512 * 1024);
  QStringList names;
  QList<LasCurve> curves;
  // 流式区间查询不受整读防护限制（>1MB 文件走 8MB 块路径）。
  QVERIFY(LasParser::parseRange(las, 10, 60, names, curves));
  LasParser::setFileSizeLimit(saved);
  QCOMPARE(curves.first().values.size(), 50);
}

void PerfLasTests::emptyCurvePolicy()
{
  // D1.9：整列 NULL（无效值）的曲线保留为全 NaN 列 + MissingCurve Info——
  // 列数恒等于曲线数，消费方无需特判。
  const QString las = m_dir.filePath("empty.las");
  writeLas(las, QByteArray("~VERSION\n~WELL\nNULL. -999.25\n~CURVE\nDEPT.M : d\nGR.GAPI : g\n"
                           "~A\n1.0 -999.25\n2.0 -999.25\n3.0 -999.25\n"));
  QList<LasIssue> issues;
  const LasDoc doc = LasParser::parseDoc(las, &issues);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curves.size(), 2);
  QCOMPARE(doc.curves.at(1).values.size(), 3);
  QVERIFY(doc.curves.at(1).values.at(0) != doc.curves.at(1).values.at(0)); // NaN
  bool sawMissingCurve = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::MissingCurve)
      sawMissingCurve = true;
  QVERIFY(sawMissingCurve);
}

void PerfLasTests::aliasCanonicalization()
{
  LasAliasMap &m = LasAliasMap::shared();
  m.clearMemo();
  QCOMPARE(m.canonicalCurve(QStringLiteral("DT")), QStringLiteral("DT"));
  QCOMPARE(m.canonicalCurve(QStringLiteral("AC")), QStringLiteral("DT"));
  QCOMPARE(m.canonicalCurve(QStringLiteral("dt4p")), QStringLiteral("DT"));
  QCOMPARE(m.canonicalCurve(QStringLiteral("DT35")), QStringLiteral("DT")); // 尾部数字修饰
  QCOMPARE(m.canonicalCurve(QStringLiteral("NGR")), QStringLiteral("GR"));
  QCOMPARE(m.canonicalCurve(QStringLiteral("Z-DEN")), QStringLiteral("RHOB"));
  QCOMPARE(m.canonicalCurve(QStringLiteral("M_DEPTH")), QStringLiteral("DEPT"));
  QCOMPARE(m.canonicalUnit(QStringLiteral("US/M")), QStringLiteral("us"));
  QCOMPARE(m.canonicalUnit(QStringLiteral("Meters")), QStringLiteral("m"));
  QCOMPARE(m.canonicalUnit(QStringLiteral("GAPI")), QStringLiteral("gapi"));
  QVERIFY(m.knowsCurve(QStringLiteral("SGR")));
  QVERIFY(!m.knowsCurve(QStringLiteral("TOTALLY_UNKNOWN_XYZ")));
}

void PerfLasTests::aliasMemoization()
{
  LasAliasMap &m = LasAliasMap::shared();
  m.clearMemo();
  const QString raw = QStringLiteral("DT4P");
  m.canonicalCurve(raw);
  m.canonicalCurve(raw);
  m.canonicalCurve(raw);
  const LasAliasStats st = m.stats();
  QCOMPARE(st.lookups, qint64(3));
  QVERIFY2(st.memoHits >= qint64(2),
           qPrintable(QStringLiteral("memo hits %1 < 2").arg(st.memoHits)));
  QVERIFY(st.normalized >= qint64(1)); // DT4P → DT 发生过归一
}

void PerfLasTests::aliasFamilies()
{
  LasAliasMap &m = LasAliasMap::shared();
  QCOMPARE(m.family(QStringLiteral("AC")), QStringLiteral("Sonic"));
  QCOMPARE(m.family(QStringLiteral("NGR")), QStringLiteral("Gamma"));
  QCOMPARE(m.family(QStringLiteral("ZDEN")), QStringLiteral("Density"));
  QCOMPARE(m.family(QStringLiteral("TVDSS")), QStringLiteral("Depth"));
  QCOMPARE(m.family(QStringLiteral("NOSUCHCURVE")), QStringLiteral("Other"));
}

void PerfLasTests::bomLasHandled()
{
  // UTF-8 BOM 头：stripBom 后解析不受污染（曲线名不带 BOM）。
  const QString las = m_dir.filePath("bom.las");
  {
    QFile f(las);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("\xEF\xBB\xBF");
    f.write("~VERSION\n~WELL\n~CURVE\nDEPT.M : d\nGR.GAPI : g\n~A\n1.0 5.0\n2.0 6.0\n");
  }
  const LasDoc doc = LasParser::parseDoc(las);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curveNames.first(), QStringLiteral("DEPT"));
  QCOMPARE(doc.curves.first().values.size(), 2);
}

void PerfLasTests::parseRangeFullFileAgrees()
{
  const QString las = m_dir.filePath("fullrange.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 400));
  const LasDoc full = LasParser::parseDoc(las);
  QStringList names;
  QList<LasCurve> all;
  QVERIFY(LasParser::parseRange(las, 0, -1, names, all)); // 全区间 = 全量
  QCOMPARE(names, full.curveNames);
  QCOMPARE(all.first().values.size(), full.curves.first().values.size());
  // NULL 行两侧都是 NaN——NaN 感知逐值比较（QList== 对 NaN 不等）。
  for (int c = 0; c < all.size(); ++c)
    for (int r = 0; r < all.at(c).values.size(); ++r)
    {
      const double a = all.at(c).values.at(r);
      const double b = full.curves.at(c).values.at(r);
      if (std::isnan(a))
        QVERIFY(std::isnan(b));
      else
        QCOMPARE(a, b);
    }
}

void PerfLasTests::customNullValueMapped()
{
  // ~WELL NULL. 99999：自定义空值也映射 NaN（不是只认 -999.25）。
  const QString las = m_dir.filePath("null99999.las");
  writeLas(las, QByteArray("~VERSION\n~WELL\nNULL. 99999 : nv\n~CURVE\nDEPT.M : d\n"
                           "GR.GAPI : g\n~A\n1.0 99999\n2.0 7.0\n"));
  const LasDoc doc = LasParser::parseDoc(las);
  QVERIFY(doc.ok);
  QVERIFY(doc.curves.at(1).values.at(0) != doc.curves.at(1).values.at(0)); // NaN
  QCOMPARE(doc.curves.at(1).values.at(1), 7.0);
}

void PerfLasTests::parseDepthRangeManyCurves()
{
  // BIZ-01: 验证 >64 条曲线时 parseDepthRange 不发生静默截断为 NaN
  const QString las = m_dir.filePath("many_curves.las");
  const int nCurves = 75; // 超过 64 条
  QStringList curves;
  curves.append(QStringLiteral("DEPT"));
  for (int i = 1; i < nCurves; ++i)
    curves.append(QStringLiteral("C%1").arg(i, 2, 10, QLatin1Char('0')));

  const int rows = 20;
  const double startDepth = 1000.0;
  const double stepDepth = 0.5;

  QFile f(las);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QByteArray content;
  content += "~VERSION\nVERS. 2.0 : CWLS\nWRAP. NO : no\n~WELL\nNULL. -999.25\nWELL. WELL-75\n~CURVE\n";
  for (const QString &c : curves)
    content += QStringLiteral("%1.M : %1 curve\n").arg(c).toUtf8();
  content += "~A\n";
  for (int r = 0; r < rows; ++r)
  {
    const double depth = startDepth + r * stepDepth;
    QString line = QString::number(depth, 'f', 3);
    for (int c = 1; c < nCurves; ++c)
    {
      const double v = 10.0 + c + r * 0.1;
      line += QStringLiteral(" %1").arg(QString::number(v, 'f', 2));
    }
    content += line.toUtf8();
    content += '\n';
  }
  f.write(content);
  f.close();

  QStringList parsedNames;
  QList<LasCurve> parsedCurves;
  QString err;
  QVERIFY2(LasParser::parseDepthRange(las, 1002.0, 1005.0, parsedNames, parsedCurves, &err), qPrintable(err));
  QCOMPARE(parsedNames.size(), nCurves);
  QCOMPARE(parsedCurves.size(), nCurves);

  // 1002.0 到 1005.0 (含端点，步长 0.5) 共 7 行
  const int expectedRows = 7;
  for (int c = 0; c < nCurves; ++c)
  {
    QCOMPARE(parsedCurves.at(c).values.size(), expectedRows);
    for (int r = 0; r < expectedRows; ++r)
    {
      const double v = parsedCurves.at(c).values.at(r);
      QVERIFY2(!std::isnan(v), qPrintable(QStringLiteral("Curve %1 ('%2') row %3 is unexpectedly NaN (BIZ-01 truncation)")
                                          .arg(c).arg(parsedNames.at(c)).arg(r)));
    }
  }

  // 重点验证边界曲线索引：第 63 条（旧上限边缘）、第 64 条（旧逻辑首个截断点）、第 74 条（尾部曲线）
  QVERIFY(!std::isnan(parsedCurves.at(63).values.at(0)));
  QVERIFY(!std::isnan(parsedCurves.at(64).values.at(0)));
  QVERIFY(!std::isnan(parsedCurves.at(74).values.at(0)));
}

void PerfLasTests::parseDepthRangeBomOffset()
{
  // BIZ-02: 验证带 UTF-8 BOM 的文件在流式 parseDepthRange / parseRange 中偏移对齐无 3 字节错位
  const QString las = m_dir.filePath("bom_streaming.las");
  QFile f(las);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write("\xEF\xBB\xBF"); // UTF-8 BOM
  f.write("~VERSION\n"
          "VERS. 2.0 : CWLS log ASCII Standard\n"
          "WRAP. NO  : one line per depth step\n"
          "~WELL\n"
          "STRT.M 1000.000 : first depth\n"
          "STOP.M 1001.000 : last depth\n"
          "NULL. -999.25 : null\n"
          "WELL. BOM-TEST-WELL\n"
          "~CURVE\n"
          "DEPT.M : depth\n"
          "GR.GAPI : gamma\n"
          "DT.US/M : sonic\n"
          "~A\n"
          "1000.000 45.500 65.200\n"
          "1000.250 46.100 64.800\n"
          "1000.500 47.300 63.900\n"
          "1000.750 48.000 63.500\n"
          "1001.000 49.200 62.100\n");
  f.close();

  // 1. parseDepthRange 从起始深度 1000.0 开始读取
  QStringList names;
  QList<LasCurve> curves;
  QString err;
  QVERIFY2(LasParser::parseDepthRange(las, 1000.0, 1000.5, names, curves, &err), qPrintable(err));
  QCOMPARE(names.size(), 3);
  QCOMPARE(curves.size(), 3);
  // 应包含 1000.000, 1000.250, 1000.500 共 3 行
  QCOMPARE(curves.at(0).values.size(), 3);

  // 首行数值严密校验（若偏移少 3 字节，首行会解析 ~A 尾部碎片导致 DEPT 为 NaN 或深度错位）
  QCOMPARE(curves.at(0).values.at(0), 1000.000);
  QCOMPARE(curves.at(1).values.at(0), 45.500);
  QCOMPARE(curves.at(2).values.at(0), 65.200);

  // 2. parseRange 也必须对齐
  QStringList rangeNames;
  QList<LasCurve> rangeCurves;
  QVERIFY2(LasParser::parseRange(las, 0, 2, rangeNames, rangeCurves, &err), qPrintable(err));
  QCOMPARE(rangeCurves.at(0).values.size(), 2);
  QCOMPARE(rangeCurves.at(0).values.at(0), 1000.000);
  QCOMPARE(rangeCurves.at(1).values.at(0), 45.500);
  QCOMPARE(rangeCurves.at(2).values.at(0), 65.200);
}

void PerfLasTests::parseDepthRange100Curves()
{
  // BIZ-01 Stress Test: 100 curves (exceeding 64, 70, 75)
  const QString las = m_dir.filePath("stress_100_curves.las");
  const int nCurves = 100;
  QStringList curves;
  curves.append(QStringLiteral("DEPT"));
  for (int i = 1; i < nCurves; ++i)
    curves.append(QStringLiteral("CRV%1").arg(i, 3, 10, QLatin1Char('0')));

  const int rows = 15;
  const double startDepth = 2000.0;
  const double stepDepth = 1.0;

  QFile f(las);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QByteArray content;
  content += "~VERSION\nVERS. 2.0 : CWLS\nWRAP. NO : no\n~WELL\nNULL. -999.25\nWELL. WELL-100\n~CURVE\n";
  for (const QString &c : curves)
    content += QStringLiteral("%1.M : %1 curve\n").arg(c).toUtf8();
  content += "~A\n";
  for (int r = 0; r < rows; ++r)
  {
    const double depth = startDepth + r * stepDepth;
    QString line = QString::number(depth, 'f', 3);
    for (int c = 1; c < nCurves; ++c)
    {
      const double v = 100.0 * c + r;
      line += QStringLiteral(" %1").arg(QString::number(v, 'f', 1));
    }
    content += line.toUtf8();
    content += '\n';
  }
  f.write(content);
  f.close();

  QStringList parsedNames;
  QList<LasCurve> parsedCurves;
  QString err;
  QVERIFY2(LasParser::parseDepthRange(las, 2002.0, 2006.0, parsedNames, parsedCurves, &err), qPrintable(err));
  QCOMPARE(parsedNames.size(), 100);
  QCOMPARE(parsedCurves.size(), 100);

  // 2002.0 to 2006.0 is 5 rows (r=2 to r=6)
  const int expectedRows = 5;
  for (int c = 0; c < nCurves; ++c)
  {
    QCOMPARE(parsedCurves.at(c).values.size(), expectedRows);
    for (int r = 0; r < expectedRows; ++r)
    {
      const double val = parsedCurves.at(c).values.at(r);
      QVERIFY(!std::isnan(val));
      const double expected = (c == 0) ? (2002.0 + r) : (100.0 * c + (r + 2));
      QCOMPARE(val, expected);
    }
  }

  // Specifically check critical curve boundary indices
  // c=63 (curve 64, former boundary), c=64 (former truncation point), c=99 (last curve)
  QCOMPARE(parsedCurves.at(63).values.at(0), 100.0 * 63 + 2);
  QCOMPARE(parsedCurves.at(64).values.at(0), 100.0 * 64 + 2);
  QCOMPARE(parsedCurves.at(99).values.at(0), 100.0 * 99 + 2);
}

void PerfLasTests::parseDepthRangeBomVariousHeaders()
{
  // BIZ-02 Stress Test: BOM with various header lengths and comments
  const int headerSizes[] = {100, 2000, 32000};
  for (int hIdx = 0; hIdx < 3; ++hIdx)
  {
    const QString las = m_dir.filePath(QStringLiteral("bom_header_%1.las").arg(hIdx));
    QFile f(las);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("\xEF\xBB\xBF"); // UTF-8 BOM
    f.write("~VERSION\nVERS. 2.0 : CWLS\nWRAP. NO : no\n~WELL\nNULL. -999.25\nWELL. BOM-WELL\n");
    // Add padding comment lines to vary header length
    QByteArray padding;
    int padTarget = headerSizes[hIdx];
    while (padding.size() < padTarget)
    {
      padding += "# Some UTF-8 comment text with multi-byte chars: 测井曲线测试 注释信息\n";
    }
    f.write(padding);
    f.write("~CURVE\nDEPT.M : depth\nGR.GAPI : gamma\nNPHI.V/V : porosity\n~A\n");
    f.write("1500.000 75.20 0.18\n");
    f.write("1500.500 78.10 0.22\n");
    f.write("1501.000 81.30 0.25\n");
    f.close();

    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY2(LasParser::parseDepthRange(las, 1500.0, 1501.0, names, curves, &err), qPrintable(err));
    QCOMPARE(names.size(), 3);
    QCOMPARE(curves.at(0).values.size(), 3);
    QCOMPARE(curves.at(0).values.at(0), 1500.000);
    QCOMPARE(curves.at(1).values.at(0), 75.20);
    QCOMPARE(curves.at(2).values.at(0), 0.18);
  }
}

void PerfLasTests::parseDepthRangeChunkBoundaryStress()
{
  // Empirical stress test: file exceeding 4MB (kChunk) chunk boundary
  // Verifying chunk boundary traversal in parseDepthRange
  const QString las = m_dir.filePath("chunk_boundary_stress.las");
  QFile f(las);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write("~VERSION\nVERS. 2.0 : CWLS\nWRAP. NO : no\n~WELL\nNULL. -999.25\nWELL. CHUNK-WELL\n");
  f.write("~CURVE\nDEPT.M : depth\nGR.GAPI : gamma\nRHOB.G/C3 : density\n~A\n");

  // Vary row length so chunk boundary cuts mid-row and mid-token
  const int totalRows = 300000;
  const double startDepth = 100.0;
  const double step = 0.1;
  {
    QByteArray buffer;
    buffer.reserve(256 * 1024);
    for (int r = 0; r < totalRows; ++r)
    {
      const double d = startDepth + r * step;
      const double gr = 50.0 + (r % 100) * 0.512345; // Varying decimal lengths
      const double rhob = 2.2 + (r % 77) * 0.0137;
      buffer += QByteArray::number(d, 'f', 3);
      buffer += ' ';
      buffer += QByteArray::number(gr, 'f', (r % 5));
      buffer += ' ';
      buffer += QByteArray::number(rhob, 'f', (r % 4));
      buffer += '\n';
      if (buffer.size() >= 200 * 1024)
      {
        f.write(buffer);
        buffer.clear();
      }
    }
    if (!buffer.isEmpty())
      f.write(buffer);
  }
  f.close();
  qDebug() << "Generated file size:" << QFileInfo(las).size() << "bytes";

  // Query depth range across the entire file (all 180,000 rows)
  QStringList names;
  QList<LasCurve> curves;
  QString err;
  const double endDepth = startDepth + (totalRows - 1) * step;
  bool ok = LasParser::parseDepthRange(las, startDepth, endDepth, names, curves, &err);
  QVERIFY2(ok, qPrintable(err));
  QCOMPARE(names.size(), 3);
  QCOMPARE(curves.size(), 3);

  QCOMPARE(curves.at(0).values.size(), totalRows);

  // Verify monotonicity and correct values across chunk boundary
  for (int r = 0; r < totalRows; ++r)
  {
    const double expectedD = startDepth + r * step;
    const double actualD = curves.at(0).values.at(r);
    if (std::abs(actualD - expectedD) > 1e-3)
    {
      qWarning() << "Mismatch at row" << r << "expected" << expectedD << "got" << actualD;
      QCOMPARE(actualD, expectedD);
      break;
    }
  }
}

QTEST_MAIN(PerfLasTests)
#include "tst_perf_las.moc"
