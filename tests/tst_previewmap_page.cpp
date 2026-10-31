#include <QtTest>
#include <QSignalSpy>

#include "../src/ui/datapreview/previewmappage.h"
#include "../src/ui/datapreview/previewidentifypanel.h"
#include "../src/ui/datapreview/previewmapstates.h"
#include "../src/ui/paleotheme.h"
#include "../src/catalog/datacatalog.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/previewrendercache.h"
#include "uipolish_capture.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QTabWidget>
#include <QToolBar>

#include <qgscoordinatereferencesystem.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptool.h>
#include <qgsvectorlayer.h>

#include <memory>

// P2 组合页契约：工具条（D1.3 显隐/互斥）、状态条（D1.6/D3.6）、书签
//（D3.7）、鹰眼（D3.9）、复制（D3.8）、错误态（D1.7）、identify 面板路由
//（D7）、量测读数、剖面侧栏路由（D5）、分析页签挂载、渲染缓存 identity
//（D6.2）。
class TestPreviewMapPage : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }
    void init()
    {
      PreviewStateMemory::clearAll();
      m_page = std::make_unique<PreviewMapPage>();
      m_page->resize(800, 600);
    }

    // Destroy QGIS widgets before QgisRuntime shuts down QApplication.
    void cleanup() { m_page.reset(); }

    void toolBarButtonsExist();
    void toolActionActivationIsExclusive();
    void toolVisibleHidesActionAndFallsBackToPan();
    void panIsActiveByDefault();
    void errorStateShowsPageAndClears();
    void renderLabelReflectsCompletion();
    void scaleLabelFormatted();
    void coordinateLabelTracksMouse();
    void bookmarkAddJumpRemove();
    void overviewVisibleToggle();
    void overviewScalesWithCanvas(); // #848
    void decorationToggleByName();
    void copyCoordinateToClipboard();
    void copyScreenshotToClipboard();
    void measureReadoutLandsInStatusBar();
    void identifyRoutesToPanel();
    void profileLineSwitchesSideTab();
    void analysisTabsAppearOnDemand();
    // goal/ui-experience-polish：错误页/状态条 token 化的修前/修后证据。
    void captureEvidence()
    {
      m_page->setError(QStringLiteral("数据源损坏"), QStringLiteral("/x/y.tif"));
      QLabel *state = nullptr;
      for (QLabel *l : m_page->findChildren<QLabel *>(QStringLiteral("stateText")))
        if (l->text() == QStringLiteral("数据源损坏"))
          state = l;
      QVERIFY2(state, "错误页应显示传入的标题");
      QVERIFY(uipolish::capturePanel(m_page.get(), QStringLiteral("previewmap_error"),
                                     QSize(800, 600)));
      m_page->clearError();
    }
    void renderCacheStoresOnCompletion();
    void lowResSnapshotShowsOverlay();

    void pageLayerManagementApi();
    void measureAreaReadoutLandsInStatusBar();
    void identifyRectRoutesToPanel();
    void overviewRendersManagedGridVector();
    // #482：鹰眼点选限定左键——中键按压不得带动主画布中心。
    void overviewOnlyLeftButtonCenters();

  private:
    QgsVectorLayer *addPointLayer(int n);
    void firePress(QgsMapTool *tool, QPoint pos, Qt::MouseButton b = Qt::LeftButton);
    void fireMove(QgsMapTool *tool, QPoint pos, Qt::MouseButtons buttons = Qt::NoButton);
    void fireRelease(QgsMapTool *tool, QPoint pos, Qt::MouseButton b = Qt::LeftButton);

    std::unique_ptr<PreviewMapPage> m_page;
};

QgsVectorLayer *TestPreviewMapPage::addPointLayer(int n)
{
  auto *vl = new QgsVectorLayer(
      QStringLiteral("Point?crs=WKT:%1&field=name:string")
          .arg(DataCatalog::localGridCrsWkt()),
      QStringLiteral("pts"), QStringLiteral("memory"));
  for (int i = 0; i < n; ++i)
  {
    QgsFeature f(vl->fields());
    f.setAttribute(QStringLiteral("name"), QStringLiteral("n%1").arg(i));
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(100.0 * i, 50.0 * i)));
    vl->dataProvider()->addFeature(f);
  }
  vl->updateExtents();
  vl->setParent(m_page.get()); // 页宿主持有（产品同构）
  m_page->addMapLayer(vl, QStringLiteral("pts"), QStringLiteral("memory://pts"));
  return vl;
}

