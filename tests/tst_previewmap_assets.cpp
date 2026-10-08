#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "helpers/previewfixture.h"
#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/previewrasteranalysis.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/datapreview/previewhistogramwidget.h"
#include "../src/ui/datapreview/previewidentifypanel.h"
#include "../src/ui/datapreview/previewmappage.h"
#include "../src/ui/datapreview/previewprofilepanel.h"
#include "../src/ui/datapreview/previewtocpanel.h"
#include "../src/ui/datapreview/previewmapstates.h"
#include "../src/ui/decorations/paleodecorations.h"

#include <qgscategorizedsymbolrenderer.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsmapcanvas.h>
#include <qgsrastershader.h>
#include <qgsrastershaderfunction.h>
#include <qgsrubberband.h>
#include <qgsrasterlayer.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsvectorlayer.h>
#include <gdal.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QDockWidget>
#include <QMainWindow>
#include <QVBoxLayout>

// P2 资产覆盖集成测试（fixture 驱动，tst_datapreview 同栈同夹具）：
// horizon 地图页（等值线/统计/直方图/版本切换/极值）、geojson（图例/标注/
// TOC 快调）、image（world file 上图/未配准引导）、井位落图（D2.6/D2.8）、
// 测区全景框架化、TOC 状态记忆、全表模式、剖面导出、状态页辅助件。

using paleo::tests::preview::makeStack;
using paleo::tests::preview::fixture;
using paleo::tests::preview::stage;
using paleo::tests::preview::importAll;
using paleo::tests::preview::Imported;

class TestPreviewMapAssets : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

    void horizonPageCarriesFramework();
    void propertiesOwnVersionControls();
    void previewDocksResizeCanvas();
    void horizonContourLayerInToc();
    void horizonStatsPanelShowsValues();
    void horizonHistogramRefreshesOnBins();
    void horizonExtremaToggleDrawsBand();
    void contourIntervalRegeneratesLayer();
    void horizonVersionComboListsVersions();
    void horizonVersionSwitchesToRawAndBack();
    void tocRasterStretchChangesRenderer();
    void tocOpacitySliderChangesLayerOpacity();
    void tocStatePersistsAcrossRebuild();
    void geojsonLegendSidebarEntries();
    void geojsonLabelToggle();
    void geojsonTocCategorizedFieldSwitch();
    void imageUngeoreferencedShowsGuide();
    void imageWithWorldFileShowsMap();
    void wellHeadMapShowsHighlight();
    void topsMapShowsPointsWhenCoords();
    void surveyPageUsesFramework();
    void attributeTableDialogFilters();
    void profilePanelExportsCsvAndPng();
    void rasterAnalysisStretchAndRamps();
    void statesHelpersContract();
    void documentTabNotMapFramework();
    void wellHeadMapLabelToggleWorks();
    void mapProductRasterPagePreview();
    void mapProductVectorPagePreview();

};

// ---------------- horizon（D2.1/2.2/2.3/2.9 + D5.x） ----------------

void TestPreviewMapAssets::horizonPageCarriesFramework()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  QVERIFY(!ids.d61.isEmpty());
  st->preview->openAsset(ids.d61);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  // 统一框架件在场：工具条/侧栏/状态条/分析页签。
  QVERIFY(page->findChild<QToolBar *>(QStringLiteral("previewMapToolBar")));
  QVERIFY(page->findChild<QTabWidget *>(QStringLiteral("previewSideTabs")));
  QVERIFY(page->findChild<QWidget *>(QStringLiteral("previewMapStatusBar")));
  auto *analysis = page->findChild<QTabWidget *>(QStringLiteral("previewAnalysisTabs"));
  QVERIFY(analysis);
  QCOMPARE(analysis->count(), 4); // 统计/直方图/等值线/极值
  auto *renderLabel = page->findChild<QLabel *>(QStringLiteral("previewRenderLabel"));
  QVERIFY(renderLabel);
}

void TestPreviewMapAssets::propertiesOwnVersionControls()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath("proj"));
  QVERIFY(st);
  QWidget details;
  new QVBoxLayout(&details);
  st->preview->setDetailsHost(&details);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  QVERIFY(!st->preview->findChild<QComboBox *>("previewVersionCombo"));
  QVERIFY(!details.isHidden());
  auto *combo = details.findChild<QComboBox *>("previewVersionCombo");
  auto *mapButton = details.findChild<QPushButton *>("showOnMapBtn");
  QVERIFY(combo && mapButton);
  st->preview->setHorizonOnMap(mapButton->property("layerId").toString(), true);
  QVERIFY(mapButton->property("onMap").toBool());
  const QString derived = combo->currentData().toString();
  combo->setCurrentIndex(combo->count() - 1); // RAW version is last.
  QTest::qWait(50);
  QVERIFY(!st->preview->findChild<QgsMapCanvas *>("horizonMapCanvas"));
  combo = details.findChild<QComboBox *>("previewVersionCombo");
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findData(derived));
  QTest::qWait(50);
  QVERIFY(st->preview->findChild<QgsMapCanvas *>("horizonMapCanvas"));
  st->preview->openAsset(ids.las);
  QVERIFY(details.isHidden());
  st->preview->openAsset(ids.d61);
  QVERIFY(!details.isHidden());
  st->preview->closeAssetTab(ids.d61);
  QVERIFY(details.isHidden());
}

