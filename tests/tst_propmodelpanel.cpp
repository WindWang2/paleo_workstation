// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QOffscreenSurface>
#include <QProgressBar>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QPainter>
#include <QSignalSpy>

#include "../src/ui/propertymodel/propertymodelpanel.h"
#include "../src/ui/seismic3d/seismic3dviewportwidget.h"
#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismicsection/seismicsectioncanvas.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace seismic;

namespace
{

SgySliceImage makeSection(int traces, int samples)
{
  SgySliceImage img;
  img.width = traces;
  img.height = samples;
  img.valueMin = -1.0f;
  img.valueMax = 1.0f;
  img.values.assign(static_cast<std::size_t>(traces * samples), 0.1f);
  return img;
}

SgySliceImage makeZone(int traces, int samples, float value, bool withNan)
{
  SgySliceImage img;
  img.width = traces;
  img.height = samples;
  img.valueMin = 0.0f;
  img.valueMax = 1.0f;
  img.values.assign(static_cast<std::size_t>(traces * samples), value);
  if (withNan)
    img.values[0] = std::numeric_limits<float>::quiet_NaN();
  return img;
}

QImage renderCanvas(SeismicSectionCanvas &canvas)
{
  QImage img(canvas.size(), QImage::Format_ARGB32);
  img.fill(Qt::white);
  QPainter painter(&img);
  canvas.render(&painter);
  return img;
}

qint64 imageDiff(const QImage &a, const QImage &b)
{
  if (a.size() != b.size())
    return -1;
  qint64 diff = 0;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x)
      diff += qAbs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
  return diff;
}

SgySliceImage solidRgba(int w, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
  SgySliceImage img;
  img.width = w;
  img.height = h;
  img.valueMin = 0;
  img.valueMax = 1;
  img.values.assign(static_cast<std::size_t>(w * h), 1.0f);
  img.rgba.assign(static_cast<std::size_t>(w * h * 4), 0);
  for (int i = 0; i < w * h; ++i)
  {
    img.rgba[static_cast<std::size_t>(i * 4 + 0)] = r;
    img.rgba[static_cast<std::size_t>(i * 4 + 1)] = g;
    img.rgba[static_cast<std::size_t>(i * 4 + 2)] = b;
    img.rgba[static_cast<std::size_t>(i * 4 + 3)] = a;
  }
  img.rgba[3] = 0; // 一角 NaN：alpha 0，着色器丢弃
  img.values[0] = std::numeric_limits<float>::quiet_NaN();
  return img;
}

} // namespace

class TestPropModelPanel : public QObject
{
  Q_OBJECT
private slots:
  void intentAndBusyGate();
  void zoneOverlayNanIsTransparent();
  void viewportQueuesPropertySlice();
  void propertySliceUploadsOnGl();
};