void TestPreviewMapPage::firePress(QgsMapTool *tool, QPoint pos, Qt::MouseButton b)
{
  QgsMapMouseEvent e(m_page->mapCanvas()->canvas(), QEvent::MouseButtonPress, pos, b, b);
  tool->canvasPressEvent(&e);
}
void TestPreviewMapPage::fireMove(QgsMapTool *tool, QPoint pos, Qt::MouseButtons buttons)
{
  QgsMapMouseEvent e(m_page->mapCanvas()->canvas(), QEvent::MouseMove, pos, Qt::NoButton,
                     buttons);
  tool->canvasMoveEvent(&e);
}
void TestPreviewMapPage::fireRelease(QgsMapTool *tool, QPoint pos, Qt::MouseButton b)
{
  QgsMapMouseEvent e(m_page->mapCanvas()->canvas(), QEvent::MouseButtonRelease, pos, b,
                     Qt::NoButton);
  tool->canvasReleaseEvent(&e);
}

void TestPreviewMapPage::toolBarButtonsExist()
{
  auto *bar = m_page->findChild<QToolBar *>(QStringLiteral("previewMapToolBar"));
  QVERIFY(bar);
  // 七件内建工具 + 全图/前后/复制/鹰眼动作都登记在案。
  const QStringList expected = {QStringLiteral("previewAction_pan"),
                                QStringLiteral("previewAction_zoomIn"),
                                QStringLiteral("previewAction_zoomOut"),
                                QStringLiteral("previewAction_identify"),
                                QStringLiteral("previewAction_measureLine"),
                                QStringLiteral("previewAction_measureArea"),
                                QStringLiteral("previewAction_profile"),
                                QStringLiteral("previewOverviewAction")};
  for (const QString &name : expected)
    QVERIFY2(m_page->findChild<QAction *>(name), qPrintable(name));
  // 侧栏三页签：图层/识别/剖面。
  auto *side = m_page->findChild<QTabWidget *>(QStringLiteral("previewSideTabs"));
  QVERIFY(side);
  QCOMPARE(side->count(), 3);
}

void TestPreviewMapPage::toolActionActivationIsExclusive()
{
  auto *identify = m_page->findChild<QAction *>(QStringLiteral("previewAction_identify"));
  auto *measure = m_page->findChild<QAction *>(QStringLiteral("previewAction_measureLine"));
  QVERIFY(identify && measure);
  identify->trigger();
  QCOMPARE(m_page->toolManager()->activeToolId(), PreviewMapToolManager::kIdentify);
  QVERIFY(identify->isChecked());
  measure->trigger();
  QCOMPARE(m_page->toolManager()->activeToolId(), PreviewMapToolManager::kMeasureLine);
  QVERIFY(measure->isChecked());
  QVERIFY(!identify->isChecked()); // 互斥
}

void TestPreviewMapPage::toolVisibleHidesActionAndFallsBackToPan()
{
  m_page->setToolVisible(PreviewMapToolManager::kProfile, false);
  QVERIFY(!m_page->isToolVisible(PreviewMapToolManager::kProfile));
  // 隐藏激活中的工具 → 回 pan。
  m_page->toolManager()->activate(PreviewMapToolManager::kProfile);
  m_page->setToolVisible(PreviewMapToolManager::kProfile, false);
  QCOMPARE(m_page->toolManager()->activeToolId(), PreviewMapToolManager::kPan);
  m_page->setToolVisible(PreviewMapToolManager::kProfile, true);
  QVERIFY(m_page->isToolVisible(PreviewMapToolManager::kProfile));
}

void TestPreviewMapPage::panIsActiveByDefault()
{
  QCOMPARE(m_page->toolManager()->activeToolId(), PreviewMapToolManager::kPan);
  auto *pan = m_page->findChild<QAction *>(QStringLiteral("previewAction_pan"));
  QVERIFY(pan->isChecked());
}

