#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "../src/io/projectclassifier.h"
#include "../src/io/wellfileparsers.h"
#include "../src/io/lasparser.h"

// plan §3 分类表 + §3 解析列契约：
// 井位/井分层/时深/层位 路径段、扩展名表、XML 内容判定；
// ExportWellHead 列、DC.dat 列（井名 层名 MD X Y Z TVD Time）、-99999→空、
// TD 表列（TIME TVDSS TVD MD）、LAS ~W WELL。
class TestProjectParsers : public QObject
{
  Q_OBJECT

private slots:
  void classifiesEachCategory();
  void classifiesXmlByContent();
  void parsesWellHead();
  void parsesWellTops();
  void parsesTimeDepth();
  void bomOnFirstDataLineIsStripped();
  void readsLasWellInfo();

private:
  QString fixture(const QString &name) const
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }
};

void TestProjectParsers::classifiesEachCategory()
{
  const QString base = QStringLiteral("/data/project_area");
  // 路径段规则（井位/井分层/时深|td/层位）
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/井位/ExportWellHead.dat")).type,
           QStringLiteral("well_head"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/井分层/DC.dat")).type,
           QStringLiteral("well_stratification"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/时深/TD/A1.dat")).type,
           QStringLiteral("time_depth"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/x/td/A1.dat")).type,
           QStringLiteral("time_depth"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/层位/D61.dat")).type,
           QStringLiteral("horizon"));
  QCOMPARE(classifyProjectPath(QStringLiteral("/any/WellHead_2020.dat")).type,
           QStringLiteral("well_head"));
  // 扩展名规则
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/井曲线/A1.Las")).type,
           QStringLiteral("well_log"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/地震体/200P_seismic.sgy")).type,
           QStringLiteral("seismic"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/参考相图/facies.geojson")).type,
           QStringLiteral("geojson"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/参考资料/规范.pdf")).type,
           QStringLiteral("document"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/参考资料/图.pptx")).type,
           QStringLiteral("document"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/参考资料/相图.png")).type,
           QStringLiteral("image_reference"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/扫描图.jpg")).type,
           QStringLiteral("image_reference"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/无地理变换.tif")).type,
           QStringLiteral("image_reference"));
  // 角色列：input vs reference
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/井曲线/A1.Las")).role,
           QStringLiteral("input"));
  QCOMPARE(classifyProjectPath(base + QStringLiteral("/参考资料/规范.pdf")).role,
           QStringLiteral("reference"));
}

void TestProjectParsers::classifiesXmlByContent()
{
  const QByteArray wellLogXml =
      "<logs><log><logcurveinfo/><logdata>1 2 3</logdata></log></logs>";
  QCOMPARE(classifyProjectImport(QStringLiteral("/参考资料/a.xml"), wellLogXml).type,
           QStringLiteral("well_log"));

  const QByteArray witsmlXml =
      "<witsml:logs xmlns:witsml='urn:x'><logcurveinfo/><logdata>1</logdata></witsml:logs>";
  QCOMPARE(classifyProjectImport(QStringLiteral("/x/a.xml"), witsmlXml).type,
           QStringLiteral("well_log"));

  const QByteArray spreadsheetXml =
      "<Workbook xmlns='urn:schemas-microsoft-com:office:spreadsheet' "
      "xmlns:ss='urn:schemas-microsoft-com:office:spreadsheet'>"
      "<Worksheet ss:Name='测井曲线'><Table><Row/></Table></Worksheet></Workbook>";
  QCOMPARE(classifyProjectImport(QStringLiteral("/参考资料/柱状图.xml"), spreadsheetXml).type,
           QStringLiteral("well_log"));

  // 判不出 → 参考
  const QByteArray junk = QStringLiteral("<html><body>noise</body></html>").toUtf8();
  QCOMPARE(classifyProjectImport(QStringLiteral("/参考资料/x.xml"), junk).type,
           QStringLiteral("unknown"));
  QCOMPARE(classifyProjectImport(QStringLiteral("/参考资料/x.xml"), junk).role,
           QStringLiteral("reference"));
}

void TestProjectParsers::parsesWellHead()
{
  QFile f(fixture(QStringLiteral("ExportWellHead.dat")));
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QVector<WellHeadRecord> rows = parseWellHeadText(f.readAll());
  QCOMPARE(rows.size(), 20);
  // A1 在 (5288.67, 8219.94)（plan §1）
  const WellHeadRecord *a1 = nullptr;
  for (const auto &r : rows)
    if (r.name == QStringLiteral("A1"))
      a1 = &r;
  QVERIFY(a1);
  QCOMPARE(a1->x, 5288.670);
  QCOMPARE(a1->y, 8219.940);
  QCOMPARE(a1->td, 2160.0);
}

void TestProjectParsers::parsesWellTops()
{
  QFile f(fixture(QStringLiteral("DC.dat")));
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QVector<WellTopRecord> tops = parseWellTopsText(f.readAll());
  QVERIFY(tops.size() > 100);
  // 多井文件：每个井名都出现
  QStringList wells;
  for (const auto &t : tops)
    if (!wells.contains(t.wellName))
      wells.append(t.wellName);
  QCOMPARE(wells.front(), QStringLiteral("A1"));
  QVERIFY(wells.contains(QStringLiteral("A20")));
  // A1 的 D61 分层存在；Time 列全为 -99999 → 空
  const WellTopRecord *a1d61 = nullptr;
  for (const auto &t : tops)
    if (t.wellName == QStringLiteral("A1") && t.topName == QStringLiteral("D61"))
      a1d61 = &t;
  QVERIFY(a1d61);
  QVERIFY(a1d61->md > 0);
  QVERIFY(!a1d61->hasTime);
  QVERIFY(a1d61->hasTvd);
  // 列序：井名 层名 MD X Y Z TVD Time(ms)
  QCOMPARE(a1d61->x, 5288.670);
  QCOMPARE(a1d61->y, 8219.940);
}

void TestProjectParsers::parsesTimeDepth()
{
  QFile f(fixture(QStringLiteral("A1_TD.dat")));
  QVERIFY(f.open(QIODevice::ReadOnly));
  const TimeDepthTable td = parseTimeDepthText(f.readAll());
  QCOMPARE(td.wellName, QStringLiteral("A1"));
  QVERIFY(td.rows.size() > 100);
  QCOMPARE(td.rows.front().timeMs, 380.0);
  QVERIFY(td.rows.front().tvd > 0);
  QVERIFY(td.rows.front().md > 0);
}

void TestProjectParsers::bomOnFirstDataLineIsStripped()
{
  // plan §3：BOM 若粘在首行数据/表头的第一个 token 上，井名会失配——
  // 三个解析入口都必须先剥 U+FEFF（trimmed() 不去它）。
  QByteArray bombed = QByteArray::fromHex("efbbbf") +
                      QByteArrayLiteral("A9 1000.0 2000.0 5.0 1500.0\n");
  const QVector<WellHeadRecord> rows = parseWellHeadText(bombed);
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.first().name, QStringLiteral("A9"));

  const QVector<WellTopRecord> tops = parseWellTopsText(
      QByteArray::fromHex("efbbbf") +
      QByteArrayLiteral("A9 D61 1800.5\n"));
  QCOMPARE(tops.size(), 1);
  QCOMPARE(tops.first().wellName, QStringLiteral("A9"));

  const TimeDepthTable td = parseTimeDepthText(
      QByteArray::fromHex("efbbbf") +
      QByteArrayLiteral("400.0 500.0 510.0 600.0\n"));
  QCOMPARE(td.rows.size(), 1);
  QCOMPARE(td.rows.first().timeMs, 400.0);
}

void TestProjectParsers::readsLasWellInfo()
{
  QString well, uwi;
  QString err;
  QVERIFY(LasParser::readWellInfo(fixture(QStringLiteral("A1.Las")), well, uwi, &err));
  QCOMPARE(well, QStringLiteral("A1"));
  QVERIFY(uwi.isEmpty()); // 该文件 UWI 为空
}

QTEST_MAIN(TestProjectParsers)
#include "tst_projectparsers.moc"
