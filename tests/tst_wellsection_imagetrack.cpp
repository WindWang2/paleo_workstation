// tests/tst_wellsection_imagetrack.cpp — 方向 79 剖面图片道行为验收：
//   · 缓存键假命中回归：imageVersion + 256（旧 8 位截断键的碰撞点）后
//     换图重画必须显示新图（代际失效语义）
//   · LOD 全载：lod=1 画缩略；放大要原图，但绘制线程不得 loadFull/acquire
//     （后台解码，回到 GUI 再 fromImage）
//   · 原图 QPixmap 与 FullImageCache 同一 8 张上限
//   · 图片锚双击 → 激活回调（井 id + 锚信息——面板编辑入口）
#include <QtTest>
#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QSemaphore>
#include <QStyleOptionGraphicsItem>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include "../src/domain/wellsection.h"
#include "../src/services/imagelod.h"
#include "../src/ui/wellsection/wellsectionscene.h"

#include <atomic>
#include <cmath>

using namespace wellsection;
using namespace wellsectionui;

namespace
{

// 800×400 纯色 PNG（磁盘原图——LOD 原图级来源）。
QString writePng(const QString &path, QRgb color, int w = 800, int h = 400)
{
  QImage img(w, h, QImage::Format_RGB32);
  img.fill(color);
  return img.save(path, "PNG") ? path : QString();
}

struct TrackFixture
{
  RenderState st;
  QString path;
  double anchorMd = 50.0;

  // 一井一锚 + 单图片道（110px）；窗口 [0,100]m、4px/m。
  void build(const QImage &thumbnail, const QString &diskPath)
  {
    st.wells.clear();
    Well w;
    w.id = QStringLiteral("well-1");
    w.name = QStringLiteral("A1");
    ImageAnchor a;
    a.md = anchorMd;
    a.caption = QStringLiteral("shot.png");
    a.image = thumbnail;
    a.assetId = QStringLiteral("ast-1");
    a.path = diskPath;
    a.fullSize = QSize(800, 400);
    w.images = {a};
    st.wells = {w};
    st.offsets = {0.0};
    st.window = {0.0, 100.0};
    st.pxPerMeter = 4.0;
    TrackSpec tr;
    tr.kind = TrackKind::Image;
    tr.width = 110;
    st.tpl.tracks = {tr};
    st.theme = SectionTheme::byId(QStringLiteral("classic"));
    path = diskPath;
  }

  // 直接渲染列项：返回画布。worldScale 模拟视图缩放（LOD 因子）
  // ——画布随缩放扩，内容画在同一逻辑位置。exposed 空 = 整列。
  QImage render(ColumnItem *col, double worldScale,
                const QRectF &exposed = QRectF())
  {
    QImage canvas(qRound((st.columnRight(0) + 2.0) * worldScale),
                  qRound((st.sceneHeight() + 2.0) * worldScale),
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);
    QPainter p(&canvas);
    if (worldScale != 1.0)
      p.setTransform(QTransform::fromScale(worldScale, worldScale));
    const QRectF bounds = col->boundingRect();
    const QRectF exp = exposed.isNull() ? bounds : exposed;
    QStyleOptionGraphicsItem opt;
    opt.exposedRect = exp;
    opt.rect = exp.toRect();
    col->paint(&p, &opt, nullptr);
    p.end();
    return canvas;
  }

  // 图片锚绘制矩形中心（item/scene 同坐标系——列项无变换）。
  QPointF anchorCenter() const
  {
    const double tw = 110 - 4.0;
    const double y = st.yForMd(0, anchorMd);
    return QPointF(st.columnLeft(0) + 2.0 + tw / 2.0, y + 4.0);
  }
};

// 解码回调要求场景和视图都还在。项本身留在栈上，析构前先移出场景。
struct SceneAttachment
{
  QGraphicsScene scene;
  QGraphicsView view;
  QGraphicsItem *item = nullptr;
  explicit SceneAttachment(QGraphicsItem *it) : view(&scene), item(it)
  {
    scene.addItem(item);
  }
  ~SceneAttachment()
  {
    if (item && item->scene() == &scene)
      scene.removeItem(item);
  }
};

struct LoadHookGuard
{
  QSemaphore gate;
  ~LoadHookGuard()
  {
    gate.release(64);
    QThreadPool::globalInstance()->waitForDone(5000);
    paleo::imagelod::setFullLoadHook({});
  }
};

} // namespace