void TestPreviewMapPage::errorStateShowsPageAndClears()
{
  QVERIFY(!m_page->errorActive());
  m_page->setError(QStringLiteral("数据源损坏"), QStringLiteral("/x/y.tif"));
  QVERIFY(m_page->errorActive());
  auto *errPage = m_page->findChild<QWidget *>(QStringLiteral("previewMapErrorPage"));
  QVERIFY(errPage);
  auto *stateText = errPage->findChild<QLabel *>(QStringLiteral("stateText"));
  QVERIFY(stateText);
  QVERIFY(stateText->text().contains(QStringLiteral("数据源损坏")));
  m_page->clearError();
  QVERIFY(!m_page->errorActive());
}

void TestPreviewMapPage::renderLabelReflectsCompletion()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  QTRY_VERIFY_WITH_TIMEOUT(m_page->renderReadout().contains(QStringLiteral("ms")), 5000);
  QVERIFY(m_page->renderReadout().contains(QStringLiteral("层")));
}

void TestPreviewMapPage::scaleLabelFormatted()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  QTRY_VERIFY_WITH_TIMEOUT(!m_page->scaleReadout().isEmpty(), 5000);
  QVERIFY(m_page->scaleReadout().startsWith(QStringLiteral("1:")));
}

void TestPreviewMapPage::coordinateLabelTracksMouse()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  QTest::qWait(30);
  QVERIFY(m_page->coordinateReadout().isEmpty()); // 尚无鼠标事件
  auto *inner = m_page->mapCanvas()->canvas();
  const auto sendMove = [inner](int x, int y) {
    QMouseEvent ev(QEvent::MouseMove, QPointF(x, y), inner->mapToGlobal(QPoint(x, y)),
                   Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(inner, &ev);
  };
  sendMove(100, 100);
  QTest::qWait(60); // 节流窗口（≥33ms）
  sendMove(120, 120);
  QTRY_VERIFY_WITH_TIMEOUT(!m_page->coordinateReadout().isEmpty(), 2000);
  QVERIFY(m_page->coordinateReadout().contains(QStringLiteral("X ")));
  QVERIFY(m_page->coordinateReadout().contains(QStringLiteral("Y ")));
}

void TestPreviewMapPage::bookmarkAddJumpRemove()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  const QgsRectangle full = m_page->mapCanvas()->currentExtent();
  m_page->addBookmarkNamed(QStringLiteral("b1"));
  QCOMPARE(m_page->bookmarkCount(), 1);
  m_page->mapCanvas()->zoomToRect(QgsRectangle(10, 10, 20, 20));
  QVERIFY(m_page->jumpToBookmark(QStringLiteral("b1")));
  const QgsRectangle back = m_page->mapCanvas()->currentExtent();
  QCOMPARE(back.width(), full.width()); // 精确复位（setViewExtent 不加边距）
  QVERIFY(m_page->removeBookmark(QStringLiteral("b1")));
  QCOMPARE(m_page->bookmarkCount(), 0);
  QVERIFY(!m_page->jumpToBookmark(QStringLiteral("b1"))); // 已删：跳转失败
}

void TestPreviewMapPage::overviewScalesWithCanvas()
{
  m_page->show();
  QVERIFY(QTest::qWaitForWindowExposed(m_page.get()));
  auto *ov = m_page->findChild<QWidget *>(QStringLiteral("previewOverviewMap"));
  QVERIFY(ov);
  QTRY_VERIFY(ov->width() <= 200 && ov->height() <= 150);
  // 小画布：原实现恒为 200x150，几乎盖住主图。
  m_page->resize(420, 300);
  QTest::qWait(300);
  if (const QString dir = qEnvironmentVariable("R6_SHOT_DIR"); !dir.isEmpty())
    m_page->grab().save(dir + QStringLiteral("/previewmap-420x300.png"));
  const QSize c = m_page->mapCanvas()->size();
  QTRY_VERIFY2(ov->width() < 200, qPrintable(QStringLiteral("%1x%2 on %3x%4")
                                                 .arg(ov->width()).arg(ov->height())
                                                 .arg(c.width()).arg(c.height())));
  QVERIFY(ov->width() >= 96 && ov->height() >= 72);
  QVERIFY(ov->width() * ov->height() * 5 < c.width() * c.height());
}