void TestPreviewMapAssets::previewDocksResizeCanvas()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath("proj"));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  st->preview->resize(1400, 1000);
  st->preview->show();
  QTest::qWait(100);
  auto *workspace = st->preview->findChild<QMainWindow *>("previewDockWorkspace");
  auto *side = st->preview->findChild<QDockWidget *>("previewSideDock");
  auto *bottom = st->preview->findChild<QDockWidget *>("previewAnalysisDock");
  auto *canvas = st->preview->findChild<QgsMapCanvas *>("horizonMapCanvas");
  QVERIFY(workspace && side && bottom && canvas);
  workspace->resizeDocks({side}, {240}, Qt::Horizontal);
  workspace->resizeDocks({bottom}, {160}, Qt::Vertical);
  QTest::qWait(30);
  const QSize before = canvas->size();
  const QSize outer = st->preview->size();
  workspace->resizeDocks({side}, {520}, Qt::Horizontal);
  workspace->resizeDocks({bottom}, {320}, Qt::Vertical);
  QTest::qWait(30);
  QCOMPARE(st->preview->size(), outer);
  QVERIFY(side->width() >= 510);
  QVERIFY(bottom->height() >= 310);
  QVERIFY(canvas->width() < before.width() - 200);
  QVERIFY(canvas->height() < before.height() - 100);
  side->setFloating(true);
  side->show();
  st->preview->openAsset(ids.las);
  QVERIFY(side->isHidden());
  st->preview->openAsset(ids.d61);
  QVERIFY(!side->isHidden());
}

void TestPreviewMapAssets::horizonContourLayerInToc()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  auto *toc = page->findChild<PreviewTocPanel *>();
  QVERIFY(toc);
  QCOMPARE(toc->layerCount(), 2); // 层位栅格 + 等值线（D2.1）
  bool sawContour = false;
  for (QgsMapLayer *l : toc->layersTopToBottom())
    if (l->name() == QStringLiteral("等值线"))
      sawContour = true;
  QVERIFY(sawContour);
}

void TestPreviewMapAssets::horizonStatsPanelShowsValues()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  QVERIFY(page);
  const char *names[] = {"horizonStatMin", "horizonStatMax", "horizonStatMean",
                         "horizonStatStd", "horizonStatValid"};
  for (const char *n : names)
  {
    auto *lbl = page->findChild<QLabel *>(QLatin1String(n));
    QVERIFY2(lbl, n);
    QVERIFY2(!lbl->text().isEmpty(), n); // D5.5 有值
  }
}

void TestPreviewMapAssets::horizonHistogramRefreshesOnBins()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  auto *hist = page->findChild<PreviewHistogramWidget *>(QStringLiteral("horizonHistogram"));
  QVERIFY(hist);
  QVERIFY(hist->histogram().valid); // D2.3 小图有数据
  // 分箱可调（D5.8）：改分箱仍产出有效直方图。
  auto *spin = page->findChild<QSpinBox *>(QStringLiteral("histBinsSpin"));
  QVERIFY(spin);
  spin->setValue(128);
  QVERIFY(hist->histogram().valid);
  QCOMPARE(hist->bins(), 128);
}

void TestPreviewMapAssets::horizonExtremaToggleDrawsBand()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  auto *peak = page->findChild<QCheckBox *>(QStringLiteral("extremaPeakCheck"));
  auto *low = page->findChild<QCheckBox *>(QStringLiteral("extremaLowCheck"));
  QVERIFY(peak && low);
  QVERIFY(!peak->isChecked());
  auto *table = page->findChild<QTableWidget *>(QStringLiteral("extremaTable"));
  QVERIFY(table);
  QCOMPARE(table->rowCount(), 0);
  peak->setChecked(true); // D5.6 开关
  low->setChecked(true);
  QVERIFY(table->rowCount() > 0);
  QVERIFY(page->findChild<QgsRubberBand *>(QStringLiteral("horizonPeaksBand")));
  QVERIFY(page->findChild<QgsRubberBand *>(QStringLiteral("horizonLowsBand")));
}

void TestPreviewMapAssets::contourIntervalRegeneratesLayer()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  auto *toc = page->findChild<PreviewTocPanel *>();
  auto *interval = page->findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"));
  QVERIFY(interval);
  interval->setValue(5.0); // D5.7 间距可调 → 重生成
  QCOMPARE(toc->layerCount(), 2); // 旧层出树、新层进树
  bool sawContour = false;
  for (QgsMapLayer *l : toc->layersTopToBottom())
    if (l->name() == QStringLiteral("等值线"))
      sawContour = true;
  QVERIFY(sawContour);
}

