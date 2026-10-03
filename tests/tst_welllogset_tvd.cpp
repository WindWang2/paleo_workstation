// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include "catalog/datacatalog.h"
#include "domain/deviationsurvey.h"
#include "services/projectdata.h"
#include "services/welllogset.h"

#include <cmath>

// goal/well-trajectory 轮3：MD/TVD 域服务验收（Oracle 第 3/5 条）——
// ProjectDataFacade::trajectoryFor（tdTableFor 同族：primary trajectory
// 链接 → 站表 → WellDeviationSurvey；无链接 nullopt 显式直井回退）+
// WellLogSet::readCurveTvd（恒等/非恒等/多文件各自映射/单位诚实面）。
class tst_welllogset_tvd : public QObject
{
  Q_OBJECT

private slots:
  void trajectoryForUnlinkedIsNullopt();
  void trajectoryForReadsTextSurvey();
  void trajectoryForReadsXmlSurvey();
  void trajectoryForRejectsInvalidTable();
  void readCurveTvdIdentityWithoutSurvey();
  void readCurveTvdMapsThroughSurvey();
  void readCurveTvdPerFileMapping();
  void readCurveTvdHonestErrors();

private:
  static QString writeText(const QString &path, const QString &text)
  {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    f.write(text.toUtf8());
    f.close();
    return path;
  }

  static QString writeLas(const QString &path, const QString &well,
                          const QStringList &mnems, double startDepth, int rows,
                          const QString &unit = QStringLiteral("M"))
  {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    QTextStream out(&file);
    out << "~Version Information\n";
    out << " VERS.                  2.0:   CWLS log ASCII Standard -VERSION 2.0\n";
    out << " WRAP.                   NO:   One line per depth step\n";
    out << "~Well Information Block\n";
    out << " STRT." << unit << "        " << QString::number(startDepth, 'f', 4) << ":\n";
    out << " STOP." << unit << "        "
        << QString::number(startDepth + rows - 1, 'f', 4) << ":\n";
    out << " STEP." << unit << "          1.0000:\n";
    out << " NULL.        -999.2500:\n";
    out << " WELL.         " << well << ":\n";
    out << "~Curve Information Block\n";
    for (const QString &mnem : mnems)
      out << " " << mnem << "." << unit << "                  :   " << mnem << "\n";
    out << "~A\n";
    for (int r = 0; r < rows; ++r)
    {
      out << QString::number(startDepth + r, 'f', 2);
      for (int c = 1; c < mnems.size(); ++c)
        out << " " << QString::number(10.0 * c + r, 'f', 2);
      out << "\n";
    }
    file.close();
    return path;
  }

  // 造斜-稳斜：0-1000m 0°→30°（方位正北），1000-2000m 稳斜 30°。
  static const char *kDevText()
  {
    return "# Well : A1\n# MD INCL AZI\n0.0 0.0 0.0\n1000.0 30.0 0.0\n2000.0 30.0 0.0\n";
  }

  // 造斜段闭式（tst_deviation 同款）：cos β = cos30° → Δtvd = 500(1+cos30)RF。
  static double buildEndTvd()
  {
    const double beta = std::acos(std::cos(30.0 * M_PI / 180.0));
    const double rf = (2.0 / beta) * std::tan(beta * 0.5);
    return 500.0 * (1.0 + std::cos(30.0 * M_PI / 180.0)) * rf;
  }

  struct Seeded
  {
    QString projectDir;
    DataCatalog catalog;
  };