void TestPreviewMapPage::overviewVisibleToggle()
{
  QVERIFY(m_page->overviewVisible());
  auto *ov = m_page->findChild<QWidget *>(QStringLiteral("previewOverviewMap"));
  QVERIFY(ov);
  m_page->setOverviewVisible(false);
  QVERIFY(!m_page->overviewVisible());
  m_page->setOverviewVisible(true);
  QVERIFY(m_page->overviewVisible());
}

void TestPreviewMapPage::decorationToggleByName()
{
  QVERIFY(m_page->decorationEnabled(QStringLiteral("scaleBar")));
  QVERIFY(m_page->decorationEnabled(QStringLiteral("northArrow")));
  QVERIFY(!m_page->decorationEnabled(QStringLiteral("grid")));
  m_page->setDecorationEnabled(QStringLiteral("grid"), true);
  QVERIFY(m_page->decorationEnabled(QStringLiteral("grid")));
  m_page->setDecorationEnabled(QStringLiteral("scaleBar"), false);
  QVERIFY(!m_page->decorationEnabled(QStringLiteral("scaleBar")));
}

void TestPreviewMapPage::copyCoordinateToClipboard()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  QTest::qWait(30);
  auto *inner = m_page->mapCanvas()->canvas();
  QMouseEvent ev(QEvent::MouseMove, QPointF(80, 80), inner->mapToGlobal(QPoint(80, 80)),
                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(inner, &ev);
  QTRY_VERIFY_WITH_TIMEOUT(!m_page->coordinateReadout().isEmpty(), 2000);
  const QString before = QApplication::clipboard()->text();
  auto *copyAction = m_page->findChild<QAction *>(QStringLiteral("previewAction_copyCoord"));
  QVERIFY(copyAction);
  copyAction->trigger(); // 工具条动作路径（D3.8）
  const QString after = QApplication::clipboard()->text();
  QVERIFY(after != before);
  const QString readout = m_page->coordinateReadout();
  const QString x = readout.section(QChar(' '), 1, 1);
  const QString y = readout.section(QChar(' '), 4, 4);
  QVERIFY(!x.isEmpty());
  QVERIFY(!y.isEmpty());
  QVERIFY(after.contains(x));
  QVERIFY(after.contains(y));
}

void TestPreviewMapPage::copyScreenshotToClipboard()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  QVERIFY(QApplication::clipboard()->pixmap().isNull());
  auto *shotAction = m_page->findChild<QAction *>(QStringLiteral("previewAction_copyShot"));
  QVERIFY(shotAction);
  shotAction->trigger(); // 工具条动作路径（D3.8）
  QVERIFY(!QApplication::clipboard()->pixmap().isNull());
}

void TestPreviewMapPage::measureReadoutLandsInStatusBar()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  m_page->toolManager()->activate(PreviewMapToolManager::kMeasureLine);
  auto *tool = static_cast<PreviewMeasureTool *>(
      m_page->toolManager()->tool(PreviewMapToolManager::kMeasureLine));
  firePress(tool, QPoint(100, 100));
  firePress(tool, QPoint(300, 100));
  QTRY_VERIFY_WITH_TIMEOUT(!m_page->measureReadout().isEmpty(), 2000);
  QVERIFY(m_page->measureReadout().contains(QStringLiteral("长度")));
}

void TestPreviewMapPage::identifyRoutesToPanel()
{
  addPointLayer(9);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  QTest::qWait(50);
  m_page->toolManager()->activate(PreviewMapToolManager::kIdentify);
  auto *tool =
      static_cast<PreviewIdentifyTool *>(m_page->toolManager()->tool(PreviewMapToolManager::kIdentify));
  auto *side = m_page->findChild<QTabWidget *>(QStringLiteral("previewSideTabs"));
  const int before = side->currentIndex();
  // 精确打在 n0(0,0) 的投影像素上（mapToPixel 同源）。
  const QgsPointXY px0 = m_page->mapCanvas()->canvas()->mapSettings().mapToPixel().transform(
      QgsPointXY(0, 0));
  const QPoint hit(int( px0.x() ), int( px0.y() ));
  firePress(tool, hit);
  fireRelease(tool, hit);
  QTRY_COMPARE(m_page->identifyPanel()->resultCount(), 1);
  QCOMPARE(side->currentIndex(), 1); // 自动切到识别页签
  Q_UNUSED(before)
}