void TestPreviewMapAssets::horizonVersionComboListsVersions()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *tabPage = tabs->widget(tabs->currentIndex());
  QVERIFY(tabPage->findChild<QWidget *>(QStringLiteral("horizonPreviewPage")));
  // verBar 与 PreviewMapPage 是兄弟节点——从 tab 页根找（D2.9 版本下拉）。
  auto *combo = tabPage->findChild<QComboBox *>(QStringLiteral("previewVersionCombo"));
  QVERIFY(combo);
  QVERIFY(combo->count() >= 2);
  bool sawRaw = false;
  bool sawDerived = false;
  for (int i = 0; i < combo->count(); ++i)
  {
    if (combo->itemText(i).contains(QStringLiteral("原始散点")))
      sawRaw = true;
    if (combo->itemText(i).contains(QStringLiteral("派生栅格")))
      sawDerived = true;
  }
  QVERIFY(sawRaw);
  QVERIFY(sawDerived);
}

void TestPreviewMapAssets::horizonVersionSwitchesToRawAndBack()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs->widget(tabs->currentIndex())
              ->findChild<QgsMapCanvas *>(QStringLiteral("horizonMapCanvas")));
  auto *combo = st->preview->findChild<QComboBox *>(QStringLiteral("previewVersionCombo"));
  QVERIFY(combo);
  const int rawIdx = [combo]() {
    for (int i = 0; i < combo->count(); ++i)
      if (combo->itemText(i).contains(QStringLiteral("原始散点")))
        return i;
    return -1;
  }();
  QVERIFY(rawIdx >= 0);
  combo->setCurrentIndex(rawIdx); // 切 RAW → 散点信息卡（无地图）
  QTest::qWait(50);               // rebuildAssetTab 完成（旧页 deleteLater 已清）
  QWidget *rawPage = tabs->widget(tabs->currentIndex());
  QVERIFY(!rawPage->findChild<QgsMapCanvas *>(QStringLiteral("horizonMapCanvas")));
  QVERIFY(rawPage->findChild<QLabel *>(QStringLiteral("stateText")));
  auto *comboRaw =
      rawPage->findChild<QComboBox *>(QStringLiteral("previewVersionCombo"));
  QVERIFY(comboRaw); // RAW 页自己的版本下拉（旧 combo 已随重建销毁）
  const int derivedIdx = [comboRaw]() {
    for (int i = 0; i < comboRaw->count(); ++i)
      if (comboRaw->itemText(i).contains(QStringLiteral("派生栅格")))
        return i;
    return -1;
  }();
  QVERIFY(derivedIdx >= 0);
  comboRaw->setCurrentIndex(derivedIdx); // 切回 DERIVED → 画布回归
  QTest::qWait(50);
  QVERIFY(tabs->widget(tabs->currentIndex())
              ->findChild<QgsMapCanvas *>(QStringLiteral("horizonMapCanvas")));
}

void TestPreviewMapAssets::tocRasterStretchChangesRenderer()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  auto *toc = page->findChild<PreviewTocPanel *>();
  auto *raster = qobject_cast<QgsRasterLayer *>(toc->layersTopToBottom().value(1));
  QVERIFY(raster);
  auto *stretch = page->findChild<QComboBox *>(QStringLiteral("tocStretchCombo"));
  QVERIFY(stretch);
  double loBefore = 0.0;
  double hiBefore = 0.0;
  if (auto *r = dynamic_cast<QgsSingleBandPseudoColorRenderer *>(raster->renderer()))
    if (auto *fn = r->shader()->rasterShaderFunction())
    {
      loBefore = fn->minimumValue();
      hiBefore = fn->maximumValue();
    }
  // 切 2%–98% 拉伸 → 渲染界收窄（D2.2/D4.3）。
  const int idx2to98 = [stretch]() {
    for (int i = 0; i < stretch->count(); ++i)
      if (stretch->itemText(i).contains(QStringLiteral("2%")))
        return i;
    return -1;
  }();
  QVERIFY(idx2to98 >= 0);
  stretch->setCurrentIndex(idx2to98);
  double loAfter = 0.0;
  double hiAfter = 0.0;
  if (auto *r = dynamic_cast<QgsSingleBandPseudoColorRenderer *>(raster->renderer()))
    if (auto *fn = r->shader()->rasterShaderFunction())
    {
      loAfter = fn->minimumValue();
      hiAfter = fn->maximumValue();
    }
  QVERIFY(loAfter >= loBefore - 1e-9);
  QVERIFY(hiAfter <= hiBefore + 1e-9);
  QVERIFY(hiAfter > loAfter);
}

