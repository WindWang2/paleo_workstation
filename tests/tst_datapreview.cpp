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
#include "../src/services/paleotaskservice.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include <qgsmapcanvas.h>

#include <QComboBox>
#include <QLabel>
#include <QPdfDocument>
#include <QPdfView>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QtEndian>

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
  void multiWellTabComboAndTitle();
  void otherAssetDoesNotChangeChosenWell();
  void loadingFailureAndRetryStates();
  void retryRebuildsAfterFix();
  void wellHeadShowsBottomAndTypeColumns();
  void lasMissingCurveIsDisabled();
  void topsTableShowsXYColumns();
  void timeDepthEmptyShowsNoTable();
  void horizonShowsRejectedAndCollisions();
  void seismicTabLabelsAndTieMarker();
  void seismicDecodeRunsThroughTaskService();
  void seismicTieShowsReasonWithoutTd();
  void seismicDefaultsToTieWellInline();
  void tamperedExternalSourceShowsShaMismatch();
  void document_pdfRendersInTab();
  void document_stubbedConverterYieldsDerived();
  void document_converterMissingFailsHonest();
  void coordinateStatusMappingIsChinese();
  void unresolvedMultiWellTabShowsDeadEnd();
  void lasMultiCurveAndZooming();
  void surveyAreaOpensQgisCanvasAndEmitsShowOnMain();

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
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  // §4 标题：「文件名 · 井名」（过滤型标签带井后缀）。
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("A1_TD.dat · A1"));
  st->preview->openAsset(ids.las); // 重选 → 聚焦已有标签，不开新
  QCOMPARE(st->preview->tabCount(), 2);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("A1.Las · A1"));

  // 阶段 B 验收语义：切到时深再切回，标签仍在
  st->preview->openAsset(ids.td);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("A1_TD.dat · A1"));
  st->preview->openAsset(ids.las);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("A1.Las · A1"));
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
  // §4：多井井位表未选井前不高亮；「井」下拉框选中 A1 才发信号。
  QCOMPARE(spy.count(), 0);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY2(combo, "multi-well well_head tab must carry its own 井 combo");
  const int idx = combo->findText(QStringLiteral("A1"));
  QVERIFY(idx >= 0);
  combo->setCurrentIndex(idx);
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
  // T29：按钮带 layerId 身份（双向同步按它寻址）。
  QCOMPARE(btn->property("layerId").toString(), QStringLiteral("horizon.D61"));
  btn->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("horizon.D61"));
  // 点击只发意图——按钮态由 shell 回调（显示成功/可见性变化）驱动。
  QCOMPARE(btn->text(), QStringLiteral("在地图上显示"));

  // 双向同步（T29）：shell 报告已在地图上 → 按钮改名；图层树隐藏 → 跟随回退。
  st->preview->setHorizonOnMap(QStringLiteral("horizon.D61"), true);
  QCOMPARE(btn->text(), QStringLiteral("已在地图上"));
  QVERIFY(btn->property("onMap").toBool());
  st->preview->setHorizonOnMap(QStringLiteral("horizon.D61"), false);
  QCOMPARE(btn->text(), QStringLiteral("在地图上显示"));
  QVERIFY(!btn->property("onMap").toBool());
  // 其它 layerId 不误伤。
  st->preview->setHorizonOnMap(QStringLiteral("horizon.D62"), true);
  QCOMPARE(btn->text(), QStringLiteral("在地图上显示"));
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

  // well_stratification：多井 DC.dat 自带「井」下拉框；选中 A1 后见分层表
  st->preview->openAsset(ids.tops);
  QWidget *topsPage = tabs->widget(tabs->currentIndex());
  auto *topsCombo = topsPage->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY2(topsCombo, "multi-well tops tab needs its own 井 combo");
  topsCombo->setCurrentIndex(topsCombo->findText(QStringLiteral("A1")));
  auto *table = topsPage->findChild<QTableWidget *>(QStringLiteral("topsTable"));
  QVERIFY2(table, "tops tab needs the stratification table after a well is chosen");
  QVERIFY(table->rowCount() > 10);

  // time_depth / well_head / horizon / seismic / image / document / geojson
  st->preview->openAsset(ids.td);
  QVERIFY(tabs->widget(tabs->currentIndex())->findChildren<QWidget *>().size() > 1);
  st->preview->openAsset(ids.wellHead);
  QWidget *whPage = tabs->widget(tabs->currentIndex());
  auto *whCombo = whPage->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY2(whCombo, "multi-well well_head tab needs its own 井 combo");
  whCombo->setCurrentIndex(whCombo->findText(QStringLiteral("A1")));
  QVERIFY(whPage->findChildren<QLabel *>().size() >= 5);
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
  bool sawSpace = false;
  for (QLabel *l : tabs->widget(tabs->currentIndex())->findChildren<QLabel *>())
    if (l->text().contains(QStringLiteral("经纬度，与本测网不是同一空间")))
      sawSpace = true;
  QVERIFY2(sawSpace, "geojson tab must carry the CRS-mismatch caption");
  // D11：GeoJSON 标签带「临时配准（手工仿射）…」入口，确认后走
  // provisionalRegistrationRequested（对话框壳层测试不点）。
  auto *regBtn = tabs->widget(tabs->currentIndex())
                     ->findChild<QPushButton *>(
                         QStringLiteral("provisionalRegisterButton"));
  QVERIFY2(regBtn, "geojson tab needs the provisional-registration entry");
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
  auto *combo = topsPage->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A1")));
  auto *table = topsPage->findChild<QTableWidget *>(QStringLiteral("topsTable"));
  QVERIFY(table);
  QCOMPARE(table->columnCount(), 6);
  // DC.dat 里 Time 全为 -99999 → 显示空（不填假时间）；Time 列在第 5 列（X/Y 之后）
  QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Time(ms)"));
  for (int r = 0; r < table->rowCount(); ++r)
  {
    QTableWidgetItem *timeItem = table->item(r, 5);
    QVERIFY(timeItem);
    QVERIFY2(timeItem->text().isEmpty(), qPrintable(timeItem->text()));
  }
}

