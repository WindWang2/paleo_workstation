#include <QtTest>
#include <QSignalSpy>

#include "../src/qgis/previewmaptools.h"
#include "../src/qgis/qgisruntime.h"

#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptool.h>
#include <qgsrectangle.h>
#include <qgsrubberband.h>
#include <qgsvectorlayer.h>

#include <memory>

// P2 D1.2/D3.x/D5.1/D7 工具族契约：注册表互斥激活、Esc 回 pan、量测（线/面，
// 平面米制 + ≤30Hz 节流）、identify（点/框）、剖面线（拖线/累计/微拖拒绝）。
// 画布事件用 QgsMapMouseEvent 五参 ctor 合成（与 QgsMapCanvas 内部同构）。
class TestPreviewMapTools : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }
    void init()
    {
      m_canvas = std::make_unique<QgsMapCanvas>();
      m_canvas->resize(400, 300);
      m_canvas->setDestinationCrs(QgsCoordinateReferenceSystem::fromEpsgId(3857));
      // 隐藏画布的 resize 事件延迟到 show——先暴露再钉范围，mapToPixel 才是
      // 真实尺寸（工具事件合成依赖它）。
      m_canvas->show();
      QTest::qWaitForWindowExposed(m_canvas.get());
      m_canvas->setExtent(QgsRectangle(0, 0, 400, 300)); // 1 px = 1 m 便于断言
      m_mgr = std::make_unique<PreviewMapToolManager>(m_canvas.get());
    }

    void registryHasBuiltinTools();
    void displayNamesAreChinese();
    void activateIsMutuallyExclusive();
    void activateUnknownToolFails();
    void cancelToDefaultLandsOnPan();
    void formatLengthAndArea();
    void measureLineAddsPointsAndEmits();
    void measureLineThrottlesTo30Hz();
    void measureLineDoubleClickFinishes();
    void measureAreaNeedsThreePoints();
    void measureAreaComputesAreaAndPerimeter();
    void measureClearEmitsCleared();
    void identifyClickEmitsPoint();
    void identifyDragEmitsRect();
    void identifyEscapeReturnsToPan();
    void profileDragEmitsLine();
    void profileMicroDragRejected();
    void profileRightClickClearsAll();
    void profileEscapeClearsAndReturnsPan();

  private:
    // 合成画布事件并直投工具（事件对象必须是具名局部——取临时地址不合法）。
    void firePress(QgsMapTool *tool, QPoint pos, Qt::MouseButton b = Qt::LeftButton)
    {
      QgsMapMouseEvent e(m_canvas.get(), QEvent::MouseButtonPress, pos, b, b);
      tool->canvasPressEvent(&e);
    }
    void fireMove(QgsMapTool *tool, QPoint pos, Qt::MouseButtons buttons = Qt::NoButton)
    {
      QgsMapMouseEvent e(m_canvas.get(), QEvent::MouseMove, pos, Qt::NoButton, buttons);
      tool->canvasMoveEvent(&e);
    }
    void fireRelease(QgsMapTool *tool, QPoint pos, Qt::MouseButton b = Qt::LeftButton)
    {
      QgsMapMouseEvent e(m_canvas.get(), QEvent::MouseButtonRelease, pos, b, Qt::NoButton);
      tool->canvasReleaseEvent(&e);
    }
    void fireDblClick(QgsMapTool *tool, QPoint pos)
    {
      QgsMapMouseEvent e(m_canvas.get(), QEvent::MouseButtonDblClick, pos, Qt::LeftButton,
                         Qt::NoButton);
      tool->canvasDoubleClickEvent(&e);
    }

    QgsPointXY mapOf(int px, int py)
    {
      return m_canvas->mapSettings().mapToPixel().toMapCoordinates(px, py);
    }

    std::unique_ptr<QgsMapCanvas> m_canvas;
    std::unique_ptr<PreviewMapToolManager> m_mgr;
};