  // 落盘 catalog + 受管 LAS/dev 文件 + 链接，再经门面读回（managed 布局与
  // tst_projectdata 同款：artifacts/raw/<asset>/<version>/<name>）。
  bool seedProject(QTemporaryDir &tmp, const QStringList &lasSpecs, bool withDeviation,
                   const QByteArray &devOverride = QByteArray())
  {
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    if (!QDir().mkpath(projectDir))
      return false;
    if (!m_catalog.open(projectDir))
      return false;
    CatalogEntity w;
    w.id = QStringLiteral("well-1");
    w.entityType = QStringLiteral("well");
    w.name = QStringLiteral("A1");
    w.hasSurface = true;
    w.surfaceX = 1000.0;
    w.surfaceY = 2000.0;
    w.coordinateStatus = QStringLiteral("untransformed");
    if (!m_catalog.addEntity(w))
      return false;
    m_projectDir = projectDir;

    int assetNo = 0;
    for (const QString &spec : lasSpecs)
    {
      const QStringList parts = spec.split(QLatin1Char('|'));
      // <fileName>|<start>|<rows>|<wellField>
      ++assetNo;
      CatalogAsset asset;
      asset.id = QStringLiteral("ast-las-%1").arg(assetNo);
      asset.type = QStringLiteral("well_log");
      asset.format = QStringLiteral("las");
      asset.displayName = parts.at(0);
      if (!m_catalog.addAsset(asset))
        return false;
      CatalogVersion ver;
      ver.id = QStringLiteral("ver-las-%1").arg(assetNo);
      ver.assetId = asset.id;
      ver.stage = QStringLiteral("RAW");
      ver.path = DataCatalog::managedPath(QStringLiteral("raw"), asset.id, ver.id,
                                          parts.at(0));
      if (!m_catalog.addVersion(ver))
        return false;
      if (writeText(QDir(projectDir).filePath(ver.path), QString()).isEmpty())
        return false;
      writeLas(QDir(projectDir).filePath(ver.path), parts.at(3),
               {QStringLiteral("DEPT"), QStringLiteral("GR")},
               parts.at(1).toDouble(), parts.at(2).toInt());
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = QStringLiteral("well-1");
      l.assetId = asset.id;
      l.role = QStringLiteral("well_log");
      l.isPrimary = assetNo == 1;
      l.unresolved = false;
      l.ordinal = assetNo - 1;
      if (!m_catalog.addLink(l))
        return false;
    }

    if (withDeviation)
    {
      CatalogAsset dev;
      dev.id = QStringLiteral("ast-dev");
      dev.type = QStringLiteral("well_deviation");
      dev.format = QStringLiteral("dat");
      dev.displayName = QStringLiteral("A1.deviation.dat");
      if (!m_catalog.addAsset(dev))
        return false;
      CatalogVersion ver;
      ver.id = QStringLiteral("ver-dev");
      ver.assetId = dev.id;
      ver.stage = QStringLiteral("RAW");
      ver.path = DataCatalog::managedPath(QStringLiteral("raw"), dev.id, ver.id,
                                          QStringLiteral("A1.deviation.dat"));
      if (!m_catalog.addVersion(ver))
        return false;
      const QByteArray body = devOverride.isEmpty() ? QByteArray(kDevText()) : devOverride;
      const QString devPath = QDir(projectDir).filePath(ver.path);
      QDir().mkpath(QFileInfo(devPath).absolutePath());
      QFile f(devPath);
      if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
      f.write(body);
      f.close();
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = QStringLiteral("well-1");
      l.assetId = dev.id;
      l.role = QStringLiteral("trajectory");
      l.isPrimary = true;
      l.unresolved = false;
      if (!m_catalog.addLink(l))
        return false;
    }
    return true;
  }

  DataCatalog m_catalog;
  QString m_projectDir;
};

void tst_welllogset_tvd::trajectoryForUnlinkedIsNullopt()
{
  QTemporaryDir tmp;
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|0|3|A1")}, false));
  ProjectDataFacade facade;
  facade.setCatalog(&m_catalog, m_projectDir);
  const auto survey = facade.trajectoryFor(QStringLiteral("well-1"));
  QVERIFY(!survey.has_value());               // 无测斜井：nullopt，不是垂井对象
  QVERIFY(facade.lastError().isEmpty());      // 未链接不是错误（直井回退显式）
}

