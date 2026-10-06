#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/ui/paleotheme.h" // 渲染稳定化：vendor 字体钉死
#include "../src/ui/layout/layoutexportactions.h"
#include "../src/ui/layout/layouttemplates.h"
#include "../src/services/paleotaskservice.h" // #85 异步导出路径
#include "../src/qgis/qgisruntime.h"

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitemshape.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutpoint.h>
#include <qgslayoutsize.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsgeometry.h>
#include <qgslayertree.h>
#include <qgslayoutitemmap.h>
#include <qgsvectorlayer.h>

// Task C contract tests: PaleoLayoutExportActions (export core + actions) and
// PaleoLayoutTemplates (template save/load + builtin page-size library), over
// real QgsPrintLayouts, offscreen, against real files in QTemporaryDir.
// Empirical QGIS 4.2.2 facts these tests rely on (probed before writing impl):
//   - ImageExportSettings.pages selects 0-based pages, single file per page.
//   - Multi-page image/SVG exports suffix files "_2", "_3", ... after the
//     first page's base name.
//   - PdfExportSettings/SvgExportSettings have no page list; a page range is
//     honored by cloning the layout and deleting the unselected pages.
class TestLayoutExport : public QObject
{
    Q_OBJECT

  private slots:
    // 渲染回归稳定化（wave3）：PDF 文本走 vendor 字体，跨平台可比。
    void initTestCase() { PaleoTheme::pinRenderEnvironment(); }

    void actionsAndBuiltinMetadata()
    {
      PaleoLayoutExportActions exports;
      for ( QAction *a : { exports.exportPngAction(), exports.exportPdfAction(), exports.exportSvgAction() } )
      {
        QVERIFY( a != nullptr );
        QVERIFY( !a->text().isEmpty() );
        QVERIFY( !a->objectName().isEmpty() );
      }
      // TEST-04：原断言自比较恒真。真实不变量：动作 getter 幂等（重复取
      // 同一 QAction*——宿主重复接线不得得到新对象），且三个导出动作是
      // 互不相同的实例（getter 串了对象即红）。
      QCOMPARE( exports.exportPngAction(), exports.exportPngAction() );
      QVERIFY( exports.exportPngAction() != exports.exportPdfAction() );
      QVERIFY( exports.exportPngAction() != exports.exportSvgAction() );

      PaleoLayoutTemplates templates;
      QVERIFY( !templates.saveAsTemplateAction()->text().isEmpty() );
      QVERIFY( !templates.loadFromTemplateAction()->text().isEmpty() );

      // 方向 25：页面规格 5 键（补 A3 横/竖）+ 图件内容模板 12 复合键。
      const QStringList keys = PaleoLayoutTemplates::builtinKeys();
      QCOMPARE( keys, QStringList( { QStringLiteral( "a4_landscape" ),
                                     QStringLiteral( "a4_portrait" ),
                                     QStringLiteral( "a3_landscape" ),
                                     QStringLiteral( "a3_portrait" ),
                                     QStringLiteral( "a0_landscape" ) } ) );
      for ( const QString &key : keys )
      {
        QVERIFY( !PaleoLayoutTemplates::builtinTitle( key ).isEmpty() );
        QAction *a = templates.applyBuiltinAction( key );
        QVERIFY( a != nullptr );
        QVERIFY( !a->text().isEmpty() );
      }
      const QStringList figureKeys = PaleoLayoutTemplates::figureBuiltinKeys();
      QCOMPARE( figureKeys.size(), 12 );
      QCOMPARE( figureKeys.first(), QStringLiteral( "well_position@a4_landscape" ) );
      for ( const QString &key : figureKeys )
      {
        QVERIFY( !PaleoLayoutTemplates::builtinTitle( key ).isEmpty() );
        QVERIFY( templates.applyBuiltinAction( key ) != nullptr );
      }
      QCOMPARE( templates.applyBuiltinAction( QStringLiteral( "bogus" ) ), nullptr );
      QCOMPARE( templates.applyBuiltinAction( QStringLiteral( "bogus@a4_landscape" ) ), nullptr );

      // Templates dir is relocatable; default is resolved lazily.
      templates.setTemplatesDir( QStringLiteral( "/tmp" ) );
      QCOMPARE( templates.templatesDir(), QStringLiteral( "/tmp" ) );
    }