void TestPreviewMapTools::registryHasBuiltinTools()
{
  const QStringList ids = m_mgr->toolIds();
  for (const QString &id : {PreviewMapToolManager::kPan, PreviewMapToolManager::kZoomIn,
                            PreviewMapToolManager::kZoomOut, PreviewMapToolManager::kIdentify,
                            PreviewMapToolManager::kMeasureLine,
                            PreviewMapToolManager::kMeasureArea,
                            PreviewMapToolManager::kProfile})
    QVERIFY2(ids.contains(id), qPrintable(id));
  QVERIFY(m_mgr->tool(PreviewMapToolManager::kPan) != nullptr);
}

void TestPreviewMapTools::displayNamesAreChinese()
{
  QCOMPARE(m_mgr->toolDisplayName(PreviewMapToolManager::kMeasureLine), QStringLiteral("距离测量"));
  QCOMPARE(m_mgr->toolDisplayName(PreviewMapToolManager::kProfile), QStringLiteral("层位剖面线"));
}

void TestPreviewMapTools::activateIsMutuallyExclusive()
{
  QSignalSpy activatedSpy(m_mgr.get(), &PreviewMapToolManager::toolActivated);
  QVERIFY(m_mgr->activate(PreviewMapToolManager::kIdentify));
  QCOMPARE(m_mgr->activeToolId(), PreviewMapToolManager::kIdentify);
  QVERIFY(m_mgr->activate(PreviewMapToolManager::kMeasureLine));
  QCOMPARE(m_mgr->activeToolId(), PreviewMapToolManager::kMeasureLine);
  QCOMPARE(m_canvas->mapTool(), m_mgr->tool(PreviewMapToolManager::kMeasureLine));
  QCOMPARE(activatedSpy.count(), 2);
  // 重复激活幂等（不再发信号）。
  QVERIFY(m_mgr->activate(PreviewMapToolManager::kMeasureLine));
  QCOMPARE(activatedSpy.count(), 2);
}

void TestPreviewMapTools::activateUnknownToolFails()
{
  QVERIFY(!m_mgr->activate(QStringLiteral("no-such-tool")));
  QVERIFY(m_mgr->activeToolId().isEmpty());
}

void TestPreviewMapTools::cancelToDefaultLandsOnPan()
{
  m_mgr->activate(PreviewMapToolManager::kMeasureArea);
  m_mgr->cancelToDefault();
  QCOMPARE(m_mgr->activeToolId(), PreviewMapToolManager::kPan);
}

void TestPreviewMapTools::formatLengthAndArea()
{
  QCOMPARE(PreviewMapFormat::length(12.34), QStringLiteral("12.3 m"));
  QCOMPARE(PreviewMapFormat::length(1234.5), QStringLiteral("1.23 km"));
  QCOMPARE(PreviewMapFormat::area(123.4), QStringLiteral("123.4 m²"));
  QCOMPARE(PreviewMapFormat::area(2.5e6), QStringLiteral("2.500 km²"));
}

void TestPreviewMapTools::measureLineAddsPointsAndEmits()
{
  m_mgr->activate(PreviewMapToolManager::kMeasureLine);
  auto *tool = static_cast<PreviewMeasureTool *>(m_mgr->tool(PreviewMapToolManager::kMeasureLine));
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::measurementChanged);
  firePress(tool, QPoint(10, 150));
  QCOMPARE(spy.count(), 1); // press 即发一帧
  firePress(tool, QPoint(110, 150));
  QCOMPARE(tool->points().size(), 2);
  // 期望值与工具同一变换源（画布 mapToPixel）——不假设 1px=1m。
  const double expectLen = mapOf(10, 150).distance(mapOf(110, 150));
  QVERIFY(qAbs(tool->currentLength() - expectLen) < 0.01);
  const double len = spy.last().at(1).toDouble();
  QVERIFY(qAbs(len - expectLen) < 0.01);
}

