#include <QtTest>
#include <QLabel>
#include <QPushButton>

#include "../src/ui/layout/layoutitempanel.h"
#include "../src/qgis/qgisruntime.h"

#include <qgsfillsymbol.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutitemwidget.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

// Subtask B: PaleoLayoutItemPanel hosts the native per-item property widget
// (QgsLayoutItemBaseWidget via QgsGui::layoutItemGuiRegistry()->createItemWidget)
// and adds the Paleo business override区 (horizon title injection + scalebar
// presets). These tests pin the hosting contract over a real QgsPrintLayout,
// offscreen, mirroring tst_layoutshell's bootstrap.
class TestLayoutItemPanel : public QObject
{
    Q_OBJECT

  private slots:
    void setItemHostsNativeBaseWidget()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "图件标题" ) );
      layout.addLayoutItem( label );

      PaleoLayoutItemPanel panel;
      QCOMPARE( panel.item(), nullptr );
      panel.setItem( label );
      QCOMPARE( panel.item(), label );

      // Native hosting contract: a QgsLayoutItemBaseWidget is in the panel tree.
      QgsLayoutItemBaseWidget *hosted = panel.findChild<QgsLayoutItemBaseWidget *>();
      QVERIFY( hosted != nullptr );
    }

    void sameItemDoesNotRebuild()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *label = new QgsLayoutItemLabel( &layout );
      layout.addLayoutItem( label );

      PaleoLayoutItemPanel panel;
      panel.setItem( label );
      QgsLayoutItemBaseWidget *first = panel.findChild<QgsLayoutItemBaseWidget *>();
      QVERIFY( first != nullptr );

      panel.setItem( label ); // same item: must not rebuild
      QCOMPARE( panel.findChild<QgsLayoutItemBaseWidget *>(), first );
    }

    void setNullClearsAndShowsPlaceholder()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *label = new QgsLayoutItemLabel( &layout );
      layout.addLayoutItem( label );

      PaleoLayoutItemPanel panel;
      panel.setItem( label );
      QVERIFY( panel.findChild<QgsLayoutItemBaseWidget *>() != nullptr );

      panel.setItem( nullptr );
      QCOMPARE( panel.item(), nullptr );
      QCOMPARE( panel.findChild<QgsLayoutItemBaseWidget *>(), nullptr );

      QLabel *placeholder = panel.findChild<QLabel *>( QStringLiteral( "placeholderLabel" ) );
      QVERIFY( placeholder != nullptr );
      QVERIFY( !placeholder->text().isEmpty() );
      QVERIFY( placeholder->isVisibleTo( &panel ) );

      // Re-setting null stays clean.
      panel.setItem( nullptr );
      QCOMPARE( panel.findChild<QgsLayoutItemBaseWidget *>(), nullptr );
    }

    void itemDestructionIsSafe()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "t" ) );
      layout.addLayoutItem( label );

      PaleoLayoutItemPanel panel;
      panel.setActiveHorizonTitle( QStringLiteral( "C6" ) );
      panel.setItem( label );
      QVERIFY( panel.findChild<QgsLayoutItemBaseWidget *>() != nullptr );

      QSignalSpy spy( &panel, &PaleoLayoutItemPanel::itemChanged );
      layout.removeLayoutItem( label ); // deletes the item (deferred deletion)
      QTest::qWait( 10 );               // let destroyed() reach the panel

      QCOMPARE( panel.item(), nullptr ); // no dangling pointer
      QCOMPARE( panel.findChild<QgsLayoutItemBaseWidget *>(), nullptr );
      QCOMPARE( spy.count(), 1 ); // destruction notifies integrators
      QCOMPARE( qvariant_cast<QgsLayoutItem *>( spy.takeFirst().at( 0 ) ), nullptr );

      // Panel stays usable after the hosted item died.
      auto *other = new QgsLayoutItemLabel( &layout );
      layout.addLayoutItem( other );
      panel.setItem( other );
      QVERIFY( panel.findChild<QgsLayoutItemBaseWidget *>() != nullptr );
    }

    void horizonTitleInjection()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "旧标题" ) );
      layout.addLayoutItem( label );

      PaleoLayoutItemPanel panel;
      QCOMPARE( panel.activeHorizonTitle(), QString() );

      panel.setItem( label );
      panel.setActiveHorizonTitle( QStringLiteral( "C6" ) );
      QCOMPARE( panel.activeHorizonTitle(), QStringLiteral( "C6" ) );

      QVERIFY( panel.applyHorizonTitle() );
      QCOMPARE( label->text(), QStringLiteral( "C6" ) ); // full-replacement policy

      // Empty title: refused, label untouched.
      panel.setActiveHorizonTitle( QString() );
      QVERIFY( !panel.applyHorizonTitle() );
      QCOMPARE( label->text(), QStringLiteral( "C6" ) );

      // Non-label / no item: refused.
      panel.setActiveHorizonTitle( QStringLiteral( "D53" ) );
      panel.setItem( nullptr );
      QVERIFY( !panel.applyHorizonTitle() );
    }

    void scalebarPresetsChangeStyleAndUnits()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *sb = new QgsLayoutItemScaleBar( &layout );
      layout.addLayoutItem( sb );

      PaleoLayoutItemPanel panel;
      panel.setItem( sb );

      // Preset 0 单厢简洁
      panel.applyScalebarPreset( 0 );
      QCOMPARE( sb->style(), QStringLiteral( "Single Box" ) );
      QCOMPARE( sb->units(), Qgis::DistanceUnit::Kilometers );
      QCOMPARE( sb->unitLabel(), QStringLiteral( "km" ) );
      QCOMPARE( sb->numberOfSegments(), 4 );
      QCOMPARE( sb->numberOfSegmentsLeft(), 2 );

      // Preset 1 黑白双厢
      panel.applyScalebarPreset( 1 );
      QCOMPARE( sb->style(), QStringLiteral( "Double Box" ) );
      QCOMPARE( sb->numberOfSegments(), 6 );
      QCOMPARE( sb->numberOfSegmentsLeft(), 0 );
      QVERIFY( sb->fillSymbol() != nullptr );
      QVERIFY( sb->alternateFillSymbol() != nullptr );
      QCOMPARE( sb->fillSymbol()->color(), QColor( Qt::white ) );
      QCOMPARE( sb->alternateFillSymbol()->color(), QColor( Qt::black ) );

      // Preset 2 线段刻度
      panel.applyScalebarPreset( 2 );
      QCOMPARE( sb->style(), QStringLiteral( "Line Ticks Down" ) );
      QCOMPARE( sb->units(), Qgis::DistanceUnit::Meters );
      QCOMPARE( sb->unitLabel(), QStringLiteral( "m" ) );
      QCOMPARE( sb->numberOfSegments(), 5 );
      QCOMPARE( sb->numberOfSegmentsLeft(), 0 );
    }

    void invalidPresetAndWrongTypeAreNoOps()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *sb = new QgsLayoutItemScaleBar( &layout );
      layout.addLayoutItem( sb );
      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "标题" ) );
      layout.addLayoutItem( label );

      PaleoLayoutItemPanel panel;
      panel.setItem( sb );
      panel.applyScalebarPreset( 2 );
      const QString style = sb->style();

      panel.applyScalebarPreset( -1 ); // out of range
      panel.applyScalebarPreset( 99 );
      QCOMPARE( sb->style(), style );

      // Current item not a scalebar: no-op, no crash.
      panel.setItem( label );
      panel.applyScalebarPreset( 1 );
      QCOMPARE( sb->style(), style );
      QCOMPARE( label->text(), QStringLiteral( "标题" ) );
    }

    void itemChangedEmittedOnPanelPathEdits()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "旧标题" ) );
      layout.addLayoutItem( label );

      PaleoLayoutItemPanel panel;
      QSignalSpy spy( &panel, &PaleoLayoutItemPanel::itemChanged );

      panel.setItem( label );
      QCOMPARE( spy.count(), 0 ); // selection is not a modification

      panel.setActiveHorizonTitle( QStringLiteral( "C6" ) );
      QVERIFY( panel.applyHorizonTitle() );
      QVERIFY( spy.count() >= 1 );
      QCOMPARE( qvariant_cast<QgsLayoutItem *>( spy.last().at( 0 ) ), label );

      spy.clear();
      label->setText( QStringLiteral( "D53" ) ); // direct item edit forwards too (coarse grain)
      QVERIFY( spy.count() >= 1 );
      QCOMPARE( qvariant_cast<QgsLayoutItem *>( spy.last().at( 0 ) ), label );
    }

    void businessUiDisableReasons()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      auto *label = new QgsLayoutItemLabel( &layout );
      layout.addLayoutItem( label );
      auto *sb = new QgsLayoutItemScaleBar( &layout );
      layout.addLayoutItem( sb );

      PaleoLayoutItemPanel panel;
      panel.setActiveHorizonTitle( QStringLiteral( "C6" ) );

      QPushButton *applyBtn = panel.findChild<QPushButton *>( QStringLiteral( "applyHorizonButton" ) );
      QPushButton *presetBtn = panel.findChild<QPushButton *>( QStringLiteral( "presetDoubleBoxButton" ) );
      QVERIFY( applyBtn != nullptr );
      QVERIFY( presetBtn != nullptr );

      panel.setItem( label );
      QVERIFY( applyBtn->isEnabled() ); // label current + title set
      QVERIFY( !presetBtn->isEnabled() ); // label is not a scalebar
      QVERIFY( !presetBtn->toolTip().isEmpty() ); // §35: disabled needs a reason tooltip

      panel.setItem( sb );
      QVERIFY( presetBtn->isEnabled() );
      QVERIFY( !applyBtn->isEnabled() ); // scalebar is not a label
      QVERIFY( !applyBtn->toolTip().isEmpty() );

      // No horizon title set: apply disabled with reason.
      panel.setActiveHorizonTitle( QString() );
      panel.setItem( label );
      QVERIFY( !applyBtn->isEnabled() );
      QVERIFY( !applyBtn->toolTip().isEmpty() );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutItemPanel tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layoutitempanel.moc"
