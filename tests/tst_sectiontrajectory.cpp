// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

#include "catalog/datacatalog.h"
#include "domain/deviationsurvey.h"
#include "services/projectdata.h"
#include "workflow/sectionworkbench.h"
#include "workflow/welltrajectorylayer.h"

#include <cmath>
#include <numbers>
#include <set>

// goal/well-trajectory 轮4：消费面迁移验收（Oracle 第 4/5/6 条）——
// 剖面井轨按真实轨迹（投影坐标 = 井口 + 位移；无测斜井轨迹空、垂直简化
// 保持）；平面轨迹线 GeoJSON 端点 = 井底投影坐标；老工程（无 deviation
// 链接）全直井路径行为不变。
class tst_sectiontrajectory : public QObject
{
  Q_OBJECT

private slots:
  void sectionWellCarriesTrajectory();
  void sectionWellWithoutSurveyStaysVertical();
  void mapLayerEndpointsAreBottomProjection();
  void mapLayerEmptyWithoutSurveys();

private:
  // 剖面线：沿 +x 的水平线 y=2000，长 4000（井口 x=1000 处横穿）。
  static glm::dvec2 projectOntoLine(double x, double y)
  {
    return {x, 2000.0};
  }

  static QString writeText(const QString &path, const QByteArray &content)
  {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    f.write(content);
    f.close();
    return path;
  }

  static QString writeLas(const QString &path, double start, int rows)
  {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    QTextStream out(&file);
    out << "~Version Information\n VERS. 2.0:\n WRAP. NO:\n~Well\n";
    out << " STRT.M " << QString::number(start, 'f', 2) << ":\n";
    out << " STOP.M " << QString::number(start + rows - 1, 'f', 2) << ":\n";
    out << " STEP.M 1.0:\n NULL. -999.25:\n WELL. A1:\n~Curve\n DEPT.M:\n GR.API:\n~A\n";
    for (int r = 0; r < rows; ++r)
      out << QString::number(start + r, 'f', 2) << " " << QString::number(50.0 + r) << "\n";
    file.close();
    return path;
  }

  // 造斜-稳斜（方位 90°=正东）：0-1000m 0°→30°，1000-2000m 稳斜 30°。
  static QByteArray devText()
  {
    return QByteArray("# Well : A1\n# MD INCL AZI\n"
                      "0.0 0.0 90.0\n"
                      "1000.0 30.0 90.0\n"
                      "2000.0 30.0 90.0\n");
  }

  bool seed(QTemporaryDir &tmp, bool withDeviation, bool withLog = true)
  {
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    if (!QDir().mkpath(projectDir))
      return false;
    if (!m_cat.open(projectDir))
      return false;
    m_projectDir = projectDir;
    CatalogEntity w;
    w.id = QStringLiteral("well-1");
    w.entityType = QStringLiteral("well");
    w.name = QStringLiteral("A1");
    w.hasSurface = true;
    w.surfaceX = 1000.0;
    w.surfaceY = 2000.0; // 剖面线 y=2000 上
    w.td = 2000.0;
    w.coordinateStatus = QStringLiteral("untransformed");
    if (!m_cat.addEntity(w))
      return false;

    if (withLog)
    {
      CatalogAsset asset;
      asset.id = QStringLiteral("ast-las");
      asset.type = QStringLiteral("well_log");
      asset.format = QStringLiteral("las");
      asset.displayName = QStringLiteral("A1.Las");
      if (!m_cat.addAsset(asset))
        return false;
      CatalogVersion ver;
      ver.id = QStringLiteral("ver-las");
      ver.assetId = asset.id;
      ver.stage = QStringLiteral("RAW");
      ver.path = DataCatalog::managedPath(QStringLiteral("raw"), asset.id, ver.id,
                                          QStringLiteral("A1.Las"));
      if (!m_cat.addVersion(ver))
        return false;
      writeLas(QDir(projectDir).filePath(ver.path), 0.0, 2000);
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = QStringLiteral("well-1");
      l.assetId = asset.id;
      l.role = QStringLiteral("well_log");
      l.isPrimary = true;
      if (!m_cat.addLink(l))
        return false;
    }
    if (withDeviation)
    {
      CatalogAsset dev;
      dev.id = QStringLiteral("ast-dev");
      dev.type = QStringLiteral("well_deviation");
      dev.format = QStringLiteral("dat");
      dev.displayName = QStringLiteral("A1-deviation.dat");
      if (!m_cat.addAsset(dev))
        return false;
      CatalogVersion ver;
      ver.id = QStringLiteral("ver-dev");
      ver.assetId = dev.id;
      ver.stage = QStringLiteral("RAW");
      ver.path = DataCatalog::managedPath(QStringLiteral("raw"), dev.id, ver.id,
                                          QStringLiteral("A1-deviation.dat"));
      if (!m_cat.addVersion(ver))
        return false;
      writeText(QDir(projectDir).filePath(ver.path), devText());
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = QStringLiteral("well-1");
      l.assetId = dev.id;
      l.role = QStringLiteral("trajectory");
      l.isPrimary = true;
      if (!m_cat.addLink(l))
        return false;
    }
    return true;
  }

  DataCatalog m_cat;
  QString m_projectDir;
};