void TestPreviewMapAssets::tocOpacitySliderChangesLayerOpacity()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  auto *toc = page->findChild<PreviewTocPanel *>();
  auto *raster = qobject_cast<QgsRasterLayer *>(toc->layersTopToBottom().value(1));
  QVERIFY(raster);
  auto *slider = page->findChild<QSlider *>(QStringLiteral("tocOpacitySlider"));
  QVERIFY(slider);
  auto *list = page->findChild<QListWidget *>(QStringLiteral("previewTocList"));
  QVERIFY(list);
  list->setCurrentRow(1); // 选中层位栅格行（row 0 是等值线）——透明度作用于当前层
  QCOMPARE(raster->opacity(), 1.0);
  slider->setValue(50); // D4.2
  QCOMPARE(raster->opacity(), 0.5);
}

void TestPreviewMapAssets::tocStatePersistsAcrossRebuild()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.d61);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  auto *toc = page->findChild<PreviewTocPanel *>();
  auto *list = page->findChild<QListWidget *>(QStringLiteral("previewTocList"));
  QVERIFY(list);
  // 取消勾选「等值线」（index 0 = 顶 = 等值线）。
  const QString contourName = list->item(0)->text();
  list->item(0)->setCheckState(Qt::Unchecked);
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("horizonMapCanvas"));
  QVERIFY(canvas);
  QTest::qWait(50);
  // 重建（换版本再回来）→ TOC 状态记忆恢复（D4.7）。
  st->preview->openAsset(ids.d61);
  auto *combo = st->preview->findChild<QComboBox *>(QStringLiteral("previewVersionCombo"));
  QVERIFY(combo);
  const int rawIdx = [combo]() {
    for (int i = 0; i < combo->count(); ++i)
      if (combo->itemText(i).contains(QStringLiteral("原始散点")))
        return i;
    return -1;
  }();
  combo->setCurrentIndex(rawIdx);
  QTest::qWait(50);
  auto *comboBack =
      st->preview->findChild<QComboBox *>(QStringLiteral("previewVersionCombo"));
  const int derivedIdx = [comboBack]() {
    for (int i = 0; i < comboBack->count(); ++i)
      if (comboBack->itemText(i).contains(QStringLiteral("派生栅格")))
        return i;
    return -1;
  }();
  comboBack->setCurrentIndex(derivedIdx);
  QTest::qWait(50);
  auto *page2 = st->preview->findChild<QWidget *>(QStringLiteral("horizonPreviewPage"));
  auto *list2 = page2->findChild<QListWidget *>(QStringLiteral("previewTocList"));
  QVERIFY(list2);
  bool foundUnChecked = false;
  for (int i = 0; i < list2->count(); ++i)
    if (list2->item(i)->text() == contourName &&
        list2->item(i)->checkState() == Qt::Unchecked)
      foundUnChecked = true;
  QVERIFY(foundUnChecked);
}

// ---------------- geojson（D2.4/D2.6/D4.4） ----------------

void TestPreviewMapAssets::geojsonLegendSidebarEntries()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.geojson);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("faciesPreviewPage"));
  QVERIFY(page);
  auto *toc = page->findChild<PreviewTocPanel *>();
  QVERIFY(toc);
  QVERIFY(toc->legendEntryCount() > 0); // D2.4 图例侧栏有分类条目
  QVERIFY(page->findChild<QWidget *>(QStringLiteral("previewLegendBox")));
}

void TestPreviewMapAssets::geojsonLabelToggle()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.geojson);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("faciesPreviewPage"));
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("faciesMapCanvas"));
  QVERIFY(canvas);
  auto *vlayer = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vlayer);
  QVERIFY(vlayer->labelsEnabled());
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  auto *toggle =
      tabs->widget(tabs->currentIndex())->findChild<QToolButton *>(QStringLiteral("btnToggleLabels"));
  QVERIFY(toggle); // D2.6 名称标注开关（geojson 分支工具条，与地图页兄弟）
  QVERIFY(toggle->isChecked());
  toggle->click();
  QVERIFY(!vlayer->labelsEnabled());
  toggle->click();
  QVERIFY(vlayer->labelsEnabled());
}

void TestPreviewMapAssets::geojsonTocCategorizedFieldSwitch()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.geojson);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("faciesPreviewPage"));
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("faciesMapCanvas"));
  auto *vlayer = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vlayer);
  auto *categorized =
      page->findChild<QCheckBox *>(QStringLiteral("tocCategorizedCheck"));
  auto *field = page->findChild<QComboBox *>(QStringLiteral("tocFieldCombo"));
  QVERIFY(categorized && field);
  QCOMPARE(field->count(), vlayer->fields().size());
  categorized->setChecked(true); // D4.4 分类切换（字段变化重分类）
  QVERIFY(dynamic_cast<QgsCategorizedSymbolRenderer *>(vlayer->renderer()) != nullptr);
  if (field->count() > 1)
    field->setCurrentIndex(1);
  QVERIFY(dynamic_cast<QgsCategorizedSymbolRenderer *>(vlayer->renderer()) != nullptr);
  categorized->setChecked(false); // 单色模式
  QVERIFY(dynamic_cast<QgsSingleSymbolRenderer *>(vlayer->renderer()) != nullptr);
}