// §4：多井资产（DC.dat、井位表）每个标签自带标为「井」的下拉框，只列已决
// 链接的井；默认未选 → 正文「先选择一口井」+ 裸文件名标题；选中后标题
// 变「文件名 · 井名」并按该井过滤。
void TestDataPreview::multiWellTabComboAndTitle()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.tops);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());

  auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY2(combo, "multi-well asset tab must carry a 井 combo");
  QCOMPARE(combo->count(), 20); // DC.dat 挂到 A1–A20 的已决链接
  QCOMPARE(combo->currentIndex(), -1); // 默认空
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(state);
  QCOMPARE(state->text(), QStringLiteral("先选择一口井"));
  QVERIFY(!page->findChild<QTableWidget *>(QStringLiteral("topsTable")));
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("DC.dat"));

  const int idx = combo->findText(QStringLiteral("A1"));
  QVERIFY(idx >= 0);
  combo->setCurrentIndex(idx);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("DC.dat · A1"));
  auto *table = page->findChild<QTableWidget *>(QStringLiteral("topsTable"));
  QVERIFY(table);
  QVERIFY(table->rowCount() > 10);
  QVERIFY(table->rowCount() < 100); // 只 A1 的层，不是全文件 516 行
}

// §4：下拉框不改其他标签里已选的井；选中另一条资产也不改本标签的井。
void TestDataPreview::otherAssetDoesNotChangeChosenWell()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.tops);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *topsPage = tabs->widget(tabs->currentIndex());
  auto *combo = topsPage->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A2")));
  QCOMPARE(combo->currentText(), QStringLiteral("A2"));

  st->preview->openAsset(ids.las); // 打开另一条资产 → tops 标签的井不动
  QCOMPARE(combo->currentText(), QStringLiteral("A2"));
  int topsIdx = -1;
  for (int i = 0; i < tabs->count(); ++i)
    if (st->preview->assetIdAt(i) == ids.tops)
      topsIdx = i;
  QVERIFY(topsIdx >= 0);
  QCOMPARE(tabs->tabText(topsIdx), QStringLiteral("DC.dat · A2"));

  st->preview->openAsset(ids.tops); // 重选同一资产聚焦，不清选择
  QCOMPARE(combo->currentText(), QStringLiteral("A2"));
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("DC.dat · A2"));
}

