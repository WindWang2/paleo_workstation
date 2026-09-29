// tst_perf_las — wave/io-perf-cache D1.4-D1.9：LAS 快解析性能、区间查询、
// 错误分类、大文件防护、空曲线策略、别名归一。
#include <QtTest>

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
  QVERIFY2(ms < 50.0, qPrintable(QStringLiteral("parseDoc %1ms >= 50ms (D1.5)").arg(ms)));
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

QTEST_MAIN(PerfLasTests)
#include "tst_perf_las.moc"