// ---------------- image（D2.7/D2.11） ----------------

void TestPreviewMapAssets::imageUngeoreferencedShowsGuide()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.png);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *page = tabs->widget(tabs->currentIndex());
  // 未配准：图片查看器 + 引导按钮，不上图。
  QVERIFY(!page->findChild<QgsMapCanvas *>(QStringLiteral("imageMapCanvas")));
  QVERIFY(page->findChild<QPushButton *>(QStringLiteral("goRegisterGuideBtn")));
}

void TestPreviewMapAssets::imageWithWorldFileShowsMap()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  // 造带 world file 的图片夹具（6 参数：像素 10m、原点 (100,200)）。
  const QString dir = tmp.filePath(QStringLiteral("georef"));
  QDir().mkpath(dir);
  QFile::copy(fixture(QStringLiteral("tiny.png")), QDir(dir).filePath(QStringLiteral("tiny.png")));
  QFile wld(QDir(dir).filePath(QStringLiteral("tiny.wld")));
  QVERIFY(wld.open(QIODevice::WriteOnly | QIODevice::Text));
  wld.write("10.0\n0.0\n0.0\n-10.0\n100.0\n200.0\n");
  wld.close();
  QString err;
  const QString assetId = st->importSvc->importProjectFile(
      QDir(dir).filePath(QStringLiteral("tiny.png")), &err);
  QVERIFY2(!assetId.isEmpty(), qPrintable(err));
  st->preview->openAsset(assetId);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("imagePreviewPage"));
  QVERIFY(page); // D2.7：配准 → 栅格上图
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("imageMapCanvas"));
  QVERIFY(canvas);
  QCOMPARE(canvas->layers().size(), 1);
  auto *raster = qobject_cast<QgsRasterLayer *>(canvas->layers().first());
  QVERIFY(raster);
  // world file 生效：8×4 像素 ×10m = 80×40。
  QVERIFY(qAbs(raster->extent().width() - 80.0) < 0.01);
  QVERIFY(qAbs(raster->extent().height() - 40.0) < 0.01);
}

// ---------------- 井位落图（D2.6/D2.8） ----------------

void TestPreviewMapAssets::wellHeadMapShowsHighlight()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.wellHead);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  auto *combo = tabs->widget(tabs->currentIndex())->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A1")));
  QTest::qWait(50);
  auto *canvas =
      st->preview->findChild<QgsMapCanvas *>(QStringLiteral("wellHeadMapCanvas"));
  QVERIFY(canvas); // D2.6 井位图
  auto *vl = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vl);
  QVERIFY(vl->featureCount() >= 1);
  // 高亮井存在（role=highlight）。
  int highlightCount = 0;
  QgsFeatureIterator it = vl->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f))
    if (f.attribute(QStringLiteral("role")).toString() == QStringLiteral("highlight"))
      ++highlightCount;
  QCOMPARE(highlightCount, 1);
  auto *labelToggle =
      st->preview->findChild<QCheckBox *>(QStringLiteral("wellLabelToggle"));
  QVERIFY(labelToggle); // 名称标注开关（D2.6）
}

void TestPreviewMapAssets::topsMapShowsPointsWhenCoords()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.tops);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  auto *combo = tabs->widget(tabs->currentIndex())->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A1")));
  QTest::qWait(50);
  auto *canvas = st->preview->findChild<QgsMapCanvas *>(QStringLiteral("topsMapCanvas"));
  QVERIFY(canvas); // D2.8：DC.dat 行带 X/Y → 分层顶点落图
  auto *vl = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vl);
  QVERIFY(vl->featureCount() > 0);
}

// ---------------- survey（框架化） ----------------

void TestPreviewMapAssets::surveyPageUsesFramework()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  st->preview->setProject(st->projectSvc.project());
  st->preview->openSurveyArea();
  // 框架件在场 + 既有 objectName 兼容面全保。
  QVERIFY(st->preview->findChild<QToolBar *>(QStringLiteral("previewMapToolBar")));
  QVERIFY(st->preview->findChild<QgsMapCanvas *>(QStringLiteral("surveyMapCanvas")));
  QVERIFY(st->preview->findChild<QWidget *>(QStringLiteral("previewMapStatusBar")));
  auto *decorMgr =
      st->preview->findChild<PaleoDecorationManager *>(QStringLiteral("surveyAreaDecorManager"));
  QVERIFY(decorMgr);
  QVERIFY(st->preview->findChild<QLabel *>(QStringLiteral("surveyAreaExtentLabel")));
  QVERIFY(st->preview->findChild<QgsRubberBand *>(QStringLiteral("surveyAreaRubberBand")));
  for (const char *n : {"btnSurveyFullExtent", "btnSurveyZoomIn", "btnSurveyZoomOut",
                        "btnSurveyPan", "btnToggleSurveyBoundary", "btnToggleScaleBar",
                        "btnToggleNorthArrow", "btnToggleGrid", "btnSwitchToMainCanvas"})
    QVERIFY2(st->preview->findChild<QToolButton *>(QString::fromUtf8(n)), n);
}