// §4：加载态「正在读取」+文件名（同步读取前置钩子）；失败态「读取失败」+
// 原因+文件名+「重试」按钮。
void TestDataPreview::loadingFailureAndRetryStates()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);

  const QString badGeo = tmp.filePath(QStringLiteral("bad.geojson"));
  {
    QFile f(badGeo);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ not json");
  }
  QString err;
  const QString assetId = st->importSvc->importProjectFile(badGeo, &err);
  QVERIFY2(!assetId.isEmpty(), qPrintable(err));

  st->preview->openAsset(assetId);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());

  auto *loading = page->findChild<QLabel *>(QStringLiteral("loadingText"));
  QVERIFY2(loading, "loading-state hook must exist");
  QVERIFY(loading->text().contains(QStringLiteral("正在读取")));
  QVERIFY(loading->text().contains(QStringLiteral("bad.geojson")));

  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(state);
  QVERIFY(state->text().contains(QStringLiteral("读取失败")));
  QVERIFY(state->text().contains(QStringLiteral("GeoJSON 解析失败")));
  QVERIFY(state->text().contains(QStringLiteral("bad.geojson")));
  QVERIFY2(page->findChild<QPushButton *>(QStringLiteral("retryBtn")),
           "failure state must offer 重试");
}

// 「重试」重建该标签：受管文件修好后再点，内容回来。
void TestDataPreview::retryRebuildsAfterFix()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.tops.isEmpty());

  // 把受管副本置为不可读 → 打开即失败态。
  const CatalogVersion v = st->importSvc->catalog()->currentVersion(ids.tops);
  const QString abs = st->importSvc->absolutePathForVersion(v);
  QVERIFY(QFile::exists(abs));
  QFile::setPermissions(abs, QFileDevice::Permissions{});

  st->preview->openAsset(ids.tops);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A1"))); // 触发按井读取
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(state);
  QVERIFY(state->text().contains(QStringLiteral("读取失败")));
  QVERIFY(state->text().contains(QStringLiteral("DC.dat")));
  auto *retry = page->findChild<QPushButton *>(QStringLiteral("retryBtn"));
  QVERIFY(retry);

  QFile::setPermissions(abs, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ReadGroup | QFileDevice::ReadOther);
  retry->click();
  QCOMPARE(st->preview->tabCount(), 1);
  QVERIFY2(page->findChild<QComboBox *>(QStringLiteral("wellCombo")),
           "retry must rebuild the tab");
  QVERIFY2(page->findChild<QTableWidget *>(QStringLiteral("topsTable")),
           "重建时恢复已选的井 → 分层表回来");
}

// §4：井口标签补 BottomX、BottomY、WellType；数值 JetBrains Mono 9pt 右对齐。
void TestDataPreview::wellHeadShowsBottomAndTypeColumns()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.wellHead);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A1")));

  bool sawBottomX = false, sawBottomY = false, sawType = false, sawStatus = false;
  bool sawStatusText = false;
  QLabel *monoValue = nullptr;
  for (QLabel *l : page->findChildren<QLabel *>())
  {
    const QString t = l->text();
    if (t == QStringLiteral("BottomX"))
      sawBottomX = true;
    if (t == QStringLiteral("BottomY"))
      sawBottomY = true;
    if (t == QStringLiteral("WellType"))
      sawType = true;
    if (t == QString::fromUtf8("坐标状态")) // T27 中文化
      sawStatus = true;
    if (t == QString::fromUtf8("工程坐标 · 米 · 未投影")) // untransformed → 状态栏同句
    {
      sawStatusText = true;
      QVERIFY(l->styleSheet().contains(QStringLiteral("#5D6E80"))); // 计划：仍 text-muted
    }
    if (t == QStringLiteral("5288.67"))
      monoValue = l;
  }
  QVERIFY(sawBottomX && sawBottomY && sawType && sawStatus && sawStatusText);
  QVERIFY2(monoValue, "A1 的 BottomX/X 值 5288.67 必须显示");
  QVERIFY(monoValue->font().families().contains(QStringLiteral("JetBrains Mono")));
  QVERIFY(monoValue->alignment() & Qt::AlignRight);
}

// T27：coordinate_status 枚举显示串中文化（§4 计划文案；未知/空按没有坐标）。
void TestDataPreview::coordinateStatusMappingIsChinese()
{
  QCOMPARE(DataPreviewTabs::coordinateStatusText(QStringLiteral("ok")),
           QString::fromUtf8("坐标有效"));
  QCOMPARE(DataPreviewTabs::coordinateStatusText(QStringLiteral("untransformed")),
           QString::fromUtf8("工程坐标 · 米 · 未投影"));
  QCOMPARE(DataPreviewTabs::coordinateStatusText(QStringLiteral("invalid")),
           QString::fromUtf8("坐标无效"));
  QCOMPARE(DataPreviewTabs::coordinateStatusText(QStringLiteral("missing")),
           QString::fromUtf8("没有坐标"));
  QCOMPARE(DataPreviewTabs::coordinateStatusText(QString()),
           QString::fromUtf8("没有坐标"));
}