void TestPreviewMapTools::measureLineThrottlesTo30Hz()
{
  m_mgr->activate(PreviewMapToolManager::kMeasureLine);
  auto *tool = static_cast<PreviewMeasureTool *>(m_mgr->tool(PreviewMapToolManager::kMeasureLine));
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::measurementChanged);
  firePress(tool, QPoint(10, 150));
  const int afterPress = spy.count();
  for (int i = 0; i < 25; ++i) // 突发移动全在 33ms 窗内 → 几乎全被节流
    fireMove(tool, QPoint(20 + i, 150), Qt::LeftButton);
  QVERIFY(spy.count() - afterPress <= 2); // ≤30Hz（D6.5）
  QTest::qWait(50);                       // 过窗后再动 → 必出一帧
  fireMove(tool, QPoint(200, 150), Qt::LeftButton);
  QVERIFY(spy.count() - afterPress >= 1);
}

void TestPreviewMapTools::measureLineDoubleClickFinishes()
{
  m_mgr->activate(PreviewMapToolManager::kMeasureLine);
  auto *tool = static_cast<PreviewMeasureTool *>(m_mgr->tool(PreviewMapToolManager::kMeasureLine));
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::measurementChanged);
  firePress(tool, QPoint(10, 150));
  firePress(tool, QPoint(210, 150));
  fireDblClick(tool, QPoint(210, 150));
  QVERIFY(tool->isFinished());
  QVERIFY(spy.last().at(3).toBool()); // finished 帧
  QCOMPARE(tool->currentArea(), 0.0); // 线型不算面积
}

void TestPreviewMapTools::measureAreaNeedsThreePoints()
{
  m_mgr->activate(PreviewMapToolManager::kMeasureArea);
  auto *tool = static_cast<PreviewMeasureTool *>(m_mgr->tool(PreviewMapToolManager::kMeasureArea));
  firePress(tool, QPoint(10, 10));
  firePress(tool, QPoint(210, 10));
  fireDblClick(tool, QPoint(210, 10));
  QVERIFY(!tool->isFinished()); // 两点不成面
  firePress(tool, QPoint(10, 10));
  firePress(tool, QPoint(210, 10));
  firePress(tool, QPoint(210, 200));
  fireDblClick(tool, QPoint(210, 200));
  QVERIFY(tool->isFinished());
}

void TestPreviewMapTools::measureAreaComputesAreaAndPerimeter()
{
  m_mgr->activate(PreviewMapToolManager::kMeasureArea);
  auto *tool = static_cast<PreviewMeasureTool *>(m_mgr->tool(PreviewMapToolManager::kMeasureArea));
  const QgsPointXY p00 = mapOf(10, 10);
  const QgsPointXY p10 = mapOf(110, 10);
  const QgsPointXY p11 = mapOf(110, 110);
  const QgsPointXY p01 = mapOf(10, 110);
  const double w = p00.distance(p10);
  const double h = p10.distance(p11);
  firePress(tool, QPoint(10, 10));
  firePress(tool, QPoint(110, 10));
  firePress(tool, QPoint(110, 110));
  firePress(tool, QPoint(10, 110));
  QVERIFY(qAbs(tool->currentArea() - w * h) < 1.0); // 矩形面积（同源变换）
  QVERIFY(tool->currentLength() > 2 * (w + h) * 0.95); // 周长口径
}

void TestPreviewMapTools::measureClearEmitsCleared()
{
  m_mgr->activate(PreviewMapToolManager::kMeasureLine);
  auto *tool = static_cast<PreviewMeasureTool *>(m_mgr->tool(PreviewMapToolManager::kMeasureLine));
  firePress(tool, QPoint(10, 150));
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::measurementCleared);
  tool->clear();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(tool->points().size(), 0);
}

void TestPreviewMapTools::identifyClickEmitsPoint()
{
  m_mgr->activate(PreviewMapToolManager::kIdentify);
  auto *tool = static_cast<PreviewIdentifyTool *>(m_mgr->tool(PreviewMapToolManager::kIdentify));
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::identifyPointRequested);
  firePress(tool, QPoint(50, 50));
  fireRelease(tool, QPoint(50, 50)); // 无拖动 → 点查
  QCOMPARE(spy.count(), 1);
  const QgsPointXY pt = spy.at(0).at(0).value<QgsPointXY>();
  QCOMPARE(pt, mapOf(50, 50)); // 与画布变换同源
}

