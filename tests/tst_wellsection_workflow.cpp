#include "catalog/datacatalog.h"
#include "domain/faultset.h"
#include "domain/seismic/sgyvolume.h"
#include "metadata/faultsetstore.h"
#include "metadata/wellsectionstore.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellsectionworkflow.h"
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>

namespace {

QString writeLas(const QString &path, const QString &depthUnit,
                 const QStringList &mnems, double startDepth, int rows) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return QString();
  QTextStream out(&file);
  out << "~Version Information\n";
  out << " VERS.                  2.0:   CWLS log ASCII Standard -VERSION 2.0\n";
  out << " WRAP.                   NO:   One line per depth step\n";
  out << "~Well Information Block\n";
  out << " STRT." << depthUnit << "        "
      << QString::number(startDepth, 'f', 4) << ":\n";
  out << " STOP." << depthUnit << "        "
      << QString::number(startDepth + rows - 1, 'f', 4) << ":\n";
  out << " STEP." << depthUnit << "          1.0000:\n";
  out << " NULL.        -999.2500:\n";
  out << " WELL.         TEST_WELL:\n";
  out << "~Curve Information Block\n";
  for (int c = 0; c < mnems.size(); ++c) {
    const QString unit = c == 0 ? depthUnit : QStringLiteral("GAPI");
    out << " " << mnems[c] << "." << unit << "                  :   " << mnems[c]
        << "\n";
  }
  out << "~A\n";
  for (int r = 0; r < rows; ++r) {
    out << QString::number(startDepth + r, 'f', 2);
    for (int c = 1; c < mnems.size(); ++c)
      out << " " << QString::number(10.0 * c + r, 'f', 2);
    out << "\n";
  }
  file.close();
  return path;
}

bool addEntity(DataCatalog &cat, const QString &id, const QString &name,
               bool coords, double x, double y, double td) {
  CatalogEntity e;
  e.id = id;
  e.entityType = QStringLiteral("well");
  e.name = name;
  e.hasSurface = coords;
  e.surfaceX = x;
  e.surfaceY = y;
  e.coordinateStatus = coords ? QStringLiteral("untransformed")
                              : QStringLiteral("missing");
  e.td = td;
  return cat.addEntity(e);
}

bool addFileLink(DataCatalog &cat, const QString &wellId, const QString &role,
                 const QString &assetId, const QString &path,
                 const QString &type, QString *err) {
  CatalogAsset a;
  a.id = assetId;
  a.type = type;
  a.displayName = QFileInfo(path).fileName();
  if (a.displayName.isEmpty())
    a.displayName = assetId;
  if (!cat.addAsset(a, err))
    return false;
  CatalogVersion v;
  v.id = "v-" + assetId;
  v.assetId = assetId;
  v.managed = false;
  v.path = path;
  v.stage = QStringLiteral("RAW");
  if (!cat.addVersion(v, err))
    return false;
  EntityAssetLink l;
  l.entityId = wellId;
  l.entityType = QStringLiteral("well");
  l.role = role;
  l.assetId = assetId;
  l.isPrimary = true;
  return cat.addLink(l, err);
}

bool writeText(const QString &path, const QString &text) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  f.write(text.toUtf8());
  f.close();
  return QFile::exists(path);
}

