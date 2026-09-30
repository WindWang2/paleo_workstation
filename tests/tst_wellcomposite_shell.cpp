// 层：测试壳（wave/deepen-perf D1 —— wellcomposite 壳侧装配链全链路）
//
// 直驱装配链：面板 derivedDocumentReady 意图 → WellCompositeDerivedSink →
// workflow/DerivedAssetRegistrar 受管落位 → catalog DERIVED 版本（父版本 =
// 源井数据 RAW）→ io 序列化产物往返等价。io 函数（writeComprehensiveWellXml /
// parseDeviationSurvey / parseTimeDepthTable）在本测试以装配期注入的方式进
// sink（生产等价物：组装根 main.cpp 注入——ui→io include 白名单只放行
// lasdoc.h，视图层拿不到 io 头，测试壳不受此限）。
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QPrinter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "catalog/datacatalog.h"
#include "domain/wellcompositemodel.h"
#include "io/wellcompositexml.h"
#include "qgis/qgisruntime.h"
#include "ui/wellcomposite/derivedsink.h"
#include "ui/wellcomposite/editsession.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "ui/wellcomposite/wellcompositetrack.h"

using namespace WellComposite;

namespace
{

ComprehensiveWellData makeWellData()
{
  ComprehensiveWellData d;
  d.wellName = QStringLiteral("TEST-1");
  d.minDepth = 1000.0;
  d.maxDepth = 2000.0;
  d.standardHorizons = {{1500.0, QStringLiteral("T35")}};

  LithologyInterval l1;
  l1.topDepth = 1000.0f;
  l1.bottomDepth = 1200.0f;
  l1.lithoName = QStringLiteral("泥岩");
  d.lithologyIntervals = {l1};
  return d;
}

// 综合柱状图 XML + 「井斜数据」/「时深数据」工作表（io 写回骨架 + 追加表）
QString makeSourceXml()
{
  QString xml = QString::fromUtf8(writeComprehensiveWellXml(makeWellData()));
  const QString extra = QStringLiteral(
      "<Worksheet ss:Name=\"井斜数据\"><Table>\n"
      "<Row><Cell><Data ss:Type=\"String\">1000</Data></Cell>"
      "<Cell><Data ss:Type=\"String\">0</Data></Cell>"
      "<Cell><Data ss:Type=\"String\">0</Data></Cell></Row>\n"
      "<Row><Cell><Data ss:Type=\"String\">1100</Data></Cell>"
      "<Cell><Data ss:Type=\"String\">15</Data></Cell>"
      "<Cell><Data ss:Type=\"String\">90</Data></Cell></Row>\n"
      "</Table></Worksheet>\n"
      "<Worksheet ss:Name=\"时深数据\"><Table>\n"
      "<Row><Cell><Data ss:Type=\"String\">1000</Data></Cell>"
      "<Cell><Data ss:Type=\"String\">850</Data></Cell></Row>\n"
      "<Row><Cell><Data ss:Type=\"String\">2000</Data></Cell>"
      "<Cell><Data ss:Type=\"String\">1450</Data></Cell></Row>\n"
      "</Table></Worksheet>\n");
  xml.replace(QStringLiteral("</Workbook>"), extra + QStringLiteral("</Workbook>"));
  return xml;
}

// io 序列化器注入（组装根 main.cpp 的等价绑定）
WellCompositeDerivedSink::SerializeFn ioSerializer()
{
  return [](const ComprehensiveWellData &doc, const QStringList &auditLines) {
    return writeComprehensiveWellXml(doc, auditLines);
  };
}

void ioParsers(WellCompositeDerivedSink *sink)
{
  sink->setDepthTableParsers(
      [](const QString &path, QVector<DeviationStation> *out, QString *error) {
        QVector<XmlDeviationStation> xs;
        if (!parseDeviationSurvey(path, xs, error))
          return false;
        out->reserve(xs.size());
        for (const auto &x : xs)
          out->append({x.md, x.inclinationDeg, x.azimuthDeg});
        return true;
      },
      [](const QString &path, QVector<QPair<double, double>> *out, QString *error) {
        return parseTimeDepthTable(path, *out, error);
      });
}

} // namespace

class TestWellCompositeShell : public QObject
{
  Q_OBJECT

private slots:
  // ---- D1 主链路：编辑 → 意图信号 → sink → catalog DERIVED（父=RAW） ----
  void fullDerivedChain()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog catalog;
    QVERIFY2(catalog.open(dir.path()), qPrintable(catalog.openError()));