void TestPreviewMapPage::profileLineSwitchesSideTab()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  m_page->toolManager()->activate(PreviewMapToolManager::kProfile);
  auto *tool =
      static_cast<PreviewProfileTool *>(m_page->toolManager()->tool(PreviewMapToolManager::kProfile));
  QSignalSpy spy(m_page.get(), &PreviewMapPage::profileLineDrawn);
  firePress(tool, QPoint(50, 200));
  fireMove(tool, QPoint(400, 200), Qt::LeftButton);
  fireRelease(tool, QPoint(400, 200));
  QCOMPARE(spy.count(), 1);
  auto *side = m_page->findChild<QTabWidget *>(QStringLiteral("previewSideTabs"));
  QCOMPARE(side->currentIndex(), 2); // 切到剖面页签
}

void TestPreviewMapPage::analysisTabsAppearOnDemand()
{
  auto *tabs = m_page->findChild<QTabWidget *>(QStringLiteral("previewAnalysisTabs"));
  QVERIFY(tabs);
  QVERIFY(!tabs->isVisible()); // 无页签时隐藏
  auto *w = new QWidget(m_page.get());
  m_page->addAnalysisTab(QStringLiteral("统计"), w);
  QCOMPARE(tabs->count(), 1);
  QVERIFY(!tabs->isHidden()); // 显隐语义：无页签显式隐藏、有页签显式显示
}

void TestPreviewMapPage::renderCacheStoresOnCompletion()
{
  addPointLayer(3);
  m_page->setRenderCacheIdentity(QStringLiteral("page-asset"), QStringLiteral("v1"));
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  // zoom 后的 completed 帧把当前范围存进缓存（D6.2）——QTRY 直接等命中。
  QTRY_VERIFY_WITH_TIMEOUT(
      [&]() {
        const QSize sz = m_page->mapCanvas()->canvas()->size();
        const QString key = PreviewRenderCache::makeKey(
            QStringLiteral("page-asset"), QStringLiteral("v1"),
            m_page->mapCanvas()->currentExtent(), sz.width(), sz.height());
        return !PreviewRenderCache::instance().lookup(key).isNull();
      }(),
      5000);
}

void TestPreviewMapPage::lowResSnapshotShowsOverlay()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  m_page->setRenderCacheIdentity(QStringLiteral("cached-snapshot"), QStringLiteral("v1"));
  const QSize size = m_page->mapCanvas()->canvas()->size();
  QImage image(size, QImage::Format_ARGB32); image.fill(Qt::white);
  m_page->showLowResSnapshot();
  QVERIFY(!m_page->mapCanvas()->overlayVisible()); // 未灌缓存时不铺低清帧。
  PreviewRenderCache::instance().store(PreviewRenderCache::makeKey(
      QStringLiteral("cached-snapshot"), QStringLiteral("v1"),
      m_page->mapCanvas()->currentExtent(), size.width(), size.height()), image);
  m_page->showLowResSnapshot();
  QVERIFY(m_page->mapCanvas()->overlayVisible()); // 缓存命中才先上。
  // 真渲完成后 overlay 让位。
  QTRY_VERIFY_WITH_TIMEOUT(!m_page->mapCanvas()->overlayVisible(), 5000);
}

void TestPreviewMapPage::pageLayerManagementApi()
{
  QSignalSpy changedSpy(m_page.get(), &PreviewMapPage::mapLayersChanged);
  QgsVectorLayer *a = addPointLayer(2);
  QCOMPARE(m_page->mapLayerCount(), 1);
  QCOMPARE(changedSpy.count(), 1);
  QgsVectorLayer *b = addPointLayer(3);
  QCOMPARE(m_page->mapLayerCount(), 2);
  m_page->removeMapLayer(a);
  QCOMPARE(m_page->mapLayerCount(), 1);
  QCOMPARE(m_page->mapCanvas()->layers().size(), 1);
  m_page->clearMapLayers();
  QCOMPARE(m_page->mapLayerCount(), 0);
  QVERIFY(m_page->mapCanvas()->layers().isEmpty());
  QVERIFY(changedSpy.count() >= 4); // 增删清都有事件
}