// T31 死胡同：未决资产的多井 tab 无井可挂时给「挂到这口井」入口说明；
// 工程里一口井都没有时指向导入——都不留空白页。
void TestDataPreview::unresolvedMultiWellTabShowsDeadEnd()
{
  // 场景一：工程有井（A1），但这个 tops 资产未决（井名 Z9 不匹配）。
  {
    QTemporaryDir tmp;
    auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
    QVERIFY(st != nullptr);
    QString err;
    QVERIFY(!st->importSvc->importProjectFile(
                 fixture(QStringLiteral("ExportWellHead.dat")), &err)
                 .isEmpty()); // 建 A1
    const QString topsDir = tmp.filePath(QString::fromUtf8("井分层"));
    QDir().mkpath(topsDir);
    const QString topsPath = QDir(topsDir).filePath(QStringLiteral("Z9.dat"));
    {
      QFile f(topsPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("#WellTops File From SMI\n"
              "#WellName    Name         MD           X            Y            Z            TVD          Time(ms)    \n"
              "Z9           A            942.500      1000.0       2000.0       -942.500     942.500      -99999.000  \n");
    }
    const QString z9 = st->importSvc->importProjectFile(topsPath, &err);
    QVERIFY2(!z9.isEmpty(), qPrintable(err));

    st->preview->openAsset(z9);
    auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
    QWidget *page = tabs->widget(tabs->currentIndex());
    auto *deadEnd = page->findChild<QLabel *>(QStringLiteral("deadEndText"));
    QVERIFY2(deadEnd, "unresolved multi-well tab must explain the dead end");
    QVERIFY(deadEnd->text().contains(QString::fromUtf8("还没有挂到任何井")));
    QVERIFY(deadEnd->text().contains(QString::fromUtf8("挂到这口井")));
  }

  // 场景二：工程里一口井都没有 → 指向导入工区文件夹。
  {
    QTemporaryDir tmp;
    auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
    QVERIFY(st != nullptr);
    const QString topsDir = tmp.filePath(QString::fromUtf8("井分层"));
    QDir().mkpath(topsDir);
    const QString topsPath = QDir(topsDir).filePath(QStringLiteral("Z9.dat"));
    {
      QFile f(topsPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("#WellTops File From SMI\n"
              "#WellName    Name         MD           X            Y            Z            TVD          Time(ms)    \n"
              "Z9           A            942.500      1000.0       2000.0       -942.500     942.500      -99999.000  \n");
    }
    QString err;
    const QString z9 = st->importSvc->importProjectFile(topsPath, &err);
    QVERIFY2(!z9.isEmpty(), qPrintable(err));
    st->preview->openAsset(z9);
    auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
    QWidget *page = tabs->widget(tabs->currentIndex());
    auto *deadEnd = page->findChild<QLabel *>(QStringLiteral("deadEndText"));
    QVERIFY(deadEnd);
    QVERIFY(deadEnd->text().contains(QString::fromUtf8("还没有井")));
    QVERIFY(deadEnd->text().contains(QString::fromUtf8("导入工区文件夹")));
  }
}

// §4：LAS 约定曲线 GR/AC/DEN 缺了就给禁用项，tooltip「这条曲线不在文件里」。
void TestDataPreview::lasMissingCurveIsDisabled()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  QString err;
  QVERIFY(!st->importSvc->importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err)
               .isEmpty());
  const QString las = tmp.filePath(QStringLiteral("A1.las")); // 只有 DEPT+GR
  {
    QFile f(las);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("~Version Information\n"
            " VERS.  2.0:\n"
            " WRAP.  NO:\n"
            "~Well Information\n"
            " STRT.m 1000.0:\n"
            " STOP.m 1005.0:\n"
            " STEP.m 1.0:\n"
            " NULL. -9999.99:\n"
            " WELL. A1:\n"
            "~Curve Information\n"
            " DEPT.m : DEPTH\n"
            " GR.    : GAMMA\n"
            "~A\n"
            "1000.0 10.0\n"
            "1001.0 20.0\n"
            "1002.0 30.0\n"
            "1003.0 -9999.99\n"
            "1004.0 40.0\n"
            "1005.0 50.0\n");
  }
  const QString lasId = st->importSvc->importProjectFile(las, &err);
  QVERIFY2(!lasId.isEmpty(), qPrintable(err));
  st->preview->openAsset(lasId);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *combo = page->findChild<QComboBox *>(QStringLiteral("curveCombo"));
  QVERIFY(combo);
  auto *model = qobject_cast<QStandardItemModel *>(combo->model());
  QVERIFY(model);
  const int ac = combo->findText(QStringLiteral("AC"));
  QVERIFY2(ac >= 0, "约定曲线缺了也要出现（禁用项）");
  QVERIFY(model->item(ac) && !model->item(ac)->isEnabled());
  QCOMPARE(combo->itemData(ac, Qt::ToolTipRole).toString(),
           QStringLiteral("这条曲线不在文件里"));
  const int gr = combo->findText(QStringLiteral("GR"));
  QVERIFY(gr >= 0);
  QVERIFY(model->item(gr)->isEnabled()); // 文件里的曲线可选
  QCOMPARE(combo->currentText(), QStringLiteral("GR"));
}