void tst_welllogset_tvd::trajectoryForReadsTextSurvey()
{
  QTemporaryDir tmp;
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|0|3|A1")}, true));
  ProjectDataFacade facade;
  facade.setCatalog(&m_catalog, m_projectDir);
  const auto survey = facade.trajectoryFor(QStringLiteral("well-1"));
  QVERIFY(survey.has_value());
  QCOMPARE(survey->stations().size(), 3);
  QCOMPARE(survey->totalDepth(), 2000.0);
  QVERIFY(!survey->isVertical());
  // 造斜段末闭式（tst_deviation 手算同款）。
  QVERIFY(std::fabs(survey->tvdAt(1000.0) - buildEndTvd()) < 1e-9);
  QVERIFY(facade.lastError().isEmpty());
}

void tst_welllogset_tvd::trajectoryForReadsXmlSurvey()
{
  QTemporaryDir tmp;
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|0|3|A1")}, true,
                      QByteArray())); // 先占位：XML 需要写成 .xml 资产
  // 直接覆写受管文件为 SpreadsheetML（路径同 managed 布局；门面按后缀分流）。
  CatalogAsset dev;
  dev.id = QStringLiteral("ast-dev-xml");
  dev.type = QStringLiteral("well_deviation");
  dev.format = QStringLiteral("xml");
  dev.displayName = QStringLiteral("A1.deviation.xml");
  QVERIFY(m_catalog.addAsset(dev));
  CatalogVersion ver;
  ver.id = QStringLiteral("ver-dev-xml");
  ver.assetId = dev.id;
  ver.stage = QStringLiteral("RAW");
  ver.path = DataCatalog::managedPath(QStringLiteral("raw"), dev.id, ver.id,
                                      QStringLiteral("A1.deviation.xml"));
  QVERIFY(m_catalog.addVersion(ver));
  const QByteArray xml =
      "<?xml version=\"1.0\"?><Workbook "
      "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">"
      "<Worksheet ss:Name=\"井斜数据\"><Table>"
      "<Row><Cell><Data ss:Type=\"Number\">0</Data></Cell>"
      "<Cell><Data ss:Type=\"Number\">0</Data></Cell>"
      "<Cell><Data ss:Type=\"Number\">0</Data></Cell></Row>"
      "<Row><Cell><Data ss:Type=\"Number\">1000</Data></Cell>"
      "<Cell><Data ss:Type=\"Number\">30</Data></Cell>"
      "<Cell><Data ss:Type=\"Number\">0</Data></Cell></Row>"
      "</Table></Worksheet></Workbook>";
  const QString xmlPath = QDir(m_projectDir).filePath(ver.path);
  QDir().mkpath(QFileInfo(xmlPath).absolutePath());
  QFile f(xmlPath);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write(xml);
  f.close();
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.entityId = QStringLiteral("well-1");
  l.assetId = dev.id;
  l.role = QStringLiteral("trajectory");
  l.isPrimary = false; // 非主——primary 仍是 .dat 版，本条为历史版本语义
  l.unresolved = false;
  QVERIFY(m_catalog.addLink(l));

  ProjectDataFacade facade;
  facade.setCatalog(&m_catalog, m_projectDir);
  const auto survey = facade.trajectoryFor(QStringLiteral("well-1"));
  QVERIFY(survey.has_value());
  QCOMPARE(survey->stations().size(), 3); // primary 的 .dat 版生效（3 站）
}

void tst_welllogset_tvd::trajectoryForRejectsInvalidTable()
{
  QTemporaryDir tmp;
  // MD 重复站表 → nullopt + lastError 记因（不静默降级直井）。
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|0|3|A1")}, true,
                      QByteArray("# Well : A1\n0 0 0\n500 10 45\n500 12 45\n")));
  ProjectDataFacade facade;
  facade.setCatalog(&m_catalog, m_projectDir);
  const auto survey = facade.trajectoryFor(QStringLiteral("well-1"));
  QVERIFY(!survey.has_value());
  QVERIFY(!facade.lastError().isEmpty());
  QVERIFY(facade.lastError().contains(QStringLiteral("严格递增")));
}

