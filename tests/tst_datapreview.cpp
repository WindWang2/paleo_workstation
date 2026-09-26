#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/ui/datapreview/datapreviewtabs.h"

#include <QComboBox>
#include <QLabel>
#include <QPdfDocument>
#include <QPdfView>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>

// plan §4 数据页预览标签：重选聚焦、可关闭、空态/失败/外链缺失文案、
// 九类资产各自的面板内容、well_head 高亮信号、horizon 在地图上显示信号。
class TestDataPreview : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
    std::unique_ptr<DataPreviewTabs> preview;
    QString metaPath;
  };

  static std::unique_ptr<Stack> makeStack(const QString &projectDir)
  {
    if (!QDir().mkpath(projectDir))
      return nullptr;
    auto s = std::make_unique<Stack>();
    s->metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(s->metaPath);
    if (!s->manifest->open())
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->importSvc = std::make_unique<DataImportService>(s->layerSvc.get(), s->store.get());
    s->importSvc->setProjectDir(projectDir);
    s->preview = std::make_unique<DataPreviewTabs>();
    s->preview->setImportService(s->importSvc.get());
    return s;
  }

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  static QString stage(const QTemporaryDir &tmp, const QString &dir, const QString &name,
                       const QString &asName = QString())
  {
    const QString d = tmp.filePath(dir);
    if (!QDir().mkpath(d))
      return QString();
    const QString dst = QDir(d).filePath(asName.isEmpty() ? name : asName);
    return QFile::copy(fixture(name), dst) ? dst : QString();
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  void emptyStateBeforeAnyTab();
  void reselectFocusesExistingTab();
  void closableTabsRestoreEmptyState();
  void wellHeadSelectionEmitsHighlight();
  void missingExternalSourceShowsState();
  void horizonTabOffersShowOnMap();
  void everyTypeOpensContent();
  void topsTimeColumnStaysBlank();
  void tamperedExternalSourceShowsShaMismatch();
  void document_pdfRendersInTab();
  void document_stubbedConverterYieldsDerived();
  void document_converterMissingFailsHonest();

private:
  // 共享一次导入的夹具集（每个测试自建栈，互不污染）。
  struct Imported
  {
    QString wellHead, las, tops, td, d61, sgy, png, pdf, geojson, sgySource;
  };
  static Imported importAll(Stack &st, const QTemporaryDir &tmp)
  {
    Imported out;
    QString err;
    out.wellHead = st.importSvc->importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err);
    out.las = st.importSvc->importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    const QString topsPath = stage(tmp, QString::fromUtf8("井分层"), QStringLiteral("DC.dat"));
    out.tops = st.importSvc->importProjectFile(topsPath, &err);
    const QString tdPath = stage(tmp, QString::fromUtf8("时深"), QStringLiteral("A1_TD.dat"));
    out.td = st.importSvc->importProjectFile(tdPath, &err);
    const QString d61Path = stage(tmp, QString::fromUtf8("层位"), QStringLiteral("D61_sample.dat"),
                                  QStringLiteral("D61.dat"));
    out.d61 = st.importSvc->importProjectFile(d61Path, &err);
    out.sgySource = tmp.filePath(QStringLiteral("vol.sgy"));
    QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), out.sgySource);
    out.sgy = st.importSvc->importProjectFile(out.sgySource, &err);
    out.png = st.importSvc->importProjectFile(fixture(QStringLiteral("tiny.png")), &err);
    out.pdf = st.importSvc->importProjectFile(fixture(QStringLiteral("tiny.pdf")), &err);
    out.geojson = st.importSvc->importProjectFile(fixture(QStringLiteral("facies.geojson")), &err);
    return out;
  }
};

void TestDataPreview::emptyStateBeforeAnyTab()
{
  DataPreviewTabs pv;
  auto *empty = pv.findChild<QLabel *>(QStringLiteral("previewEmptyLabel"));
  QVERIFY(empty);
  QVERIFY(empty->isVisible() || !empty->isHidden()); // 未开标签时可见
  QCOMPARE(empty->text(), QStringLiteral("还没有打开的预览 — 在列表中选择一条数据"));
  QCOMPARE(pv.tabCount(), 0);
}