    void pngExportWritesValidFile()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      addLabel( &layout, QStringLiteral( "PNG TEST" ) );

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "out.png" ) );
      PaleoLayoutExportActions exports;
      const auto outcome = exports.exportLayout( &layout, path, PaleoLayoutExportActions::Format::Png,
                                                 300.0, PaleoLayoutExportActions::PageRange() );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QCOMPARE( outcome.files, QStringList( { path } ) );

      QFile f( path );
      QVERIFY( f.open( QIODevice::ReadOnly ) );
      QCOMPARE( f.read( 8 ), QByteArray( "\x89PNG\x0D\x0A\x1A\x0A", 8 ) );
      QVERIFY( f.size() > 0 );

      // A4 landscape default page (297x210mm) at 300dpi: ~3508x2480 px.
      const QImage img( path );
      QVERIFY( !img.isNull() );
      QVERIFY( qAbs( img.width() - 3508 ) <= 3 );
      QVERIFY( qAbs( img.height() - 2480 ) <= 3 );
    }

    void pdfExportWritesValidFile()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      addLabel( &layout, QStringLiteral( "PDF TEST" ) );

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "out.pdf" ) );
      PaleoLayoutExportActions exports;
      const auto outcome = exports.exportLayout( &layout, path, PaleoLayoutExportActions::Format::Pdf,
                                                 300.0, PaleoLayoutExportActions::PageRange() );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QCOMPARE( outcome.files, QStringList( { path } ) );

      QFile f( path );
      QVERIFY( f.open( QIODevice::ReadOnly ) );
      QCOMPARE( f.read( 4 ), QByteArray( "%PDF", 4 ) );
      QVERIFY( f.size() > 0 );
    }

    void svgExportWritesValidFile()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      addLabel( &layout, QStringLiteral( "SVG TEST" ) );

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "out.svg" ) );
      PaleoLayoutExportActions exports;
      const auto outcome = exports.exportLayout( &layout, path, PaleoLayoutExportActions::Format::Svg,
                                                 96.0, PaleoLayoutExportActions::PageRange() );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QCOMPARE( outcome.files, QStringList( { path } ) );

      QFile f( path );
      QVERIFY( f.open( QIODevice::ReadOnly ) );
      QVERIFY( f.readAll().contains( "<svg" ) );
    }

    void pageRangeExportsOnlyFirstPage()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      layout.pageCollection()->page( 0 )->setPageSize( QgsLayoutSize( 210, 297, Qgis::LayoutUnit::Millimeters ) );
      addLabel( &layout, QStringLiteral( "PAGE ONE" ) );
      QgsLayoutItemPage *page2 = layout.pageCollection()->extendByNewPage();
      auto *label2 = new QgsLayoutItemLabel( &layout );
      label2->setText( QStringLiteral( "PAGE TWO" ) );
      label2->attemptResize( QgsLayoutSize( 60, 15, Qgis::LayoutUnit::Millimeters ) );
      label2->attemptMove( QgsLayoutPoint( 10, page2->pos().y() + 10, Qgis::LayoutUnit::Millimeters ) );
      layout.addLayoutItem( label2 );
      QCOMPARE( layout.pageCollection()->pageCount(), 2 );

      PaleoLayoutExportActions exports;
      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "first.png" ) );

      // Range mode, 0-based inclusive from/to.
      PaleoLayoutExportActions::PageRange range;
      range.mode = PaleoLayoutExportActions::PageRange::Mode::Range;
      range.fromPage = 0;
      range.toPage = 0;
      auto outcome = exports.exportLayout( &layout, path, PaleoLayoutExportActions::Format::Png, 96.0, range );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QCOMPARE( outcome.files, QStringList( { path } ) );
      QVERIFY( QFile::exists( path ) );
      // Exactly one file: the second page must NOT have produced a suffixed sibling.
      QCOMPARE( QDir( dir.path() ).entryList( QDir::Files ).size(), 1 );

      // CurrentPage mode pointing at page 0 behaves the same.
      PaleoLayoutExportActions::PageRange current;
      current.mode = PaleoLayoutExportActions::PageRange::Mode::Current;
      current.currentPage = 0;
      const QString path2 = dir.filePath( QStringLiteral( "cur.png" ) );
      outcome = exports.exportLayout( &layout, path2, PaleoLayoutExportActions::Format::Png, 96.0, current );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QCOMPARE( outcome.files, QStringList( { path2 } ) );
      QCOMPARE( QDir( dir.path() ).entryList( QDir::Files ).size(), 2 ); // first.png + cur.png

      // A two-page range exports both pages: base + "_2" sibling (QGIS naming).
      PaleoLayoutExportActions::PageRange both;
      both.mode = PaleoLayoutExportActions::PageRange::Mode::Range;
      both.fromPage = 0;
      both.toPage = 1;
      const QString path3 = dir.filePath( QStringLiteral( "both.png" ) );
      outcome = exports.exportLayout( &layout, path3, PaleoLayoutExportActions::Format::Png, 96.0, both );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QVERIFY( QFile::exists( path3 ) );
      QVERIFY( QFile::exists( dir.filePath( QStringLiteral( "both_2.png" ) ) ) );
      QCOMPARE( outcome.files.size(), 2 );

      // Page selection also works for single-file formats (clone+trim path).
      const QString pdfPath = dir.filePath( QStringLiteral( "p1only.pdf" ) );
      auto pdfOutcome = exports.exportLayout( &layout, pdfPath, PaleoLayoutExportActions::Format::Pdf, 96.0, range );
      QVERIFY2( pdfOutcome.ok, qPrintable( pdfOutcome.error ) );
      QCOMPARE( pdfOutcome.files, QStringList( { pdfPath } ) );
      const QString svgPath = dir.filePath( QStringLiteral( "p1only.svg" ) );
      auto svgOutcome = exports.exportLayout( &layout, svgPath, PaleoLayoutExportActions::Format::Svg, 96.0, range );
      QVERIFY2( svgOutcome.ok, qPrintable( svgOutcome.error ) );
      QCOMPARE( svgOutcome.files, QStringList( { svgPath } ) );

      // Trimming a page that is NOT the first must re-flow the kept page's
      // items with it (QGS reflows pages, not items) — verify the label of
      // page 2 actually lands on the exported page via dark-pixel counting.
      PaleoLayoutExportActions::PageRange second;
      second.mode = PaleoLayoutExportActions::PageRange::Mode::Range;
      second.fromPage = 1;
      second.toPage = 1;
      const QString path4 = dir.filePath( QStringLiteral( "second.png" ) );
      outcome = exports.exportLayout( &layout, path4, PaleoLayoutExportActions::Format::Png, 96.0, second );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QCOMPARE( outcome.files, QStringList( { path4 } ) );
      const QImage secondImg( path4 );
      QVERIFY( !secondImg.isNull() );
      // 96 dpi -> 96/25.4 px per mm; label occupies (10..70, 10..25) mm after
      // deletePage()'s reflow carried it up with its page.
      const double pxPerMm = 96.0 / 25.4;
      int dark = 0;
      for ( int y = static_cast<int>( 10 * pxPerMm ); y <= static_cast<int>( 25 * pxPerMm ); ++y )
      {
        for ( int x = static_cast<int>( 10 * pxPerMm ); x <= static_cast<int>( 70 * pxPerMm ); ++x )
        {
          if ( secondImg.pixelColor( x, y ).lightness() < 128 )
            ++dark;
        }
      }
      QVERIFY2( dark > 20, qPrintable( QStringLiteral( "label text not on exported page (%1 dark pixels)" ).arg( dark ) ) );

      // The source layout is untouched by range exports (no cloning side effects).
      QCOMPARE( layout.pageCollection()->pageCount(), 2 );
    }

    void templateRoundTripPreservesItemsAndSizes()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      layout.pageCollection()->page( 0 )->setPageSize( QgsLayoutSize( 210, 297, Qgis::LayoutUnit::Millimeters ) );

      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "ROUNDTRIP" ) );
      label->setId( QStringLiteral( "rt-label" ) );
      label->attemptResize( QgsLayoutSize( 63.5, 21.7, Qgis::LayoutUnit::Millimeters ) );
      layout.addLayoutItem( label );

      auto *shape = QgsLayoutItemShape::create( &layout );
      shape->setId( QStringLiteral( "rt-shape" ) );
      shape->attemptResize( QgsLayoutSize( 88.8, 44.4, Qgis::LayoutUnit::Millimeters ) );
      shape->attemptMove( QgsLayoutPoint( 20, 120, Qgis::LayoutUnit::Millimeters ) );
      layout.addLayoutItem( shape );

      const QgsLayoutSize labelSize = label->sizeWithUnits();
      const QgsLayoutSize shapeSize = shape->sizeWithUnits();
      QCOMPARE( contentItemCount( &layout ), 2 );

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "rt.qpt" ) );
      PaleoLayoutTemplates templates;
      const auto saved = templates.saveTemplate( &layout, path );
      QVERIFY2( saved.ok, qPrintable( saved.error ) );
      QVERIFY( QFile::exists( path ) );

      layout.clear();
      QCOMPARE( contentItemCount( &layout ), 0 );
      QCOMPARE( layout.pageCollection()->pageCount(), 0 );

      const auto loaded = templates.loadTemplate( &layout, path );
      QVERIFY2( loaded.ok, qPrintable( loaded.error ) );
      QCOMPARE( loaded.itemCount, 2 );
      QCOMPARE( contentItemCount( &layout ), 2 );

      QCOMPARE( layout.pageCollection()->pageCount(), 1 );
      const QgsLayoutSize pageSize = layout.pageCollection()->page( 0 )->sizeWithUnits();
      QCOMPARE( pageSize.units(), Qgis::LayoutUnit::Millimeters );
      QVERIFY( qAbs( pageSize.width() - 210.0 ) < 0.01 );
      QVERIFY( qAbs( pageSize.height() - 297.0 ) < 0.01 );

      const QgsLayoutItem *relabel = layout.itemById( QStringLiteral( "rt-label" ) );
      const QgsLayoutItem *reshape = layout.itemById( QStringLiteral( "rt-shape" ) );
      QVERIFY( relabel != nullptr );
      QVERIFY( reshape != nullptr );
      QVERIFY( qAbs( relabel->sizeWithUnits().width() - labelSize.width() ) < 0.01 );
      QVERIFY( qAbs( relabel->sizeWithUnits().height() - labelSize.height() ) < 0.01 );
      QVERIFY( qAbs( reshape->sizeWithUnits().width() - shapeSize.width() ) < 0.01 );
      QVERIFY( qAbs( reshape->sizeWithUnits().height() - shapeSize.height() ) < 0.01 );
    }

    void applyBuiltinSetsPageSize()
    {
      PaleoLayoutTemplates templates;
      const QString tdir = templatesDirUnderRepo();
      QVERIFY2( !tdir.isEmpty(), "docs/templates not found next to tests/" );
      templates.setTemplatesDir( tdir );

      struct Row { const char *key; double w; double h; };
      const Row rows[] = {
        { "a4_landscape", 297.0, 210.0 },
        { "a4_portrait", 210.0, 297.0 },
        { "a0_landscape", 1189.0, 841.0 },
      };

      for ( const Row &row : rows )
      {
        QgsProject project;
        QgsPrintLayout layout( &project );
        layout.initializeDefaults();
        const auto res = templates.applyBuiltin( &layout, QLatin1String( row.key ) );
        QVERIFY2( res.ok, qPrintable( res.error ) );
        QVERIFY2( res.itemCount == 0, "builtins are page-setup-only templates" );
        QCOMPARE( layout.pageCollection()->pageCount(), 1 );
        const QgsLayoutSize size = layout.pageCollection()->page( 0 )->sizeWithUnits();
        QCOMPARE( size.units(), Qgis::LayoutUnit::Millimeters );
        QVERIFY2( qAbs( size.width() - row.w ) < 0.01, qPrintable( QStringLiteral( "%1 width %2" ).arg( row.key ).arg( size.width() ) ) );
        QVERIFY2( qAbs( size.height() - row.h ) < 0.01, qPrintable( QStringLiteral( "%1 height %2" ).arg( row.key ).arg( size.height() ) ) );
      }

      // Unknown key reports failure without touching the layout.
      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      const auto bad = templates.applyBuiltin( &layout, QStringLiteral( "bogus" ) );
      QVERIFY( !bad.ok );
      QVERIFY( !bad.error.isEmpty() );
      QCOMPARE( layout.pageCollection()->pageCount(), 1 ); // unchanged
    }

    void errorPathsAndSignal()
    {
      PaleoLayoutExportActions exports;
      PaleoLayoutTemplates templates;
      const QString tdir = templatesDirUnderRepo();
      QVERIFY2( !tdir.isEmpty(), "docs/templates not found next to tests/" );
      templates.setTemplatesDir( tdir );

      // Null layout.
      auto outcome = exports.exportLayout( nullptr, QStringLiteral( "/tmp/x.png" ),
                                           PaleoLayoutExportActions::Format::Png, 300.0,
                                           PaleoLayoutExportActions::PageRange() );
      QVERIFY( !outcome.ok );
      QVERIFY( !outcome.error.isEmpty() );

      // Unwritable destination: 父路径是普通文件——两平台都必败。
      // （原 /nonexistent-dir-xyz 在 Windows 管理员权限下可建目录而变可写。）
      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      addLabel( &layout, QStringLiteral( "ERR" ) );
      QTemporaryDir unwritableRoot;
      QVERIFY2( unwritableRoot.isValid(), "temp dir for unwritable case" );
      QFile parentAsFile( unwritableRoot.filePath( QStringLiteral( "parent" ) ) );
      QVERIFY( parentAsFile.open( QIODevice::WriteOnly ) );
      parentAsFile.close();
      outcome = exports.exportLayout( &layout, parentAsFile.fileName() + QStringLiteral( "/out.png" ),
                                      PaleoLayoutExportActions::Format::Png, 300.0,
                                      PaleoLayoutExportActions::PageRange() );
      QVERIFY( !outcome.ok );
      QVERIFY( !outcome.error.isEmpty() );

      // Empty page selection.
      PaleoLayoutExportActions::PageRange range;
      range.mode = PaleoLayoutExportActions::PageRange::Mode::Range;
      range.fromPage = 5;
      range.toPage = 9;
      outcome = exports.exportLayout( &layout, QStringLiteral( "/tmp/y.png" ),
                                      PaleoLayoutExportActions::Format::Png, 300.0, range );
      QVERIFY( !outcome.ok );

      // Missing template file.
      const auto missing = templates.loadTemplate( &layout, QStringLiteral( "/nonexistent-dir-xyz/t.qpt" ) );
      QVERIFY( !missing.ok );
      QVERIFY( !missing.error.isEmpty() );

      // exportFinished is emitted from the core export (programmatic + UI paths).
      QSignalSpy spy( &exports, &PaleoLayoutExportActions::exportFinished );
      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "sig.png" ) );
      exports.exportLayout( &layout, path, PaleoLayoutExportActions::Format::Png, 96.0,
                            PaleoLayoutExportActions::PageRange() );
      QCOMPARE( spy.count(), 1 );
      QCOMPARE( spy.at( 0 ).at( 0 ).toString(), path );
      QCOMPARE( spy.at( 0 ).at( 1 ).toBool(), true );
    }

    // #85：注入任务服务后导出在 worker 上跑（版面 XML 快照 → 重建 → 导出），
    // exportFinished 经任务 finished 回包送达——调用方线程不被导出阻塞。
    void asyncExportViaTaskService()
    {
      PaleoTaskService tasks;
      PaleoLayoutExportActions exports;
      exports.setTaskService( &tasks );

      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      addLabel( &layout, QStringLiteral( "ASYNC" ) );

      QTemporaryDir dir;
      QVERIFY( dir.isValid() );
      const QString path = dir.filePath( QStringLiteral( "async.png" ) );

      QSignalSpy spy( &exports, &PaleoLayoutExportActions::exportFinished );
      QVERIFY2( exports.exportLayoutAsync( &layout, path,
                                           PaleoLayoutExportActions::Format::Png, 96.0,
                                           PaleoLayoutExportActions::PageRange() ),
                "exportLayoutAsync refused to take the job" );
      QCOMPARE( tasks.tasks().size(), 1 );

      QVERIFY2( spy.wait( 30000 ), "exportFinished did not arrive from the worker task" );
      QCOMPARE( spy.count(), 1 );
      QCOMPARE( spy.at( 0 ).at( 0 ).toString(), path );
      QCOMPARE( spy.at( 0 ).at( 1 ).toBool(), true );
      QVERIFY( QFile::exists( path ) );
      QVERIFY( !tasks.tasks().first()->running() );
      QCOMPARE( tasks.tasks().first()->state(), PaleoTask::State::Succeeded );

      // 无任务服务时该缝诚实拒绝（调用方回退同步路径）。
      PaleoLayoutExportActions bare;
      QVERIFY( !bare.exportLayoutAsync( &layout, QStringLiteral( "/tmp/nosvc.png" ),
                                        PaleoLayoutExportActions::Format::Png, 96.0,
                                        PaleoLayoutExportActions::PageRange() ) );
      QVERIFY( !bare.exportLayoutAsync( nullptr, QStringLiteral( "/tmp/nosvc.png" ),
                                        PaleoLayoutExportActions::Format::Png, 96.0,
                                        PaleoLayoutExportActions::PageRange() ) );
    }

    // #161：结果按任务携带 + 进行中互斥。第一次故意写进不可写路径，
    // 进行中立刻再导一次 → 第二次被拒（不新建任务、立即报失败、路径是它
    // 自己的）；第一次完成后报的是它自己的失败，第三次正常成功。
    // 旧实现：第二次也起任务（tasks()==2），两次结果共用 m_pendingOutcome 单槽。
    void asyncExportOutcomePerTaskAndBusyRejected()
    {
      PaleoTaskService tasks;
      PaleoLayoutExportActions exports;
      exports.setTaskService( &tasks );

      QgsProject project;
      QgsPrintLayout layout( &project );
      layout.initializeDefaults();
      addLabel( &layout, QStringLiteral( "BUSY" ) );

      QTemporaryDir dir;
      QVERIFY( dir.isValid() );
      QFile blocker( dir.filePath( QStringLiteral( "notadir" ) ) );
      QVERIFY( blocker.open( QIODevice::WriteOnly ) );
      blocker.close();
      const QString badPath = dir.filePath( QStringLiteral( "notadir/first.png" ) ); // 父路径是文件
      const QString secondPath = dir.filePath( QStringLiteral( "second.png" ) );
      const QString thirdPath = dir.filePath( QStringLiteral( "third.png" ) );

      QSignalSpy spy( &exports, &PaleoLayoutExportActions::exportFinished );
      QVERIFY( exports.exportLayoutAsync( &layout, badPath, PaleoLayoutExportActions::Format::Png,
                                          96.0, PaleoLayoutExportActions::PageRange() ) );
      QVERIFY( exports.exportInFlight() );
      QVERIFY( !exports.exportPngAction()->isEnabled() );

      QVERIFY( exports.exportLayoutAsync( &layout, secondPath, PaleoLayoutExportActions::Format::Png,
                                          96.0, PaleoLayoutExportActions::PageRange() ) );
      QCOMPARE( tasks.tasks().size(), 1 ); // 被拒：没有第二个任务
      QCOMPARE( spy.count(), 1 );           // 拒绝即时回报
      QCOMPARE( spy.at( 0 ).at( 0 ).toString(), secondPath );
      QCOMPARE( spy.at( 0 ).at( 1 ).toBool(), false );

      QVERIFY2( spy.wait( 30000 ), "first export never reported" );
      QCOMPARE( spy.count(), 2 );
      QCOMPARE( spy.at( 1 ).at( 0 ).toString(), badPath );
      QCOMPARE( spy.at( 1 ).at( 1 ).toBool(), false );
      QVERIFY( !exports.exportInFlight() );
      QVERIFY( exports.exportPngAction()->isEnabled() );
      QVERIFY( !QFile::exists( secondPath ) );

      QVERIFY( exports.exportLayoutAsync( &layout, thirdPath, PaleoLayoutExportActions::Format::Png,
                                          96.0, PaleoLayoutExportActions::PageRange() ) );
      QVERIFY2( spy.wait( 30000 ), "third export never reported" );
      QCOMPARE( spy.count(), 3 );
      QCOMPARE( spy.at( 2 ).at( 0 ).toString(), thirdPath );
      QCOMPARE( spy.at( 2 ).at( 1 ).toBool(), true );
      QVERIFY( QFile::exists( thirdPath ) );
    }

    // #161：工程快照与活工程零共享——重建出的私有工程图层 id 相同但对象
    // 不同，memory 图层要素随快照带过去，图层树结构一致；抓快照后从活工程
    // 删图层/改要素不影响重建结果。
    void projectSnapshotIsIsolated()
    {
      QgsProject live;
      auto *mem = new QgsVectorLayer( QStringLiteral( "Point?crs=EPSG:4326&field=name:string" ),
                                      QStringLiteral( "wells" ), QStringLiteral( "memory" ) );
      QVERIFY( mem->isValid() );
      for ( int i = 0; i < 3; ++i )
      {
        QgsFeature f( mem->fields() );
        f.setAttribute( 0, QStringLiteral( "w%1" ).arg( i ) );
        f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 110 + i, 30 + i ) ) );
        QVERIFY( mem->dataProvider()->addFeature( f ) );
      }
      live.addMapLayer( mem, false );
      QgsLayerTreeGroup *group = live.layerTreeRoot()->addGroup( QStringLiteral( "井位" ) );
      group->addLayer( mem );
      const QString memId = mem->id();

      PaleoLayoutExportActions::ProjectSnapshot snap;
      QString reason;
      QVERIFY2( PaleoLayoutExportActions::captureProjectSnapshot( &live, snap, &reason ), qPrintable( reason ) );

      // 抓完快照后活工程变化（GUI 线程照常编辑/删图层）
      live.removeMapLayer( memId );
      QVERIFY( !live.mapLayer( memId ) );

      QString error;
      std::unique_ptr<QgsProject> priv = PaleoLayoutExportActions::rebuildProject( snap, &error );
      QVERIFY2( priv, qPrintable( error ) );
      auto *copy = qobject_cast<QgsVectorLayer *>( priv->mapLayer( memId ) );
      QVERIFY( copy );
      QVERIFY( copy->isValid() );
      QCOMPARE( copy->featureCount(), 3LL );
      QCOMPARE( copy->name(), QStringLiteral( "wells" ) );
      QgsLayerTreeGroup *privGroup = priv->layerTreeRoot()->findGroup( QStringLiteral( "井位" ) );
      QVERIFY( privGroup );
      QVERIFY( privGroup->findLayer( memId ) );
      QCOMPARE( privGroup->findLayer( memId )->layer(), copy );

      // 有未提交编辑的图层不能忠实重现 → 诚实拒绝，调用方回退同步。
      auto *editing = new QgsVectorLayer( QStringLiteral( "Point?crs=EPSG:4326" ),
                                          QStringLiteral( "editing" ), QStringLiteral( "memory" ) );
      live.addMapLayer( editing );
      QVERIFY( editing->startEditing() );
      QgsFeature f( editing->fields() );
      f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 1, 1 ) ) );
      QVERIFY( editing->addFeature( f ) );
      QVERIFY( !PaleoLayoutExportActions::captureProjectSnapshot( &live, snap, &reason ) );
      QVERIFY( !reason.isEmpty() );
      editing->rollBack();
    }

    // #161：带地图项的版面经任务池导出——导出启动后立刻从活工程删掉地图
    // 引用的图层（模拟 GUI 线程并发改工程），worker 只渲染私有副本，导出
    // 照常成功且出图非空白。
    void asyncExportSurvivesLiveLayerRemoval()
    {
      PaleoTaskService tasks;
      PaleoLayoutExportActions exports;
      exports.setTaskService( &tasks );

      QgsProject live;
      auto *mem = new QgsVectorLayer( QStringLiteral( "Polygon?crs=EPSG:3857" ),
                                      QStringLiteral( "box" ), QStringLiteral( "memory" ) );
      QgsFeature f( mem->fields() );
      f.setGeometry( QgsGeometry::fromRect( QgsRectangle( 0, 0, 1000, 1000 ) ) );
      QVERIFY( mem->dataProvider()->addFeature( f ) );
      mem->updateExtents();
      live.addMapLayer( mem );
      live.setCrs( QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:3857" ) ) );

      QgsPrintLayout layout( &live );
      layout.initializeDefaults();
      auto *map = new QgsLayoutItemMap( &layout );
      map->attemptMove( QgsLayoutPoint( 10, 10, Qgis::LayoutUnit::Millimeters ) );
      map->attemptResize( QgsLayoutSize( 100, 100, Qgis::LayoutUnit::Millimeters ) );
      map->setCrs( live.crs() );
      map->setLayers( { mem } );
      map->setKeepLayerSet( true );
      map->setExtent( QgsRectangle( -100, -100, 1100, 1100 ) );
      layout.addLayoutItem( map );

      QTemporaryDir dir;
      QVERIFY( dir.isValid() );
      const QString path = dir.filePath( QStringLiteral( "map.png" ) );
      QSignalSpy spy( &exports, &PaleoLayoutExportActions::exportFinished );
      QVERIFY( exports.exportLayoutAsync( &layout, path, PaleoLayoutExportActions::Format::Png,
                                          96.0, PaleoLayoutExportActions::PageRange() ) );
      live.removeMapLayer( mem ); // 活图层删除——worker 不得再碰它

      QVERIFY2( spy.wait( 30000 ), "export never reported" );
      QCOMPARE( spy.at( 0 ).at( 1 ).toBool(), true );
      QImage img( path );
      QVERIFY( !img.isNull() );
      // 地图框中心落在多边形里：默认单符号填充不是纯白
      const QPoint centre( img.width() * 60 / 297, img.height() * 60 / 210 );
      QVERIFY2( img.pixelColor( centre ) != QColor( Qt::white ), "map item rendered blank" );
    }

  private:
    static void addLabel( QgsLayout *layout, const QString &text )
    {
      auto *label = new QgsLayoutItemLabel( layout );
      label->setText( text );
      label->attemptResize( QgsLayoutSize( 60, 15, Qgis::LayoutUnit::Millimeters ) );
      // attemptMove (not raw setPos): only it updates QgsLayoutItem's position
      // bookkeeping, which template/clone serialization round-trips.
      label->attemptMove( QgsLayoutPoint( 10, 10, Qgis::LayoutUnit::Millimeters ) );
      layout->addLayoutItem( label );
    }

    static int contentItemCount( QgsLayout *layout )
    {
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      int count = 0;
      for ( QgsLayoutItem *item : items )
      {
        if ( !dynamic_cast<QgsLayoutItemPage *>( item ) )
          ++count;
      }
      return count;
    }

    // <repo>/docs/templates derived from this test file's location.
    static QString templatesDirUnderRepo()
    {
      QDir dir( QFileInfo( QStringLiteral( __FILE__ ) ).absolutePath() );
      if ( dir.cdUp() && dir.cd( QStringLiteral( "docs" ) ) && dir.cd( QStringLiteral( "templates" ) ) )
        return dir.absolutePath();
      return QString();
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutExport tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layoutexport.moc"
