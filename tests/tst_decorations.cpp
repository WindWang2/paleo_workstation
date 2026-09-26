#include <QtTest>
#include <QDockWidget>
#include <QImage>
#include <QPainter>
#include <QApplication>
#include <QFontInfo>
#include <QWidget>

#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsmapdecoration.h>
#include <qgsmapsettings.h>
#include <qgsmaptoolpan.h>
#include <qgsrectangle.h>

#include "../src/ui/decorations/paleodecorations.h"
#include "../src/ui/vertexeditorshim.h"
#include "../src/ui/paleotheme.h" // 渲染稳定化：vendor 字体 + Fusion 钉死

// ET9 deliverable: canvas decorations (scale bar / north arrow / grid) and the
// vertex-editor dock shim. Upstream QgsDecorationItem / QgsDecorationScaleBar /
// QgsDecorationNorthArrow / QgsDecorationGrid / QgsVertexEditor live in
// libqgis_app (APP_EXPORT, headers not installed), so PaleoDecorationManager
// drives bespoke QgsMapDecoration subclasses from QgsMapCanvas::renderComplete
// (there is no QgsMapCanvas::addDecorationItem in QGIS 4.x) and
// PaleoVertexEditorShim provides the dock plumbing around a minimal editor
// widget until QgsVertexEditor can be ported.
class TestDecorations : public QObject
{
  Q_OBJECT

  private slots:
    // 渲染回归稳定化（TODOS P1 / wave3）：装 vendor 字体并钉 Fusion——
    // 消除跨平台字体替换与平台样式差异带来的像素噪点（假失败）。
    void initTestCase()
    {
      PaleoTheme::pinRenderEnvironment();
      const QFontInfo info(qApp->font());
      QVERIFY2(info.family() == QStringLiteral("Noto Sans SC"),
               qPrintable(QStringLiteral("app font must resolve to the vendored "
                                         "family, got: ") + info.family()));
    }

    // 同帧双渲染逐字节一致（字体钉住后文本基线可复现的前提）。
    void pinnedRenderIsDeterministic()
    {
      QgsMapCanvas canvas;
      PaleoDecorationManager mgr( &canvas );
      mgr.setScaleBarEnabled( true );
      mgr.setGridEnabled( true );
      const auto render = [&canvas, &mgr] {
        QImage img( canvas.size(), QImage::Format_ARGB32_Premultiplied );
        img.fill( Qt::white );
        QPainter p( &img );
        mgr.paintDecorations( &p );
        p.end();
        return img;
      };
      QCOMPARE( render(), render() );
    }

    void togglesChangeItemCount();
    void enabledFlagsRoundtrip();
    void decorationsPaintPixels();
    void disabledDecorationPaintsNothing();
    void vertexShimDockLifecycle();
    void vertexShimAutoShow();

  private:
    // Paints the manager's active decorations onto a white image, returning the
    // count of pixels that differ from pure white.
    static int paintAndCount( QgsMapCanvas &canvas, PaleoDecorationManager &mgr )
    {
      QImage img( canvas.size(), QImage::Format_ARGB32_Premultiplied );
      img.fill( Qt::white );
      {
        QPainter p( &img );
        mgr.paintDecorations( &p );
      }
      int painted = 0;
      for ( int y = 0; y < img.height(); ++y )
        for ( int x = 0; x < img.width(); ++x )
          if ( img.pixel( x, y ) != qRgba( 255, 255, 255, 255 ) )
            ++painted;
      return painted;
    }
};

void TestDecorations::togglesChangeItemCount()
{
  QgsMapCanvas canvas;
  PaleoDecorationManager mgr( &canvas );

  QCOMPARE( mgr.decorationItems().size(), 0 );

  mgr.setScaleBarEnabled( true );
  QCOMPARE( mgr.decorationItems().size(), 1 );

  mgr.setNorthArrowEnabled( true );
  mgr.setGridEnabled( true );
  QCOMPARE( mgr.decorationItems().size(), 3 );

  mgr.setScaleBarEnabled( false );
  QCOMPARE( mgr.decorationItems().size(), 2 );

  mgr.setNorthArrowEnabled( false );
  mgr.setGridEnabled( false );
  QCOMPARE( mgr.decorationItems().size(), 0 );
}