void TestPreviewMapTools::identifyDragEmitsRect()
{
  m_mgr->activate(PreviewMapToolManager::kIdentify);
  auto *tool = static_cast<PreviewIdentifyTool *>(m_mgr->tool(PreviewMapToolManager::kIdentify));
  QSignalSpy pointSpy(m_mgr.get(), &PreviewMapToolManager::identifyPointRequested);
  QSignalSpy rectSpy(m_mgr.get(), &PreviewMapToolManager::identifyRectRequested);
  firePress(tool, QPoint(10, 10));
  fireMove(tool, QPoint(100, 100), Qt::LeftButton); // >5px 成框
  fireRelease(tool, QPoint(100, 100));
  QCOMPARE(rectSpy.count(), 1);
  QCOMPARE(pointSpy.count(), 0);
  const QgsRectangle rect = rectSpy.at(0).at(0).value<QgsRectangle>();
  QCOMPARE(rect.xMinimum(), mapOf(10, 10).x());
  QCOMPARE(rect.xMaximum(), mapOf(100, 100).x());
}

void TestPreviewMapTools::identifyEscapeReturnsToPan()
{
  m_mgr->activate(PreviewMapToolManager::kIdentify);
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  static_cast<PreviewIdentifyTool *>(m_mgr->tool(PreviewMapToolManager::kIdentify))
      ->keyPressEvent(&esc);
  QCOMPARE(m_mgr->activeToolId(), PreviewMapToolManager::kPan); // Esc 语义
}

void TestPreviewMapTools::profileDragEmitsLine()
{
  m_mgr->activate(PreviewMapToolManager::kProfile);
  auto *tool = static_cast<PreviewProfileTool *>(m_mgr->tool(PreviewMapToolManager::kProfile));
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::profileLineDrawn);
  firePress(tool, QPoint(20, 150));
  fireMove(tool, QPoint(200, 150), Qt::LeftButton);
  fireRelease(tool, QPoint(200, 150));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.at(0).at(2).toInt(), 1); // totalLines
  const QgsPointXY p1 = spy.at(0).at(0).value<QgsPointXY>();
  const QgsPointXY p2 = spy.at(0).at(1).value<QgsPointXY>();
  QCOMPARE(p1, mapOf(20, 150));
  QCOMPARE(p2, mapOf(200, 150));
  QCOMPARE(tool->lineCount(), 1);
}

void TestPreviewMapTools::profileMicroDragRejected()
{
  m_mgr->activate(PreviewMapToolManager::kProfile);
  auto *tool = static_cast<PreviewProfileTool *>(m_mgr->tool(PreviewMapToolManager::kProfile));
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::profileLineDrawn);
  firePress(tool, QPoint(20, 150));
  fireMove(tool, QPoint(24, 150), Qt::LeftButton); // 4px < 8px 阈值
  fireRelease(tool, QPoint(24, 150));
  QCOMPARE(spy.count(), 0);
  QCOMPARE(tool->lineCount(), 0);
}

void TestPreviewMapTools::profileRightClickClearsAll()
{
  m_mgr->activate(PreviewMapToolManager::kProfile);
  auto *tool = static_cast<PreviewProfileTool *>(m_mgr->tool(PreviewMapToolManager::kProfile));
  firePress(tool, QPoint(20, 150));
  fireRelease(tool, QPoint(200, 150));
  QCOMPARE(tool->lineCount(), 1);
  QSignalSpy spy(m_mgr.get(), &PreviewMapToolManager::profileLinesCleared);
  firePress(tool, QPoint(0, 0), Qt::RightButton);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(tool->lineCount(), 0);
}

void TestPreviewMapTools::profileEscapeClearsAndReturnsPan()
{
  m_mgr->activate(PreviewMapToolManager::kProfile);
  auto *tool = static_cast<PreviewProfileTool *>(m_mgr->tool(PreviewMapToolManager::kProfile));
  firePress(tool, QPoint(20, 150));
  fireRelease(tool, QPoint(200, 150));
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  tool->keyPressEvent(&esc);
  QCOMPARE(tool->lineCount(), 0);
  QCOMPARE(m_mgr->activeToolId(), PreviewMapToolManager::kPan);
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestPreviewMapTools tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_previewmap_tools.moc"
