// tests/tst_imagelod.cpp — 方向 79 图片道装载 LOD 验收：
//   · 两级装载：缩略（解码期降采样，最长边 ≤256、只缩不放）+ fullSize 几何
//   · 内存比率门：200 张 1024² JPEG，Σ sizeInBytes（LOD）/ Σ（全分辨率）
//     < 0.10（比率口径——禁绝对 MB 墙；系数见 ledger）
//   · EXIF Orientation：手工注入 APP1（tag 0x0112=6）的样张在装载路径
//     正立（尺寸互换 + 像素采样）
//   · FullImageCache LRU：8 张封顶、失败装载不占额
//   · 透明 PNG 棋盘底笔刷
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QImageWriter>
#include <QTemporaryDir>

#include "../src/services/imagelod.h"

#include <cmath>

using namespace paleo::imagelod;

namespace
{

// 1024×1024 半分异色 JPEG（左红右蓝）——异色供 EXIF 旋转后采样判向。
QString writeJpegHalves(const QString &path, int w, int h)
{
  QImage img(w, h, QImage::Format_RGB32);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      img.setPixel(x, y, x < w / 2 ? qRgb(220, 20, 20) : qRgb(20, 20, 220));
  return img.save(path, "JPEG", 90) ? path : QString();
}

// EXIF Orientation 样张：QImageWriter::setTransformation 让 JPEG 写侧落
// orientation 标签（等价相机旋转记录）——不用手工拼 APP1 字节（脆弱且
// 依赖 Qt 内部解析容错）。存储像素仍是 400×200 异色半。
QString writeExifRotatedJpeg(const QString &path)
{
  QImage img(400, 200, QImage::Format_RGB32);
  for (int y = 0; y < 200; ++y)
    for (int x = 0; x < 400; ++x)
      img.setPixel(x, y, x < 200 ? qRgb(220, 20, 20) : qRgb(20, 20, 220));
  QImageWriter writer(path, QByteArray("JPEG"));
  writer.setTransformation(QImageIOHandler::TransformationRotate90);
  writer.setQuality(90);
  return writer.write(img) ? path : QString();
}

} // namespace

class TestImageLod : public QObject
{
  Q_OBJECT
  private slots:
    void thumbnailGeometryAndNoUpscale()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString wide =
          writeJpegHalves(dir.filePath(QStringLiteral("wide.jpg")), 800, 400);
      QVERIFY(!wide.isEmpty());
      const TrackImage ti = loadThumbnail(wide);
      QVERIFY(!ti.isNull());
      QCOMPARE(ti.fullSize, QSize(800, 400));
      QVERIFY(ti.thumbnail.width() <= 256 && ti.thumbnail.height() <= 256);
      QCOMPARE(ti.thumbnail.size(), QSize(256, 128)); // 等比、最长边贴边
      QVERIFY(!ti.hasAlpha);

      // 小图只缩不放：64×64 保持原尺寸。
      const QString small =
          writeJpegHalves(dir.filePath(QStringLiteral("small.jpg")), 64, 64);
      QVERIFY(!small.isEmpty());
      const TrackImage ts = loadThumbnail(small);
      QVERIFY(!ts.isNull());
      QCOMPARE(ts.thumbnail.size(), QSize(64, 64));
      QCOMPARE(ts.fullSize, QSize(64, 64));

      // 竖长图：最长边贴 256。
      const QString tall =
          writeJpegHalves(dir.filePath(QStringLiteral("tall.jpg")), 300, 900);
      QVERIFY(!tall.isEmpty());
      const TrackImage tt = loadThumbnail(tall);
      QCOMPARE(tt.thumbnail.size(), QSize(85, 256));