// §4：分层表列 层名 MD TVD X Y Time(ms)；数字列 mono 右对齐。
void TestDataPreview::topsTableShowsXYColumns()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.tops);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A1")));
  auto *table = page->findChild<QTableWidget *>(QStringLiteral("topsTable"));
  QVERIFY(table);
  QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("层名"));
  QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("X"));
  QCOMPARE(table->horizontalHeaderItem(4)->text(), QStringLiteral("Y"));
  // A1 的 D61 分层点坐标 (5288.670, 8219.940)（§1）
  int d61 = -1;
  for (int r = 0; r < table->rowCount(); ++r)
    if (table->item(r, 0) && table->item(r, 0)->text() == QStringLiteral("D61"))
      d61 = r;
  QVERIFY(d61 >= 0);
  QCOMPARE(table->item(d61, 3)->text(), QStringLiteral("5288.67"));
  QCOMPARE(table->item(d61, 4)->text(), QStringLiteral("8219.94"));
  QVERIFY(table->item(d61, 3)->textAlignment() & Qt::AlignRight);
  QVERIFY(table->item(d61, 3)->font().families().contains(QStringLiteral("JetBrains Mono")));
}

// §4：TD 剔除 -99999 后没有可用样点 → 正文「无时深表」，不画假线。
void TestDataPreview::timeDepthEmptyShowsNoTable()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  QString err;
  QVERIFY(!st->importSvc->importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err)
               .isEmpty());
  const QString tdDir = tmp.filePath(QString::fromUtf8("时深"));
  QVERIFY(QDir().mkpath(tdDir));
  const QString tdPath = QDir(tdDir).filePath(QStringLiteral("empty.dat"));
  {
    QFile f(tdPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("# Well : A1\n380.0 -99999 -99999 -99999\n382.0 -99999 -99999 -99999\n");
  }
  const QString tdId = st->importSvc->importProjectFile(tdPath, &err);
  QVERIFY2(!tdId.isEmpty(), qPrintable(err));
  st->preview->openAsset(tdId);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(state);
  QCOMPARE(state->text(), QStringLiteral("无时深表"));
}

// §4/§3：层位标签显示拒绝点数与碰撞次数（越界点不写入，只计数）。
void TestDataPreview::horizonShowsRejectedAndCollisions()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);

  const QString dir = tmp.filePath(QString::fromUtf8("层位"));
  QVERIFY(QDir().mkpath(dir));
  const QString hz = QDir(dir).filePath(QStringLiteral("D61.dat"));
  {
    QFile f(hz);
    QVERIFY(f.open(QIODevice::WriteOnly));
    // 两行在网内 + 一行重复像元（碰撞）+ 一行越界（拒绝）。
    f.write("# Grid_size:411x641# Survey(Inline,Crossline,x,y)\n"
            "# P1:      1315,      4165,     0.00000,     0.00000\n"
            "# P2:      1315,      4805, 12793.00000,     0.00000\n"
            "# P3:      1725,      4805, 12793.00000, 16406.00000\n"
            "# Z_units: ms\n"
            "0.00000        0.00000        1254.54800     1315         4165\n"
            "19.98906       0.00000        1254.09700     1315         4166\n"
            "19.98906       0.00000        1254.00000     1315         4166\n"
            "9999.00000     0.00000        1200.00000     9999         9999\n");
  }
  QString err;
  const QString id = st->importSvc->importProjectFile(hz, &err);
  QVERIFY2(!id.isEmpty(), qPrintable(err));

  st->preview->openAsset(id);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  bool sawCounts = false;
  for (QLabel *l : page->findChildren<QLabel *>())
    if (l->text().contains(QStringLiteral("拒绝 1")) &&
        l->text().contains(QStringLiteral("碰撞 1")))
      sawCounts = true;
  QVERIFY2(sawCounts, "horizon tab must show 拒绝点数 and 碰撞次数");
  auto *btn = page->findChild<QPushButton *>(QStringLiteral("showOnMapBtn"));
  QVERIFY(btn);
  QVERIFY(btn->isEnabled()); // 有派生栅格 → 可用
}

