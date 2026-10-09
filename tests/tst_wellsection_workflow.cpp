#include "catalog/datacatalog.h"
#include "domain/faultset.h"
#include "domain/seismic/sgyvolume.h"
#include "domain/welllogfacies.h"
#include "metadata/faultsetstore.h"
#include "metadata/wellsectionstore.h"
#include "services/paleotaskservice.h"
#include "services/projectdata.h"
#include "services/seismictaskservice.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellfaciesworkflow.h"
#include "workflow/wellsectionworkflow.h"
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
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
  // 图片道（core 井附件 + depthMd 锚）：facade imagesFor 收录/排除面 +
  // workflow 全链（collectCoreImages 清单 → 任务线程 QImage 装载 →
  // sectionReady 带 Well::images，md 升序）。无锚/未决/坏文件如实跳过。
  void coreImageAttachmentFlow() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));

    // 三张 8x8 测试图（PNG，可解码），深度锚经 extra["depthMd"]。
    const auto writePng = [&dir](const QString &name, QRgb color) {
      QImage img(8, 8, QImage::Format_RGB32);
      img.fill(color);
      const QString p = dir.filePath(name);
      return img.save(p, "PNG") ? p : QString();
    };
    const QString imgDeep = writePng(QStringLiteral("core_deep.png"), qRgb(200, 30, 30));
    const QString imgShallow = writePng(QStringLiteral("core_shallow.png"), qRgb(30, 30, 200));
    const QString imgNoAnchor = writePng(QStringLiteral("core_noanchor.png"), qRgb(30, 200, 30));
    QVERIFY(!imgDeep.isEmpty() && !imgShallow.isEmpty() && !imgNoAnchor.isEmpty());
    const auto addCore = [&](const QString &assetId, const QString &path,
                             const QVariant &depth, bool unresolved) {
      CatalogAsset a;
      a.id = assetId;
      a.type = QStringLiteral("image_reference");
      a.displayName = QFileInfo(path).fileName();
      if (!cat.addAsset(a, &err))
        return false;
      CatalogVersion v;
      v.id = "v-" + assetId;
      v.assetId = assetId;
      v.managed = false;
      v.path = path;
      v.fileName = QFileInfo(path).fileName();
      v.stage = QStringLiteral("RAW");
      if (depth.isValid())
        v.extra.insert(QStringLiteral("depthMd"), depth);
      if (!cat.addVersion(v, &err))
        return false;
      EntityAssetLink l;
      l.entityId = QStringLiteral("well-1");
      l.entityType = QStringLiteral("well");
      l.role = QStringLiteral("core");
      l.assetId = assetId;
      l.isPrimary = false;
      l.unresolved = unresolved;
      return cat.addLink(l, &err);
    };
    QVERIFY2(addCore(QStringLiteral("ast-img1"), imgDeep, 1800.0, false), qPrintable(err));
    QVERIFY2(addCore(QStringLiteral("ast-img2"), imgShallow, 1200.0, false), qPrintable(err));
    QVERIFY2(addCore(QStringLiteral("ast-img3"), imgNoAnchor, {}, false), qPrintable(err)); // 无锚不收
    QVERIFY2(addCore(QStringLiteral("ast-img4"), imgDeep, 1500.0, true), qPrintable(err)); // 未决不收

    // facade 查询面：收录两张（升序），无锚/未决排除。
    ProjectDataFacade facade;
    facade.setCatalog(&cat, dir.path());
    const QVector<WellImageAnchor> anchors =
        facade.imagesFor(QStringLiteral("well-1"));
    QCOMPARE(anchors.size(), 2);
    QCOMPARE(anchors.at(0).depthMd, 1200.0);
    QCOMPARE(anchors.at(1).depthMd, 1800.0);
    QCOMPARE(anchors.at(1).caption, QStringLiteral("core_deep.png"));

    // workflow 全链（同步路径：无任务服务 → loadCurveBodies 直跑）。
    WellSectionWorkflow wf(&cat);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    wf.request({QStringLiteral("well-1")}, {QStringLiteral("GR")});
    QTRY_COMPARE(spy.size(), 1);
    const QVector<wellsection::Well> wells =
        spy.at(0).at(1).value<QVector<wellsection::Well>>();
    QCOMPARE(wells.size(), 1);
    QCOMPARE(wells.at(0).images.size(), 2);
    QCOMPARE(wells.at(0).images.at(0).md, 1200.0);
    QCOMPARE(wells.at(0).images.at(1).md, 1800.0);
    QVERIFY(!wells.at(0).images.at(0).image.isNull());
    QCOMPARE(wells.at(0).images.at(1).image.size(), QSize(8, 8));
  }

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
    QVERIFY(warnBlob.contains(QStringLiteral("MD")));
    QVERIFY(warnBlob.contains(QStringLiteral("拒收 1 行")));
    QVERIFY(warnBlob.contains(QStringLiteral("tops.dat：第 4 行")));
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
                     links, wellsection::DepthDomain::MD, &err);
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
                        links, wellsection::DepthDomain::MD, &err);
    QCOMPARE(rec.version, 2);
    auto rec2 = reopened.load(QStringLiteral("default"), &err);
    QCOMPARE(rec2.version, 2);
    QCOMPARE(rec2.wellIds.size(), 2);
    QVERIFY(rec2.linkOverrides[0].connected);

    // 多节互不干扰：另一节 id 各自版本从 1 起。
    rec = reopened.save(QStringLiteral("fence-1"),
                        {QStringLiteral("well-1")}, {},
                        wellsection::DepthDomain::MD, &err);
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
        weird, wellsection::DepthDomain::MD, &err);
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

  // 深度域入剖面状态（方向 69）：round-trip + 旧库无列迁移（缺字段默认 MD）。
  void sectionStoreDepthDomain() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = QDir(dir.path()).filePath(QStringLiteral(
        "t.project.sqlite"));

    // 旧 schema 夹具：方向 69 前的五列表（无 depth_domain），直接写一行——
    // 模拟用户工程库的旧数据。连接名避开 store 的命名空间。
    {
      QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                  QStringLiteral("oldschema_setup"));
      db.setDatabaseName(dbPath);
      QVERIFY(db.open());
      QSqlQuery schema(db);
      QVERIFY(schema.exec(QStringLiteral(
          "CREATE TABLE well_section_edits ("
          "section_id TEXT PRIMARY KEY, well_ids TEXT NOT NULL, "
          "link_overrides TEXT NOT NULL, version INTEGER NOT NULL, "
          "updated_utc TEXT NOT NULL)")));
      QVERIFY(schema.exec(QStringLiteral(
          "INSERT INTO well_section_edits VALUES "
          "('default', 'well-1', '', 1, '2026-01-01T00:00:00.000')")));
      db.close();
      QSqlDatabase::removeDatabase(QStringLiteral("oldschema_setup"));
    }

    // 旧库打开 → 补列迁移；旧行读回 depthDomain = MD（向后兼容口径）。
    metadata::WellSectionStore store(dbPath);
    QString err;
    QVERIFY2(store.open(&err), qPrintable(err));
    auto rec = store.load(QStringLiteral("default"), &err);
    QVERIFY(rec.valid());
    QCOMPARE(rec.version, 1);
    QCOMPARE(rec.wellIds, QStringList({QStringLiteral("well-1")}));
    QCOMPARE(rec.depthDomain, wellsection::DepthDomain::MD);

    // 存 TVD → 重开新实例读回 TVD（工程级 round-trip）。
    rec = store.save(QStringLiteral("default"),
                     {QStringLiteral("well-1"), QStringLiteral("well-2")}, {},
                     wellsection::DepthDomain::TVD, &err);
    QVERIFY2(rec.valid(), qPrintable(err));
    QCOMPARE(rec.depthDomain, wellsection::DepthDomain::TVD);
    metadata::WellSectionStore reopened(dbPath);
    rec = reopened.load(QStringLiteral("default"), &err);
    QCOMPARE(rec.depthDomain, wellsection::DepthDomain::TVD);
    // 缺省参数 = MD（不显式给域时落 MD）。
    rec = reopened.save(QStringLiteral("fence-1"),
                        {QStringLiteral("well-1")}, {},
                        wellsection::DepthDomain::MD, &err);
    QVERIFY(rec.valid());
    QCOMPARE(reopened.load(QStringLiteral("fence-1"), &err).depthDomain,
             wellsection::DepthDomain::MD);

    // 无记录 ≠ MD 覆盖（R1-3 M1）：壳层恢复分支据 rec.valid() 判别——
    // 无行时 load 返回 version 0（invalid），面板保留 QSettings 缺省偏好，
    // 不得被 store 缺省 MD 静默盖回；显式落过库的记录（哪怕 MD）valid() 为
    // 真，恢复路径才以工程级覆盖。壳层 lambda 不可测，此处钉 store 层判别
    // 语义。
    const QString freshPath = QDir(dir.path()).filePath(QStringLiteral(
        "fresh.project.sqlite"));
    metadata::WellSectionStore fresh(freshPath);
    QVERIFY2(fresh.open(&err), qPrintable(err));
    rec = fresh.load(QStringLiteral("default"), &err);
    QVERIFY(!rec.valid());
    QCOMPARE(rec.version, 0);
    QCOMPARE(rec.depthDomain, wellsection::DepthDomain::MD); // 结构缺省
    rec = fresh.save(QStringLiteral("default"), {QStringLiteral("well-1")},
                     {}, wellsection::DepthDomain::MD, &err);
    QVERIFY2(rec.valid(), qPrintable(err)); // 显式 MD = 有效工程级状态
    QCOMPARE(fresh.load(QStringLiteral("default"), &err).depthDomain,
             wellsection::DepthDomain::MD);
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

  // TVD 域数据源：trajectory 角色井斜 → 各井 survey；无链接 = 无测斜（不
  // 告警：非数据损坏，TVD 域如实标注不可用）；坏表 → surveyError 如实 +
  // 告警（不下拽邻居）。
  void trajectoryAttached() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
    // well-1：有效文本站表（造斜-稳斜）。
    const QString devPath = dir.filePath(QStringLiteral("dev.dat"));
    QVERIFY(writeText(devPath, QStringLiteral(
                                   "# MD INCL AZI\n"
                                   "0.0 0.0 0.0\n"
                                   "1000.0 30.0 0.0\n"
                                   "2000.0 30.0 0.0\n")));
    QVERIFY2(addFileLink(cat, QStringLiteral("well-1"),
                         QStringLiteral("trajectory"),
                         QStringLiteral("dev-1"), devPath,
                         QStringLiteral("well_deviation"), &err),
             qPrintable(err));
    // well-3：坏站表（MD 重复）——不可解析。
    const QString badPath = dir.filePath(QStringLiteral("dev-bad.dat"));
    QVERIFY(writeText(badPath, QStringLiteral("0.0 10.0 0.0\n0.0 20.0 90.0\n")));
    QVERIFY2(addFileLink(cat, QStringLiteral("well-3"),
                         QStringLiteral("trajectory"),
                         QStringLiteral("dev-3"), badPath,
                         QStringLiteral("well_deviation"), &err),
             qPrintable(err));

    WellSectionWorkflow wf(&cat);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    wf.request({QStringLiteral("well-1"), QStringLiteral("well-2"),
                QStringLiteral("well-3")},
               {});
    QCOMPARE(spy.size(), 1);
    const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
    QCOMPARE(wells.size(), 3);
    // well-1：survey 挂载，TVD < MD（造斜）。
    QVERIFY(wells[0].survey.has_value());
    QVERIFY(wells[0].surveyError.isEmpty());
    QVERIFY(wells[0].tvdOf(1500.0) < 1500.0);
    QVERIFY(wells[0].tvdDisplayable());
    // well-2：无链接 = 无测斜——TVD≡MD、无告警（非数据损坏；TVD 域如实
    // 标注不可用）。
    QVERIFY(!wells[1].survey.has_value());
    QVERIFY(wells[1].surveyError.isEmpty());
    QCOMPARE(wells[1].tvdOf(1500.0), 1500.0);
    // well-3：坏表 → surveyError 如实 + 告警含井名。
    QVERIFY(!wells[2].survey.has_value());
    QVERIFY(!wells[2].surveyError.isEmpty());
    QVERIFY(!wells[2].tvdDisplayable());
    const QStringList warnings = spy[0][2].toStringList();
    bool warned = false;
    for (const QString &w : warnings)
      warned = warned || (w.contains(QStringLiteral("A3")) &&
                          w.contains(wells[2].surveyError));
    QVERIFY2(warned, "坏表井应在告警中如实点名");
    // well-2（无测斜）不应出现在任何井斜告警里——不告警是对的（非数据
    // 损坏，TVD 域如实标注即可）。注意只圈井斜告警：
    // 分层读面诊断（f7fe2311 起）按井名挂，共享 tops 文件的文件级
    // 拒收行会如实落在 A2 名下，与本断言无关。
    for (const QString &w : warnings)
      QVERIFY2(!w.contains(QStringLiteral("井斜轨迹")) ||
                   !w.contains(QStringLiteral("A2")),
               "无测斜井（A2）不应出现在任何井斜告警里");

    // 同一坏文件挂多口井：每口都必须如实标注（error 出参按调用写明，
    // 不靠 lastError 残留对比——轮 1 修复的回归面）。
    DataCatalog cat2;
    QTemporaryDir dir2;
    QVERIFY(dir2.isValid());
    QVERIFY2(cat2.open(dir2.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat2, QDir(dir2.path()), &err), qPrintable(err));
    const QString badShared =
        QDir(dir2.path()).filePath(QStringLiteral("dev-shared.dat"));
    QVERIFY(writeText(badShared, QStringLiteral("0.0 10.0 0.0\n0.0 20.0 90.0\n")));
    QVERIFY2(addFileLink(cat2, QStringLiteral("well-1"),
                         QStringLiteral("trajectory"),
                         QStringLiteral("dev-s1"), badShared,
                         QStringLiteral("well_deviation"), &err),
             qPrintable(err));
    QVERIFY2(addFileLink(cat2, QStringLiteral("well-2"),
                         QStringLiteral("trajectory"),
                         QStringLiteral("dev-s2"), badShared,
                         QStringLiteral("well_deviation"), &err),
             qPrintable(err));
    WellSectionWorkflow wf2(&cat2);
    QSignalSpy spy2(&wf2, &WellSectionWorkflow::sectionReady);
    wf2.request({QStringLiteral("well-1"), QStringLiteral("well-2")}, {});
    const auto wells2 = spy2[0][1].value<QVector<wellsection::Well>>();
    QCOMPARE(wells2.size(), 2);
    for (const auto &w : wells2)
      QVERIFY2(!w.surveyError.isEmpty() && !w.tvdDisplayable(),
               "共享坏文件的两口井都必须标 TVD 不可用");
    const QStringList warnings2 = spy2[0][2].toStringList();
    int hits = 0;
    for (const QString &w : warnings2)
      if (w.contains(QStringLiteral("井斜轨迹不可用")))
        ++hits;
    QCOMPARE(hits, 2);
  }

  // 第二解释源（方向 69 扩展）：role=="cuttings" 岩屑录井文件（CSV 夹具）。
  // 优先级（按井）：有该井段的 well_litho_intervals > cuttings > 无（GR
  // 回落）；cuttings 段 source=Interpreted、provenance=「岩屑录井」；
  // 读失败告警带 fileName。
  void cuttingsLithoSecondSource() {
    const auto seedCuttings = [](DataCatalog &cat, const QString &wellId,
                                 const QString &assetId, const QString &path,
                                 QString *err, int versionNumber = 1) {
      CatalogAsset a;
      a.id = assetId;
      a.type = QStringLiteral("cuttings");
      a.displayName = QFileInfo(path).fileName();
      if (!cat.addAsset(a, err))
        return false;
      CatalogVersion v;
      v.id = QStringLiteral("v-") + assetId;
      v.assetId = assetId;
      v.versionNumber = versionNumber;
      v.managed = false;
      v.path = path;
      v.fileName = QFileInfo(path).fileName();
      v.stage = QStringLiteral("RAW");
      if (!cat.addVersion(v, err))
        return false;
      EntityAssetLink l;
      l.entityId = wellId;
      l.entityType = QStringLiteral("well");
      l.role = QStringLiteral("cuttings");
      l.assetId = assetId;
      l.isPrimary = true;
      return cat.addLink(l, err);
    };
    const auto csvText = QStringLiteral(
        "顶深,底深,岩性,描述\n"
        "150,200,泥岩,深灰色\n"
        "100,150,细砂岩,褐灰色\n");

    // 仅 cuttings → cuttings 生效（升序、词面/深度对、来源标注）。
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataCatalog cat;
      QString err;
      QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
      QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
      const QString csvPath = dir.filePath(QStringLiteral("cuttings.csv"));
      QVERIFY(writeText(csvPath, csvText));
      QVERIFY2(seedCuttings(cat, QStringLiteral("well-1"),
                            QStringLiteral("cut-1"), csvPath, &err),
               qPrintable(err));

      WellSectionWorkflow wf(&cat);
      QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
      wf.request({QStringLiteral("well-1"), QStringLiteral("well-2")}, {});
      QCOMPARE(spy.size(), 1);
      const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
      QCOMPARE(wells.size(), 2);
      QCOMPARE(wells[0].litho.size(), 2);
      QCOMPARE(wells[0].litho[0].topMd, 100.0);
      QCOMPARE(wells[0].litho[0].baseMd, 150.0);
      QCOMPARE(wells[0].litho[0].litho, QStringLiteral("细砂岩"));
      QCOMPARE(wells[0].litho[1].topMd, 150.0);
      QCOMPARE(wells[0].litho[1].litho, QStringLiteral("泥岩"));
      for (const auto &seg : wells[0].litho) {
        QCOMPARE(seg.source, wellsection::LithoSource::Interpreted);
        QCOMPARE(seg.provenance, QStringLiteral("岩屑录井（cuttings.csv）"));
      }
      // well-2 无 cuttings 链接 → 空（GR 回落是正常态，不告警）。
      QVERIFY(wells[1].litho.isEmpty());
      for (const QString &w : spy[0][2].toStringList())
        QVERIFY2(!w.contains(QStringLiteral("岩屑")),
                 "正常路径不应有岩屑告警");
    }

    // 优先级：同井并存解释资产与 cuttings → 解释资产（well_litho_intervals）胜出。
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataCatalog cat;
      QString err;
      QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
      QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
      const QString csvPath = dir.filePath(QStringLiteral("cuttings.csv"));
      QVERIFY(writeText(csvPath, csvText));
      QVERIFY2(seedCuttings(cat, QStringLiteral("well-1"),
                            QStringLiteral("cut-1"), csvPath, &err),
               qPrintable(err));
      const QString jsonPath = dir.filePath(QStringLiteral("well-litho.json"));
      QVERIFY(writeText(jsonPath, QStringLiteral(
                                      "{\"schema\":1,\"intervals\":["
                                      "{\"wellId\":\"well-1\",\"top\":10,"
                                      "\"base\":20,\"litho\":\"解释泥岩\"}]}")));
      QVERIFY2(addFileLink(cat, QStringLiteral("well-1"),
                           QStringLiteral("interpretation"),
                           QStringLiteral("li-1"), jsonPath,
                           QStringLiteral("well_litho_intervals"), &err),
               qPrintable(err));

      WellSectionWorkflow wf(&cat);
      QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
      wf.request({QStringLiteral("well-1")}, {});
      const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
      QCOMPARE(wells[0].litho.size(), 1);
      QCOMPARE(wells[0].litho[0].litho, QStringLiteral("解释泥岩"));
      QCOMPARE(wells[0].litho[0].provenance, QString()); // 老资产无 provenance
    }

    // 诚实面：注册后删掉文件 → 读取失败告警（带 fileName）+ 段为空。
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataCatalog cat;
      QString err;
      QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
      QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
      const QString csvPath = dir.filePath(QStringLiteral("cuttings.csv"));
      QVERIFY(writeText(csvPath, csvText));
      QVERIFY2(seedCuttings(cat, QStringLiteral("well-1"),
                            QStringLiteral("cut-1"), csvPath, &err),
               qPrintable(err));
      QVERIFY(QFile::remove(csvPath));

      WellSectionWorkflow wf(&cat);
      QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
      wf.request({QStringLiteral("well-1")}, {});
      const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
      QVERIFY(wells[0].litho.isEmpty());
      bool readWarned = false;
      for (const QString &w : spy[0][2].toStringList())
        readWarned = readWarned || (w.contains(QStringLiteral("岩屑录井文件读取失败")) &&
                                    w.contains(QStringLiteral("cuttings.csv")));
      QVERIFY2(readWarned, "读失败要如实告警并点名文件（回落不是静默伪装）");
    }

    // 按井兜底（M1 修复面）：井有解释资产链接、但资产内无该井段（只有其它
    // 井的段）→ cuttings 兜底生效；资产可读且对他人有段 → 不新增告警。
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataCatalog cat;
      QString err;
      QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
      QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
      const QString csvPath = dir.filePath(QStringLiteral("cuttings.csv"));
      QVERIFY(writeText(csvPath, csvText));
      QVERIFY2(seedCuttings(cat, QStringLiteral("well-1"),
                            QStringLiteral("cut-1"), csvPath, &err),
               qPrintable(err));
      const QString jsonPath = dir.filePath(QStringLiteral("well-litho.json"));
      QVERIFY(writeText(jsonPath, QStringLiteral(
                                      "{\"schema\":1,\"intervals\":["
                                      "{\"wellId\":\"well-9\",\"top\":10,"
                                      "\"base\":20,\"litho\":\"解释泥岩\"}]}")));
      QVERIFY2(addFileLink(cat, QStringLiteral("well-1"),
                           QStringLiteral("interpretation"),
                           QStringLiteral("li-1"), jsonPath,
                           QStringLiteral("well_litho_intervals"), &err),
               qPrintable(err));

      WellSectionWorkflow wf(&cat);
      QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
      wf.request({QStringLiteral("well-1")}, {});
      QCOMPARE(spy.size(), 1);
      const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
      QCOMPARE(wells.size(), 1);
      QCOMPARE(wells[0].litho.size(), 2);
      QCOMPARE(wells[0].litho[0].litho, QStringLiteral("细砂岩"));
      for (const auto &seg : wells[0].litho) {
        QCOMPARE(seg.source, wellsection::LithoSource::Interpreted);
        QCOMPARE(seg.provenance, QStringLiteral("岩屑录井（cuttings.csv）"));
      }
      for (const QString &w : spy[0][2].toStringList())
        QVERIFY2(!w.contains(QStringLiteral("解释岩性资产")) &&
                     !w.contains(QStringLiteral("岩屑")),
                 "无该井段的按井兜底不应新增告警");
    }

    // 多份 cuttings 链接：取最新版本，落选版本进警告 + 写入 asset extra
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataCatalog cat;
      QString err;
      QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
      QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));

      const QString csvOldPath = dir.filePath(QStringLiteral("cuttings_v1.csv"));
      const QString csvNewPath = dir.filePath(QStringLiteral("cuttings_v2.csv"));
      QVERIFY(writeText(csvOldPath, csvText));
      QVERIFY(writeText(csvNewPath, QStringLiteral(
                                        "顶深,底深,岩性,描述\n"
                                        "80,120,白云岩,灰白色\n")));
      QVERIFY2(seedCuttings(cat, QStringLiteral("well-1"),
                            QStringLiteral("cut-old"), csvOldPath, &err, 1),
               qPrintable(err));
      QVERIFY2(seedCuttings(cat, QStringLiteral("well-1"),
                            QStringLiteral("cut-new"), csvNewPath, &err, 2),
               qPrintable(err));

      WellSectionWorkflow wf(&cat);
      QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
      wf.request({QStringLiteral("well-1")}, {});
      QCOMPARE(spy.size(), 1);
      const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
      QCOMPARE(wells.size(), 1);
      QCOMPARE(wells[0].litho.size(), 1);
      QCOMPARE(wells[0].litho[0].litho, QStringLiteral("白云岩"));
      QCOMPARE(wells[0].litho[0].provenance,
               QStringLiteral("岩屑录井（cuttings_v2.csv）"));

      // 警告中包含多份提示、选用与落选文件名及版本号
      bool multiWarned = false;
      const QStringList warnings = spy[0][2].toStringList();
      for (const QString &w : warnings) {
        if (w.contains(QStringLiteral("多份岩屑录井数据")) &&
            w.contains(QStringLiteral("cuttings_v2.csv")) &&
            w.contains(QStringLiteral("cuttings_v1.csv"))) {
          multiWarned = true;
          break;
        }
      }
      QVERIFY2(multiWarned, "多份 cuttings 必须发出警告点名选用与落选版本");

      // 验证落选版本的 extra 记录
      const CatalogVersion oldVer = cat.currentVersion(QStringLiteral("cut-old"));
      QCOMPARE(oldVer.extra.value(QStringLiteral("cuttings_selection")).toString(),
               QStringLiteral("unselected"));
      QCOMPARE(oldVer.extra.value(QStringLiteral("unselected_superseded_by")).toString(),
               QStringLiteral("cuttings_v2.csv"));
    }

    // 同一资产上版本号更高的 PDF 派生件不能盖住岩屑表。
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataCatalog cat;
      QString err;
      QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
      QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));
      const QString csvPath = dir.filePath(QStringLiteral("A1岩屑录井数据.csv"));
      QVERIFY(writeText(csvPath, csvText));
      QVERIFY2(seedCuttings(cat, QStringLiteral("well-1"),
                            QStringLiteral("cut-1"), csvPath, &err, 1),
               qPrintable(err));
      const QString pdfPath = dir.filePath(QStringLiteral("A1岩屑录井数据.pdf"));
      QVERIFY(writeText(pdfPath, QStringLiteral("%PDF-1.4")));
      CatalogVersion pdf;
      pdf.id = QStringLiteral("v-pdf");
      pdf.assetId = QStringLiteral("cut-1");
      pdf.versionNumber = 2;
      pdf.managed = false;
      pdf.path = pdfPath;
      pdf.fileName = QStringLiteral("A1岩屑录井数据.pdf");
      pdf.stage = QStringLiteral("DERIVED");
      QVERIFY2(cat.addVersion(pdf, &err), qPrintable(err));

      WellSectionWorkflow wf(&cat);
      QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
      wf.request({QStringLiteral("well-1")}, {});
      QCOMPARE(spy.size(), 1);
      const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
      QCOMPARE(wells.size(), 1);
      QCOMPARE(wells[0].litho.size(), 2);
      QCOMPARE(wells[0].litho[0].litho, QStringLiteral("细砂岩"));
      QCOMPARE(wells[0].litho[0].provenance,
               QStringLiteral("岩屑录井（A1岩屑录井数据.csv）"));
    }
  }

  // 解释岩性段：catalog 资产 well_litho_intervals 按井 interpretation 链接
  // 消费（升序、按 wellId 匹配；空词面/逆序段跳过；无链接井留空走 GR 回落）。
  void lithoSegmentsAttached() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));

    const QString jsonPath = dir.filePath(QStringLiteral("well-litho.json"));
    QVERIFY(writeText(jsonPath, QStringLiteral(
                                    "{\"schema\":1,\"intervals\":["
                                    "{\"wellId\":\"well-1\",\"top\":100,\"base\":150,\"litho\":\"细砂岩\"},"
                                    "{\"wellId\":\"well-1\",\"top\":150,\"base\":200,\"litho\":\"泥岩\"},"
                                    "{\"wellId\":\"well-1\",\"top\":300,\"base\":280,\"litho\":\"逆序段\"},"
                                    "{\"wellId\":\"well-1\",\"top\":400,\"base\":420,\"litho\":\"  \"},"
                                    "{\"wellId\":\"well-9\",\"top\":1,\"base\":2,\"litho\":\"灰岩\"}"
                                    "]}")));
    CatalogAsset a;
    a.id = QStringLiteral("li-1");
    a.type = QStringLiteral("well_litho_intervals");
    a.format = QStringLiteral("json");
    a.displayName = QStringLiteral("well-litho.json");
    QVERIFY2(cat.addAsset(a, &err), qPrintable(err));
    CatalogVersion v;
    v.id = QStringLiteral("lv-1");
    v.assetId = a.id;
    v.managed = false;
    v.path = jsonPath;
    v.stage = QStringLiteral("DERIVED");
    v.versionNumber = 1;
    QVERIFY2(cat.addVersion(v, &err), qPrintable(err));
    // 方向 69：按井 interpretation 链接消费——无链接的资产不再全工程可见。
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = QStringLiteral("well-1");
    link.assetId = a.id;
    link.role = QStringLiteral("interpretation");
    QVERIFY2(cat.addLink(link, &err), qPrintable(err));

    WellSectionWorkflow wf(&cat);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    wf.request({QStringLiteral("well-1"), QStringLiteral("well-2")}, {});
    QCOMPARE(spy.size(), 1);
    const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
    QCOMPARE(wells.size(), 2);
    // well-1：两段有效（逆序/空词面跳过），升序、词面保真、来源标注。
    QCOMPARE(wells[0].litho.size(), 2);
    QCOMPARE(wells[0].litho[0].topMd, 100.0);
    QCOMPARE(wells[0].litho[0].litho, QStringLiteral("细砂岩"));
    QCOMPARE(wells[0].litho[1].litho, QStringLiteral("泥岩"));
    QVERIFY(wells[0].litho[1].baseMd > wells[0].litho[1].topMd);
    for (const auto &seg : wells[0].litho) {
      QCOMPARE(seg.source, wellsection::LithoSource::Interpreted);
      QVERIFY(seg.provenance.isEmpty()); // 老资产无 provenance → 不伪造
    }
    // well-2：无链接 → 空（视图回落 GR 推断；A 井资产不漏给 B 井）。
    QVERIFY(wells[1].litho.isEmpty());
    // 告警如实：两个无效段（逆序 + 空词面）计数点名。
    const QStringList warnings = spy[0][2].toStringList();
    bool counted = false;
    for (const QString &w : warnings)
      counted = counted || w.contains(QStringLiteral("2 个无效段"));
    QVERIFY2(counted, "无效段应进告警（回落不静默吞数据质量问题）");

    // ---- 告警回归网（轮 2）：schema 不支持 / 文件读不了 → 告警 + 回落 ----
    const auto seedLithoAsset = [&dir](DataCatalog &c, const QString &assetId,
                                       const QString &path, int version,
                                       QString *e) {
      CatalogAsset a;
      a.id = assetId;
      a.type = QStringLiteral("well_litho_intervals");
      a.format = QStringLiteral("json");
      a.displayName = QStringLiteral("litho.json");
      if (!c.addAsset(a, e))
        return false;
      CatalogVersion v;
      v.id = QStringLiteral("v-") + assetId;
      v.assetId = a.id;
      v.managed = false;
      v.path = path;
      v.stage = QStringLiteral("DERIVED");
      v.versionNumber = version;
      if (!c.addVersion(v, e))
        return false;
      // 方向 69：消费走 interpretation 链接——种子资产必须挂到井上才可达。
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = QStringLiteral("well-1");
      l.assetId = assetId;
      l.role = QStringLiteral("interpretation");
      return c.addLink(l, e);
    };
    {
      // schema 未知 → 拒读 + 告警；不挂任何段。
      QTemporaryDir d2;
      DataCatalog c2;
      QString e2;
      QVERIFY2(c2.open(d2.path(), &e2), qPrintable(e2));
      QVERIFY2(buildCatalog(c2, QDir(d2.path()), &e2), qPrintable(e2));
      const QString p2 = QDir(d2.path()).filePath(QStringLiteral("l2.json"));
      QVERIFY(writeText(p2, QStringLiteral(
                                "{\"schema\":2,\"intervals\":["
                                "{\"wellId\":\"well-1\",\"top\":1,\"base\":2,"
                                "\"litho\":\"灰岩\"}]}")));
      QVERIFY2(seedLithoAsset(c2, QStringLiteral("li-s"), p2, 1, &e2),
               qPrintable(e2));
      WellSectionWorkflow wf2(&c2);
      QSignalSpy spy2(&wf2, &WellSectionWorkflow::sectionReady);
      wf2.request({QStringLiteral("well-1")}, {});
      const auto ws2 = spy2[0][1].value<QVector<wellsection::Well>>();
      QVERIFY(ws2[0].litho.isEmpty());
      bool schemaWarned = false;
      for (const QString &w : spy2[0][2].toStringList())
        schemaWarned = schemaWarned || w.contains(QStringLiteral("schema"));
      QVERIFY2(schemaWarned, "schema 不支持要如实告警");
    }
    {
      // 版本路径指向不存在文件 → 读取失败告警 + 回落。
      QTemporaryDir d3;
      DataCatalog c3;
      QString e3;
      QVERIFY2(c3.open(d3.path(), &e3), qPrintable(e3));
      QVERIFY2(buildCatalog(c3, QDir(d3.path()), &e3), qPrintable(e3));
      QVERIFY2(seedLithoAsset(
                   c3, QStringLiteral("li-m"),
                   QDir(d3.path()).filePath(QStringLiteral("gone.json")), 1,
                   &e3),
               qPrintable(e3));
      WellSectionWorkflow wf3(&c3);
      QSignalSpy spy3(&wf3, &WellSectionWorkflow::sectionReady);
      wf3.request({QStringLiteral("well-1")}, {});
      const auto ws3 = spy3[0][1].value<QVector<wellsection::Well>>();
      QVERIFY(ws3[0].litho.isEmpty());
      bool readWarned = false;
      for (const QString &w : spy3[0][2].toStringList())
        readWarned = readWarned || w.contains(QStringLiteral("解释岩性资产读取失败"));
      QVERIFY2(readWarned, "读失败要如实告警（回落不是静默伪装）");
    }
  }

  // 方向 69 生产者→消费者闭环：welllogfacies 预测结果（伪结果直达
  // publishLithoAsset 缝，与 completed 信号同一条生产者路）落 catalog
  // DERIVED 资产 + per-well interpretation 链接 → 剖面按井链接消费：
  // A 井资产不漏给 B 井，段带来源标注（source/provenance）。
  void wellFaciesLithoAssetRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    QVERIFY2(buildCatalog(cat, QDir(dir.path()), &err), qPrintable(err));

    WellComposite::ComprehensiveWellData data;
    data.wellName = QStringLiteral("A1");
    WellFaciesWorkflow faciesWf;
    faciesWf.setData(data);
    faciesWf.setCatalog(&cat); // projectDir 按 catalog 当前工程推导

    WellFaciesResult result;
    result.jobId = QStringLiteral("job-9");
    result.modelName = QStringLiteral("测试微相");
    result.modelVersion = QStringLiteral("v2");
    WellComposite::TextInterval iv1, iv2, bad;
    iv1.topDepth = 100.0f;
    iv1.bottomDepth = 150.0f;
    iv1.text = QStringLiteral("河口坝");
    iv2.topDepth = 150.0f;
    iv2.bottomDepth = 200.0f;
    iv2.text = QStringLiteral("席状砂");
    bad.topDepth = 300.0f;
    bad.bottomDepth = 280.0f; // 逆序 → 跳过
    bad.text = QStringLiteral("逆序段");
    result.intervals = {iv1, iv2, bad};
    const QString publishError = faciesWf.publishLithoAsset(result);
    QVERIFY2(publishError.isEmpty(), qPrintable(publishError));

    // DERIVED 资产登记 + per-well interpretation 链接。
    QVector<CatalogAsset> lithoAssets;
    for (const CatalogAsset &a : cat.assets())
      if (a.type == QLatin1String("well_litho_intervals"))
        lithoAssets << a;
    QCOMPARE(lithoAssets.size(), 1);
    bool linked = false;
    for (const EntityAssetLink &l :
         cat.linksForEntity(QStringLiteral("well-1")))
      linked = linked || (l.assetId == lithoAssets.first().id &&
                          l.role == QLatin1String("interpretation"));
    QVERIFY2(linked, "登记后必须挂 per-well interpretation 链接");

    // schema 字段断言（生产者契约逐字段）。
    const CatalogVersion cv = cat.currentVersion(lithoAssets.first().id);
    QFile f(DataCatalog::resolvedVersionPath(dir.path(), cv));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("schema")).toInt(), 1);
    const QJsonObject prov =
        root.value(QStringLiteral("provenance")).toObject();
    QCOMPARE(prov.value(QStringLiteral("source")).toString(),
             QStringLiteral("welllogfacies"));
    QCOMPARE(prov.value(QStringLiteral("modelName")).toString(),
             QStringLiteral("测试微相"));
    QCOMPARE(prov.value(QStringLiteral("modelVersion")).toString(),
             QStringLiteral("v2"));
    QCOMPARE(prov.value(QStringLiteral("jobId")).toString(),
             QStringLiteral("job-9"));
    const QJsonArray intervals = root.value(QStringLiteral("intervals")).toArray();
    QCOMPARE(intervals.size(), 2); // 逆序段被生产者跳过
    QCOMPARE(intervals[0].toObject().value(QStringLiteral("wellId")).toString(),
             QStringLiteral("well-1"));
    QCOMPARE(intervals[0].toObject().value(QStringLiteral("top")).toDouble(),
             100.0);
    QCOMPARE(intervals[0].toObject().value(QStringLiteral("litho")).toString(),
             QStringLiteral("河口坝"));

    // 消费面：按井链接——A 井资产不漏给 B 井，段带 source/provenance。
    WellSectionWorkflow wf(&cat);
    QSignalSpy spy(&wf, &WellSectionWorkflow::sectionReady);
    wf.request({QStringLiteral("well-1"), QStringLiteral("well-2")}, {});
    QCOMPARE(spy.size(), 1);
    const auto wells = spy[0][1].value<QVector<wellsection::Well>>();
    QCOMPARE(wells.size(), 2);
    QCOMPARE(wells[0].litho.size(), 2);
    QCOMPARE(wells[0].litho[0].litho, QStringLiteral("河口坝"));
    QCOMPARE(wells[0].litho[0].source,
             wellsection::LithoSource::Interpreted);
    QCOMPARE(wells[0].litho[0].provenance,
             QStringLiteral("welllogfacies 测试微相 v2"));
    QVERIFY2(wells[1].litho.isEmpty(),
             "B 井无链接：A 井资产不得漏给 B 井（GR 回落是正常态）");

    // 诚实面：未绑定 catalog / 井名未唯一解析 → 不写资产，如实原因。
    WellFaciesWorkflow unbound;
    unbound.setData(data);
    QVERIFY(!unbound.publishLithoAsset(result).isEmpty());
    WellComposite::ComprehensiveWellData ghost;
    ghost.wellName = QStringLiteral("NO-SUCH-WELL");
    WellFaciesWorkflow ghostWf;
    ghostWf.setData(ghost);
    ghostWf.setCatalog(&cat);
    QVERIFY(!ghostWf.publishLithoAsset(result).isEmpty());
    QVector<CatalogAsset> after;
    for (const CatalogAsset &a : cat.assets())
      if (a.type == QLatin1String("well_litho_intervals"))
        after << a;
    QCOMPARE(after.size(), 1); // 两次失败都没多写资产
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
    store.save(QStringLiteral("default"), {QStringLiteral("well-1")}, {},
               wellsection::DepthDomain::MD, &err);
    store.save(QStringLiteral("fence-1"), {QStringLiteral("well-1"),
                                           QStringLiteral("well-2")},
               {}, wellsection::DepthDomain::MD, &err);
    store.save(QStringLiteral("fence-2"), {QStringLiteral("well-3")}, {},
               wellsection::DepthDomain::MD, &err);
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
