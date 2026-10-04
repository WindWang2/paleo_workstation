#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "../src/qgis/qgislayoutservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/standardelements.h"

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemmapoverview.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutmanager.h>
#include <qgslayoutpagecollection.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

// 方向 25 M5：布局随工程持久化——QgsLayoutManager 挂在 QgsProject 上，
// QGZ 读写应带回完整布局（元素/绑定/页面规格逐字段，Oracle 1 的工程级
// round-trip）。QgsProject 堆分配并故意泄漏（QGIS 4.2.2 栈析构崩溃路径）。
class TestLayoutPersistence : public QObject
{
  Q_OBJECT

  private:
    static QgsLayoutItem *findItem( QgsLayout *layout, const QString &id )
    {
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      for ( QgsLayoutItem *item : items )
        if ( item->id() == id )
          return item;
      return nullptr;
    }

  private slots:
    void layoutSurvivesProjectRoundTrip()
    {
      QTemporaryDir dir;
      QVERIFY( dir.isValid() );
      const QString qgz = dir.filePath( QStringLiteral( "roundtrip.qgz" ) );

      // ---- 建工程 + 版面（入 layoutManager = 随工程持久化的挂点）---------
      auto *project = new QgsProject(); // leaked on purpose
      project->setFileName( qgz );
      QVERIFY( project->write( qgz ) ); // 空工程先落一次（建 zip 后端）

      QgisLayoutService svc( project );
      QString err;
      QgsLayout *layout = svc.createLayout( QStringLiteral( "facies_map" ), &err );
      QVERIFY2( layout != nullptr, qPrintable( err ) );
      QVERIFY( PaleoStandardElements::populateFigureLayout(
          qobject_cast<QgsPrintLayout *>( layout ), PaleoStandardElements::FigureKind::Facies,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );

      QVERIFY( project->write( qgz ) );

      // ---- 重开：新 QgsProject 读回同一 .qgz --------------------------------
      auto *reopened = new QgsProject(); // leaked on purpose
      QVERIFY( reopened->read( qgz ) );

      QgsMasterLayoutInterface *iface =
          reopened->layoutManager()->layoutByName( QStringLiteral( "facies_map" ) );
      QVERIFY2( iface != nullptr, "layout missing after project round-trip" );
      auto *layout2 = dynamic_cast<QgsPrintLayout *>( iface );
      QVERIFY( layout2 != nullptr );

      // ---- 元素树逐项一致 ---------------------------------------------------
      for ( const char *id : { "map", "scalebar", "legend", "northArrow", "inset",
                               "title", "subtitle", "signature" } )
      {
        QVERIFY2( findItem( layout2, QString::fromLatin1( id ) ) != nullptr,
                  qPrintable( QString::fromLatin1( id ) ) );
      }

      // ---- 页面规格 ---------------------------------------------------------
      QgsLayoutItemPage *page = layout2->pageCollection()->page( 0 );
      QVERIFY( page != nullptr );
      QCOMPARE( page->pageSize().units(), Qgis::LayoutUnit::Millimeters );
      QVERIFY( qAbs( page->pageSize().width() - 297.0 ) < 0.5 );
      QVERIFY( qAbs( page->pageSize().height() - 210.0 ) < 0.5 );

      // ---- 绑定逐字段 -------------------------------------------------------
      auto *map = qobject_cast<QgsLayoutItemMap *>( findItem( layout2, QStringLiteral( "map" ) ) );
      auto *bar =
          qobject_cast<QgsLayoutItemScaleBar *>( findItem( layout2, QStringLiteral( "scalebar" ) ) );
      auto *legend =
          qobject_cast<QgsLayoutItemLegend *>( findItem( layout2, QStringLiteral( "legend" ) ) );
      auto *inset =
          qobject_cast<QgsLayoutItemMap *>( findItem( layout2, QStringLiteral( "inset" ) ) );
      QVERIFY( map && bar && legend && inset );
      QCOMPARE( bar->linkedMap(), map ); // 比例尺回指主图（uuid 重联）
      QCOMPARE( legend->linkedMap(), map );
      QCOMPARE( legend->syncMode(), Qgis::LegendSyncMode::AllProjectLayers );
      QVERIFY( legend->legendFilterByMapEnabled() );
      QCOMPARE( inset->overviews()->size(), 1 ); // 插图 overview 回指主图
      QCOMPARE( inset->overviews()->overview( 0 )->linkedMap(), map );
      QVERIFY( inset->overviews()->overview( 0 )->enabled() );

      // ---- 标题文本 ---------------------------------------------------------
      auto *title =
          qobject_cast<QgsLayoutItemLabel *>( findItem( layout2, QStringLiteral( "title" ) ) );
      QVERIFY( title != nullptr );
      QCOMPARE( title->text(), QStringLiteral( "沉积相图" ) );

      // ---- 服务面：重开后 layout 列表/CRUD 正常 ------------------------------
      QgisLayoutService svc2( reopened );
      QVERIFY( svc2.layoutNames().contains( QStringLiteral( "facies_map" ) ) );
      QVERIFY( svc2.createLayout( QStringLiteral( "facies_map" ) ) ==
               nullptr ); // 同名仍拒绝
      QVERIFY( svc2.removeLayout( QStringLiteral( "facies_map" ) ) );
      QVERIFY( reopened->layoutManager()->layoutByName( QStringLiteral( "facies_map" ) ) == nullptr );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutPersistence tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layoutpersistence.moc"