// ---------------- 全表/剖面/分析件（D7.3/D5.3/D5.8） ----------------

void TestPreviewMapAssets::attributeTableDialogFilters()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.geojson);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("faciesPreviewPage"));
  auto *canvas = page->findChild<QgsMapCanvas *>(QStringLiteral("faciesMapCanvas"));
  auto *vlayer = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vlayer);
  const int total = vlayer->featureCount();
  QVERIFY(total > 0);
  PreviewAttributeTableDialog dlg(vlayer);
  auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("attrFullTable"));
  QVERIFY(table);
  QVERIFY(table->rowCount() > 0); // 首页数据
  QVERIFY(dlg.pageCount() >= 1);
  // 过滤（D7.3）：相字段含文本过滤。
  auto *filterEdit = dlg.findChild<QLineEdit *>(QStringLiteral("attrFilterEdit"));
  auto *filterApply = dlg.findChild<QPushButton *>(QStringLiteral("attrFilterApply"));
  QVERIFY(filterEdit && filterApply);
  // 全列过滤一个必不存在的串 → 0 行。
  filterEdit->setText(QStringLiteral("__no_such_value__"));
  filterApply->click();
  QCOMPARE(table->rowCount(), 0);
  filterEdit->setText(QString());
  filterApply->click();
  QVERIFY(table->rowCount() > 0);
}

void TestPreviewMapAssets::profilePanelExportsCsvAndPng()
{
  QTemporaryDir tmp;
  PreviewProfilePanel panel;
  QVector<PreviewProfilePanel::Sample> samples;
  for (int i = 0; i < 10; ++i)
    samples.append({double(i * 10), double(i) * 2.5, true});
  panel.addProfile(QStringLiteral("p1"), samples, QgsPointXY(0, 0), QgsPointXY(100, 100));
  QCOMPARE(panel.profileCount(), 1);
  const QString csvPath = tmp.filePath(QStringLiteral("p.csv"));
  const QString pngPath = tmp.filePath(QStringLiteral("p.png"));
  panel.setExportTargetForTesting(csvPath, pngPath);
  auto *csvBtn = panel.findChild<QToolButton *>(QStringLiteral("profileCsvBtn"));
  auto *pngBtn = panel.findChild<QToolButton *>(QStringLiteral("profilePngBtn"));
  QVERIFY(csvBtn && pngBtn);
  csvBtn->click(); // D5.3 导出 CSV（测试注入路径不弹窗）
  pngBtn->click(); // D5.3 导出 PNG
  QFile csv(csvPath);
  QVERIFY(csv.open(QIODevice::ReadOnly));
  const QString content = QString::fromUtf8(csv.readAll());
  QVERIFY(content.startsWith(QStringLiteral("series,distance,value")));
  QVERIFY(content.count(QLatin1Char('\n')) >= 11);
  QVERIFY(QFile::exists(pngPath) && QFileInfo(pngPath).size() > 0);
  // 多剖面叠绘（D5.4）。
  panel.addProfile(QStringLiteral("p2"), samples, QgsPointXY(5, 5), QgsPointXY(105, 105));
  QCOMPARE(panel.profileCount(), 2);
  panel.clearProfiles();
  QCOMPARE(panel.profileCount(), 0);
}

void TestPreviewMapAssets::rasterAnalysisStretchAndRamps()
{
  // 直方图 2%–98%：两端各 10% 的低计数 bin 应被截掉。
  PreviewRasterAnalysis::Histogram h;
  h.valid = true;
  h.bins = 10;
  h.lo = 0.0;
  h.hi = 100.0;
  h.counts = {1, 0, 50, 60, 55, 58, 62, 59, 0, 1};
  const auto bounds = PreviewRasterAnalysis::stretchBounds(
      h, PreviewRasterAnalysis::Stretch::Percent2To98);
  QVERIFY(bounds.first >= 20.0 - 1e-9); // 第一个尖峰 bin 起头被截
  QVERIFY(bounds.second <= 80.0 + 1e-9);
  // 手动值域透传。
  const auto manual = PreviewRasterAnalysis::stretchBounds(
      h, PreviewRasterAnalysis::Stretch::Manual, -5.0, 5.0);
  QCOMPARE(manual.first, -5.0);
  QCOMPARE(manual.second, 5.0);
  // 色带预设 ≥4 档（D2.2）。
  QVERIFY(PreviewRasterAnalysis::rampPresets().size() >= 4);
  QVERIFY(PreviewRasterAnalysis::rampPreset(QStringLiteral("depthBlues")) != nullptr);
  QVERIFY(PreviewRasterAnalysis::rampPreset(QStringLiteral("bipolar"))->bipolar);
  QVERIFY(PreviewRasterAnalysis::rampPreset(QStringLiteral("no-such")) == nullptr);
}