// §4：地震标签 纵测线/横测线 切换；A1 D61 标定经 TD 表得到 ms。
void TestDataPreview::seismicTabLabelsAndTieMarker()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.sgy.isEmpty());
  st->preview->openAsset(ids.sgy);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());

  auto *mode = page->findChild<QComboBox *>(QStringLiteral("lineMode"));
  QVERIFY2(mode, "seismic tab needs 纵测线/横测线 selector");
  QCOMPARE(mode->itemText(0), QStringLiteral("纵测线"));
  QCOMPARE(mode->itemText(1), QStringLiteral("横测线"));
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  QVERIFY(no);
  QCOMPARE(no->minimum(), 1000); // mini_seismic 冻结测网 inline 1000–1002
  QCOMPARE(no->maximum(), 1002);

  // A1 的 D61 TVD 1935 落在 A1_TD 可用范围内 → 有数值标定。
  auto *tie = page->findChild<QLabel *>(QStringLiteral("tieLabel"));
  QVERIFY(tie);
  QVERIFY(tie->text().startsWith(QStringLiteral("A1 D61")));
  QVERIFY(tie->text().contains(QStringLiteral("ms")));
  // 标题「文件名 · 测线」（本夹具井位在测网外 → 落回最小 inline）。
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("vol.sgy · IL1000"));
}

// D1：接了任务服务 → 索引/SHA/解码走任务池；完成后标题带测线后缀；
// 换测线派发新任务（索引缓存命中不再重建）。
void TestDataPreview::seismicDecodeRunsThroughTaskService()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  PaleoProjectStore taskStore;
  PaleoTaskService taskSvc(&taskStore);
  st->preview->setTaskService(&taskSvc);

  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.sgy.isEmpty());
  st->preview->openAsset(ids.sgy);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());

  // 解码任务注册进任务服务并跑成功。
  QTRY_VERIFY_WITH_TIMEOUT(!taskSvc.tasks().isEmpty(), 3000);
  PaleoTask *t1 = taskSvc.tasks().first();
  QTRY_COMPARE_WITH_TIMEOUT(t1->state(), PaleoTask::State::Succeeded, 5000);
  QCOMPARE(tabs->tabText(tabs->currentIndex()),
           QStringLiteral("vol.sgy · IL1000"));
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  QVERIFY(no);

  // 换测线 → 第二个任务（索引缓存命中，不再 open 全文件）。
  no->setValue(1001);
  QTRY_VERIFY_WITH_TIMEOUT(taskSvc.tasks().size() >= 2, 3000);
  PaleoTask *t2 = taskSvc.tasks().at(1);
  QTRY_COMPARE_WITH_TIMEOUT(t2->state(), PaleoTask::State::Succeeded, 5000);
  QCOMPARE(tabs->tabText(tabs->currentIndex()),
           QStringLiteral("vol.sgy · IL1001"));
}

// §4：没有 TD 表 → 标定写原因「无时深表」，绝不造时间。
void TestDataPreview::seismicTieShowsReasonWithoutTd()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  QString err;
  QVERIFY(!st->importSvc->importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err)
               .isEmpty());
  const QString topsPath = stage(tmp, QString::fromUtf8("井分层"), QStringLiteral("DC.dat"));
  QVERIFY(!st->importSvc->importProjectFile(topsPath, &err).isEmpty());
  const QString sgy = tmp.filePath(QStringLiteral("vol.sgy"));
  QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
  QVERIFY(!st->importSvc->importProjectFile(sgy, &err).isEmpty());

  const auto assets = st->importSvc->assets(QStringLiteral("seismic"));
  QCOMPARE(assets.size(), 1);
  st->preview->openAsset(assets.first());
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *tie = page->findChild<QLabel *>(QStringLiteral("tieLabel"));
  QVERIFY(tie);
  QVERIFY(tie->text().contains(QStringLiteral("无时深表")));
  QVERIFY(!tie->text().contains(QStringLiteral("ms"))); // 不造时间
}

