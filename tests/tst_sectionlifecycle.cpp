// 层：测试壳（wave/deepen-perf D2 —— 连井剖面生命周期边界穷举）
//
// attachSections 语义（PaleoMainWindow::attachSections，src/ui/paleomainwindow_
// sections.cpp）刚改过：清除剖面连线 / 对话框唤回 / dock 显隐联动 band /
// 切体清路线 / 换工程清活动体。本套件经真实壳（AppContext + PaleoMainWindow
// + attachWorkflows）驱动边界情形：切体、换层位、关/换工程、绘制期间取消、
// 提取在途切体、重开对话框期间收到事件。
#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDateTime>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>

#include "../src/app/appcontext.h"
#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/domain/seismic/sgyvolume.h"
#include "../src/linkage/seismicmaplink.h"
#include "../src/qgis/seismicsectiontool.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"
#include "../src/ui/seismicsection/sectionsetupdialog.h"
#include "../src/ui/wellcomposite/derivedsink.h"
#include "../src/workflow/sectionworkbench.h"
#include <qgsmapcanvas.h>
#include <qgsrubberband.h>

namespace
{

// 合成 4x4 体（同 tst_sections_alignment 的 SEG-Y 夹具口径）
QString makeVolume(QTemporaryDir &dir, const QString &name)
{
  const auto path = dir.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return QString();
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
    for (int xl = 0; xl < 4; ++xl)
    {
      QByteArray header(240, 0);
      put32(header, 0, il * 4 + xl + 1);
      put32(header, 188, 1000 + il);
      put32(header, 192, 2000 + xl);
      put16(header, 114, 64);
      file.write(header);
      QByteArray samples(64 * 4, 0);
      for (int k = 0; k < 64; ++k)
      {
        const float v = std::sin(k * .4f + il + xl);
        quint32 bits;
        std::memcpy(&bits, &v, 4);
        qToBigEndian(bits, reinterpret_cast<uchar *>(samples.data()) + 4 * k);
      }
      file.write(samples);
    }
  file.close();
  return path;
}

std::shared_ptr<seismic::SgyVolume> loadVolume(const QString &path)
{
  auto volume = std::make_shared<seismic::SgyVolume>();
  std::string error;
  if (!volume->Load(path.toStdString(), error))
    return nullptr;
  return volume;
}

SurveyGridGeometry validGrid()
{
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

// attachSections 建的剖面连线 band（#1B73D0 / 宽 2）。QgsRubberBand 挂在
// canvas 的 QGraphicsScene 里（非 QObject 子——findChildren 摸不到）。
QgsRubberBand *sectionBand(QgsMapCanvas *canvas)
{
  const auto items = canvas->scene()->items();
  for (auto *it : items)
  {
    auto *b = qgraphicsitem_cast<QgsRubberBand *>(it);
    if (b && b->width() == 2 && b->strokeColor() == QColor(QStringLiteral("#1B73D0")))
      return b;
  }
  return nullptr;
}

} // namespace

class TestSectionLifecycle : public QObject
{
  Q_OBJECT
public:
  explicit TestSectionLifecycle(AppContext *ctx, QObject *parent = nullptr)
    : QObject(parent), m_ctx(ctx) {}

private:
  AppContext *m_ctx = nullptr;
  PaleoMainWindow *m_win = nullptr;

  seismic::SeismicSectionDockWidget *dock() const
  {
    return m_win->findChild<seismic::SeismicSectionDockWidget *>(
        QStringLiteral("seismicSectionDock"));
  }
  SectionSetupDialog *setup() const
  {
    return m_win->findChild<SectionSetupDialog *>();
  }
  SectionWorkbench *workbench() const { return m_win->findChild<SectionWorkbench *>(); }