void TestPreviewMapAssets::statesHelpersContract()
{
  // D2.7 world file 探测。
  QTemporaryDir tmp;
  const QString dir = tmp.path();
  const QString img = QDir(dir).filePath(QStringLiteral("a.png"));
  QFile(img).open(QIODevice::WriteOnly);
  QCOMPARE(PreviewMapStates::detectWorldFile(img), QString());
  QFile wld(QDir(dir).filePath(QStringLiteral("a.wld")));
  wld.open(QIODevice::WriteOnly);
  wld.write("1\n");
  wld.close();
  QCOMPARE(PreviewMapStates::detectWorldFile(img),
           QDir(dir).filePath(QStringLiteral("a.wld")));

  // D2.12 不支持页：类型名 + 可支持清单。
  QWidget host;
  auto *page = PreviewMapStates::buildUnsupportedPage(QStringLiteral("mystery"), &host);
  QCOMPARE(page->objectName(), QStringLiteral("previewUnsupportedPage"));
  QVERIFY(page->findChild<QLabel *>(QStringLiteral("stateText"))->text()
              .contains(QStringLiteral("mystery")));

  // D2.11 大图提示条。
  auto *hintBar = PreviewMapStates::buildBigRasterHintBar(QStringLiteral("大图提示"), &host);
  QVERIFY(hintBar->objectName() == QStringLiteral("previewBigRasterHint"));

  // 会话记忆：书签增删（D3.7）。
  PreviewStateMemory::clearAll();
  PreviewStateMemory::Bookmark bm;
  bm.name = QStringLiteral("v1");
  bm.extent = QgsRectangle(0, 0, 10, 10);
  PreviewStateMemory::addBookmark(QStringLiteral("k"), bm);
  QCOMPARE(PreviewStateMemory::bookmarks(QStringLiteral("k")).size(), 1);
  PreviewStateMemory::addBookmark(QStringLiteral("k"), bm); // 同名覆盖不翻倍
  QCOMPARE(PreviewStateMemory::bookmarks(QStringLiteral("k")).size(), 1);
  QVERIFY(PreviewStateMemory::removeBookmark(QStringLiteral("k"), QStringLiteral("v1")));
  QVERIFY(PreviewStateMemory::bookmarks(QStringLiteral("k")).isEmpty());
}

void TestPreviewMapAssets::documentTabNotMapFramework()
{
  // 回归保护：非地图资产（document）不挂地图框架件。
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.pdf);
  auto *page = st->preview->findChild<QWidget *>(QStringLiteral("pdfView"));
  QVERIFY(page);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QWidget *tabPage = tabs->widget(tabs->currentIndex());
  QVERIFY(!tabPage->findChild<QToolBar *>(QStringLiteral("previewMapToolBar")));
  QVERIFY(!tabPage->findChild<QTabWidget *>(QStringLiteral("previewSideTabs")));
}

void TestPreviewMapAssets::wellHeadMapLabelToggleWorks()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);
  const Imported ids = importAll(*st, tmp);
  st->preview->openAsset(ids.wellHead);
  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  auto *combo =
      tabs->widget(tabs->currentIndex())->findChild<QComboBox *>(QStringLiteral("wellCombo"));
  QVERIFY(combo);
  combo->setCurrentIndex(combo->findText(QStringLiteral("A1")));
  QTest::qWait(50);
  auto *canvas =
      st->preview->findChild<QgsMapCanvas *>(QStringLiteral("wellHeadMapCanvas"));
  QVERIFY(canvas);
  auto *vl = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vl->labelsEnabled()); // 默认开
  auto *toggle =
      st->preview->findChild<QCheckBox *>(QStringLiteral("wellLabelToggle"));
  QVERIFY(toggle->isChecked());
  toggle->setChecked(false); // 名称标注开关生效
  QVERIFY(!vl->labelsEnabled());
  toggle->setChecked(true);
  QVERIFY(vl->labelsEnabled());
}

// ---------------- 成果图件（成果图件预览） ----------------