void TestDataPreview::reselectFocusesExistingTab()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.las.isEmpty());
  QVERIFY(!ids.td.isEmpty());

  st->preview->openAsset(ids.las);
  st->preview->openAsset(ids.td);
  QCOMPARE(st->preview->tabCount(), 2);
  st->preview->openAsset(ids.las); // 重选 → 聚焦已有标签，不开新
  QCOMPARE(st->preview->tabCount(), 2);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("A1.Las"));

  // 阶段 B 验收语义：切到时深再切回，标签仍在
  st->preview->openAsset(ids.td);
  st->preview->openAsset(ids.las);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("A1.Las"));
}

void TestDataPreview::closableTabsRestoreEmptyState()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.las);
  st->preview->openAsset(ids.td);
  QCOMPARE(st->preview->tabCount(), 2);
  st->preview->closeAssetTab(ids.td);
  QCOMPARE(st->preview->tabCount(), 1);
  st->preview->closeAssetTab(ids.las);
  QCOMPARE(st->preview->tabCount(), 0);
  auto *empty = st->preview->findChild<QLabel *>(QStringLiteral("previewEmptyLabel"));
  QVERIFY(empty);
  QVERIFY(!empty->isHidden());
}

void TestDataPreview::wellHeadSelectionEmitsHighlight()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.wellHead.isEmpty());
  QSignalSpy spy(st->preview.get(), &DataPreviewTabs::wellSelected);
  st->preview->openAsset(ids.wellHead);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("well-A1"));
}

void TestDataPreview::missingExternalSourceShowsState()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.sgySource.isEmpty());
  QFile::remove(ids.sgySource); // 外链源消失
  st->preview->openAsset(ids.sgy);
  QVERIFY(st->preview->isMissingSourceState(ids.sgy));
}

void TestDataPreview::horizonTabOffersShowOnMap()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.d61.isEmpty());
  QSignalSpy spy(st->preview.get(), &DataPreviewTabs::showHorizonOnMapRequested);
  st->preview->openAsset(ids.d61);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *btn = page->findChild<QPushButton *>(QStringLiteral("showOnMapBtn"));
  QVERIFY2(btn, "horizon tab must offer show-on-map");
  btn->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("horizon.D61"));
}

void TestDataPreview::everyTypeOpensContent()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));

  // well_log：曲线组合框默认 GR 可见
  st->preview->openAsset(ids.las);
  QWidget *lasPage = tabs->widget(tabs->currentIndex());
  auto *combo = lasPage->findChild<QComboBox *>(QStringLiteral("curveCombo"));
  QVERIFY2(combo, "well_log tab needs curve selector");
  QVERIFY(combo->findText(QStringLiteral("GR")) >= 0);
  QCOMPARE(combo->currentText(), QStringLiteral("GR"));

  // well_stratification：A1 分层表
  st->preview->openAsset(ids.tops);
  QWidget *topsPage = tabs->widget(tabs->currentIndex());
  auto *table = topsPage->findChild<QTableWidget *>(QStringLiteral("topsTable"));
  QVERIFY2(table, "tops tab needs the stratification table");
  QVERIFY(table->rowCount() > 10);

  // time_depth / well_head / horizon / seismic / image / document / geojson
  st->preview->openAsset(ids.td);
  QVERIFY(tabs->widget(tabs->currentIndex())->findChildren<QWidget *>().size() > 1);
  st->preview->openAsset(ids.wellHead);
  QVERIFY(tabs->widget(tabs->currentIndex())->findChildren<QLabel *>().size() >= 5);
  st->preview->openAsset(ids.d61);
  QVERIFY(tabs->widget(tabs->currentIndex())->findChild<QPushButton *>(QStringLiteral("showOnMapBtn")));
  st->preview->openAsset(ids.sgy);
  QVERIFY2(tabs->widget(tabs->currentIndex())->findChild<QSpinBox *>() != nullptr,
           "seismic tab needs a line selector");
  st->preview->openAsset(ids.png);
  QVERIFY(!tabs->widget(tabs->currentIndex())->findChildren<QLabel *>().isEmpty());
  st->preview->openAsset(ids.pdf);
  QVERIFY2(tabs->widget(tabs->currentIndex())
                   ->findChildren<QPushButton *>()
                   .size() >= 1,
           "document tab needs open-externally button");
  st->preview->openAsset(ids.geojson);
  bool sawUnregistered = false;
  for (QLabel *l : tabs->widget(tabs->currentIndex())->findChildren<QLabel *>())
    if (l->text().contains(QStringLiteral("未配准")))
      sawUnregistered = true;
  QVERIFY(sawUnregistered);
  QCOMPARE(st->preview->tabCount(), 9);
}

