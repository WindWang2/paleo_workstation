// 层：测试壳
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <limits>

#include "catalog/datacatalog.h"
#include "metadata/paleoprojectstore.h"
#include "services/paleotaskservice.h"
#include "services/petrophyscomputeservice.h"
#include "services/welllogset.h"
#include "io/timedeptool.h"
#include "io/wellfileparsers.h"

#include "welllogfixturewriters.h"

// 方向 44 Oracle 2/5：混格式并集（LAS+LIS+DLIS 同一消费面）、TIME 基准经
// 时深表对齐（曲线匹配参考道）、无表「线性重采样」口径可见、timeIndexToDepth
// 与 timedeptool 逆插值契约。夹具由 welllogfixturewriters.h 逐字节生成。

using lisfix::u16be;
using paleo::petrophys::PetroPhysTaskService;

namespace
{
bool writeLasFile(const QString &path, const QString &wellName,
                  const QVector<QPair<QString, QVector<double>>> &curves)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  QTextStream ts(&f);
  ts << "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nNULL. -999.25:\n";
  if (!wellName.isEmpty())
    ts << "WELL. " << wellName << " :\n";
  ts << "~Curve\n";
  for (int i = 0; i < curves.size(); ++i)
  {
    const QString unit = i == 0 ? QStringLiteral("M") : QStringLiteral("");
    ts << curves.at(i).first << "." << unit << " :\n";
  }
  ts << "~ASCII\n";
  const int n = curves.first().second.size();
  for (int row = 0; row < n; ++row)
  {
    for (int c = 0; c < curves.size(); ++c)
    {
      const double v = curves.at(c).second.at(row);
      if (c)
        ts << " ";
      ts << (std::isnan(v) ? QStringLiteral("-999.25") : QString::number(v, 'f', 4));
    }
    ts << "\n";
  }
  return true;
}

struct LogEntry
{
  QString assetId;
  QString versionId;
  QString path;
  bool primary = false;
  int ordinal = 0;
};

bool addWellWithLogs(DataCatalog *cat, const QString &wellId,
                     const QVector<LogEntry> &logs, QString *error)
{
  CatalogEntity ent;
  ent.id = wellId;
  ent.entityType = QStringLiteral("well");
  ent.name = wellId;
  ent.hasSurface = true;
  ent.surfaceX = 100;
  ent.surfaceY = 200;
  ent.coordinateStatus = QStringLiteral("untransformed");
  ent.td = 2000;
  if (!cat->addEntity(ent, error))
    return false;
  for (const LogEntry &log : logs)
  {
    CatalogAsset asset;
    asset.id = log.assetId;
    asset.type = QStringLiteral("well_log");
    asset.format = QFileInfo(log.path).suffix().toLower();
    asset.displayName = QFileInfo(log.path).fileName();
    if (!cat->addAsset(asset, error))
      return false;
    CatalogVersion ver;
    ver.id = log.versionId;
    ver.assetId = log.assetId;
    ver.stage = QStringLiteral("RAW");
    ver.versionNumber = 1;
    ver.managed = false;
    ver.path = QFileInfo(log.path).absoluteFilePath();
    ver.fileName = QFileInfo(log.path).fileName();
    if (!cat->addVersion(ver, error))
      return false;
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = wellId;
    link.assetId = log.assetId;
    link.role = QStringLiteral("well_log");
    link.isPrimary = log.primary;
    link.ordinal = log.ordinal;
    if (!cat->addLink(link, error))
      return false;
  }
  return true;
}