void tst_welllogset_tvd::readCurveTvdIdentityWithoutSurvey()
{
  QTemporaryDir tmp;
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|100|5|A1")}, false));
  const QVector<WellCurveRef> refs =
      WellLogSet::wellCurveIndex(&m_catalog, m_projectDir, QStringLiteral("well-1"));
  QCOMPARE(refs.size(), 1);
  QString err;
  WellLogSet::WellCurveTvdSamples out;
  QVERIFY(WellLogSet::readCurveTvd(refs.at(0), nullptr, &out, &err));
  QVERIFY(err.isEmpty());
  QCOMPARE(out.md.size(), 5);
  QCOMPARE(out.md.at(0), 100.0);
  QCOMPARE(out.md.at(4), 104.0);
  // 无测斜井显式恒等：tvd ≡ md（直井语义，不是兜底猜测）。
  for (int i = 0; i < out.md.size(); ++i)
    QCOMPARE(out.tvd.at(i), out.md.at(i));
  // 无效空表对象同语义（isValid=false → 恒等）。
  paleo::WellDeviationSurvey empty;
  WellLogSet::WellCurveTvdSamples out2;
  QVERIFY(WellLogSet::readCurveTvd(refs.at(0), &empty, &out2, &err));
  QCOMPARE(out2.tvd, out.tvd);
}

void tst_welllogset_tvd::readCurveTvdMapsThroughSurvey()
{
  QTemporaryDir tmp;
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|0|2001|A1")}, true));
  ProjectDataFacade facade;
  facade.setCatalog(&m_catalog, m_projectDir);
  const auto survey = facade.trajectoryFor(QStringLiteral("well-1"));
  QVERIFY(survey.has_value());

  const QVector<WellCurveRef> refs =
      WellLogSet::wellCurveIndex(&m_catalog, m_projectDir, QStringLiteral("well-1"));
  QCOMPARE(refs.size(), 1);
  QString err;
  WellLogSet::WellCurveTvdSamples out;
  QVERIFY(WellLogSet::readCurveTvd(refs.at(0), &*survey, &out, &err));
  QCOMPARE(out.md.size(), 2001);
  // 定向井非恒等：造斜后 tvd < md 且闭式锚点对拍。
  QVERIFY(out.tvd.at(1500) < out.md.at(1500));
  QVERIFY(std::fabs(out.tvd.at(1000) - buildEndTvd()) < 1e-9);
  QVERIFY(std::fabs(out.tvd.at(2000) -
                   (buildEndTvd() + 1000.0 * std::cos(30.0 * M_PI / 180.0))) < 1e-9);
  // 首站前（md=0 邻域）恒等段：0-1 站 incl 0→30 的插值仍在，但 md=0 处 tvd=0。
  QCOMPARE(out.tvd.at(0), 0.0);
}