// 三井夹具：well-1 坐标+分层+LAS(m)+TD；well-2 坐标+分层+LAS(FT)；
// well-3 无坐标+分层+LAS(未知深度单位)。
bool buildCatalog(DataCatalog &cat, const QDir &dir, QString *err) {
  if (!addEntity(cat, QStringLiteral("well-1"), QStringLiteral("A1"), true,
                 100, 0, 350) ||
      !addEntity(cat, QStringLiteral("well-2"), QStringLiteral("A2"), true,
                 200, 0, 0) ||
      !addEntity(cat, QStringLiteral("well-3"), QStringLiteral("A3"), false, 0,
                 0, 0))
    return false;
  const QString topsPath = dir.filePath(QStringLiteral("tops.dat"));
  // A1 分层故意倒序 + 一行 -99999 缺 MD。
  if (!writeText(topsPath, QStringLiteral(
                               "#WellTops File From SMI\n"
                               "#WellName    Name         MD           X            Y            Z            TVD          Time(ms)\n"
                               "A1           B            200.000      100.000      0.000        -190.000     190.000      -99999.000\n"
                               "A1           C            -99999.000   100.000      0.000        -290.000     290.000      -99999.000\n"
                               "A1           A            100.000      100.000      0.000        -90.000      90.000       -99999.000\n"
                               "A2           A            110.000      200.000      0.000        -100.000     100.000      -99999.000\n"
                               "A2           B            210.000      200.000      0.000        -200.000     200.000      -99999.000\n"
                               "A3           A            120.000      0.000        0.000        -110.000     110.000      -99999.000\n")))
    return false;
  for (const QString &id : {QStringLiteral("well-1"), QStringLiteral("well-2"),
                            QStringLiteral("well-3")})
    if (!addFileLink(cat, id, QStringLiteral("tops"),
                     QStringLiteral("ast-tops-%1").arg(id), topsPath,
                     QStringLiteral("well_stratification"), err))
      return false;
  const QString tdPath = dir.filePath(QStringLiteral("a1_td.dat"));
  if (!writeText(tdPath, QStringLiteral(
                             "#TimeDepth File From SMI\n"
                             "# Well : A1\n"
                             "#TIME            TVDSS            TVD            MD\n"
                             "100.000          -1000.000        1000.000       1000.000\n"
                             "200.000          -2000.000        2000.000       2000.000\n")))
    return false;
  if (!addFileLink(cat, QStringLiteral("well-1"), QStringLiteral("time_depth"),
                   QStringLiteral("ast-td"), tdPath,
                   QStringLiteral("time_depth"), err))
    return false;
  const QString las1 =
      writeLas(dir.filePath(QStringLiteral("a1.las")), QStringLiteral("M"),
               {QStringLiteral("DEPT"), QStringLiteral("NGR"),
                QStringLiteral("RT")},
               500.0, 5);
  const QString las2 =
      writeLas(dir.filePath(QStringLiteral("a2.las")), QStringLiteral("FT"),
               {QStringLiteral("DEPT"), QStringLiteral("GR"),
                QStringLiteral("NPHI")},
               1000.0, 3);
  const QString las3 =
      writeLas(dir.filePath(QStringLiteral("a3.las")), QStringLiteral("QQ"),
               {QStringLiteral("DEPT"), QStringLiteral("SP")}, 300.0, 3);
  if (las1.isEmpty() || las2.isEmpty() || las3.isEmpty())
    return false;
  return addFileLink(cat, QStringLiteral("well-1"), QStringLiteral("well_log"),
                     QStringLiteral("ast-las1"), las1,
                     QStringLiteral("well_log"), err) &&
         addFileLink(cat, QStringLiteral("well-2"), QStringLiteral("well_log"),
                     QStringLiteral("ast-las2"), las2,
                     QStringLiteral("well_log"), err) &&
         addFileLink(cat, QStringLiteral("well-3"), QStringLiteral("well_log"),
                     QStringLiteral("ast-las3"), las3,
                     QStringLiteral("well_log"), err);
}

// 4x4 合成 SEG-Y（inlines 1000-1003 / xlines 2000-2003 / 64 样点 / 2000us），
// 同 tst_sections_alignment 夹具。
bool writeSegy(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(QByteArray(3200, ' '));
  QByteArray binary(400, 0);
  auto put16 = [](QByteArray &data, int offset, qint16 value) {
    qToBigEndian(value, reinterpret_cast<uchar *>(data.data()) + offset);
  };
  auto put32 = [](QByteArray &data, int offset, qint32 value) {
    qToBigEndian(value, reinterpret_cast<uchar *>(data.data()) + offset);
  };
  put16(binary, 12, 4);
  put16(binary, 16, 2000);
  put16(binary, 20, 64);
  put16(binary, 24, 5);
  file.write(binary);
  for (int il = 0; il < 4; ++il)
    for (int xl = 0; xl < 4; ++xl) {
      QByteArray header(240, 0);
      put32(header, 0, il * 4 + xl + 1);
      put32(header, 188, 1000 + il);
      put32(header, 192, 2000 + xl);
      put16(header, 114, 64);
      file.write(header);
      QByteArray samples(64 * 4, 0);
      for (int k = 0; k < 64; ++k) {
        const float v = std::sin(k * .4f + il + xl);
        quint32 bits;
        std::memcpy(&bits, &v, 4);
        qToBigEndian(bits, reinterpret_cast<uchar *>(samples.data()) + 4 * k);
      }
      file.write(samples);
    }
  file.close();
  return QFile::exists(path);
}

SurveyGridGeometry makeGrid() {
  SurveyGridGeometry grid;
  grid.valid = true;
  grid.a = 100;
  grid.d = 100;
  grid.p1Inline = 1000;
  grid.p1Xline = 2000;
  grid.inlineMin = 1000;
  grid.inlineMax = 1003;
  grid.xlineMin = 2000;
  grid.xlineMax = 2003;
  return grid;
}