void TestDataPreview::topsTimeColumnStaysBlank()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.tops);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *topsPage = tabs->widget(tabs->currentIndex());
  auto *table = topsPage->findChild<QTableWidget *>(QStringLiteral("topsTable"));
  QVERIFY(table);
  // DC.dat 里 Time 全为 -99999 → 显示空（不填假时间）
  for (int r = 0; r < table->rowCount(); ++r)
  {
    QTableWidgetItem *timeItem = table->item(r, 3);
    QVERIFY(timeItem);
    QVERIFY2(timeItem->text().isEmpty(), qPrintable(timeItem->text()));
  }
}

// §3：外链源在入库后被动过 → 标签页如实写「源文件与入库时的 SHA-256 不一致」，
// 不给解码入口（没有测线选择器，没有剖面）。
void TestDataPreview::tamperedExternalSourceShowsShaMismatch()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const QString sgy = tmp.filePath(QStringLiteral("vol.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
  QString err;
  const QString assetId = st->importSvc->importProjectFile(sgy, &err);
  QVERIFY2(!assetId.isEmpty(), qPrintable(err));
  const CatalogVersion v = st->importSvc->catalog()->currentVersion(assetId);
  QVERIFY(!v.managed);
  QVERIFY(!v.sha256.isEmpty());

  QFile f(sgy);
  QVERIFY(f.open(QIODevice::Append));
  QCOMPARE(f.write("X"), 1);
  f.close();

  st->preview->openAsset(assetId);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  QWidget *page = tabs->widget(tabs->currentIndex());
  QVERIFY(page);
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY2(state, "sha-mismatch tab must show the honest state line");
  QCOMPARE(state->text(), QStringLiteral("源文件与入库时的 SHA-256 不一致"));
  QVERIFY(!page->findChild<QSpinBox *>()); // 不解码
}

void TestDataPreview::document_pdfRendersInTab()
{
  // PDF 原件不经转换：直接进 QPdfView，catalog 里仍只有 RAW 版本。
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  QString err;
  const QString pdfId = st->importSvc->importProjectFile(
      fixture(QStringLiteral("tiny.pdf")), &err);
  QVERIFY2(!pdfId.isEmpty(), qPrintable(err));

  st->preview->openAsset(pdfId);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  auto *view = tabs->widget(tabs->currentIndex())
                   ->findChild<QPdfView *>(QStringLiteral("pdfView"));
  QVERIFY2(view, "pdf document should render in QPdfView, not degrade");
  QVERIFY(view->document());
  QCOMPARE(view->document()->status(), QPdfDocument::Status::Ready);
  QCOMPARE(view->document()->pageCount(), 1);
  QCOMPARE(st->importSvc->catalog()->versionsForAsset(pdfId).size(), 1);
}

void TestDataPreview::document_stubbedConverterYieldsDerived()
{
  // stub 转换器（不依赖真 soffice）：把 tiny.pdf 复制为 <stem>.pdf，
  // 走完 DERIVED 登记 + 信号 → 标签重建 → pdfView 的全链路。
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);

  const QString docPath = tmp.filePath(QStringLiteral("report.docx"));
  {
    QFile f(docPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not a real docx — the stub converter ignores content");
  }
  const QString stub = tmp.filePath(QStringLiteral("fake_soffice.sh"));
  {
    QFile s(stub);
    QVERIFY(s.open(QIODevice::WriteOnly));
    s.write(QStringLiteral("#!/bin/sh\n"
                           "outdir=\"\"; src=\"\"\n"
                           "while [ $# -gt 0 ]; do\n"
                           "  if [ \"$1\" = \"--outdir\" ]; then outdir=\"$2\"; shift 2\n"
                           "  else src=\"$1\"; shift; fi\n"
                           "done\n"
                           "base=$(basename \"$src\"); base=\"${base%.*}\"\n"
                           "cp \"%1\" \"$outdir/$base.pdf\"\n")
                .arg(fixture(QStringLiteral("tiny.pdf")))
                .toUtf8());
  }
  QFile::setPermissions(stub, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                  QFileDevice::ReadOther);
  st->importSvc->setDocumentConverterProgram(stub);

  QString err;
  const QString docId = st->importSvc->importProjectFile(docPath, &err);
  QVERIFY2(!docId.isEmpty(), qPrintable(err));

  QSignalSpy readySpy(st->importSvc.get(), &DataImportService::documentPdfReady);
  QSignalSpy failSpy(st->importSvc.get(), &DataImportService::documentPdfFailed);
  st->preview->openAsset(docId);
  QCOMPARE(st->importSvc->documentPdfState(docId), DataImportService::DocPdfState::Pending);

  QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 15000);
  QCOMPARE(failSpy.count(), 0);
  QCOMPARE(st->importSvc->documentPdfState(docId), DataImportService::DocPdfState::Ready);

  // DERIVED 版本登记：父=RAW、受管、sha256 已算、文件在盘。
  const auto versions = st->importSvc->catalog()->versionsForAsset(docId);
  QCOMPARE(versions.size(), 2);
  QString rawId;
  CatalogVersion derived;
  for (const CatalogVersion &v : versions)
  {
    if (v.stage == QLatin1String("RAW"))
      rawId = v.id;
    if (v.stage == QLatin1String("DERIVED"))
      derived = v;
  }
  QVERIFY(!rawId.isEmpty());
  QVERIFY(!derived.id.isEmpty());
  QVERIFY(derived.fileName.endsWith(QStringLiteral(".pdf")));
  QVERIFY(!derived.sha256.isEmpty());
  QVERIFY(derived.managed);
  QVERIFY(derived.parentVersionIds.contains(rawId));
  QVERIFY(QFile::exists(st->importSvc->documentPdfPath(docId)));

  // 信号驱动的标签重建后，pdfView 就位。
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  auto *view = tabs->widget(tabs->currentIndex())
                   ->findChild<QPdfView *>(QStringLiteral("pdfView"));
  QVERIFY2(view, "converted pdf should render after rebuild");
  QCOMPARE(view->document()->status(), QPdfDocument::Status::Ready);
}

void TestDataPreview::document_converterMissingFailsHonest()
{
  // 无转换器：Failed 态如实报「找不到 LibreOffice」，标签保留降级面。
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  st->importSvc->setDocumentConverterProgram(QString()); // 强制不可用

  const QString docPath = tmp.filePath(QStringLiteral("report.pptx"));
  {
    QFile f(docPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("fake");
  }
  QString err;
  const QString docId = st->importSvc->importProjectFile(docPath, &err);
  QVERIFY(!docId.isEmpty());

  QSignalSpy failSpy(st->importSvc.get(), &DataImportService::documentPdfFailed);
  st->preview->openAsset(docId);
  QCOMPARE(failSpy.count(), 1);
  QCOMPARE(st->importSvc->documentPdfState(docId), DataImportService::DocPdfState::Failed);
  QVERIFY(st->importSvc->documentPdfError(docId).contains(QStringLiteral("LibreOffice")));

  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(state && state->text().contains(QStringLiteral("无 PDF 预览")));
  QVERIFY(page->findChildren<QPushButton *>().size() >= 1); // 用系统程序打开
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestDataPreview tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_datapreview.moc"