void TestPropModelPanel::intentAndBusyGate()
{
  PropertyModelPanel panel;
  QCOMPARE(panel.layerCount(), 10);
  QCOMPARE(panel.aggregator(), 1);
  QCOMPARE(panel.idwPower(), 2.0);
  QCOMPARE(panel.overlayAlpha(), 0.65);
  QCOMPARE(panel.curveMnemonic(), QStringLiteral("GR"));
  auto *build = panel.findChild<QToolButton *>(QStringLiteral("propBuildButton"));
  auto *cancel = panel.findChild<QToolButton *>(QStringLiteral("propCancelButton"));
  QVERIFY(build && cancel);
  QVERIFY(!build->isEnabled()); // 顶底面还空
  QVERIFY(!cancel->isEnabled());

  panel.findChild<QLineEdit *>(QStringLiteral("propTopEdit"))->setText(QStringLiteral("C3"));
  panel.findChild<QLineEdit *>(QStringLiteral("propBotEdit"))->setText(QStringLiteral("C3"));
  QVERIFY(!build->isEnabled()); // 顶底同名，不发意图
  panel.findChild<QLineEdit *>(QStringLiteral("propBotEdit"))->setText(QStringLiteral("D72"));
  QVERIFY(build->isEnabled());

  panel.findChild<QSpinBox *>(QStringLiteral("propLayersSpin"))->setValue(20);
  auto *agg = panel.findChild<QComboBox *>(QStringLiteral("propAggCombo"));
  agg->setCurrentIndex(agg->findData(3));

  QSignalSpy spy(&panel, &PropertyModelPanel::buildRequested);
  QSignalSpy alphaSpy(&panel, &PropertyModelPanel::alphaChanged);
  QSignalSpy cancelSpy(&panel, &PropertyModelPanel::cancelRequested);
  panel.findChild<QSlider *>(QStringLiteral("propAlphaSlider"))->setValue(40);
  build->click();
  QCOMPARE(spy.count(), 1);
  const QVariantList args = spy.takeFirst();
  QCOMPARE(args.at(0).toString(), QStringLiteral("C3"));
  QCOMPARE(args.at(1).toString(), QStringLiteral("D72"));
  QCOMPARE(args.at(2).toString(), QStringLiteral("GR"));
  QCOMPARE(args.at(3).toInt(), 20);
  QCOMPARE(args.at(4).toInt(), 3);
  QCOMPARE(args.at(5).toDouble(), 2.0);
  QCOMPARE(args.at(6).toDouble(), 0.4);
  QCOMPARE(alphaSpy.count(), 1);

  panel.setBusy(true);
  QVERIFY(!build->isEnabled());
  QVERIFY(cancel->isEnabled());
  cancel->click();
  QCOMPARE(cancelSpy.count(), 1);
  panel.updateProgress(40, QStringLiteral("充填"));
  QCOMPARE(panel.findChild<QProgressBar *>(QStringLiteral("propProgress"))->value(), 40);
  panel.showResult(true, QStringLiteral("800000 单元"));
  QVERIFY(!panel.isBusy());
  QVERIFY(build->isEnabled());
  QCOMPARE(panel.findChild<QLabel *>(QStringLiteral("propStatus"))->text(),
           QStringLiteral("800000 单元"));
}

void TestPropModelPanel::zoneOverlayNanIsTransparent()
{
  SeismicSectionCanvas canvas;
  canvas.resize(320, 240);
  canvas.setSectionData(makeSection(16, 12), 2.0f);
  const QImage baseline = renderCanvas(canvas);

  canvas.setZoneOverlay(makeZone(16, 12, std::numeric_limits<float>::quiet_NaN(), false));
  QVERIFY(canvas.hasZoneOverlay());
  QVERIFY(imageDiff(baseline, renderCanvas(canvas)) < 50);

  canvas.setZoneOverlay(makeZone(16, 12, 1.0f, true));
  QVERIFY(imageDiff(baseline, renderCanvas(canvas)) > 500);

  canvas.setColorMap(SectionColorMapType::Grayscale);
  QVERIFY(canvas.hasZoneOverlay());
  canvas.setColorMapInverted(true);
  QVERIFY(canvas.hasZoneOverlay());
  canvas.setColorMapInverted(false);
  canvas.setColorMap(SectionColorMapType::RedWhiteBlue);

  canvas.setZoneOverlay(makeZone(3, 3, 1.0f, false));
  QVERIFY(canvas.hasZoneOverlay()); // 失配被忽略，上一层还在

  canvas.setSectionData(makeSection(8, 8), 2.0f);
  QVERIFY(!canvas.hasZoneOverlay());

  canvas.setSectionData(makeSection(16, 12), 2.0f);
  canvas.setZoneOverlay(makeZone(16, 12, 0.5f, false));
  QVERIFY(canvas.hasZoneOverlay());
  canvas.clearData();
  QVERIFY(!canvas.hasZoneOverlay());

  canvas.setSectionData(makeSection(16, 12), 2.0f);
  canvas.setZoneOverlay(makeZone(16, 12, 0.5f, false));
  canvas.setTimeSliceData(makeSection(16, 12), 120.0, 1, 12, 1, 16);
  QVERIFY(canvas.hasZoneOverlay());
  canvas.setTimeSliceData(makeSection(8, 6), 120.0, 1, 6, 1, 8);
  QVERIFY(!canvas.hasZoneOverlay());

  // 底图全是 NaN（不走 LUT）。换色标后叠层像素变了，说明是重烘焙而不是丢掉。
  SeismicSectionCanvas lutCanvas;
  lutCanvas.resize(320, 240);
  SgySliceImage nanBase = makeSection(16, 12);
  nanBase.values.assign(nanBase.values.size(), std::numeric_limits<float>::quiet_NaN());
  lutCanvas.setSectionData(nanBase, 2.0f);
  lutCanvas.setZoneOverlay(makeZone(16, 12, 1.0f, false));
  const QImage beforeLut = renderCanvas(lutCanvas);
  lutCanvas.setColorMap(SectionColorMapType::Grayscale);
  QVERIFY(lutCanvas.hasZoneOverlay());
  QVERIFY(imageDiff(beforeLut, renderCanvas(lutCanvas)) > 100);
  const QImage gray = renderCanvas(lutCanvas);
  lutCanvas.setColorMapInverted(true);
  QVERIFY(lutCanvas.hasZoneOverlay());
  QVERIFY(imageDiff(gray, renderCanvas(lutCanvas)) > 100);
}