  // 提取一条剖面（真实链路：triggerSectionFromMapPolyline → extract 请求 →
  // dock 异步提取 → finished）。返回是否成功。（等待宏拆到 void 助手——
  // QTRY_* 失败路径是裸 return，不能出现在返回 bool 的函数体里。）
  // 注意：setVolume 会按当前模式自动重提切片，切片完成也发
  // sectionExtractionFinished——等到「带上路线的那次」才算剖面提取就绪。
  bool extractOnce(SeismicMapLink *link, seismic::SeismicSectionDockWidget *d)
  {
    QSignalSpy done(d, &seismic::SeismicSectionDockWidget::sectionExtractionFinished);
    QSignalSpy fromMap(link, &SeismicMapLink::sectionExtractedFromMap);
    link->triggerSectionFromMapPolyline({{0, 0}, {100, 200}, {200, 200}},
                                        QStringLiteral("lifecycle"));
    if (!fromMap.isEmpty() && !fromMap.first().at(0).toBool())
      qInfo() << "EXTRACT-REJECT" << fromMap.first().at(1).toString();
    waitExtracted(d, &done);
    return d->hasRoute() && done.last().at(0).toBool();
  }
  static void waitExtracted(seismic::SeismicSectionDockWidget *d, QSignalSpy *spy)
  {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10000;
    while (!d->hasRoute() && QDateTime::currentMSecsSinceEpoch() < deadline)
      QApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!d->hasRoute())
    {
      QStringList msgs;
      for (const auto &args : *spy)
        msgs << QStringLiteral("%1|%2").arg(args.at(0).toBool() ? "ok" : "err",
                                             args.at(1).toString());
      qInfo() << "EXTRACT-WAIT no route; finished signals:" << msgs.join(QStringLiteral(" ; "))
              << "volume:" << (d->volume() ? QString::fromStdString(d->volume()->Path().string())
                                           : QStringLiteral("null"));
    }
  }



private slots:
  void initTestCase()
  {
    QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime");
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
    m_win = new PaleoMainWindow(m_ctx->canvasCtl(), m_ctx->projectSvc(),
                                m_ctx->layerSvc(), m_ctx->toolSvc(), m_ctx->selection());
    m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                           m_ctx->compositionWf(), m_ctx->validationWf(),
                           m_ctx->importSvc(), m_ctx->seismicLink(),
                           m_ctx->processingSvc(), m_ctx->store(),
                           m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());
    QVERIFY(dock());
    QVERIFY(setup());
    QVERIFY(workbench());
    QVERIFY(m_ctx->seismicLink());
    m_win->show(); // 真实显隐语义（band/对话框 isVisible 需要祖先可见）
    QApplication::processEvents();
  }

  void cleanupTestCase()
  {
    // 已知坑规避（报 lead）：SeismicSectionTool 析构 delete 橡皮带
    //（src/linkage/seismicsectiontool.cpp:24）——QgsMapCanvas 析构序下场景
    // 先死，delete 场景项悬空 SIGSEGV（PreviewMapCanvas 决策①同类坑）。
    // 测试收尾在画布存活时先拆工具；工具本体修复归 linkage 属主（Track A）。
    if (m_ctx && m_ctx->seismicLink())
      if (auto *tool = m_ctx->seismicLink()->sectionCaptureTool())
      {
        // 画布存活时先摘当前工具再拆（直接 delete 活动工具会把画布的
        // mTool 留成悬空指针，关窗路径再踩一次）
        m_ctx->canvasCtl()->canvas()->unsetMapTool(tool);
        delete tool;
      }
    delete m_win;
    m_win = nullptr;
  }

  // D1 壳接线自查：attachWorkflows 装好全局派生登记 sink（catalog 已绑）。
  void wellCompositeDerivedSinkInstalled()
  {
    auto *sink = WellComposite::WellCompositeDerivedSink::defaultSink();
    QVERIFY(sink);
    QVERIFY(sink->isBound());
  }

  // 提取 → band 上线 + dock 有路线（后续边界用例的地基）
  void extractPopulatesBandAndDock()
  {
    QTemporaryDir dir;
    const auto vol = loadVolume(makeVolume(dir, QStringLiteral("a.sgy")));
    QVERIFY(vol);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol); // 先设体（会按体索引重建/清测网几何）再配测网
    link->setGridGeometry(validGrid());
    QCOMPARE(link->activeVolume().get(), vol.get());

    auto *d = dock();
    QVERIFY(extractOnce(link, d));
    QVERIFY(d->hasRoute());
    auto *band = sectionBand(m_ctx->canvasCtl()->canvas());
    QVERIFY2(band, "attachSections must own a section rubber band on the map canvas");
    QVERIFY(band->isVisible());
    QVERIFY(!band->asGeometry().isEmpty());
  }

  // 切体：band 路线清空 + dock 复位（hasRoute 复归 false——「保存剖面新版本」
  // 随之失效，不残留旧体路线）。
  void volumeSwitchClearsRouteAndBand()
  {
    QTemporaryDir dir;
    const auto vol1 = loadVolume(makeVolume(dir, QStringLiteral("v1.sgy")));
    const auto vol2 = loadVolume(makeVolume(dir, QStringLiteral("v2.sgy")));
    QVERIFY(vol1 && vol2);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol1); // 先体后测网（setActiveVolume 重建测网几何）
    link->setGridGeometry(validGrid());
    auto *d = dock();
    QVERIFY(extractOnce(link, d));
    QVERIFY(d->hasRoute());
    auto *band = sectionBand(m_ctx->canvasCtl()->canvas());
    QVERIFY(band && !band->asGeometry().isEmpty());

    link->setActiveVolume(vol2); // 切体
    QCOMPARE(d->volume().get(), vol2.get());
    QVERIFY(!d->hasRoute());
    QVERIFY(band->asGeometry().isEmpty());
  }

  // 提取在途切体：dock 取消在途任务不崩溃，完成后状态一致。
  void volumeSwitchDuringExtraction()
  {
    QTemporaryDir dir;
    const auto vol1 = loadVolume(makeVolume(dir, QStringLiteral("m1.sgy")));
    const auto vol2 = loadVolume(makeVolume(dir, QStringLiteral("m2.sgy")));
    QVERIFY(vol1 && vol2);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol1); // 先体后测网（setActiveVolume 重建测网几何）
    link->setGridGeometry(validGrid());
    auto *d = dock();
    QSignalSpy done(d, &seismic::SeismicSectionDockWidget::sectionExtractionFinished);
    link->triggerSectionFromMapPolyline({{0, 0}, {100, 200}, {200, 200}},
                                        QStringLiteral("midflight"));
    link->setActiveVolume(vol2); // 在途即切
    QVERIFY(!d->hasRoute());
    QApplication::processEvents();
    QCOMPARE(d->volume().get(), vol2.get());
  }

  // 换层位后保存：剖面版本记录的是提取时捕获的层位（routeHorizon 快照语义）
  void horizonSnapshotSurvivesLaterSelectionChange()
  {
    QTemporaryDir dir;
    QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("p.qgz"))));
    const auto vol = loadVolume(makeVolume(dir, QStringLiteral("h.sgy")));
    QVERIFY(vol);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol); // 先体后测网（setActiveVolume 重建测网几何）
    link->setGridGeometry(validGrid());
    auto *d = dock();

    m_ctx->selection()->setActiveHorizon(QStringLiteral("T2"));
    QVERIFY(extractOnce(link, d));
    QVERIFY(d->hasRoute());
    m_ctx->selection()->setActiveHorizon(QStringLiteral("T4")); // 提取后换层位

    QMetaObject::invokeMethod(setup(), "saveRequested",
                              Q_ARG(QString, QStringLiteral("horizon-snap")));
    QApplication::processEvents();
    const auto sections = workbench()->savedSections();
    QVERIFY(!sections.isEmpty());
    bool found = false;
    for (const auto &s : sections)
    {
      const auto m = s.toMap();
      if (!m.value(QStringLiteral("name")).toString().startsWith(QStringLiteral("horizon-snap")))
        continue;
      found = true;
      // 层位快照落在 section.json 内容里（提取时捕获，不受后续选择影响）
      const auto v = m_ctx->importSvc()->catalog()->versionById(
          m.value(QStringLiteral("id")).toString());
      const QString path = DataCatalog::resolvedVersionPath(
          QFileInfo(m_ctx->projectSvc()->projectPath()).absolutePath(), v);
      QFile f(path);
      QVERIFY(f.open(QIODevice::ReadOnly));
      const auto doc = QJsonDocument::fromJson(f.readAll());
      QCOMPARE(doc.object().value(QStringLiteral("horizon")).toString(),
               QStringLiteral("T2"));
    }
    QVERIFY(found);
  }

  // 清除剖面连线：band 隐藏+清空、dock 路线复位；再展开 dock 不复活旧线。
  void clearRequestedResetsEverything()
  {
    QTemporaryDir dir;
    const auto vol = loadVolume(makeVolume(dir, QStringLiteral("c.sgy")));
    QVERIFY(vol);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol); // 先体后测网（setActiveVolume 重建测网几何）
    link->setGridGeometry(validGrid());
    auto *d = dock();
    QVERIFY(extractOnce(link, d));
    QVERIFY(d->hasRoute());
    auto *band = sectionBand(m_ctx->canvasCtl()->canvas());
    QVERIFY(band && band->isVisible());

    QMetaObject::invokeMethod(setup(), "clearRequested");
    QVERIFY(!d->hasRoute());
    QVERIFY(band->asGeometry().isEmpty());
    QVERIFY(!band->isVisible());

    d->show(); // dock 重新展开：路线已清，连线不复活
    QApplication::processEvents();
    QVERIFY(!band->isVisible());
  }

  // dock 显隐联动：收起 dock 连线隐藏；重新展开且有路线时恢复。
  void dockVisibilityTogglesBand()
  {
    QTemporaryDir dir;
    const auto vol = loadVolume(makeVolume(dir, QStringLiteral("d.sgy")));
    QVERIFY(vol);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol); // 先体后测网（setActiveVolume 重建测网几何）
    link->setGridGeometry(validGrid());
    auto *d = dock();
    QVERIFY(extractOnce(link, d));
    auto *band = sectionBand(m_ctx->canvasCtl()->canvas());
    QVERIFY(band && band->isVisible());

    d->hide();
    QTRY_VERIFY(!band->isVisible());
    d->show();
    QTRY_VERIFY(band->isVisible());
  }

  // 关/换工程：catalog 重开（path 变化）→ 活动体清空 + 对话框井表刷新
  void projectReopenClearsActiveVolume()
  {
    QTemporaryDir dir1;
    QTemporaryDir dir2;
    QVERIFY(m_ctx->projectSvc()->createProject(dir1.filePath(QStringLiteral("one.qgz"))));
    const auto vol = loadVolume(makeVolume(dir1, QStringLiteral("pr.sgy")));
    QVERIFY(vol);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol); // 先体后测网
    link->setGridGeometry(validGrid());
    QVERIFY(link->activeVolume());

    // 换工程（catalog 在新 path 重开 → changed → attachSections 检测 path 变化）
    QVERIFY(m_ctx->projectSvc()->createProject(dir2.filePath(QStringLiteral("two.qgz"))));
    QTRY_VERIFY_WITH_TIMEOUT(!link->activeVolume(), 5000);
  }

  // 绘制期间取消 / 失败：为绘制而隐藏的对话框被唤回；非绘制期收到事件不打扰
  void drawingCancelAndFailureRecallDialog()
  {
    QTemporaryDir dir;
    const auto vol = loadVolume(makeVolume(dir, QStringLiteral("e.sgy")));
    QVERIFY(vol);
    auto *link = m_ctx->seismicLink();
    link->setActiveVolume(vol); // 先体后测网
    link->setGridGeometry(validGrid());
    auto *s = setup();
    s->hide();
    QVERIFY(!s->isVisible());

    // 非绘制期的取消/失败：不强行唤回（drawingFromSetup=false 早退）
    QMetaObject::invokeMethod(link, "sectionCaptureCancelled");
    QMetaObject::invokeMethod(link, "sectionExtractedFromMap", Q_ARG(bool, false),
                              Q_ARG(QString, QStringLiteral("x")));
    QApplication::processEvents();
    QVERIFY(!s->isVisible());

    // 绘制请求 → 对话框隐藏（drawRequested 处理里 setup->hide()）
    QMetaObject::invokeMethod(s, "drawRequested");
    QApplication::processEvents();
    QVERIFY(!s->isVisible());
    // Esc 取消 → 唤回
    QMetaObject::invokeMethod(link, "sectionCaptureCancelled");
    QApplication::processEvents();
    QVERIFY(s->isVisible());

    // 再次进入绘制 → 地图提取失败 → 唤回 + 消息
    s->hide();
    QMetaObject::invokeMethod(s, "drawRequested");
    QMetaObject::invokeMethod(link, "sectionExtractedFromMap", Q_ARG(bool, false),
                              Q_ARG(QString, QStringLiteral("提取失败：模拟错误")));
    QApplication::processEvents();
    QVERIFY(s->isVisible());
    s->hide();
  }

  // Empirical stress-testing: SeismicSectionTool activation/deactivation and map tool switching
  void seismicSectionToolActivationAndSwitchingStress()
  {
    // 1. Tool constructed with nullptr canvas
    {
      auto *orphanTool = new SeismicSectionTool(nullptr);
      QVERIFY(orphanTool->canvas() == nullptr);
      // QgsMapTool::activate() dereferences canvas()->setCursor(), so orphan tool cannot be activated
      delete orphanTool;
    }

    // 2. Map tool switching and activation cycling on real canvas
    auto *canvas = m_ctx->canvasCtl()->canvas();
    QVERIFY(canvas != nullptr);

    auto *toolA = new SeismicSectionTool(canvas);
    auto *toolB = new SeismicSectionTool(canvas);

    QSignalSpy cancelSpy(toolA, &SeismicSectionTool::captureCancelled);
    QSignalSpy pathSpy(toolA, &SeismicSectionTool::sectionPathCaptured);

    // Rapid activation/deactivation cycling
    for (int i = 0; i < 50; ++i)
    {
      toolA->activate();
      toolA->deactivate();
    }

    // Tool switching via canvas
    canvas->setMapTool(toolA);
    QCOMPARE(canvas->mapTool(), toolA);

    // Switch away to toolB
    canvas->setMapTool(toolB);
    QCOMPARE(canvas->mapTool(), toolB);

    // Switch back to toolA
    canvas->setMapTool(toolA);
    QCOMPARE(canvas->mapTool(), toolA);

    // Send Escape key event to trigger captureCancelled and unsetMapTool
    QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escEvent);
    // Directly invoking keyPressEvent on tool to verify signal
    QMetaObject::invokeMethod(toolA, "keyPressEvent", Q_ARG(QKeyEvent *, &escEvent));
    QCOMPARE(cancelSpy.count(), 1);

    // Clean up
    canvas->unsetMapTool(toolA);
    canvas->unsetMapTool(toolB);
    delete toolA;
    delete toolB;
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");
  TestSectionLifecycle tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}
#include "tst_sectionlifecycle.moc"