wellsection::Well sectionWell(const QString &name, double x, double y,
                              bool withTd) {
  wellsection::Well w;
  w.name = name;
  w.x = x;
  w.y = y;
  if (withTd) {
    seismic::TimeDepthModel model;
    model.setCheckshots({{1000, 100}, {2000, 200}});
    w.timeDepth = wellsection::TimeDepth{model, 0.0, QString("时深表")};
  }
  return w;
}

// 网格内坐标 ↔ (inline,xline)：x = 100*(xl-2000), y = 100*(inl-1000)。
wellsection::Well gridWell(const QString &name, int inl, int xl, bool withTd) {
  return sectionWell(name, (xl - 2000) * 100.0, (inl - 1000) * 100.0, withTd);
}

} // namespace

class TestWellSectionWorkflow : public QObject {
  Q_OBJECT
private slots:
  void syncBuild() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));

    WellSectionWorkflow wf(&cat);
    const auto choices = wf.wellChoices();
    QCOMPARE(choices.size(), 3);
    QCOMPARE(choices[0].id, QString("well-1"));
    QVERIFY(choices[0].hasCoordinates);
    QVERIFY(choices[2].hasCoordinates == false);
    QCOMPARE(wf.availableMnemonics({"well-1", "well-2", "well-3"}),
             QStringList({"GR", "NGR", "NPHI", "RT", "SP"}));

    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    // 顺序按请求；well-9 不存在 → 告警跳过。
    // SP 只存在于 well-3 的未知单位文件——请求它才触发文件装载/告警。
    const int gen = wf.request({"well-2", "well-1", "well-3", "well-9"},
                               {"GR", "RT", "SP"});
    QCOMPARE(spy.size(), 1); // 无任务服务 → 同步发射
    QCOMPARE(spy[0][0].toInt(), gen);
    const auto wells =
        spy[0][1].value<QVector<wellsection::Well>>();
    const auto warnings = spy[0][2].toStringList();
    QCOMPARE(wells.size(), 3);
    QCOMPARE(wells[0].id, QString("well-2"));
    QCOMPARE(wells[1].id, QString("well-1"));
    QCOMPARE(wells[2].id, QString("well-3"));
    const QString warnBlob = warnings.join(QLatin1Char('\n'));
    QVERIFY(warnBlob.contains(QStringLiteral("未找到井 well-9")));
    QVERIFY(warnBlob.contains(QStringLiteral("缺 MD")));
    QVERIFY(warnBlob.contains(QStringLiteral("深度单位未知")));

    const auto &w1 = wells[1];
    QCOMPARE(w1.tops.size(), 2); // C 顶缺 MD 未参与
    QCOMPARE(w1.tops[0].name, QString("A"));
    QCOMPARE(w1.tops[0].md, 100.0); // 分层已按 MD 升序
    QCOMPARE(w1.tops[1].md, 200.0);
    QCOMPARE(w1.totalDepth, 350.0);
    QVERIFY(w1.hasCoordinates());
    QCOMPARE(w1.x, 100.0);
    QVERIFY(w1.timeDepth.has_value());
    QCOMPARE(w1.timeDepth->status, QStringLiteral("时深表"));
    QCOMPARE(w1.timeDepth->twtAt(1500), 150.0);
    const auto *gr = w1.curve("GR");
    QVERIFY(gr); // 别名：请求 GR → 实际 NGR
    QCOMPARE(gr->mnemonic, QString("GR"));
    QCOMPARE(gr->sourceMnemonic, QString("NGR"));
    QCOMPARE(gr->depths.first(), 500.0f);
    QCOMPARE(gr->values.first(), 10.0f);
    QCOMPARE(gr->depths.size(), 5);
    const auto *rt = w1.curve("RT");
    QVERIFY(rt);
    QCOMPARE(rt->sourceMnemonic, QString("RT"));

    const auto &w2 = wells[0];
    QVERIFY(!w2.timeDepth.has_value()); // 无时深关联
    QVERIFY(std::isnan(w2.totalDepth));
    const auto *gr2 = w2.curve("GR");
    QVERIFY(gr2);
    QCOMPARE(gr2->sourceMnemonic, QString("GR"));
    QVERIFY(qAbs(gr2->depths[0] - 304.8f) < 1e-3f); // FT → 米
    QVERIFY(qAbs(gr2->depths[2] - 305.4096f) < 1e-3f);
    QVERIFY(!w2.curve("RT"));

    const auto &w3 = wells[2];
    QVERIFY(!w3.hasCoordinates());
    QVERIFY(w3.curves.isEmpty()); // 未知深度单位整文件跳过
    QCOMPARE(w3.tops.size(), 1);
  }

  // 补心海拔链路：catalog 实体 kb → ProjectWell → wellsection::Well
  //（海拔基准面的取数前提；缺数据井 kb=0，海拔模式退化为井深模式）。
  void kbPropagation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    CatalogEntity e;
    e.id = QStringLiteral("well-1");
    e.entityType = QStringLiteral("well");
    e.name = QStringLiteral("A1");
    e.hasSurface = true;
    e.surfaceX = 100;
    e.surfaceY = 0;
    e.coordinateStatus = QStringLiteral("untransformed");
    e.kb = 25.5;
    QVERIFY2(cat.addEntity(e), "addEntity");
    WellSectionWorkflow wf(&cat);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    wf.request({QStringLiteral("well-1")}, {});
    QCOMPARE(spy.size(), 1);
    const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
    QCOMPARE(wells.size(), 1);
    QCOMPARE(wells[0].kb, 25.5);
    // 井深表/偏移走 datum（Elevation → 25.5）。
    QCOMPARE(wellsection::datumOffset(
                 wells[0],
                 wellsection::Datum{wellsection::DatumMode::Elevation, QString()}),
             25.5);
  }

  // 井序/连线改接 round-trip 写回 project.sqlite 且版本号正确推进（Oracle #2）。
  void sectionStoreRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = QDir(dir.path()).filePath(QStringLiteral(
        "test.project.sqlite"));
    metadata::WellSectionStore store(dbPath);
    QString err;
    QVERIFY2(store.open(&err), qPrintable(err));

    // 新库无行 → version 0。
    auto rec = store.load(QStringLiteral("default"), &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(rec.version, 0);
    QVERIFY(!rec.valid());

    // 首存：井序 + 断开的连线；version 1。
    QVector<metadata::WellSectionLinkOverride> links;
    links << metadata::WellSectionLinkOverride{
        QStringLiteral("well-1"), QStringLiteral("well-2"),
        QStringLiteral("A"), false};
    rec = store.save(QStringLiteral("default"),
                     {QStringLiteral("well-2"), QStringLiteral("well-1"),
                      QStringLiteral("well-3")},
                     links, &err);
    QVERIFY2(rec.valid(), qPrintable(err));
    QCOMPARE(rec.version, 1);

    // 重开新实例读回：井序逐位一致，改接集一致。
    metadata::WellSectionStore reopened(dbPath);
    rec = reopened.load(QStringLiteral("default"), &err);
    QVERIFY(rec.valid());
    QCOMPARE(rec.version, 1);
    QCOMPARE(rec.wellIds,
             QStringList({QStringLiteral("well-2"), QStringLiteral("well-1"),
                          QStringLiteral("well-3")}));
    QCOMPARE(rec.linkOverrides.size(), 1);
    QCOMPARE(rec.linkOverrides[0].leftWellId, QStringLiteral("well-1"));
    QCOMPARE(rec.linkOverrides[0].rightWellId, QStringLiteral("well-2"));
    QCOMPARE(rec.linkOverrides[0].topName, QStringLiteral("A"));
    QVERIFY(!rec.linkOverrides[0].connected);

    // 再存（重排井序 + 重连）→ version 2；旧值被替换非追加。
    links[0].connected = true;
    rec = reopened.save(QStringLiteral("default"),
                        {QStringLiteral("well-3"), QStringLiteral("well-1")},
                        links, &err);
    QCOMPARE(rec.version, 2);
    auto rec2 = reopened.load(QStringLiteral("default"), &err);
    QCOMPARE(rec2.version, 2);
    QCOMPARE(rec2.wellIds.size(), 2);
    QVERIFY(rec2.linkOverrides[0].connected);

    // 多节互不干扰：另一节 id 各自版本从 1 起。
    rec = reopened.save(QStringLiteral("fence-1"),
                        {QStringLiteral("well-1")}, {}, &err);
    QCOMPARE(rec.version, 1);
    QCOMPARE(reopened.load(QStringLiteral("default"), &err).version, 2);

    // 分隔符转义：井 id/顶名含 , ; | \ 时 round-trip 不坏行。
    metadata::WellSectionStore escStore(dbPath);
    QVector<metadata::WellSectionLinkOverride> weird;
    weird << metadata::WellSectionLinkOverride{
        QStringLiteral("well,a"), QStringLiteral("well;b"),
        QStringLiteral("顶|名\\反斜杠"), false};
    const auto escRec = escStore.save(
        QStringLiteral("default"),
        {QStringLiteral("well,a"), QStringLiteral("well;b"),
         QStringLiteral("w|3"), QStringLiteral("w;4")},
        weird, &err);
    QVERIFY(escRec.valid());
    QCOMPARE(escRec.version, 3); // default 节第三次落盘
    const auto escBack =
        escStore.load(QStringLiteral("default"), &err);
    QCOMPARE(escBack.wellIds,
             QStringList({QStringLiteral("well,a"),
                          QStringLiteral("well;b"), QStringLiteral("w|3"),
                          QStringLiteral("w;4")}));
    QCOMPARE(escBack.linkOverrides.size(), 1);
    QCOMPARE(escBack.linkOverrides[0].leftWellId, QStringLiteral("well,a"));
    QCOMPARE(escBack.linkOverrides[0].topName,
             QStringLiteral("顶|名\\反斜杠"));
    QVERIFY(!escBack.linkOverrides[0].connected);

  }

  // 断层投绘：FaultSetStore 断面 mesh ∩ 井径 curtain → FaultTrace 集。
  void faultProjectionFromStore() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FaultSetStore fstore(
        QDir(dir.path()).filePath(QStringLiteral("f.project.sqlite")));
    QString err;
    QVERIFY2(fstore.open(&err), qPrintable(err));

    paleo::fault::FaultSet set;
    const QString fid = set.addFault(QStringLiteral("F1"));
    paleo::fault::FaultSurfaceMesh mesh;
    // 倾斜平面：x∈[0,100]、y∈[-50,50]、z=x/10（x=20→2，x=80→8）。
    mesh.vertices = {{0, -50, 0, QString(), -1},
                     {100, -50, 10, QString(), -1},
                     {100, 50, 10, QString(), -1},
                     {0, 50, 0, QString(), -1}};
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    QVERIFY(set.setSurface(fid, mesh));
    QVERIFY(fstore.save(set, &err));

    DataCatalog cat;
    WellSectionWorkflow wf(&cat);
    // 无 store → 状态提示。
    const auto none = wf.faultProjection({});
    QVERIFY(!none.status.isEmpty());
    QVERIFY(none.traces.isEmpty());

    wf.setFaultSetStore(&fstore);
    auto mk = [](const QString &id, double x) {
      wellsection::Well w;
      w.id = id;
      w.x = x;
      w.y = 0;
      return w;
    };
    const auto fp = wf.faultProjection({mk("w1", 20), mk("w2", 80)});
    QCOMPARE(fp.status, QString());
    QCOMPARE(fp.traces.size(), 1);
    QCOMPARE(fp.traces[0].faultName, QStringLiteral("F1"));
    QVERIFY(fp.traces[0].points.size() >= 2);
    QCOMPARE(fp.traces[0].points.first().along, 0.0);
    QCOMPARE(fp.traces[0].points.last().along, 1.0);
    QCOMPARE(qRound(fp.traces[0].points.first().depth * 10), 20); // x=20→2
    QCOMPARE(qRound(fp.traces[0].points.last().depth * 10), 80);  // x=80→8
    // 井位远离断面 → 状态「不穿过」。
    const auto miss = wf.faultProjection({mk("a", 200), mk("b", 300)});
    QVERIFY(miss.traces.isEmpty());
    QVERIFY(!miss.status.isEmpty());
    // 缺坐标 → 状态提示。
    wellsection::Well nox;
    nox.id = QStringLiteral("n");
    const auto bad = wf.faultProjection({mk("w1", 20), nox});
    QVERIFY(bad.traces.isEmpty());
    QVERIFY(!bad.status.isEmpty());
  }

  // 相代码充填段：catalog 派生资产 well_facies_intervals 最新版本 → 各井
  // facies（升序、按 wellId 匹配；无资产 → 空）。
  void faciesSegmentsAttached() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));

    const QString jsonPath = dir.filePath(QStringLiteral("well-facies.json"));
    QVERIFY(writeText(jsonPath, QStringLiteral(
                                   "{\"schema\":1,\"intervals\":["
                                   "{\"wellId\":\"well-2\",\"top\":120,\"base\":180,\"classId\":3,\"meanConfidence\":0.8,\"sampleCount\":12},"
                                   "{\"wellId\":\"well-1\",\"top\":100,\"base\":150,\"classId\":0,\"meanConfidence\":0.7,\"sampleCount\":9},"
                                   "{\"wellId\":\"well-1\",\"top\":150,\"base\":200,\"classId\":11,\"meanConfidence\":0.6,\"sampleCount\":8},"
                                   "{\"wellId\":\"well-9\",\"top\":1,\"base\":2,\"classId\":1,\"meanConfidence\":0.5,\"sampleCount\":1}"
                                   "]}")));
    // 资产 + 两版（v1 旧数据，v2 新数据）——只取最新。
    const QString v1 = dir.filePath(QStringLiteral("well-facies-v1.json"));
    QVERIFY(writeText(v1, QStringLiteral(
                                "{\"schema\":1,\"intervals\":["
                                "{\"wellId\":\"well-1\",\"top\":1,\"base\":2,\"classId\":9,\"meanConfidence\":0.1,\"sampleCount\":1}"
                                "]}")));
    CatalogAsset a;
    a.id = QStringLiteral("fa-1");
    a.type = QStringLiteral("well_facies_intervals");
    a.format = QStringLiteral("json");
    a.displayName = QStringLiteral("well-facies.json");
    QVERIFY2(cat.addAsset(a, &err), qPrintable(err));
    for (const auto &vp : {std::pair<QString, QString>{QStringLiteral("fv-1"), v1},
                           std::pair<QString, QString>{QStringLiteral("fv-2"), jsonPath}}) {
      CatalogVersion v;
      v.id = vp.first;
      v.assetId = a.id;
      v.managed = false;
      v.path = vp.second;
      v.stage = QStringLiteral("DERIVED");
      v.versionNumber = vp.first == QLatin1String("fv-2") ? 2 : 1;
      QVERIFY2(cat.addVersion(v, &err), qPrintable(err));
    }

    WellSectionWorkflow wf(&cat);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    wf.request({QStringLiteral("well-1"), QStringLiteral("well-2")}, {});
    QCOMPARE(spy.size(), 1);
    const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
    QCOMPARE(wells.size(), 2);
    // well-1：两段（v2 数据、非 v1），升序。
    QCOMPARE(wells[0].facies.size(), 2);
    QCOMPARE(wells[0].facies[0].topMd, 100.0);
    QCOMPARE(wells[0].facies[0].classId, 0);
    QCOMPARE(wells[0].facies[1].classId, 11);
    // well-2：一段。
    QCOMPARE(wells[1].facies.size(), 1);
    QCOMPARE(wells[1].facies[0].classId, 3);
    QVERIFY(wells[1].facies[0].baseMd > wells[1].facies[0].topMd);
  }

  // 栅状图多节：sectionIds 发现 + remove 收尾（条数收缩清尾行）。
  void fenceStoreSections() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = QDir(dir.path()).filePath(QStringLiteral(
        "f.project.sqlite"));
    metadata::WellSectionStore store(dbPath);
    QString err;
    QVERIFY2(store.open(&err), qPrintable(err));
    store.save(QStringLiteral("default"), {QStringLiteral("well-1")}, {}, &err);
    store.save(QStringLiteral("fence-1"), {QStringLiteral("well-1"),
                                           QStringLiteral("well-2")}, {}, &err);
    store.save(QStringLiteral("fence-2"), {QStringLiteral("well-3")}, {}, &err);
    const QStringList ids = store.sectionIds(&err);
    QCOMPARE(ids, QStringList({QStringLiteral("default"),
                               QStringLiteral("fence-1"),
                               QStringLiteral("fence-2")}));
    QVERIFY(store.remove(QStringLiteral("fence-2"), &err));
    const QStringList after = store.sectionIds(&err);
    QVERIFY(!after.contains(QStringLiteral("fence-2")));
    QCOMPARE(store.load(QStringLiteral("fence-2"), &err).version, 0);
    QVERIFY(store.remove(QStringLiteral("fence-2"), &err)); // 无该节幂等
  }

  void workbenchCalibration() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
    SectionWorkbench wb(&cat);
    QVERIFY2(wb.setCalibration(QStringLiteral("well-2"), true, 2000, -10,
                             &err),
             qPrintable(err));
    WellSectionWorkflow wf(&cat);
    wf.setSectionWorkbench(&wb);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    wf.request({"well-2", "well-1"}, {});
    QCOMPARE(spy.size(), 1);
    const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
    QVERIFY(wells[0].timeDepth.has_value()); // well-2 常速校正
    QCOMPARE(wells[0].timeDepth->status, QStringLiteral("常速校正"));
    QCOMPARE(wells[0].timeDepth->shiftMs, -10.0);
    QCOMPARE(wells[0].timeDepth->twtAt(500), 490.0); // v=2000 → md=ms, −10
    QVERIFY(wells[1].timeDepth.has_value());
    QCOMPARE(wells[1].timeDepth->status, QStringLiteral("时深表"));
  }

  void asyncSupersededRequest() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
    PaleoTaskService tasks;
    WellSectionWorkflow wf(&cat);
    wf.setTaskService(&tasks);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    const int g1 = wf.request({"well-1", "well-2"}, {"GR", "RT"});
    const int g2 = wf.request({"well-1"}, {"RT"});
    QVERIFY(g2 > g1);
    QTRY_VERIFY_WITH_TIMEOUT(spy.size() == 1, 10000);
    QCOMPARE(spy.size(), 1); // 被顶替的请求不再发射
    QCOMPARE(spy[0][0].toInt(), g2);
    const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
    QCOMPARE(wells.size(), 1);
    QCOMPARE(wells[0].id, QString("well-1"));
    QVERIFY(wells[0].curve("RT"));
    QVERIFY(!wells[0].curve("GR")); // 只取了 RT
  }

  // #128：同步/早退路径在 request()/requestSeismic() 返回之前就 emit——
  // 接收方必须能在发射当下判定「这是当前代」。壳层据 currentGeneration()
  // 过滤（旧壳层用返回值赋给 lastGen，同步那一发被当陈旧丢掉）。
  void generationIsCurrentAtSyncEmit() {
    DataCatalog cat;
    WellSectionWorkflow wf(&cat);
    bool sectionCurrent = false, seismicCurrent = false;
    int sectionHits = 0, seismicHits = 0;
    connect(&wf, &WellSectionWorkflow::sectionReady, this,
            [&](int gen, const QVector<wellsection::Well> &, const QStringList &) {
              ++sectionHits;
              sectionCurrent = gen == wf.currentGeneration();
            });
    connect(&wf, &WellSectionWorkflow::seismicReady, this,
            [&](int gen, const wellsection::SeismicStrip &) {
              ++seismicHits;
              seismicCurrent = gen == wf.currentSeismicGeneration();
            });
    const int g = wf.request({QStringLiteral("nope")}, {});
    QCOMPARE(sectionHits, 1); // 无任务服务 → 同步发射
    QVERIFY(sectionCurrent);
    QCOMPARE(g, wf.currentGeneration());
    const auto wells = QVector<wellsection::Well>{
        gridWell(QStringLiteral("A1"), 1000, 2000, true),
        gridWell(QStringLiteral("A2"), 1001, 2000, true)};
    wf.requestSeismic(wells, {}); // 无体 → 早退同步发射
    QCOMPARE(seismicHits, 1);
    QVERIFY(seismicCurrent);
    // cancel()（工程切换 #124）作废两条当前代。
    const int sg = wf.currentSeismicGeneration();
    wf.cancel();
    QVERIFY(wf.currentGeneration() != g);
    QVERIFY(wf.currentSeismicGeneration() != sg);
    disconnect(&wf, nullptr, this, nullptr);
  }

  void seismicStatusPaths() {
    DataCatalog cat;
    WellSectionWorkflow wf(&cat);
    const auto wells = QVector<wellsection::Well>{
        gridWell(QStringLiteral("A1"), 1000, 2000, true),
        gridWell(QStringLiteral("A2"), 1001, 2000, true)};
    QSignalSpy spy(&wf, &WellSectionWorkflow::seismicReady);
    // 无体 → 「未加载带坐标的地震体」，缝原因 = 状态，同步发射。
    int g = wf.requestSeismic(wells, {});
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy[0][0].toInt(), g);
    auto strip = spy[0][1].value<wellsection::SeismicStrip>();
    QCOMPARE(strip.status, QStringLiteral("未加载带坐标的地震体"));
    QCOMPARE(strip.gaps.size(), 1);
    QCOMPARE(strip.gaps[0].reason, strip.status);
    // 有体无测网 → 同样状态。
    strip = {};
    WellSectionWorkflow::SeismicSource src;
    src.volume = std::make_shared<seismic::SgyVolume>();
    wf.requestSeismic(wells, src);
    QCOMPARE(spy.size(), 2);
    QCOMPARE(spy[1][1].value<wellsection::SeismicStrip>().status,
             QStringLiteral("未加载带坐标的地震体"));
    // 无地震任务服务 → 服务不可用。
    src.grid = makeGrid();
    wf.requestSeismic(wells, src);
    QCOMPARE(spy.size(), 3);
    QCOMPARE(spy[2][1].value<wellsection::SeismicStrip>().status,
             QStringLiteral("地震任务服务不可用"));
    // 单井 → 空缝集。
    wf.requestSeismic({wells.first()}, src);
    QCOMPARE(spy.size(), 4);
    QVERIFY(spy[3][1].value<wellsection::SeismicStrip>().gaps.isEmpty());
  }

  void seismicGapReasons() {
    DataCatalog cat;
    WellSectionWorkflow wf(&cat);
    PaleoTaskService tasks;
    seismic::SeismicTaskService seismic(&tasks);
    wf.setSeismicTaskService(&seismic);
    auto volume = std::make_shared<seismic::SgyVolume>();
    WellSectionWorkflow::SeismicSource src;
    src.volume = volume;
    src.grid = makeGrid();

    wellsection::Well noCoords = sectionWell(QStringLiteral("NC"), qQNaN(),
                                             qQNaN(), true);
    wellsection::Well out = gridWell(QStringLiteral("OUT"), 1000, 2000, true);
    out.x = 1.0e6;
    out.y = 1.0e6;
    const QVector<wellsection::Well> wells = {
        gridWell(QStringLiteral("G1"), 1000, 2000, true),
        noCoords,
        gridWell(QStringLiteral("G3"), 1001, 2000, true),
        gridWell(QStringLiteral("NT"), 1002, 2000, false),
        gridWell(QStringLiteral("G5"), 1003, 2000, true),
        out,
        gridWell(QStringLiteral("G7"), 1001, 2001, true),
        sectionWell(QStringLiteral("G7t"), 110.0, 110.0, true), // 同道
    };
    QSignalSpy spy(&wf, &WellSectionWorkflow::seismicReady);
    wf.requestSeismic(wells, src);
    QCOMPARE(spy.size(), 1); // 全部缝出原因 → 同步发射
    const auto strip = spy[0][1].value<wellsection::SeismicStrip>();
    QCOMPARE(strip.gaps.size(), 7);
    QCOMPARE(strip.gaps[0].reason, QStringLiteral("NC 缺少井口坐标"));
    QCOMPARE(strip.gaps[1].reason, QStringLiteral("NC 缺少井口坐标"));
    QCOMPARE(strip.gaps[2].reason, QStringLiteral("NT 缺少时深关系"));
    QCOMPARE(strip.gaps[3].reason, QStringLiteral("NT 缺少时深关系"));
    QCOMPARE(strip.gaps[4].reason, QStringLiteral("OUT 不在地震测网内"));
    QCOMPARE(strip.gaps[5].reason, QStringLiteral("OUT 不在地震测网内"));
    QCOMPARE(strip.gaps[6].reason, QStringLiteral("两井位于同一地震道"));
    QVERIFY(!strip.anyValid());
  }

  void seismicExtractionAndSupersede() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("grid.sgy"));
    QVERIFY(writeSegy(sgy));
    auto volume = std::make_shared<seismic::SgyVolume>();
    std::string serr;
    QVERIFY2(volume->Load(sgy.toStdString(), serr), serr.c_str());
    PaleoTaskService tasks;
    seismic::SeismicTaskService seismic(&tasks);
    DataCatalog cat;
    WellSectionWorkflow wf(&cat);
    wf.setSeismicTaskService(&seismic);
    WellSectionWorkflow::SeismicSource src;
    src.volume = volume;
    src.grid = makeGrid();
    src.timeOriginMs = 50;
    const QVector<wellsection::Well> wells = {
        gridWell(QStringLiteral("A1"), 1000, 2000, true),
        gridWell(QStringLiteral("A2"), 1003, 2000, true)};
    QSignalSpy spy(&wf, &WellSectionWorkflow::seismicReady);
    const int g1 = wf.requestSeismic(wells, src);
    const int g2 = wf.requestSeismic(wells, src);
    QVERIFY(g2 > g1);
    QTRY_VERIFY_WITH_TIMEOUT(spy.size() == 1, 15000);
    QCOMPARE(spy.size(), 1); // 被顶替的请求不再发射
    QCOMPARE(spy[0][0].toInt(), g2);
    const auto strip = spy[0][1].value<wellsection::SeismicStrip>();
    QVERIFY(strip.status.isEmpty());
    QVERIFY(strip.anyValid());
    QCOMPARE(strip.gaps.size(), 1);
    const auto &gap = strip.gaps[0];
    QVERIFY(gap.valid());
    QVERIFY(gap.columns >= 2);
    QCOMPARE(gap.samples, 64);
    QCOMPARE(gap.startMs, 50.0);
    QCOMPARE(gap.stepMs, 2.0); // 2000us
    QVERIFY(strip.clip > 0.0f);
    QVERIFY(std::isfinite(gap.sampleAt(0.0, 50.0)));
    QVERIFY(std::isfinite(gap.sampleAt(1.0, 50.0)));
  }
};

QTEST_GUILESS_MAIN(TestWellSectionWorkflow)
#include "tst_wellsection_workflow.moc"