void tst_sectiontrajectory::sectionWellCarriesTrajectory()
{
  QTemporaryDir tmp;
  QVERIFY(seed(tmp, true));
  SectionWorkbench bench(&m_cat);
  const std::vector<seismic::SectionWellInfo> wells = bench.sectionWells();
  QCOMPARE(wells.size(), std::size_t(1));
  const seismic::SectionWellInfo &w = wells.front();
  QCOMPARE(w.trajectory.size(), std::size_t(3));

  // 数值锚点：造斜段闭式（0→30° 全狗腿，方位恒 90°）。
  const double beta = std::acos(std::cos(30.0 * std::numbers::pi / 180.0));
  const double rf = (2.0 / beta) * std::tan(beta * 0.5);
  const double buildTvd = 500.0 * (1.0 + std::cos(30.0 * std::numbers::pi / 180.0)) * rf;
  const double buildEast = 500.0 * (0.0 + std::sin(30.0 * std::numbers::pi / 180.0)) * rf;
  QVERIFY(std::fabs(w.trajectory.at(1).tvd - buildTvd) < 1e-9);
  QVERIFY(std::fabs(w.trajectory.at(1).x - (1000.0 + buildEast)) < 1e-9);
  QCOMPARE(w.trajectory.at(1).y, 2000.0); // 方位正东：北向位移 0
  // 稳斜段：TVD/East 各按 cos/sin30 直线推进。
  QVERIFY(std::fabs(w.trajectory.at(2).tvd - (buildTvd + 1000.0 * std::cos(30.0 * std::numbers::pi / 180.0))) < 1e-9);
  QVERIFY(std::fabs(w.trajectory.at(2).x - (1000.0 + buildEast + 1000.0 * std::sin(30.0 * std::numbers::pi / 180.0))) < 1e-9);

  // 剖面横向投影：井轨沿线 y=2000 展开 → 各站 trace 距离 = 东向位移。
  // （SectionWellProjector 的横向坐标是沿剖面线累计距离——x 即位移量。）
  const double alongTop = w.trajectory.at(0).x - 1000.0;
  const double alongEnd = w.trajectory.at(2).x - 1000.0;
  QCOMPARE(alongTop, 0.0);
  QVERIFY(std::fabs(alongEnd - (buildEast + 1000.0 * std::sin(30.0 * std::numbers::pi / 180.0))) < 1e-9);

  // 井底坐标同步补齐（数据面），TD=2000 与末站重合无外延。
  QVERIFY(std::fabs(w.bottomX - w.trajectory.at(2).x) < 1e-9);
  QCOMPARE(w.bottomY, 2000.0);
}

void tst_sectiontrajectory::sectionWellWithoutSurveyStaysVertical()
{
  QTemporaryDir tmp;
  QVERIFY(seed(tmp, false));
  SectionWorkbench bench(&m_cat);
  const std::vector<seismic::SectionWellInfo> wells = bench.sectionWells();
  QCOMPARE(wells.size(), std::size_t(1));
  const seismic::SectionWellInfo &w = wells.front();
  QVERIFY(w.trajectory.empty()); // 无测斜：不虚构造斜（垂直简化由视图兜）
  QCOMPARE(w.bottomX, 0.0);      // 老路径未填井底——与迁移前行为一致
  QCOMPARE(w.bottomY, 0.0);
}

void tst_sectiontrajectory::mapLayerEndpointsAreBottomProjection()
{
  QTemporaryDir tmp;
  QVERIFY(seed(tmp, true));
  ProjectDataFacade facade;
  facade.setCatalog(&m_cat, m_projectDir);
  QString err;
  const QByteArray bytes = paleo::wellTrajectoriesGeoJson(&facade, &m_cat, &err);
  QVERIFY2(!bytes.isEmpty(), qPrintable(err));
  QVERIFY(err.isEmpty());

  const QJsonDocument doc = QJsonDocument::fromJson(bytes);
  QVERIFY(doc.isObject());
  const QJsonObject root = doc.object();
  QCOMPARE(root.value(QStringLiteral("type")).toString(),
           QStringLiteral("FeatureCollection"));
  const QJsonArray feats = root.value(QStringLiteral("features")).toArray();
  QCOMPARE(feats.size(), 1);
  const QJsonObject geom = feats.at(0).toObject().value(QStringLiteral("geometry")).toObject();
  QCOMPARE(geom.value(QStringLiteral("type")).toString(), QStringLiteral("LineString"));
  const QJsonArray coords = geom.value(QStringLiteral("coordinates")).toArray();
  QCOMPARE(coords.size(), 3);

  // 端点断言（Oracle 4 平面图）：首点 = 井口，末点 = 井底投影坐标。
  QCOMPARE(coords.at(0).toArray().at(0).toDouble(), 1000.0);
  QCOMPARE(coords.at(0).toArray().at(1).toDouble(), 2000.0);
  const auto survey = facade.trajectoryFor(QStringLiteral("well-1"));
  QVERIFY(survey.has_value());
  const paleo::TrajectoryPoint tip = survey->pointAt(2000.0); // TD
  QCOMPARE(coords.at(2).toArray().at(0).toDouble(), 1000.0 + tip.east);
  QCOMPARE(coords.at(2).toArray().at(1).toDouble(), 2000.0 + tip.north);
  // 属性带井身份（图层面板/联动消费）。
  const QJsonObject props = feats.at(0).toObject().value(QStringLiteral("properties")).toObject();
  QCOMPARE(props.value(QStringLiteral("id")).toString(), QStringLiteral("well-1"));
  QCOMPARE(props.value(QStringLiteral("name")).toString(), QStringLiteral("A1"));
}

void tst_sectiontrajectory::mapLayerEmptyWithoutSurveys()
{
  QTemporaryDir tmp;
  QVERIFY(seed(tmp, false));
  ProjectDataFacade facade;
  facade.setCatalog(&m_cat, m_projectDir);
  QString err;
  QVERIFY(paleo::wellTrajectoriesGeoJson(&facade, &m_cat, &err).isEmpty());
  QVERIFY(err.isEmpty()); // 没有测斜（区别于测斜坏：那种情况 error 非空）
}

QTEST_MAIN(tst_sectiontrajectory)
#include "tst_sectiontrajectory.moc"