void TestPreviewMapAssets::mapProductRasterPagePreview()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);

  // 构造 4x4 测试 GeoTIFF 成果
  const QString tifPath = tmp.filePath(QStringLiteral("seismic_pred.tif"));
  GDALDriverH drv = GDALGetDriverByName("GTiff");
  QVERIFY(drv != nullptr);
  GDALDatasetH ds = GDALCreate(drv, tifPath.toUtf8().constData(), 4, 4, 1, GDT_Float64, nullptr);
  QVERIFY(ds != nullptr);
  double geo[6] = {0.0, 100.0, 0.0, 400.0, 0.0, -100.0};
  GDALSetGeoTransform(ds, geo);
  double vals[16] = {
    1.0, 2.0, 3.0, 4.0,
    2.0, 3.0, 4.0, 5.0,
    3.0, 4.0, 5.0, 6.0,
    4.0, 5.0, 6.0, 7.0
  };
  (void)GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, 4, 4, vals, 4, 4, GDT_Float64, 0, 0);
  GDALClose(ds);

  CatalogAsset a;
  a.id = QStringLiteral("asset-seis-pred");
  a.type = QStringLiteral("seismic_prediction");
  a.displayName = QStringLiteral("D61-seismic_prediction");
  CatalogVersion v;
  v.id = QStringLiteral("v-seis-pred");
  v.assetId = a.id;
  v.path = tifPath;
  v.managed = false;
  v.extra.insert(QStringLiteral("mapping_product"), true);
  v.extra.insert(QStringLiteral("kind"), QStringLiteral("seismic_prediction"));
  v.extra.insert(QStringLiteral("layer_type"), QStringLiteral("raster"));
  v.extra.insert(QStringLiteral("title"), QStringLiteral("D61 地震预测"));

  st->importSvc->catalog()->addAsset(a);
  st->importSvc->catalog()->addVersion(v);

  st->preview->openAsset(a.id);

  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  QWidget *page = tabs->widget(tabs->currentIndex());
  QVERIFY(page);
  // 成果图件预览页面挂载成功
  auto *mapPage = page->findChild<QWidget *>(QStringLiteral("mapProductPreviewPage"));
  QVERIFY(mapPage);
  auto *canvas = mapPage->findChild<QgsMapCanvas *>(QStringLiteral("mapProductCanvas"));
  QVERIFY(canvas);
  QCOMPARE(canvas->layers().size(), 1);
  auto *rl = qobject_cast<QgsRasterLayer *>(canvas->layers().first());
  QVERIFY(rl);
  QVERIFY(rl->isValid());
  QCOMPARE(rl->width(), 4);
  QCOMPARE(rl->height(), 4);

  // 分析面板存在且有统计与直方图
  auto *analysis = mapPage->findChild<QTabWidget *>(QStringLiteral("previewAnalysisTabs"));
  QVERIFY(analysis);
  QVERIFY(analysis->count() >= 2);

  // 不存在“未配准”或“不支持预览”错误标签
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(!state || (!state->text().contains(QStringLiteral("不支持预览")) &&
                     !state->text().contains(QStringLiteral("未配准"))));
}

void TestPreviewMapAssets::mapProductVectorPagePreview()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st);

  // 使用既有 facies.geojson 作为矢量成果
  const QString vecPath = stage(tmp, QStringLiteral("artifacts"), QStringLiteral("facies.geojson"));
  QVERIFY(!vecPath.isEmpty());

  CatalogAsset a;
  a.id = QStringLiteral("asset-facies-poly");
  a.type = QStringLiteral("facies_polygons");
  a.displayName = QStringLiteral("D61-facies_polygons");
  CatalogVersion v;
  v.id = QStringLiteral("v-facies-poly");
  v.assetId = a.id;
  v.path = vecPath;
  v.managed = false;
  v.extra.insert(QStringLiteral("mapping_product"), true);
  v.extra.insert(QStringLiteral("kind"), QStringLiteral("facies_polygons"));
  v.extra.insert(QStringLiteral("layer_type"), QStringLiteral("vector"));
  v.extra.insert(QStringLiteral("title"), QStringLiteral("D61 相多边形"));

  st->importSvc->catalog()->addAsset(a);
  st->importSvc->catalog()->addVersion(v);

  st->preview->openAsset(a.id);

  auto *tabs = st->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  QVERIFY(tabs);
  QWidget *page = tabs->widget(tabs->currentIndex());
  QVERIFY(page);
  auto *mapPage = page->findChild<QWidget *>(QStringLiteral("mapProductPreviewPage"));
  QVERIFY(mapPage);
  auto *canvas = mapPage->findChild<QgsMapCanvas *>(QStringLiteral("mapProductCanvas"));
  QVERIFY(canvas);
  QCOMPARE(canvas->layers().size(), 1);
  auto *vl = qobject_cast<QgsVectorLayer *>(canvas->layers().first());
  QVERIFY(vl);
  QVERIFY(vl->isValid());
  QVERIFY(vl->featureCount() > 0);

  // 标注开关和属性表动作存在
  auto *actLabels = page->findChild<QAction *>(QStringLiteral("previewAction_toggleLabels"));
  auto *actAttrs = page->findChild<QAction *>(QStringLiteral("previewAction_openAttrTable"));
  QVERIFY(actLabels);
  QVERIFY(actAttrs);

  // 测试标注开关功能
  const bool initialLabel = vl->labelsEnabled();
  QVERIFY(initialLabel);
  actLabels->setChecked(!initialLabel);
  QCOMPARE(vl->labelsEnabled(), !initialLabel);
  actLabels->setChecked(initialLabel);
  QCOMPARE(vl->labelsEnabled(), initialLabel);

  // 不存在“未配准”或“不支持预览”
  auto *state = page->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(!state || (!state->text().contains(QStringLiteral("不支持预览")) &&
                     !state->text().contains(QStringLiteral("未配准"))));
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestPreviewMapAssets tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_previewmap_assets.moc"
