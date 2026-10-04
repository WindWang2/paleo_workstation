#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layouttemplatestore.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/standardelements.h"
#include "../src/workflow/layouttemplatelibrary.h"

#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

// 方向 25 M1b：版式模板库——qpt 内容进 catalog 受管区（版本/SHA），
// 库语义（名字/重命名/删除）进 project.sqlite 的 layout_templates 表。
// 纪律同 tst_layoutdesigner_full：QgsProject 堆分配并故意泄漏。
class TestLayoutTemplateLibrary : public QObject
{
  Q_OBJECT

  private:
    QgsPrintLayout *makeLayout()
    {
      auto *project = new QgsProject(); // leaked on purpose
      auto *layout = new QgsPrintLayout( project );
      layout->initializeDefaults();
      layout->setName( QStringLiteral( "test_layout" ) );
      return layout;
    }

    static int contentItems( QgsLayout *layout )
    {
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      int n = 0;
      for ( QgsLayoutItem *item : items )
        if ( !item->id().isEmpty() )
          ++n;
      return n;
    }

  private slots:
    void saveListApplyRenameRemove()
    {
      QTemporaryDir projectDir;
      QVERIFY( projectDir.isValid() );
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );
      LayoutTemplateStore store( projectDir.path() + QStringLiteral( "/meta.sqlite" ) );
      QVERIFY( store.open() );
      PaleoLayoutTemplateLibrary::Library library( &catalog, &store, projectDir.path() );