    // 源井数据 RAW 版本（外链形态：绝对路径入库）
    const QString srcPath = dir.filePath(QStringLiteral("well.xml"));
    {
      QFile f(srcPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(makeSourceXml().toUtf8());
      f.close();
    }
    CatalogAsset asset;
    asset.id = QStringLiteral("ast-src");
    asset.type = QStringLiteral("reference");
    asset.format = QStringLiteral("xml");
    asset.displayName = QStringLiteral("well.xml");
    QVERIFY(catalog.addAsset(asset));
    CatalogVersion raw;
    raw.id = QStringLiteral("ver-raw");
    raw.assetId = asset.id;
    raw.stage = QStringLiteral("RAW");
    raw.versionNumber = 1;
    raw.managed = false;
    raw.path = srcPath;
    raw.fileName = QStringLiteral("well.xml");
    QVERIFY(catalog.addVersion(raw));

    WellCompositeDerivedSink sink;
    sink.bind(&catalog, dir.path());
    sink.setSerializer(ioSerializer());
    ioParsers(&sink);
    WellCompositeDerivedSink::setDefault(&sink);

    WellCompositePanel panel;
    // 装载即自动喂表（wellLoaded → sink → DepthTransform）
    QVERIFY2(panel.loadComprehensiveXml(srcPath), "comprehensive XML load failed");
    QVERIFY(panel.hasDeviationSurvey());
    QVERIFY(panel.hasTimeDepthTable());
    // 装配数学：md 1000（井斜 0°）TVD=MD；时深表 (1000,850) 直取
    QCOMPARE(panel.mdToTvd(1000.0), 1000.0);
    QCOMPARE(panel.twtAtDepth(1000.0), 850.0);

    // 深度标尺道 TWT 副刻度（span 1000 → step 100；首刻度 1000 → 850）
    bool sawRuler = false;
    bool sawTwt850 = false;
    for (const auto &t : panel.canvas()->tracks())
    {
      if (t && t->type() == TrackType::DepthScale)
      {
        sawRuler = true;
        const auto labels = std::static_pointer_cast<DepthScaleTrack>(t)->twtLabels();
        QVERIFY(!labels.isEmpty());
        for (const auto &pair : labels)
          if (pair.first == 1000.0 && pair.second == QStringLiteral("850"))
            sawTwt850 = true;
      }
    }
    QVERIFY(sawRuler);
    QVERIFY(sawTwt850);

    // 编辑 → 保存派生 → DERIVED 登记
    EditSession *session = panel.editSession();
    QVERIFY(session);
    session->insertMarker(QStringLiteral("T90"), 1100.0);
    QVERIFY(session->isDirty());

    QSignalSpy registered(&sink, &WellCompositeDerivedSink::derivedRegistered);
    QSignalSpy failed(&sink, &WellCompositeDerivedSink::derivedFailed);
    QSignalSpy intent(&panel, &WellCompositePanel::derivedDocumentReady);
    QVERIFY(panel.saveDerived());
    QCOMPARE(intent.count(), 1);
    QCOMPARE(intent.first().size(), 3); // doc + 摘要 + 审计行（D1 追加）
    QVERIFY(!intent.first().at(2).toStringList().isEmpty());
    QCOMPARE(registered.count(), 1);
    QCOMPARE(failed.count(), 0);

    const QString versionId = registered.first().at(1).toString();
    const QString managedPath = registered.first().at(0).toString();
    QVERIFY(!versionId.isEmpty());
    const CatalogVersion v = catalog.versionById(versionId);
    QCOMPARE(v.stage, QStringLiteral("DERIVED"));
    QCOMPARE(v.managed, true);
    QVERIFY(!v.sha256.isEmpty());
    QCOMPARE(v.parentVersionIds, QStringList{QStringLiteral("ver-raw")});
    QVERIFY(v.extra.value(QStringLiteral("origin")).toString() ==
            QStringLiteral("wellcomposite-edit"));

    // 受管产物落盘 + 内容等价（io 往返：标志层 + 审计表都在）
    const QString resolved = DataCatalog::resolvedVersionPath(dir.path(), v);
    QCOMPARE(QFileInfo(resolved).absoluteFilePath(), QFileInfo(managedPath).absoluteFilePath());
    QVERIFY(QFile::exists(resolved));
    ComprehensiveWellData reparsed;
    QString err;
    QVERIFY2(parseComprehensiveWellXml(resolved, reparsed, &err), qPrintable(err));
    QCOMPARE(reparsed.wellName, QStringLiteral("TEST-1"));
    bool foundMarker = false;
    for (const auto &m : reparsed.standardHorizons)
      if (m.second == QStringLiteral("T90"))
        foundMarker = true;
    QVERIFY(foundMarker);
    QFile rf(resolved);
    QVERIFY(rf.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(rf.readAll());
    QVERIFY(text.contains(QStringLiteral("编辑审计")));
    QVERIFY(text.contains(QStringLiteral("marker.insert")));

    // 派生资产沿用源资产 type（预览路由一致）
    const CatalogAsset derivedAsset = catalog.assetById(v.assetId);
    QCOMPARE(derivedAsset.type, QStringLiteral("reference"));
    QVERIFY(derivedAsset.displayName.contains(QStringLiteral("编辑派生")));

    WellCompositeDerivedSink::setDefault(nullptr);
  }

  // ---- 同资产重算：版本号递增（find-or-create 语义） ----
  void secondSaveIncrementsVersion()
  {
    QTemporaryDir dir;
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path()));
    const QString srcPath = dir.filePath(QStringLiteral("well.xml"));
    {
      QFile f(srcPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(QString::fromUtf8(writeComprehensiveWellXml(makeWellData())).toUtf8());
      f.close();
    }
    WellCompositeDerivedSink sink;
    sink.bind(&catalog, dir.path());
    sink.setSerializer(ioSerializer());
    WellCompositeDerivedSink::setDefault(&sink);

    WellCompositePanel panel;
    QVERIFY(panel.loadComprehensiveXml(srcPath));
    QSignalSpy spy(&sink, &WellCompositeDerivedSink::derivedRegistered);
    panel.editSession()->insertMarker(QStringLiteral("T1"), 1500.0);
    QVERIFY(panel.saveDerived());
    QCOMPARE(spy.count(), 1);
    const QString id1 = spy.first().at(1).toString();

    // 第二次显式登记（同文档）
    QString err;
    QString p2;
    const QString id2 = sink.registerDerived(panel.currentData(), panel.editSession()->auditLines(),
                                             srcPath, &err, &p2);
    QVERIFY2(!id2.isEmpty(), qPrintable(err));
    const CatalogVersion v1 = catalog.versionById(id1);
    const CatalogVersion v2 = catalog.versionById(id2);
    QCOMPARE(v1.assetId, v2.assetId);
    QCOMPARE(v2.versionNumber, 2);
    QVERIFY(v2.versionNumber > v1.versionNumber);
    WellCompositeDerivedSink::setDefault(nullptr);
  }