      // 坏路径：空位图如实回（调用方告警）。
      QVERIFY(loadThumbnail(dir.filePath(QStringLiteral("nope.jpg"))).isNull());
    }

    void memoryRatioGate200()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QDir d(dir.path());
      QVERIFY(d.mkdir(QStringLiteral("shots")));
      const int count = 200;
      QStringList files;
      for (int i = 0; i < count; ++i)
      {
        const QString p = writeJpegHalves(
            d.filePath(QStringLiteral("shots/photo_%1.jpg").arg(i, 3, 10, QChar('0'))),
            1024, 1024);
        QVERIFY(!p.isEmpty());
        files << p;
      }
      // 比率口径（ledger）：内存峰值代理 = 逐张装载的字节和（LOD 常驻面
      // vs 全分辨率常驻面）。基线不驻留（逐张弃置），只累计字节。
      qint64 fullBytes = 0;
      for (const QString &f : files)
      {
        const QImage img(f);
        QVERIFY(!img.isNull());
        fullBytes += img.sizeInBytes();
      }
      qint64 lodBytes = 0;
      for (const QString &f : files)
      {
        const TrackImage ti = loadThumbnail(f);
        QVERIFY(!ti.isNull());
        lodBytes += ti.thumbnail.sizeInBytes();
      }
      QVERIFY(fullBytes > 0);
      const double ratio = double(lodBytes) / double(fullBytes);
      QVERIFY2(ratio < 0.10,
               qPrintable(QStringLiteral("LOD/全分辨率 = %1（门 0.10；"
                                         "理论 256²/1024²≈0.0625）")
                              .arg(ratio, 0, 'f', 4)));
    }

    void exifOrientationApplied()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString rotated = writeExifRotatedJpeg(
          dir.filePath(QStringLiteral("oriented6.jpg")));
      QVERIFY2(!rotated.isEmpty(), "EXIF 样张应可写出（QImageWriter 落 orientation）");

      const TrackImage ti = loadThumbnail(rotated);
      QVERIFY2(!ti.isNull(), "EXIF 样张应可解码");
      // Orientation 6（rotate 90 CW 显示）：存储 400×200 → 显示 200×400。
      QCOMPARE(ti.fullSize, QSize(200, 400));
      QVERIFY(ti.thumbnail.width() <= ti.thumbnail.height());
      // 像素采样：存储图的左右半（红/蓝）旋转后成为上下半。JPEG 有损，
      // 用通道主导判色。
      const QImage &img = ti.thumbnail;
      const auto isRedish = [](QRgb c) { return qRed(c) > qBlue(c) + 60; };
      const auto isBlueish = [](QRgb c) { return qBlue(c) > qRed(c) + 60; };
      QVERIFY2(isRedish(img.pixel(img.width() / 2, 1)),
               "旋转后顶部应为红半（存储图左半）");
      QVERIFY2(isBlueish(img.pixel(img.width() / 2, img.height() - 2)),
               "旋转后底部应为蓝半（存储图右半）");
    }

    void fullImageCacheLru()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QStringList files;
      for (int i = 0; i < 10; ++i)
      {
        const QString p = writeJpegHalves(
            dir.filePath(QStringLiteral("c%1.jpg").arg(i)), 64, 64);
        QVERIFY(!p.isEmpty());
        files << p;
      }
      FullImageCache &cache = FullImageCache::shared();
      cache.clear();
      for (int i = 0; i < LodPolicy::kFullCacheEntries; ++i)
        QVERIFY(!cache.acquire(QStringLiteral("k%1").arg(i), files.at(i)).isNull());
      QCOMPARE(cache.size(), LodPolicy::kFullCacheEntries);
      // 第 9 张进 → 最旧 k0 逐出。
      QVERIFY(!cache.acquire(QStringLiteral("k9"), files.at(9)).isNull());
      QCOMPARE(cache.size(), LodPolicy::kFullCacheEntries);
      QVERIFY(cache.peek(QStringLiteral("k0")).isNull());
      QVERIFY(!cache.peek(QStringLiteral("k9")).isNull());
      // 坏路径装载失败不占额。
      QVERIFY(cache.acquire(QStringLiteral("bad"),
                            dir.filePath(QStringLiteral("nope.jpg")))
                  .isNull());
      QCOMPARE(cache.size(), LodPolicy::kFullCacheEntries);
      cache.clear();
      QCOMPARE(cache.size(), 0);
    }

    void alphaCheckerboardPattern()
    {
      const QBrush brush = alphaCheckerboard(6);
      QVERIFY(!brush.textureImage().isNull());
      QCOMPARE(brush.textureImage().size(), QSize(12, 12));
      // (0,0) 与 (6,0) 异色、(0,0) 与 (0,6) 异色——棋盘格。
      const QImage tile = brush.textureImage();
      QVERIFY(tile.pixelColor(0, 0) != tile.pixelColor(6, 0));
      QVERIFY(tile.pixelColor(0, 0) != tile.pixelColor(0, 6));
      QVERIFY(tile.pixelColor(0, 0) == tile.pixelColor(6, 6));
    }
};

QTEST_MAIN(TestImageLod)
#include "tst_imagelod.moc"