      // ---- 另存为：骨架模板入库 ------------------------------------------
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QVERIFY( PaleoStandardElements::populateFigureLayout(
          layout.get(), PaleoStandardElements::FigureKind::WellPosition,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );
      const int populated = contentItems( layout.get() );
      QVERIFY( populated >= 7 );

      const auto saved = library.saveAs( layout.get(), QStringLiteral( "标准井位图" ),
                                         QStringLiteral( "well_position" ),
                                         QStringLiteral( "A4" ), true );
      QVERIFY2( saved.ok, qPrintable( saved.error ) );
      QVERIFY( !saved.templateId.isEmpty() );
      QVERIFY( !saved.assetId.isEmpty() );
      QVERIFY( !saved.versionId.isEmpty() );
      QVERIFY( saved.sha256.size() == 64 );
      QVERIFY( QFile::exists( saved.managedPath ) );

      // ---- catalog 侧：资产/版本/SHA 齐全（Oracle 3）----------------------
      const CatalogAsset asset = catalog.assetById( saved.assetId );
      QCOMPARE( asset.type, QStringLiteral( "layout_template" ) );
      QCOMPARE( asset.format, QStringLiteral( "qpt" ) );
      const CatalogVersion version = catalog.versionById( saved.versionId );
      QCOMPARE( version.assetId, saved.assetId );
      QCOMPARE( version.stage, QStringLiteral( "INTERMEDIATE" ) );
      QCOMPARE( version.sha256, saved.sha256 );
      QVERIFY( version.managed );
      QCOMPARE( catalog.sha256FileHex( saved.managedPath ), saved.sha256 );
      // 受管文件只读（入库字节不回写）。
      QVERIFY( !( QFileInfo( saved.managedPath ).permissions() & QFileDevice::WriteOwner ) );

      // ---- 列表/按名 -------------------------------------------------------
      const auto listed = library.list();
      QCOMPARE( listed.size(), 1 );
      QCOMPARE( listed.first().name, QStringLiteral( "标准井位图" ) );
      QCOMPARE( listed.first().kind, QStringLiteral( "well_position" ) );
      QCOMPARE( listed.first().versionId, saved.versionId );
      QCOMPARE( library.byName( QStringLiteral( "标准井位图" ) ).id, saved.templateId );

      // ---- 套用：清场重载，元素树一致（Oracle 1 的库内路径）----------------
      QVERIFY( PaleoStandardElements::populateFigureLayout(
          layout.get(), PaleoStandardElements::FigureKind::Facies,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );
      QVERIFY( contentItems( layout.get() ) != populated ); // 换了骨架（多了插图）
      QVERIFY2( library.apply( layout.get(), listed.first() ), "apply failed" );
      QCOMPARE( contentItems( layout.get() ), populated ); // 元素树回到井位图骨架
      QList<QgsLayoutItemMap *> maps;
      layout->layoutItems( maps );
      QCOMPARE( maps.size(), 1 ); // 井位图没有插图

      // ---- 重命名 ----------------------------------------------------------
      QVERIFY( library.rename( saved.templateId, QStringLiteral( "井位图-生产版" ) ) );
      QVERIFY( library.byName( QStringLiteral( "标准井位图" ) ).isNull() );
      QCOMPARE( library.list().first().name, QStringLiteral( "井位图-生产版" ) );
      // 重名拒绝。
      QVERIFY( !library.rename( saved.templateId, QStringLiteral( "井位图-生产版" ) ) );

      // ---- 重存：同资产新版本（内容有变）------------------------------------
      QVERIFY( PaleoStandardElements::populateFigureLayout(
          layout.get(), PaleoStandardElements::FigureKind::SingleFactor,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );
      const auto resaved = library.save( layout.get(), saved.templateId );
      QVERIFY2( resaved.ok, qPrintable( resaved.error ) );
      QCOMPARE( resaved.assetId, saved.assetId );                 // 同资产
      QVERIFY( resaved.versionId != saved.versionId );            // 新版本
      QCOMPARE( catalog.versionsForAsset( saved.assetId ).size(), 2 ); // 版本历史 2 条
      QCOMPARE( library.list().first().versionId, resaved.versionId ); // 行指新版本
      QCOMPARE( library.list().first().sha256, resaved.sha256 );

      // ---- 重存（内容未变）：仍记新版本（序列化非字节稳定，账本如实）--------
      QVERIFY( library.apply( layout.get(), library.list().first() ) );
      const auto again = library.save( layout.get(), saved.templateId );
      QVERIFY2( again.ok, qPrintable( again.error ) );
      QVERIFY( again.versionId != resaved.versionId );
      QCOMPARE( again.assetId, saved.assetId );
      QCOMPARE( catalog.versionsForAsset( saved.assetId ).size(), 3 );

      // ---- 删除：库行没了；catalog 历史保留 --------------------------------
      QVERIFY( library.remove( saved.templateId ) );
      QVERIFY( library.list().isEmpty() );
      QCOMPARE( catalog.versionsForAsset( saved.assetId ).size(), 3 ); // 账本不回擦
      QVERIFY( !library.remove( saved.templateId ) );                  // 再删 = 失败
    }

    void saveAsRejectsDuplicateNameAndEmpty()
    {
      QTemporaryDir projectDir;
      QVERIFY( projectDir.isValid() );
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );
      LayoutTemplateStore store( projectDir.path() + QStringLiteral( "/meta.sqlite" ) );
      QVERIFY( store.open() );
      PaleoLayoutTemplateLibrary::Library library( &catalog, &store, projectDir.path() );

      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QVERIFY( library.saveAs( layout.get(), QStringLiteral( "唯一" ), QStringLiteral( "custom" ),
                               QStringLiteral( "A4" ), true )
                   .ok );
      const auto dup = library.saveAs( layout.get(), QStringLiteral( "唯一" ),
                                       QStringLiteral( "custom" ), QStringLiteral( "A4" ), true );
      QVERIFY( !dup.ok );
      QVERIFY( !dup.error.isEmpty() );
      QVERIFY( !library.saveAs( layout.get(), QString(), QStringLiteral( "custom" ),
                                QStringLiteral( "A4" ), true )
                    .ok );
      QVERIFY( !library.saveAs( nullptr, QStringLiteral( "空版面" ), QStringLiteral( "custom" ),
                                QStringLiteral( "A4" ), true )
                    .ok );
      QCOMPARE( library.list().size(), 1 ); // 失败路径不留半行
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutTemplateLibrary tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layouttemplatelibrary.moc"