// mouseDoubleClickEvent 是 protected——测试子类放开（只测回调语义）。
class TestColumn : public ColumnItem
{
  public:
    using ColumnItem::ColumnItem;
    void sendDoubleClick(const QPointF &itemPos)
    {
        QGraphicsSceneMouseEvent e;
        e.setPos(itemPos);
        e.setButton(Qt::LeftButton);
        e.setButtons(Qt::LeftButton);
        mouseDoubleClickEvent(&e);
    }
};

class TestWellSectionImageTrack : public QObject
{
  Q_OBJECT
  private slots:
    void cacheKeySurvivesVersionWraparound()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString redPath = writePng(dir.filePath(QStringLiteral("a.png")),
                                       qRgb(200, 20, 20));
      QVERIFY(!redPath.isEmpty());
      QImage thumb(40, 20, QImage::Format_RGB32);
      thumb.fill(qRgb(200, 20, 20));

      TrackFixture fx;
      fx.build(thumb, redPath);
      TestColumn col(&fx.st, 0);

      // 基线帧：红缩略。
      QImage frame1 = fx.render(&col, 1.0);
      QRgb c1 = frame1.pixel(fx.anchorCenter().toPoint());
      QVERIFY(qRed(c1) > qBlue(c1) + 60);

      // 版本 +256（旧 8 位键的碰撞点）并换绿缩略——重画必须绿。
      // 旧实现 (v & 0xFF) 键在 v+256 处假命中 → 仍画红。
      fx.st.imageVersion += 256;
      QImage green(40, 20, QImage::Format_RGB32);
      green.fill(qRgb(20, 200, 20));
      fx.st.wells[0].images[0].image = green;
      QImage frame2 = fx.render(&col, 1.0);
      QRgb c2 = frame2.pixel(fx.anchorCenter().toPoint());
      QVERIFY2(qGreen(c2) > qRed(c2) + 60,
               qPrintable(QStringLiteral("v+256 后应画新图（绿），实得 RGB=(%1,%2,%3)")
                              .arg(qRed(c2))
                              .arg(qGreen(c2))
                              .arg(qBlue(c2))));
    }

    void lodFullLoadOnZoomIn()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      // 磁盘原图黄、内存缩略蓝——两级来源可区分。
      const QString diskPath = writePng(dir.filePath(QStringLiteral("b.png")),
                                        qRgb(220, 220, 20));
      QVERIFY(!diskPath.isEmpty());
      QImage thumb(32, 16, QImage::Format_RGB32);
      thumb.fill(qRgb(20, 20, 220));

      TrackFixture fx;
      fx.build(thumb, diskPath);
      TestColumn col(&fx.st, 0);
      // 回调只在场景和视图都还在时 fromImage。
      SceneAttachment attached(&col);
      paleo::imagelod::FullImageCache::shared().clear();

      LoadHookGuard guard;
      std::atomic<int> guiLoads{0};
      std::atomic<int> workerLoads{0};
      QThread *gui = QThread::currentThread();
      paleo::imagelod::setFullLoadHook([&] {
        if (QThread::currentThread() == gui)
        {
          guiLoads.fetch_add(1);
          return; // 绘制线程若误入 loadFull，不能在这里把 GUI 卡住
        }
        workerLoads.fetch_add(1);
        guard.gate.acquire();
      });

      // lod=1：可见宽 106 < 32×2 → 缩略级（蓝），不读原图。
      QImage frame1 = fx.render(&col, 1.0);
      QRgb c1 = frame1.pixel(fx.anchorCenter().toPoint());
      QVERIFY(qBlue(c1) > qRed(c1) + 60);
      QCOMPARE(guiLoads.load(), 0);
      QCOMPARE(workerLoads.load(), 0);

      // 放大 ×2：可见宽 212 > 32×2 → 要原图，但本帧仍画缩略（蓝）。
      // loadFull/acquire 不得跑在调用线程上；在途时再画一帧不得再开解码。
      QImage frame2 = fx.render(&col, 2.0);
      QRgb c2 = frame2.pixel((fx.anchorCenter() * 2.0).toPoint());
      QVERIFY2(qBlue(c2) > qRed(c2) + 60,
               qPrintable(QStringLiteral("放大首帧不得同步全载（应仍为缩略蓝），"
                                         "实得 RGB=(%1,%2,%3)")
                              .arg(qRed(c2))
                              .arg(qGreen(c2))
                              .arg(qBlue(c2))));
      QCOMPARE(guiLoads.load(), 0);
      QTRY_COMPARE(workerLoads.load(), 1);
      QImage frameAgain = fx.render(&col, 2.0);
      QTest::qWait(80);
      QCOMPARE(workerLoads.load(), 1);
      QCOMPARE(guiLoads.load(), 0);
      QRgb cAgain = frameAgain.pixel((fx.anchorCenter() * 2.0).toPoint());
      QVERIFY(qBlue(cAgain) > qRed(cAgain) + 60);

      guard.gate.release(1);
      QTRY_COMPARE(col.retainedFullPixmapCount(), 1);
      QCOMPARE(guiLoads.load(), 0);
      QCOMPARE(workerLoads.load(), 1);
      QVERIFY(paleo::imagelod::FullImageCache::shared().size() > 0);

      // 回到 GUI 之后的一帧才是磁盘原图（黄）。
      QImage frame3 = fx.render(&col, 2.0);
      QRgb c3 = frame3.pixel((fx.anchorCenter() * 2.0).toPoint());
      QVERIFY2(qRed(c3) > qBlue(c3) + 60 && qGreen(c3) > qBlue(c3) + 60,
               qPrintable(QStringLiteral("后台解码回到 GUI 后应画原图（黄），"
                                         "实得 RGB=(%1,%2,%3)")
                              .arg(qRed(c3))
                              .arg(qGreen(c3))
                              .arg(qBlue(c3))));
      QCOMPARE(workerLoads.load(), 1);
      QCOMPARE(guiLoads.load(), 0);
      paleo::imagelod::FullImageCache::shared().clear();
    }

    void fullPixmapCacheCapsAtEight()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      constexpr int n = paleo::imagelod::LodPolicy::kFullCacheEntries + 1;
      QStringList paths;
      QVector<wellsection::ImageAnchor> anchors;
      QImage thumb(32, 16, QImage::Format_RGB32);
      thumb.fill(qRgb(20, 20, 220));
      for (int i = 0; i < n; ++i)
      {
        const QString path = writePng(
            dir.filePath(QStringLiteral("cap%1.png").arg(i)), qRgb(220, 220, 20));
        QVERIFY(!path.isEmpty());
        paths << path;
        wellsection::ImageAnchor a;
        a.md = 2.5 + i * 10.0; // y = 10 + i*40，列高 400 内可按 y 切开
        a.caption = QStringLiteral("c.png");
        a.image = thumb;
        a.assetId = QStringLiteral("ast-%1").arg(i);
        a.path = path;
        a.fullSize = QSize(800, 400);
        anchors.append(a);
      }

      TrackFixture fx;
      fx.build(thumb, paths.at(0));
      fx.st.wells[0].images = anchors;
      TestColumn col(&fx.st, 0);
      SceneAttachment attached(&col);
      paleo::imagelod::FullImageCache::shared().clear();

      LoadHookGuard guard;
      std::atomic<int> guiLoads{0};
      std::atomic<int> workerLoads{0};
      QThread *gui = QThread::currentThread();
      paleo::imagelod::setFullLoadHook([&] {
        if (QThread::currentThread() == gui)
          guiLoads.fetch_add(1);
        else
          workerLoads.fetch_add(1);
      });

      const QRectF bounds = col.boundingRect();
      // 九张同时可见：只解码并留下 8 张原图，再画不得加解码。
      fx.render(&col, 2.0);
      QCOMPARE(guiLoads.load(), 0);
      QTRY_COMPARE(col.retainedFullPixmapCount(),
                   paleo::imagelod::LodPolicy::kFullCacheEntries);
      QCOMPARE(workerLoads.load(), paleo::imagelod::LodPolicy::kFullCacheEntries);
      QCOMPARE(guiLoads.load(), 0);
      fx.render(&col, 2.0);
      QTest::qWait(80);
      QCOMPARE(workerLoads.load(), paleo::imagelod::LodPolicy::kFullCacheEntries);
      QCOMPARE(col.retainedFullPixmapCount(),
               paleo::imagelod::LodPolicy::kFullCacheEntries);

      // 露出第 9 张、藏起第 0 张：新的一张进缓存，不可见的第 0 张被淘汰。
      const QRectF slide(bounds.left(), 100.0, bounds.width(), bounds.height() - 100.0);
      fx.render(&col, 2.0, slide);
      const QString key8 = QStringLiteral("%1|%2").arg(
          paths.at(8), QString::number(fx.st.imageVersion));
      QTRY_VERIFY(!paleo::imagelod::FullImageCache::shared().peek(key8).isNull());
      QCOMPARE(workerLoads.load(), paleo::imagelod::LodPolicy::kFullCacheEntries + 1);
      QCOMPARE(col.retainedFullPixmapCount(),
               paleo::imagelod::LodPolicy::kFullCacheEntries);
      QCOMPARE(guiLoads.load(), 0);

      // 清掉 QImage LRU（不动 QPixmap）。第 0 张若已被淘汰，必须再解码一次；
      // 驻留张数仍是 8。调用线程仍然不读盘。
      paleo::imagelod::FullImageCache::shared().clear();
      const QRectF only0(bounds.left(), 0.0, bounds.width(), 25.0);
      fx.render(&col, 2.0, only0);
      QTRY_COMPARE(workerLoads.load(),
                   paleo::imagelod::LodPolicy::kFullCacheEntries + 2);
      QCOMPARE(col.retainedFullPixmapCount(),
               paleo::imagelod::LodPolicy::kFullCacheEntries);
      QCOMPARE(guiLoads.load(), 0);
      paleo::imagelod::FullImageCache::shared().clear();
    }

    void doubleClickActivatesAnchor()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString p = writePng(dir.filePath(QStringLiteral("c.png")),
                                 qRgb(120, 120, 120));
      QVERIFY(!p.isEmpty());
      QImage thumb(40, 20, QImage::Format_RGB32);
      thumb.fill(qRgb(120, 120, 120));

      TrackFixture fx;
      fx.build(thumb, p);
      TestColumn col(&fx.st, 0);

      QString gotWell, gotAsset;
      double gotMd = 0.0;
      col.setImageActivateCallback(
          [&](const QString &wellId, const ImageAnchor &a) {
            gotWell = wellId;
            gotAsset = a.assetId;
            gotMd = a.md;
          });

      // 先画一帧建立命中矩形，再双击矩形中心。
      fx.render(&col, 1.0);
      col.sendDoubleClick(fx.anchorCenter());
      QCOMPARE(gotWell, QStringLiteral("well-1"));
      QCOMPARE(gotAsset, QStringLiteral("ast-1"));
      QCOMPARE(gotMd, 50.0);

      // 空白处双击不激活（列其它交互不受图片道影响）。
      gotWell.clear();
      col.sendDoubleClick(QPointF(fx.st.columnLeft(0) + 3.0, 5.0));
      QVERIFY(gotWell.isEmpty());
    }
};

QTEST_MAIN(TestWellSectionImageTrack)
#include "tst_wellsection_imagetrack.moc"