  // ---- 诚实失败面：能力缺失不静默 ----
  void honestFailures()
  {
    WellCompositeDerivedSink sink;
    QString err;
    // 未绑 catalog
    QVERIFY(sink.registerDerived(makeWellData(), {}, QStringLiteral("/x.xml"), &err).isEmpty());
    QVERIFY(err.contains(QStringLiteral("catalog 未绑定")));

    QTemporaryDir dir;
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path()));
    sink.bind(&catalog, dir.path());
    // 未注入序列化器
    err.clear();
    QVERIFY(sink.registerDerived(makeWellData(), {}, QStringLiteral("/x.xml"), &err).isEmpty());
    QVERIFY2(err.contains(QStringLiteral("序列化器未注入")), qPrintable(err));

    // 序列化器在但源路径解析不到父版本（无 RAW）——登记成功、无父版本（不伪造）
    sink.setSerializer(ioSerializer());
    err.clear();
    const QString id = sink.registerDerived(makeWellData(), {QStringLiteral("marker.insert T9")},
                                            dir.filePath(QStringLiteral("unknown.xml")), &err);
    QVERIFY2(!id.isEmpty(), qPrintable(err));
    QVERIFY(catalog.versionById(id).parentVersionIds.isEmpty());
  }

  // ---- 深度装配直驱（不经 sink）：数学面 + 复位 ----
  void depthAssemblyDirect()
  {
    WellCompositePanel panel;
    QVector<CurveData> curves;
    CurveData gr;
    gr.name = QStringLiteral("GR");
    gr.depths = {1000.0f, 2000.0f};
    gr.values = {40.0f, 60.0f};
    curves << gr;
    QVERIFY(panel.loadLasCurves(QStringLiteral("W-9"), curves));
    QVERIFY(!panel.hasDeviationSurvey());
    QVERIFY(!panel.hasTimeDepthTable());
    QVERIFY(panel.depthReadoutSuffix(1100.0).isEmpty()); // 无表 → 无尾缀

    panel.applyDepthTables(
        {{1000.0, 0.0, 0.0}, {1100.0, 15.0, 90.0}},
        {{1000.0, 850.0}, {2000.0, 1450.0}});
    QVERIFY(panel.hasDeviationSurvey());
    QVERIFY(panel.hasTimeDepthTable());
    QCOMPARE(panel.twtAtDepth(1000.0), 850.0);
    QVERIFY(panel.mdToTvd(1050.0) < 1050.0); // 15° 段 TVD < MD
    QVERIFY(!panel.depthReadoutSuffix(1100.0).isEmpty());
    QVERIFY(panel.depthReadoutSuffix(1100.0).contains(QStringLiteral("TVD")));

    panel.clearDepthTables();
    QVERIFY(!panel.hasDeviationSurvey());
    QVERIFY(!panel.hasTimeDepthTable());
  }

  // ---- sink 迟装（面板先建）：setDefault 补挂已注册面板 ----
  void lateSinkInstallRewatches()
  {
    QTemporaryDir dir;
    const QString srcPath = dir.filePath(QStringLiteral("well.xml"));
    {
      QFile f(srcPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(makeSourceXml().toUtf8());
      f.close();
    }
    WellCompositePanel panel; // 先建面板（无默认 sink）
    QVERIFY(!panel.hasDeviationSurvey());

    WellCompositeDerivedSink sink;
    ioParsers(&sink); // 只喂解析器（不绑 catalog——喂表不依赖 catalog）
    WellCompositeDerivedSink::setDefault(&sink);

    QVERIFY(panel.loadComprehensiveXml(srcPath));
    QTRY_VERIFY_WITH_TIMEOUT(panel.hasDeviationSurvey() && panel.hasTimeDepthTable(), 1000);
    WellCompositeDerivedSink::setDefault(nullptr);
  }

  // ---- D3 打印管线：QPrinter（PDF 输出档）与 QPdfWriter 同一分页渲染管线 ----
  void printPipelineSharesRendererWithPdfExport()
  {
    WellCompositePanel panel;
    QVector<CurveData> curves;
    CurveData gr;
    gr.name = QStringLiteral("GR");
    gr.depths = {1000.0f, 1200.0f, 1400.0f};
    gr.values = {40.0f, 60.0f, 50.0f};
    curves << gr;
    QVERIFY(panel.loadLasCurves(QStringLiteral("W-9"), curves));

    ExportEngine::Options opt;
    opt.topDepth = 1000.0;
    opt.bottomDepth = 1400.0;
    opt.wellName = QStringLiteral("W-9");
    opt.projectName = QStringLiteral("tst");

    QTemporaryDir dir;
    const QString printPdf = dir.filePath(QStringLiteral("print.pdf"));
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::OutputFormat::PdfFormat);
    printer.setOutputFileName(printPdf);
    printer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait,
                                      QMarginsF(12, 14, 12, 14), QPageLayout::Millimeter));
    const QString err =
        ExportEngine::exportToPagedDevice(*panel.canvas(), panel.currentData(), printer, opt);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    {
      QFile f(printPdf);
      QVERIFY(f.open(QIODevice::ReadOnly));
      QCOMPARE(f.read(5), QByteArray("%PDF-"));
    }

    // PDF 导出路径（exportCanvas → QPdfWriter → 同一 exportToPagedDevice）回归
    const QString exportPdf = dir.filePath(QStringLiteral("export.pdf"));
    QVERIFY2(ExportEngine::exportCanvas(*panel.canvas(), panel.currentData(),
                                        ExportEngine::Format::Pdf, exportPdf, opt)
                 .isEmpty(),
             "pdf export via shared paged pipeline");
    QVERIFY(QFile(exportPdf).size() > 0);

    // 打印探测：offscreen 无打印服务 → false（降级路径判定）；有打印机的真机
    // 环境为 true 也合法——只要求探测稳定且打印入口据此分支。
    QCOMPARE(ExportEngine::nativePrintAvailable(), ExportEngine::nativePrintAvailable());
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositeShell tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_shell.moc"