void TestDecorations::enabledFlagsRoundtrip()
{
  QgsMapCanvas canvas;
  PaleoDecorationManager mgr( &canvas );

  QVERIFY( !mgr.isScaleBarEnabled() );
  QVERIFY( !mgr.isNorthArrowEnabled() );
  QVERIFY( !mgr.isGridEnabled() );

  mgr.setScaleBarEnabled( true );
  mgr.setNorthArrowEnabled( true );
  mgr.setGridEnabled( true );
  QVERIFY( mgr.isScaleBarEnabled() );
  QVERIFY( mgr.isNorthArrowEnabled() );
  QVERIFY( mgr.isGridEnabled() );

  mgr.setScaleBarEnabled( false );
  QVERIFY( !mgr.isScaleBarEnabled() );
  QVERIFY( mgr.isNorthArrowEnabled() );
}

void TestDecorations::decorationsPaintPixels()
{
  QgsMapCanvas canvas;
  canvas.resize( 640, 480 );
  canvas.setExtent( QgsRectangle( 0, 0, 1000, 800 ) );
  canvas.refresh();
  QCoreApplication::processEvents();
  QVERIFY( canvas.mapUnitsPerPixel() > 0 );

  PaleoDecorationManager mgr( &canvas );
  mgr.setScaleBarEnabled( true );
  mgr.setNorthArrowEnabled( true );
  mgr.setGridEnabled( true );
  QCOMPARE( mgr.decorationItems().size(), 3 );

  const int painted = paintAndCount( canvas, mgr );
  QVERIFY2( painted > 0, "enabled decorations must draw onto the canvas painter" );

  // Decorations registered through the manager are QgsMapDecoration instances
  // (the only decoration interface exported by installed QGIS 4.x headers).
  for ( QgsMapDecoration *d : mgr.decorationItems() )
    QVERIFY( d->displayName().isEmpty() == false );
}

void TestDecorations::disabledDecorationPaintsNothing()
{
  QgsMapCanvas canvas;
  canvas.resize( 320, 240 );
  canvas.setExtent( QgsRectangle( 0, 0, 100, 100 ) );
  canvas.refresh();
  QCoreApplication::processEvents();

  PaleoDecorationManager mgr( &canvas );
  QCOMPARE( paintAndCount( canvas, mgr ), 0 );
}

void TestDecorations::vertexShimDockLifecycle()
{
  QgsMapCanvas canvas;
  canvas.resize( 400, 300 );

  QWidget host;
  PaleoVertexEditorShim shim( &canvas );

  QVERIFY( !shim.hasDock() );
  QVERIFY( !shim.dockParent() );

  shim.setDockParent( &host );
  QCOMPARE( shim.dockParent(), &host );

  // editor() lazily creates the editor widget inside its dock
  QWidget *ed = shim.editor();
  QVERIFY( ed != nullptr );
  QVERIFY( shim.dock() != nullptr );
  QCOMPARE( shim.dock()->widget(), ed );
  QCOMPARE( shim.dock()->parentWidget(), &host );
  QCOMPARE( shim.editor(), ed ); // lazy-once

  // dock re-parents cleanly to a new host widget
  QWidget host2;
  shim.setDockParent( &host2 );
  QCOMPARE( shim.dock()->parentWidget(), &host2 );

  // nullptr host → floating dock, must not crash
  shim.setDockParent( nullptr );
  QVERIFY( !shim.dock()->parentWidget() );
}

void TestDecorations::vertexShimAutoShow()
{
  QgsMapCanvas canvas;
  canvas.resize( 400, 300 );
  canvas.setExtent( QgsRectangle( 0, 0, 10, 10 ) );

  QWidget host;
  PaleoVertexEditorShim shim( &canvas );
  shim.setDockParent( &host );

  QgsMapToolPan *pan = new QgsMapToolPan( &canvas );

  shim.setAutoShow( true );
  QVERIFY( shim.autoShow() );
  shim.attachToTool( pan );
  QCOMPARE( shim.attachedTool(), static_cast<QgsMapTool *>( pan ) );

  // Activating the attached tool shows the dock; switching away hides it.
  canvas.setMapTool( pan );
  QVERIFY( shim.dock() != nullptr );
  QVERIFY( !shim.dock()->isHidden() );

  canvas.unsetMapTool( pan );
  QVERIFY( shim.dock()->isHidden() );

  // autoShow disabled → tool changes leave the dock untouched
  shim.setAutoShow( false );
  canvas.setMapTool( pan );
  QVERIFY( shim.dock()->isHidden() );
  canvas.unsetMapTool( pan );

  // attachToTool(nullptr) is safe
  shim.attachToTool( nullptr );
  QVERIFY( !shim.attachedTool() );

  delete pan;
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( QStringLiteral( "/usr" ), true ); // distro install
  app.initQgis();
  TestDecorations tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_decorations.moc"