void TestPropModelPanel::viewportQueuesPropertySlice()
{
  Seismic3DViewportWidget view;
  QVERIFY(!view.isGlReady());
  PropertyBrickAxes axes;
  axes.iMax = 3;
  axes.jMax = 2;
  axes.kMax = 4;
  SgySliceImage image = makeZone(4, 5, 0.5f, true);
  QVERIFY(view.updatePropertySlice(SeismicSliceSlot::Inline, SgySliceType::Inline, 1, axes, image));
  QVERIFY(view.hasPendingPropertySlice());
  QVERIFY(view.updatePropertyStackLayer(0, 2, axes, makeZone(4, 3, 0.2f, false)));
  QVERIFY(view.updatePropertyStackLayer(0, 3, axes, makeZone(4, 3, 0.4f, false)));
  QVERIFY(view.hasPendingPropertySlice());
  view.setVolume({});
  QVERIFY(!view.hasPendingPropertySlice());
}

void TestPropModelPanel::propertySliceUploadsOnGl()
{
  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  if (!surface.isValid())
    QSKIP("offscreen surface unavailable");
  QOpenGLContext context;
  context.setFormat(format);
  if (!context.create() || !context.makeCurrent(&surface))
    QSKIP("OpenGL context could not be created in this environment");
  QOpenGLFunctions_3_3_Core gl;
  if (!gl.initializeOpenGLFunctions())
    QSKIP("OpenGL 3.3 Core functions not available");

  SeismicSliceRenderer renderer;
  QVERIFY(renderer.Initialize(&gl));
  PropertyBrickAxes axes;
  axes.iMax = 7;
  axes.jMax = 5;
  axes.kMax = 3;
  const SgySliceImage image = solidRgba(8, 6, 220, 40, 40, 255);
  QVERIFY(renderer.UpdatePropertySlice(&gl, SeismicSliceSlot::Time, axes, SgySliceType::Time, 1, image));
  QVERIFY(renderer.IsSlotReady(SeismicSliceSlot::Time));
  QVERIFY(renderer.UpdatePropertyStackLayer(&gl, 0, axes, 1, image));
  renderer.SetStackVisible(true);
  renderer.SetSliceAlpha(1.0f);

  const int W = 200, H = 150;
  GLuint fbo = 0, color = 0, depth = 0;
  gl.glGenFramebuffers(1, &fbo);
  gl.glGenTextures(1, &color);
  gl.glBindTexture(GL_TEXTURE_2D, color);
  gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  gl.glGenRenderbuffers(1, &depth);
  gl.glBindRenderbuffer(GL_RENDERBUFFER, depth);
  gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
  gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
  if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    QSKIP("property slice FBO incomplete");

  gl.glViewport(0, 0, W, H);
  gl.glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
  gl.glEnable(GL_DEPTH_TEST);
  gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  SeismicCameraController camera;
  camera.Reset();
  camera.SetTopView();
  renderer.Render(&gl, camera.BuildViewMatrix(), camera.BuildProjectionMatrix(double(W) / H));
  gl.glFinish();
  std::vector<unsigned char> pixels(static_cast<std::size_t>(W * H * 4));
  gl.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  int lit = 0;
  for (int i = 0; i < W * H; ++i)
  {
    const unsigned char r = pixels[static_cast<std::size_t>(i * 4)];
    const unsigned char b = pixels[static_cast<std::size_t>(i * 4 + 2)];
    if (r > 150 && b < 80)
      ++lit;
  }
  QVERIFY2(lit > 20, qPrintable(QStringLiteral("property slice pixels=%1").arg(lit)));
  renderer.Cleanup(&gl);
}

QTEST_MAIN(TestPropModelPanel)
#include "tst_propmodelpanel.moc"