// TIME 基准 DLIS：井名 W-TIME，通道 TT（单位 s）+ SONIC；TD 表
// (1000ms,1000m)-(1400ms,1004m) 线性 → md = 1000 + (t_ms-1000)/100
QByteArray makeTimeDlis(double t0s, int samples, double stepS)
{
  using namespace dlisfix;
  QVector<ChannelDef> channels = {
    // TT 用 FDOUBL：float32 存 1.40s 会落边界外一 ULP（1399.99997ms），
    // 严格不外推契约下按缺失处理——夹具用双精度钉住精确边界语义
    { QStringLiteral("TT"), QStringLiteral("s"), 7 /*FDOUBL*/, {}, QString() },
    { QStringLiteral("SONIC"), QStringLiteral("us/ft"), 7 /*FDOUBL*/, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("900"),
                                 { QStringLiteral("TT"), QStringLiteral("SONIC") },
                                 QString() /*INDEX-TYPE 缺失：单位判 TIME*/,
                                 QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("TIMEF"));
  b += buildOriginEflr(QStringLiteral("W-TIME"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  for (int i = 0; i < samples; ++i)
  {
    const double tS = t0s + stepS * i;
    const double md = 1000.0 + (tS * 1000.0 - 1000.0) / 100.0;
    QByteArray slotBytes;
    slotBytes += fdoublBytes(tS);
    slotBytes += fdoublBytes(100.0 + (md - 1000.0)); // SONIC = 参考道
    b += buildFdata(QStringLiteral("900"), quint64(i + 1), slotBytes);
  }
  return b;
}

QString writeTdFile(const QString &path, double t0ms, double d0, double t1ms, double d1)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return QString();
  QTextStream ts(&f);
  ts << "# Well : W-TIME\n";
  ts << QString::number(t0ms, 'f', 1) << " " << QString::number(d0, 'f', 1) << " "
     << QString::number(d0, 'f', 1) << " " << QString::number(d0, 'f', 1) << "\n";
  ts << QString::number(t1ms, 'f', 1) << " " << QString::number(d1, 'f', 1) << " "
     << QString::number(d1, 'f', 1) << " " << QString::number(d1, 'f', 1) << "\n";
  f.close();
  return path;
}

bool addTimeDepthLink(DataCatalog *cat, const QString &wellId, const QString &path,
                      QString *error)
{
  CatalogAsset asset;
  asset.id = QStringLiteral("ast-td");
  asset.type = QStringLiteral("time_depth");
  asset.format = QStringLiteral("dat");
  asset.displayName = QFileInfo(path).fileName();
  if (!cat->addAsset(asset, error))
    return false;
  CatalogVersion ver;
  ver.id = QStringLiteral("ver-td");
  ver.assetId = asset.id;
  ver.stage = QStringLiteral("RAW");
  ver.versionNumber = 1;
  ver.managed = false;
  ver.path = QFileInfo(path).absoluteFilePath();
  ver.fileName = QFileInfo(path).fileName();
  if (!cat->addVersion(ver, error))
    return false;
  EntityAssetLink link;
  link.entityType = QStringLiteral("well");
  link.entityId = wellId;
  link.assetId = asset.id;
  link.role = QStringLiteral("time_depth");
  link.isPrimary = true;
  return cat->addLink(link, error);
}
} // namespace

class TestWellLogMultiformat : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void mixedFormatUnionSameConsumptionSurface();
    void timeBasisAlignedViaTdTable();
    void timeBasisWithoutTdTableLabeledLinear();
    void basisMismatchNotedInScanWarnings();
    void timeIndexToDepthContract();
    void timedeptoolInverseContract();

  private:
    QTemporaryDir m_tempDir;
};

void TestWellLogMultiformat::initTestCase()
{
  QVERIFY(m_tempDir.isValid());
}