// §4/§7：初始测线是标定井所在 inline（分层点坐标经测网角点换算），
// 旁注「A1 所在测线」；判不出才回 min。合成 SEG-Y 把 A1 盖进测网。
static bool writeOrdinalSegy(const QString &path, int traces, int perLine,
                             qint32 frecBase, qint32 cdpBase,
                             double x0, double dx, double y0, double dy)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  f.write(QByteArray(3200, ' '));
  QByteArray bh(400, 0);
  qToBigEndian<qint16>(perLine, reinterpret_cast<uchar *>(bh.data()) + 12);
  qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(bh.data()) + 16);
  qToBigEndian<qint16>(64, reinterpret_cast<uchar *>(bh.data()) + 20);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(bh.data()) + 24); // IEEE
  f.write(bh);
  for (int i = 0; i < traces; ++i)
  {
    const int l = i / perLine, p = i % perLine;
    QByteArray th(240, 0);
    qToBigEndian<qint32>(i + 1, reinterpret_cast<uchar *>(th.data()) + 0);
    qToBigEndian<qint32>(frecBase + l, reinterpret_cast<uchar *>(th.data()) + 8);
    qToBigEndian<qint32>(cdpBase + p, reinterpret_cast<uchar *>(th.data()) + 20);
    qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(th.data()) + 70); // 坐标比例
    qToBigEndian<qint32>(static_cast<qint32>(qRound64(x0 + p * dx)),
                         reinterpret_cast<uchar *>(th.data()) + 72);
    qToBigEndian<qint32>(static_cast<qint32>(qRound64(y0 + l * dy)),
                         reinterpret_cast<uchar *>(th.data()) + 76);
    qToBigEndian<qint16>(64, reinterpret_cast<uchar *>(th.data()) + 114);
    qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(th.data()) + 116);
    f.write(th);
    f.write(QByteArray(64 * 4, 0));
  }
  return true;
}

void TestDataPreview::seismicDefaultsToTieWellInline()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  QString err;
  QVERIFY(!st->importSvc->importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err)
               .isEmpty());
  const QString topsPath = stage(tmp, QString::fromUtf8("井分层"), QStringLiteral("DC.dat"));
  QVERIFY(!st->importSvc->importProjectFile(topsPath, &err).isEmpty());
  const QString tdPath = stage(tmp, QString::fromUtf8("时深"), QStringLiteral("A1_TD.dat"));
  QVERIFY(!st->importSvc->importProjectFile(tdPath, &err).isEmpty());

  // A1 分层点 (5288.67, 8219.94)：测网 x 5280–5310、y 8210–8230 把它盖住；
  // inline = 1500 + (8219.94-8210)/(8230-8210)*2 ≈ 1501。
  const QString sgy = tmp.filePath(QStringLiteral("sgy1500.sgy"));
  QVERIFY(writeOrdinalSegy(sgy, 12, 4, 1500, 4165, 5280.0, 10.0, 8210.0, 10.0));
  const QString sgyId = st->importSvc->importProjectFile(sgy, &err);
  QVERIFY2(!sgyId.isEmpty(), qPrintable(err));

  st->preview->openAsset(sgyId);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  QVERIFY(no);
  QCOMPARE(no->minimum(), 1500);
  QCOMPARE(no->maximum(), 1502);
  QCOMPARE(no->value(), 1501); // A1 所在 inline
  bool sawCaption = false;
  for (QLabel *l : page->findChildren<QLabel *>())
    if (l->text() == QStringLiteral("A1 所在测线"))
      sawCaption = true;
  QVERIFY2(sawCaption, "seismic tab must caption 「A1 所在测线」");
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("sgy1500.sgy · IL1501"));
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

