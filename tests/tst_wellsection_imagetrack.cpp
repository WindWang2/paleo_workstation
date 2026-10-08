// tests/tst_wellsection_imagetrack.cpp — 方向 79 剖面图片道行为验收：
//   · 缓存键假命中回归：imageVersion + 256（旧 8 位截断键的碰撞点）后
//     换图重画必须显示新图（代际失效语义）
//   · LOD 全载触发：lod=1 画缩略级；放大（lod>缩略宽×系数/道宽）触发
//     原图级（从磁盘全载，EXIF 同口径）
//   · 图片锚双击 → 激活回调（井 id + 锚信息——面板编辑入口）
#include <QtTest>
#include <QApplication>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QTemporaryDir>

#include "../src/domain/wellsection.h"
#include "../src/services/imagelod.h"
#include "../src/ui/wellsection/wellsectionscene.h"

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

  // 直接渲染列项（无场景）：返回画布。worldScale 模拟视图缩放（LOD 因子）
  // ——画布随缩放扩，内容画在同一逻辑位置。
  QImage render(ColumnItem *col, double worldScale)
  {
    QImage canvas(qRound((st.columnRight(0) + 2.0) * worldScale),
                  qRound((st.sceneHeight() + 2.0) * worldScale),
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);
    QPainter p(&canvas);
    if (worldScale != 1.0)
      p.setTransform(QTransform::fromScale(worldScale, worldScale));
    QStyleOptionGraphicsItem opt;
    opt.exposedRect = col->boundingRect();
    opt.rect = col->boundingRect().toRect();
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
      paleo::imagelod::FullImageCache::shared().clear();

      // lod=1：可见宽 106 < 32×2 → 缩略级（蓝）。
      QImage frame1 = fx.render(&col, 1.0);
      QRgb c1 = frame1.pixel(fx.anchorCenter().toPoint());
      QVERIFY(qBlue(c1) > qRed(c1) + 60);

      // 放大 ×2：可见宽 212 > 32×2 → 原图级（黄，从磁盘全载）。画布随
      // 缩放扩（内容同一位置——列项无平移）。
      QImage frame2 = fx.render(&col, 2.0);
      QRgb c2 = frame2.pixel((fx.anchorCenter() * 2.0).toPoint());
      QVERIFY2(qRed(c2) > qBlue(c2) + 60 && qGreen(c2) > qBlue(c2) + 60,
               qPrintable(QStringLiteral("放大后应全载原图（黄），实得 RGB=(%1,%2,%3)")
                              .arg(qRed(c2))
                              .arg(qGreen(c2))
                              .arg(qBlue(c2))));
      QVERIFY(paleo::imagelod::FullImageCache::shared().size() > 0); // LRU 驻留
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