// Oracle 5：三格式曲线混入同一并集——目录/别名/取值同一接口，深度按各自
// 元数据归一（单位 M；DLIS/LIS basis 来自各自元数据）。
void TestWellLogMultiformat::mixedFormatUnionSameConsumptionSurface()
{
  const QString dir = m_tempDir.filePath(QStringLiteral("mixed"));
  QVERIFY(QDir().mkpath(dir));
  const QString las = dir + QStringLiteral("/main.las");
  const QString lis = dir + QStringLiteral("/rt.lis");
  const QString dlis = dir + QStringLiteral("/rhob.dlis");
  const QVector<double> depth{ 1000, 1000.5, 1001 };
  QVERIFY(writeLasFile(las, QStringLiteral("W-MIX"),
                       { { QStringLiteral("DEPT"), depth },
                         { QStringLiteral("GR"), { 40, 50, 60 } } }));
  {
    QFile f(lis);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(lisfix::buildMain().tif); // HZ28-6-2：DEPT/GR —— 改道独立文件
    f.close();
  }
  {
    using namespace dlisfix;
    QVector<ChannelDef> channels = {
      { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
      { QStringLiteral("RHOB"), QStringLiteral("g/cm3"), 7, {}, QString() },
    };
    QVector<FrameDef> frames = { { QStringLiteral("1000"),
                                   { QStringLiteral("DEPT"), QStringLiteral("RHOB") },
                                   QStringLiteral("BOREHOLE-DEPTH"),
                                   QString() } };
    QByteArray b = sul();
    b += buildFileHeaderEflr(QStringLiteral("MIXD"));
    b += buildOriginEflr(QStringLiteral("W-MIX"));
    b += buildChannelEflr(channels);
    b += buildFrameEflr(frames);
    QByteArray s1, s2, s3;
    s1 += fsinglBytes(1000.0f) + fdoublBytes(2.30);
    s2 += fsinglBytes(1000.5f) + fdoublBytes(2.35);
    s3 += fsinglBytes(1001.0f) + fdoublBytes(2.40);
    b += buildFdata(QStringLiteral("1000"), 1, s1);
    b += buildFdata(QStringLiteral("1000"), 2, s2);
    b += buildFdata(QStringLiteral("1000"), 3, s3);
    QFile f(dlis);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(b);
    f.close();
  }

  DataCatalog catalog;
  QVERIFY(catalog.open(dir));
  QString err;
  QVERIFY2(addWellWithLogs(&catalog, QStringLiteral("well-MIX"),
                           { { QStringLiteral("ast-a"), QStringLiteral("ver-a"), las, true, 0 },
                             { QStringLiteral("ast-b"), QStringLiteral("ver-b"), lis, false, 1 },
                             { QStringLiteral("ast-c"), QStringLiteral("ver-c"), dlis, false, 2 } },
                           &err),
           qPrintable(err));

  // 目录面：三文件、格式标注、基准归一（LIS/DLIS 元数据 → MD）
  WellLogWarnings warnings;
  const QVector<WellLogFile> files =
      WellLogSet::wellLogFiles(&catalog, dir, QStringLiteral("well-MIX"), &warnings);
  QCOMPARE(files.size(), 3);
  QCOMPARE(files.at(0).format, QStringLiteral("las"));
  QCOMPARE(files.at(1).format, QStringLiteral("lis"));
  QCOMPARE(files.at(2).format, QStringLiteral("dlis"));
  QCOMPARE(files.at(0).indexBasis, QStringLiteral("MD"));
  QCOMPARE(files.at(1).indexBasis, QStringLiteral("MD"));
  QCOMPARE(files.at(2).indexBasis, QStringLiteral("MD"));

  // 并集曲线：主文件 GR 保名 canonical；LIS/DLIS 独有名保原名
  const QVector<WellCurveRef> refs =
      WellLogSet::wellCurveIndex(&catalog, dir, QStringLiteral("well-MIX"));
  QStringList names;
  for (const WellCurveRef &r : refs)
    names.append(r.mnemonic);
  QVERIFY(names.contains(QStringLiteral("GR")));
  QVERIFY(names.contains(QStringLiteral("RHOB")));
  QCOMPARE(refs.size(), 3); // GR + RHOB + LIS 的 GR（重名走别名）

  // 取值面：readCurveTvd 对 DLIS 列直读（同一接口，格式无感）
  const WellCurveRef *rhob = nullptr;
  for (const WellCurveRef &r : refs)
    if (r.mnemonic == QLatin1String("RHOB"))
      rhob = &r;
  QVERIFY(rhob != nullptr);
  WellLogSet::WellCurveTvdSamples samples;
  QString rerr;
  QVERIFY2(WellLogSet::readCurveTvd(*rhob, nullptr, &samples, &rerr), qPrintable(rerr));
  QCOMPARE(samples.md, QVector<double>({ 1000.0, 1000.5, 1001.0 }));
  QCOMPARE(samples.values, QVector<double>({ 2.30, 2.35, 2.40 }));
}

// Oracle 2：带时深表者对齐后曲线匹配参考道（SONIC = 100 + (md-1000)）
void TestWellLogMultiformat::timeBasisAlignedViaTdTable()
{
  const QString dir = m_tempDir.filePath(QStringLiteral("aligned"));
  QVERIFY(QDir().mkpath(dir));
  const QString las = dir + QStringLiteral("/drv.las");
  const QString dlis = dir + QStringLiteral("/time.dlis");
  const QString td = dir + QStringLiteral("/td.dat");
  const QVector<double> depth{ 1000, 1001, 1002, 1003, 1004 };
  QVERIFY(writeLasFile(las, QStringLiteral("W-TIME"),
                       { { QStringLiteral("DEPT"), depth },
                         { QStringLiteral("GR"), { 40, 45, 50, 55, 60 } } }));
  {
    QFile f(dlis);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(makeTimeDlis(1.0, 9, 0.05)); // 1.00..1.40s → md 1000..1004
    f.close();
  }
  QVERIFY(!writeTdFile(td, 1000, 1000, 1400, 1004).isEmpty());

  DataCatalog catalog;
  QVERIFY(catalog.open(dir));
  QString err;
  QVERIFY2(addWellWithLogs(&catalog, QStringLiteral("well-TIME"),
                           { { QStringLiteral("ast-d"), QStringLiteral("ver-d"), las, true, 0 },
                             { QStringLiteral("ast-t"), QStringLiteral("ver-t"), dlis, false, 1 } },
                           &err) &&
               addTimeDepthLink(&catalog, QStringLiteral("well-TIME"), td, &err),
           qPrintable(err));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);
  PetroPhysTaskService::BatchRequest req;
  PetroPhysTaskService::WellRef well;
  well.wellId = QStringLiteral("well-TIME");
  well.lasPath = QFileInfo(las).absoluteFilePath();
  well.sourceVersionId = QStringLiteral("ver-d");
  req.wells.append(well);
  req.formula = PetroPhysTaskService::Formula::Expression;
  // 双曲线引用不做曲线驱动（歧义）→ 驱动回退主文件 LAS（MD 基准）；
  // 0*GR 使结果即重采样后的 SONIC（对齐正确性的直接断言面）
  req.expression = QStringLiteral("SONIC + 0*GR");
  req.outputMnemonic = QStringLiteral("SONIC_COPY");
  req.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(req, &catalog, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res) {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QCOMPARE(batch.wells.size(), 1);
  QVERIFY2(batch.wells.at(0).ok, qPrintable(batch.wells.at(0).error));

  // 口径可见：note 如实声明已用时深表对齐
  bool sawAlignedNote = false;
  for (const QString &n : batch.wells.at(0).notes)
    if (n.contains(QStringLiteral("已按时深表对齐")))
      sawAlignedNote = true;
  QVERIFY2(sawAlignedNote, "对齐口径 note 必须可见");

  // 对齐结果：SONIC 透传值 == 参考道 100 + (md-1000)
  const QVector<double> expect{ 100, 101, 102, 103, 104 };
  QCOMPARE(batch.wells.at(0).values.size(), 5);
  for (int i = 0; i < 5; ++i)
    QVERIFY2(std::fabs(batch.wells.at(0).values.at(i) - expect.at(i)) < 1e-6,
             qPrintable(QStringLiteral("i=%1 got=%2 want=%3")
                            .arg(i)
                            .arg(batch.wells.at(0).values.at(i))
                            .arg(expect.at(i))));
}

// Oracle 2：无时深表 → 「线性重采样」口径可见，不冒充已对齐
void TestWellLogMultiformat::timeBasisWithoutTdTableLabeledLinear()
{
  const QString dir = m_tempDir.filePath(QStringLiteral("notd"));
  QVERIFY(QDir().mkpath(dir));
  const QString las = dir + QStringLiteral("/drv2.las");
  const QString dlis = dir + QStringLiteral("/time2.dlis");
  const QVector<double> depth{ 1000, 1001, 1002 };
  QVERIFY(writeLasFile(las, QStringLiteral("W-TIME"),
                       { { QStringLiteral("DEPT"), depth },
                         { QStringLiteral("GR"), { 40, 45, 50 } } }));
  {
    QFile f(dlis);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(makeTimeDlis(1.0, 5, 0.05));
    f.close();
  }

  DataCatalog catalog;
  QVERIFY(catalog.open(dir));
  QString err;
  QVERIFY2(addWellWithLogs(&catalog, QStringLiteral("well-NT"),
                           { { QStringLiteral("ast-d"), QStringLiteral("ver-d"), las, true, 0 },
                             { QStringLiteral("ast-t"), QStringLiteral("ver-t"), dlis, false, 1 } },
                           &err),
           qPrintable(err));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);
  PetroPhysTaskService::BatchRequest req;
  PetroPhysTaskService::WellRef well;
  well.wellId = QStringLiteral("well-NT");
  well.lasPath = QFileInfo(las).absoluteFilePath();
  well.sourceVersionId = QStringLiteral("ver-d");
  req.wells.append(well);
  req.formula = PetroPhysTaskService::Formula::Expression;
  req.expression = QStringLiteral("SONIC + 0*GR");
  req.outputMnemonic = QStringLiteral("SONIC_COPY");
  req.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(req, &catalog, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res) {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QCOMPARE(batch.wells.size(), 1);
  bool sawLinearNote = false;
  for (const QString &n : batch.wells.at(0).notes)
    if (n.contains(QStringLiteral("线性重采样")) && n.contains(QStringLiteral("无时深表")))
      sawLinearNote = true;
  QVERIFY2(sawLinearNote, "无时深表必须标「线性重采样」口径");
}

// 扫描面事实告警：基准不一致逐文件列因（可见于 warnings 消费方）
void TestWellLogMultiformat::basisMismatchNotedInScanWarnings()
{
  const QString dir = m_tempDir.filePath(QStringLiteral("mismatch"));
  QVERIFY(QDir().mkpath(dir));
  const QString las = dir + QStringLiteral("/b1.las");
  const QString dlis = dir + QStringLiteral("/b2.dlis");
  const QVector<double> depth{ 1000, 1001 };
  QVERIFY(writeLasFile(las, QStringLiteral("W-B"),
                       { { QStringLiteral("DEPT"), depth },
                         { QStringLiteral("GR"), { 40, 45 } } }));
  {
    QFile f(dlis);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(makeTimeDlis(1.0, 3, 0.05));
    f.close();
  }
  DataCatalog catalog;
  QVERIFY(catalog.open(dir));
  QString err;
  QVERIFY2(addWellWithLogs(&catalog, QStringLiteral("well-B"),
                           { { QStringLiteral("ast-1"), QStringLiteral("ver-1"), las, true, 0 },
                             { QStringLiteral("ast-2"), QStringLiteral("ver-2"), dlis, false, 1 } },
                           &err),
           qPrintable(err));

  WellLogWarnings warnings;
  const QVector<WellLogFile> files =
      WellLogSet::wellLogFiles(&catalog, dir, QStringLiteral("well-B"), &warnings);
  QCOMPARE(files.size(), 2);
  QCOMPARE(files.at(1).indexBasis, QStringLiteral("TIME"));
  bool sawNote = false;
  for (const QString &m : warnings.messages)
    if (m.contains(QStringLiteral("深度基准")) && m.contains(QStringLiteral("TIME")))
      sawNote = true;
  QVERIFY2(sawNote, "基准不一致必须在扫描告警里可见");
}

// timeIndexToDepth 契约：单位折算/超表 NaN/表不可用 false 且原样不动
void TestWellLogMultiformat::timeIndexToDepthContract()
{
  TimeDepthTable td;
  TdRow a, b;
  a.timeMs = 1000;
  a.tvdss = 1000;
  a.tvd = 1000;
  a.md = 1000;
  a.hasTvd = a.hasMd = true;
  b.timeMs = 2000;
  b.tvdss = 1100;
  b.tvd = 1100;
  b.md = 1100;
  b.hasTvd = b.hasMd = true;
  td.rows = { a, b };

  QVector<double> idx{ 1.5, 1.0, 2.0, 2.5 }; // 秒；含端点与超表样点
  QString err;
  QVERIFY(WellLogSet::timeIndexToDepth(&idx, QStringLiteral("s"), td, true, &err));
  QCOMPARE(idx.at(0), 1050.0);
  QCOMPARE(idx.at(1), 1000.0);
  QCOMPARE(idx.at(2), 1100.0);
  QVERIFY(std::isnan(idx.at(3))); // 超表不外推

  // ms 原样；未知单位拒绝
  QVector<double> msIdx{ 1500 };
  QVERIFY(WellLogSet::timeIndexToDepth(&msIdx, QStringLiteral("ms"), td, true, &err));
  QCOMPARE(msIdx.at(0), 1050.0);
  QVector<double> bad{ 1500 };
  QVERIFY(!WellLogSet::timeIndexToDepth(&bad, QStringLiteral("furlongs"), td, true, &err));
  QVERIFY(!err.isEmpty());

  // 表不可用：false + 原样不动
  TimeDepthTable empty;
  QVector<double> keep{ 1.5 };
  QVERIFY(!WellLogSet::timeIndexToDepth(&keep, QStringLiteral("s"), empty, true, &err));
  QCOMPARE(keep.at(0), 1.5);
}

// timedeptool 逆插值契约（与 interpolateTimeMs 同源）
void TestWellLogMultiformat::timedeptoolInverseContract()
{
  TimeDepthTable td;
  TdRow a, b, c;
  a.timeMs = 100;
  a.tvdss = 1000;
  a.tvd = 1002;
  a.md = 1000;
  a.hasTvd = a.hasMd = true;
  b.timeMs = 200;
  b.tvdss = 1100;
  b.tvd = 1102;
  b.md = 1100;
  b.hasTvd = b.hasMd = true;
  c.timeMs = 150; // 无序（timeMs 回落）
  c.tvdss = 1050;
  c.tvd = 1052;
  c.md = 1050;
  c.hasTvd = c.hasMd = true;
  td.rows = { a, b };

  auto r = TimeDepthTool::interpolateDepthAtTimeMs(td, 150, true);
  QVERIFY(r.ok());
  QCOMPARE(r.depth, 1050.0);
  auto rTvd = TimeDepthTool::interpolateDepthAtTimeMs(td, 150, false);
  QVERIFY(rTvd.ok());
  QCOMPARE(rTvd.depth, 1052.0);
  auto out = TimeDepthTool::interpolateDepthAtTimeMs(td, 50, true);
  QVERIFY(out.status == TimeDepthTool::TdStatus::OutOfRange);

  td.rows = { a };
  QVERIFY(TimeDepthTool::interpolateDepthAtTimeMs(td, 150, true).status ==
          TimeDepthTool::TdStatus::NoTable);
  td.rows = { a, b, c }; // 100→200→150：timeMs 回落
  QVERIFY(TimeDepthTool::interpolateDepthAtTimeMs(td, 150, true).status ==
          TimeDepthTool::TdStatus::NonMonotonic);
}

QTEST_MAIN(TestWellLogMultiformat)
#include "tst_welllog_multiformat.moc"