void TestPreviewMapPage::measureAreaReadoutLandsInStatusBar()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  m_page->toolManager()->activate(PreviewMapToolManager::kMeasureArea);
  auto *tool = static_cast<PreviewMeasureTool *>(
      m_page->toolManager()->tool(PreviewMapToolManager::kMeasureArea));
  const QgsPointXY p00 = m_page->mapCanvas()->canvas()->mapSettings().mapToPixel().transform(
      QgsPointXY(0, 0));
  const QgsPointXY p10 = m_page->mapCanvas()->canvas()->mapSettings().mapToPixel().transform(
      QgsPointXY(100, 0));
  const QgsPointXY p11 = m_page->mapCanvas()->canvas()->mapSettings().mapToPixel().transform(
      QgsPointXY(100, 100));
  firePress(tool, QPoint(int(p00.x()), int(p00.y())));
  firePress(tool, QPoint(int(p10.x()), int(p10.y())));
  firePress(tool, QPoint(int(p11.x()), int(p11.y())));
  QTRY_VERIFY_WITH_TIMEOUT(m_page->measureReadout().contains(QStringLiteral("面积")), 2000);
  QVERIFY(m_page->measureReadout().contains(QStringLiteral("周长")));
}

void TestPreviewMapPage::identifyRectRoutesToPanel()
{
  addPointLayer(9);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  QTest::qWait(50);
  m_page->toolManager()->activate(PreviewMapToolManager::kIdentify);
  auto *tool =
      static_cast<PreviewIdentifyTool *>(m_page->toolManager()->tool(PreviewMapToolManager::kIdentify));
  // 框住整图（左上到右下拖框）→ 多要素命中。
  firePress(tool, QPoint(5, 5));
  fireMove(tool, QPoint(m_page->mapCanvas()->canvas()->width() - 5,
                        m_page->mapCanvas()->canvas()->height() - 5),
           Qt::LeftButton);
  fireRelease(tool, QPoint(m_page->mapCanvas()->canvas()->width() - 5,
                           m_page->mapCanvas()->canvas()->height() - 5));
  QTRY_VERIFY_WITH_TIMEOUT(m_page->identifyPanel()->resultCount() >= 1, 2000);
  auto *side = m_page->findChild<QTabWidget *>(QStringLiteral("previewSideTabs"));
  QCOMPARE(side->currentIndex(), 1);
}