void TestDataPreview::lasMultiCurveAndZooming()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.las.isEmpty());

  st->preview->openAsset(ids.las);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  QWidget *page = tabs->widget(tabs->currentIndex());
  QVERIFY(page);

  auto *panel = page->findChild<QWidget *>(QStringLiteral("curvePanel"));
  QVERIFY2(panel, "well_log preview must provide curvePanel");

  // Check chips:
  const QList<QToolButton *> buttons = page->findChildren<QToolButton *>();
  QToolButton *btnGR = nullptr;
  QToolButton *btnAC = nullptr;
  QToolButton *btnDEN = nullptr;
  QToolButton *btnBoZuK = nullptr;
  QToolButton *btnZoomIn = nullptr;
  QToolButton *btnZoomOut = nullptr;
  QToolButton *btnZoomReset = nullptr;
  QToolButton *btnAll = nullptr;
  QToolButton *btnDefault = nullptr;

  for (auto *b : buttons)
  {
    if (b->text() == QStringLiteral("GR")) btnGR = b;
    else if (b->text() == QStringLiteral("AC")) btnAC = b;
    else if (b->text() == QStringLiteral("DEN")) btnDEN = b;
    else if (b->text().compare(QStringLiteral("BoZuK"), Qt::CaseInsensitive) == 0) btnBoZuK = b;
    else if (b->text() == QStringLiteral("+")) btnZoomIn = b;
    else if (b->text() == QStringLiteral("−")) btnZoomOut = b;
    else if (b->text() == QStringLiteral("1:1 适应")) btnZoomReset = b;
    else if (b->text() == QStringLiteral("全选")) btnAll = b;
    else if (b->text().contains(QStringLiteral("常规"))) btnDefault = b;
  }

  QVERIFY(btnGR && btnGR->isChecked());
  QVERIFY(btnAC && btnAC->isChecked());
  QVERIFY(btnDEN && btnDEN->isChecked());
  QVERIFY(btnBoZuK && !btnBoZuK->isChecked());

  // Test toggling BoZuK on
  btnBoZuK->click();
  QVERIFY(btnBoZuK->isChecked());

  // Test Zoom In
  QVERIFY(btnZoomIn && btnZoomReset);
  btnZoomIn->click();
  QLabel *lblZoom = nullptr;
  for (auto *lbl : page->findChildren<QLabel *>())
    if (lbl->text().endsWith(QLatin1Char('%')))
      lblZoom = lbl;
  QVERIFY(lblZoom);
  QCOMPARE(lblZoom->text(), QStringLiteral("150%"));

  // Test Reset Zoom
  btnZoomReset->click();
  QCOMPARE(lblZoom->text(), QStringLiteral("100%"));

  // Test All preset
  QVERIFY(btnAll);
  btnAll->click();
  QVERIFY(btnGR->isChecked());
  QVERIFY(btnAC->isChecked());
  QVERIFY(btnDEN->isChecked());
  QVERIFY(btnBoZuK->isChecked());

  // Test Default preset
  QVERIFY(btnDefault);
  btnDefault->click();
  QVERIFY(btnGR->isChecked());
  QVERIFY(btnAC->isChecked());
  QVERIFY(btnDEN->isChecked());
  QVERIFY(!btnBoZuK->isChecked());
}

void TestDataPreview::surveyAreaOpensQgisCanvasAndEmitsShowOnMain()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  auto st = makeStack(dir.path());
  QVERIFY(st);

  st->preview->setProject(st->projectSvc.project());
  QCOMPARE(st->preview->tabCount(), 0);

  st->preview->openSurveyArea();
  QCOMPARE(st->preview->tabCount(), 1);

  // Tab text is 测区全景地图
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  QCOMPARE(tabs->tabText(0), QStringLiteral("测区全景地图"));

  // Check canvas exists
  auto *canvas = st->preview->findChild<QgsMapCanvas *>(QStringLiteral("surveyMapCanvas"));
  QVERIFY(canvas);

  // Check toolbar buttons exist
  auto *btnFull = st->preview->findChild<QToolButton *>(QStringLiteral("btnSurveyFullExtent"));
  auto *btnIn = st->preview->findChild<QToolButton *>(QStringLiteral("btnSurveyZoomIn"));
  auto *btnOut = st->preview->findChild<QToolButton *>(QStringLiteral("btnSurveyZoomOut"));
  auto *btnPan = st->preview->findChild<QToolButton *>(QStringLiteral("btnSurveyPan"));
  auto *btnSwitchMain = st->preview->findChild<QToolButton *>(QStringLiteral("btnSwitchToMainCanvas"));

  QVERIFY(btnFull);
  QVERIFY(btnIn);
  QVERIFY(btnOut);
  QVERIFY(btnPan);
  QVERIFY(btnSwitchMain);

  // Check button clicks work without crashing
  btnFull->click();
  btnIn->click();
  btnOut->click();
  btnPan->click();

  // Check requestShowOnMainCanvas signal
  QSignalSpy spy(st->preview.get(), &DataPreviewTabs::requestShowOnMainCanvas);
  btnSwitchMain->click();
  QCOMPARE(spy.count(), 1);

  // Re-opening focuses the existing tab without creating duplicate tab
  st->preview->openSurveyArea();
  QCOMPARE(st->preview->tabCount(), 1);
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