void tst_welllogset_tvd::readCurveTvdPerFileMapping()
{
  QTemporaryDir tmp;
  // 多文件井：主文件 0-1000，第二文件 1500-2000（同名 GR → 别名 GR@A2）。
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|0|1001|A1"),
                           QStringLiteral("A1d.Las|1500|501|A1")},
                      true));
  ProjectDataFacade facade;
  facade.setCatalog(&m_catalog, m_projectDir);
  const auto survey = facade.trajectoryFor(QStringLiteral("well-1"));
  QVERIFY(survey.has_value());

  const QVector<WellCurveRef> refs =
      WellLogSet::wellCurveIndex(&m_catalog, m_projectDir, QStringLiteral("well-1"));
  QCOMPARE(refs.size(), 2);
  // 每文件各自映射：两条 ref 的 tvd 都出自同一轨迹但覆盖各自 MD 段。
  QString err;
  for (const WellCurveRef &ref : refs)
  {
    WellLogSet::WellCurveTvdSamples out;
    QVERIFY2(WellLogSet::readCurveTvd(ref, &*survey, &out, &err),
             qPrintable(ref.mnemonic + QStringLiteral(": ") + err));
    QVERIFY(out.md.size() > 0);
    QVERIFY(out.versionId == QStringLiteral("ver-las-1") ||
            out.versionId == QStringLiteral("ver-las-2"));
    // 逐点对拍：映射就是 WellDeviationSurvey::tvdAt 本身。
    for (int i = 0; i < out.md.size(); i += 97)
      QVERIFY(std::fabs(out.tvd.at(i) - survey->tvdAt(out.md.at(i))) < 1e-12);
    if (out.versionId == QLatin1String("ver-las-1"))
      QCOMPARE(out.md.front(), 0.0);
    else
      QCOMPARE(out.md.front(), 1500.0);
  }
}

void tst_welllogset_tvd::readCurveTvdHonestErrors()
{
  QTemporaryDir tmp;
  QVERIFY(seedProject(tmp, {QStringLiteral("A1.Las|0|3|A1")}, false));
  const QVector<WellCurveRef> refs =
      WellLogSet::wellCurveIndex(&m_catalog, m_projectDir, QStringLiteral("well-1"));
  QCOMPARE(refs.size(), 1);

  // 列 0 是深度索引——不可作为曲线读（头注释契约）。
  WellCurveRef depthRef = refs.at(0);
  depthRef.column = 0;
  QString err;
  WellLogSet::WellCurveTvdSamples out;
  QVERIFY(!WellLogSet::readCurveTvd(depthRef, nullptr, &out, &err));
  QVERIFY(err.contains(QStringLiteral("无效")));

  // 越界列。
  WellCurveRef oob = refs.at(0);
  oob.column = 9;
  QVERIFY(!WellLogSet::readCurveTvd(oob, nullptr, &out, &err));
  QVERIFY(err.contains(QStringLiteral("越界")));

  // 深度单位未知：不猜折算（ft LAS 造一个）。
  const QString ftLas = writeLas(QDir(tmp.path()).filePath(QStringLiteral("ft.las")),
                                 QStringLiteral("A1"),
                                 {QStringLiteral("DEPT"), QStringLiteral("GR")},
                                 0.0, 3, QStringLiteral("FT"));
  QFile rewrite(ftLas);
  QVERIFY(rewrite.open(QIODevice::ReadOnly));
  QByteArray body = rewrite.readAll();
  rewrite.close();
  body.replace(".FT", ".XX"); // 破坏单位
  QFile w2(ftLas);
  QVERIFY(w2.open(QIODevice::WriteOnly | QIODevice::Truncate));
  w2.write(body);
  w2.close();
  WellCurveRef ftRef;
  ftRef.mnemonic = QStringLiteral("GR");
  ftRef.path = ftLas;
  ftRef.column = 1;
  err.clear();
  QVERIFY(!WellLogSet::readCurveTvd(ftRef, nullptr, &out, &err));
  QVERIFY(err.contains(QStringLiteral("单位未知")));

  // ft 单位合法折算：0.3048。
  const QString ftOk = writeLas(QDir(tmp.path()).filePath(QStringLiteral("ft2.las")),
                                QStringLiteral("A1"),
                                {QStringLiteral("DEPT"), QStringLiteral("GR")},
                                1000.0, 2, QStringLiteral("FT"));
  WellCurveRef ftOkRef;
  ftOkRef.path = ftOk;
  ftOkRef.column = 1;
  QVERIFY(WellLogSet::readCurveTvd(ftOkRef, nullptr, &out, &err));
  QVERIFY(std::fabs(out.md.at(0) - 1000.0 * 0.3048) < 1e-9);
}

QTEST_MAIN(tst_welllogset_tvd)
#include "tst_welllogset_tvd.moc"