void TestPreviewMapPage::overviewRendersManagedGridVector()
{
  // GDAL 归一化病灶（MappingArtifactWriter::restoreVectorCrs 修复对象）：
  // OGR 读回 Paleo 工程网格产物时 EDATUM 降级成 "Unknown engineering datum"，
  // 与画布规范 WKT 不再同对象——PROJ 造不出坐标操作 → fullExtent 空 →
  // 鹰眼 syncExtent 早退 → mapSettings 恒 invalid → refresh 空转 → 白屏。
  // 这里用 memory 层 + setCrs 复现同样的退化 CRS，不依赖外部 gpkg 文件。
  const QString degraded = QStringLiteral(
      "ENGCRS[\"Paleo local engineering grid\","
      "EDATUM[\"Unknown engineering datum\"],"
      "CS[Cartesian,2],"
      "AXIS[\"easting\",east,ORDER[1],LENGTHUNIT[\"metre\",1,ID[\"EPSG\",9001]]],"
      "AXIS[\"northing\",north,ORDER[2],LENGTHUNIT[\"metre\",1,ID[\"EPSG\",9001]]]]");
  const auto canonical =
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  auto *vl = new QgsVectorLayer(QStringLiteral("Polygon?field=name:string"),
                                QStringLiteral("poly"), QStringLiteral("memory"));
  vl->setCrs(QgsCoordinateReferenceSystem::fromWkt(degraded));
  QVERIFY(vl->crs().isValid());
  QVERIFY(vl->crs() != canonical);
  QgsFeature f(vl->fields());
  f.setAttribute(QStringLiteral("name"), QStringLiteral("p1"));
  f.setGeometry(QgsGeometry::fromPolygonXY(
      {{QgsPointXY(0, 0), QgsPointXY(100, 0), QgsPointXY(100, 80),
        QgsPointXY(0, 80), QgsPointXY(0, 0)}}));
  vl->dataProvider()->addFeature(f);
  vl->updateExtents();
  vl->setParent(m_page.get());
  m_page->addMapLayer(vl, QStringLiteral("poly"), QStringLiteral("memory://poly"));

  // 修复点 1：入画布时恢复规范 ENGCRS，fullExtent 才有输出。
  QCOMPARE(vl->crs(), canonical);
  QVERIFY(!m_page->mapCanvas()->fullExtent().isEmpty());

  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();

  auto *ov = m_page->findChild<QWidget *>(QStringLiteral("previewOverviewMap"));
  QVERIFY(ov);
  auto *ovCanvas = ov->findChild<QgsMapCanvas *>(QStringLiteral("previewOverviewCanvas"));
  QVERIFY(ovCanvas);
  // 修复点 2：鹰眼拿到有效 settings（extent 非空 + outputSize 非零），
  // refresh 才真正渲出内容。
  QTRY_VERIFY_WITH_TIMEOUT(ovCanvas->mapSettings().hasValidSettings(), 5000);
  QCOMPARE(ovCanvas->layers().size(), 1);
  QVERIFY(!ovCanvas->extent().isEmpty());

  // 修复点 3：停靠右下角；R6 #848 起尺寸随主画布缩放（≈22% 宽、4:3，96×72..200×150）。
  const int margin = PaleoTheme::tokens().spacingSm;
  const auto *host = m_page->mapCanvas();
  QVERIFY(ov->width() >= 96 && ov->width() <= 200);
  QVERIFY(ov->height() >= 72 && ov->height() <= 150);
  QVERIFY(qAbs(ov->width() * 3 - ov->height() * 4) <= 4);
  QVERIFY(ov->width() <= qMax(96, host->width() * 22 / 100));
  QCOMPARE(ov->x() + ov->width(), host->width() - margin);
  QCOMPARE(ov->y() + ov->height(), host->height() - margin);
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestPreviewMapPage tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

// #482：鹰眼小画布点选/拖动限定左键——中键/右键按压不得触发 centerMainAt
// 带动主画布中心（旧实现 MouseButtonPress 不看 button()，任意键都居中）。
void TestPreviewMapPage::overviewOnlyLeftButtonCenters()
{
  addPointLayer(3);
  m_page->show();
  QTest::qWaitForWindowExposed(m_page.get());
  m_page->mapCanvas()->zoomToFullExtent();
  QTRY_VERIFY_WITH_TIMEOUT(!m_page->scaleReadout().isEmpty(), 5000);
  auto *ovCanvas = m_page->findChild<QWidget *>(QStringLiteral("previewOverviewCanvas"));
  QVERIFY(ovCanvas);

  const QgsPointXY before = m_page->mapCanvas()->currentExtent().center();
  const auto pressAt = [ovCanvas](Qt::MouseButton button) {
    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(180, 12),
                   ovCanvas->mapToGlobal(QPoint(180, 12)), button, button,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(ovCanvas, &ev);
  };
  // 中键：主画布中心必须纹丝不动。
  pressAt(Qt::MiddleButton);
  QTest::qWait(30);
  const QgsPointXY afterMid = m_page->mapCanvas()->currentExtent().center();
  QCOMPARE(afterMid.x(), before.x());
  QCOMPARE(afterMid.y(), before.y());
  // 右键：同上（鹰眼不吃右键菜单路径）。
  pressAt(Qt::RightButton);
  QTest::qWait(30);
  const QgsPointXY afterRight = m_page->mapCanvas()->currentExtent().center();
  QCOMPARE(afterRight.x(), before.x());
  QCOMPARE(afterRight.y(), before.y());
  // 左键：点击偏心点（overview 右上角）必须把主画布中心带过去。
  pressAt(Qt::LeftButton);
  const QgsPointXY afterLeft = m_page->mapCanvas()->currentExtent().center();
  QVERIFY2(qAbs(afterLeft.x() - before.x()) > 1e-6 || qAbs(afterLeft.y() - before.y()) > 1e-6,
           "左键点选必须移动主画布中心");
}

#include "tst_previewmap_page.moc"
