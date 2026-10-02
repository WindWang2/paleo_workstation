#include <QtTest>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRectF>
#include <QSignalSpy>
#include <QSize>
#include <QSpinBox>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/qgis/mapbooklayout.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/layout/mapbookpanel.h"
#include "../src/workflow/mapbook.h"
#include "../src/workflow/mapbookqueue.h"

#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutpagecollection.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>

// goal/mapbook-reporting 契约测试：AOI 网格序列 → 逐格版面 → 批量导出队列，
// offscreen 全链可测，产物落盘后回读校验（文件头/尺寸/非空），不是看截图。
//
// 覆盖 Oracle：
//   1 地图册闭环（gridSequence/pngRoundTrip 全链 + 命名/计数断言）
//   2 蒙太奇 ≥3 类图项 + 产物非空/尺寸/文件头
//   3 队列：取消语义 + 单版失败其余继续 + 失败明细可查
//   4 模板变量替换（格号/坐标/日期）+ 缺变量如实报错
//   5 分层/清单由 ctest 其它项把守；本文件另测落盘目录结构与 manifest
class TestMapBook : public QObject
{
    Q_OBJECT

  private:
    PaleoMapBook::Area workArea() const { return PaleoMapBook::Area{ 0.0, 0.0, 3000.0, 3000.0 }; }

    // 非 void 夹具：用不了 QVERIFY（它宏展开成 return;），失败交给调用点断言。
    QVector<PaleoMapBook::Tile> grid3x3() const
    {
      PaleoMapBook::GridRequest request;
      request.area = workArea();
      request.cols = 3;
      request.rows = 3;
      QString error;
      const QVector<PaleoMapBook::Tile> tiles = PaleoMapBook::buildGrid( request, &error );
      if ( tiles.isEmpty() )
        qWarning( "grid3x3: %s", qPrintable( error ) );
      return tiles;
    }

    // 造一张小 PNG 当剖面/连井快照（渲染管线的产物在这里用等价物替身）。
    QString makeSnapshot( const QString &dir, const QString &name, const QSize &size ) const
    {
      const QString path = QDir( dir ).filePath( name );
      QImage image( size, QImage::Format_RGB32 );
      image.fill( Qt::white );
      for ( int x = 0; x < image.width(); ++x )
        image.setPixel( x, x % image.height(), qRgb( 40, 90, 160 ) );
      if ( !image.save( path, "PNG" ) )
        qWarning( "makeSnapshot failed: %s", qPrintable( path ) );
      return path;
    }

    QStringList contentItemTypes( QgsLayout *layout ) const
    {
      QStringList types;
      for ( QgsLayoutItem *item : layout->pageCollection()->itemsOnPage( 0 ) )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue; // 纸张不是内容项
        const QString type = QString::fromLatin1( item->metaObject()->className() );
        if ( !types.contains( type ) )
          types << type;
      }
      return types;
    }

  private slots:
    void gridCountsAndNaming()
    {
      const QVector<PaleoMapBook::Tile> tiles = grid3x3();
      QCOMPARE( tiles.size(), 9 );
      for ( int i = 0; i < tiles.size(); ++i )
        QCOMPARE( tiles.at( i ).index, i ); // 序列连续，index 即遍历序

      QCOMPARE( tiles.at( 0 ).name, QStringLiteral( "tile_1_1" ) );
      QCOMPARE( tiles.at( 8 ).name, QStringLiteral( "tile_3_3" ) );
      // 首格贴 AOI 左下、末格贴右上（浮点误差不累积到边）。
      QCOMPARE( tiles.at( 0 ).extent.xMin, 0.0 );
      QCOMPARE( tiles.at( 0 ).extent.xMax, 1000.0 );
      QCOMPARE( tiles.at( 8 ).extent.yMin, 2000.0 );
      QCOMPARE( tiles.at( 8 ).extent.yMax, 3000.0 );

      // 列主序：先走完一列内的各行。
      PaleoMapBook::GridRequest request;
      request.area = workArea();
      request.cols = 3;
      request.rows = 3;
      request.order = PaleoMapBook::Order::ColumnMajor;
      const QVector<PaleoMapBook::Tile> column = PaleoMapBook::buildGrid( request );
      QCOMPARE( column.size(), 9 );
      QCOMPARE( column.at( 0 ).row, 1 );
      QCOMPARE( column.at( 0 ).col, 1 );
      QCOMPARE( column.at( 1 ).row, 2 );
      QCOMPARE( column.at( 1 ).col, 1 );
      QCOMPARE( column.at( 3 ).row, 1 );
      QCOMPARE( column.at( 3 ).col, 2 );
    }

    void gridRejectsBadInputHonestly()
    {
      QString error;
      PaleoMapBook::GridRequest request;
      request.area = workArea();
      request.cols = 0;
      request.rows = 3;
      QVERIFY( PaleoMapBook::buildGrid( request, &error ).isEmpty() );
      QVERIFY( !error.isEmpty() );

      error.clear();
      request.cols = 3;
      request.area = PaleoMapBook::Area{ 3000.0, 0.0, 0.0, 3000.0 }; // 退化
      QVERIFY( PaleoMapBook::buildGrid( request, &error ).isEmpty() );
      QVERIFY( !error.isEmpty() );

      error.clear();
      request.area = workArea();
      request.namePattern = QStringLiteral( "tile_%{nope}" ); // 未定义变量
      QVERIFY( PaleoMapBook::buildGrid( request, &error ).isEmpty() );
      QVERIFY2( error.contains( QStringLiteral( "nope" ) ), qPrintable( error ) );
    }

    void tilesFromGivenAreaSet()
    {
      QVector<PaleoMapBook::Area> areas;
      areas << PaleoMapBook::Area{ 0.0, 0.0, 1000.0, 1000.0, QStringLiteral( "north" ) }
            << PaleoMapBook::Area{ 1000.0, 0.0, 2000.0, 1000.0, QStringLiteral( "middle" ) }
            << PaleoMapBook::Area{ 2000.0, 0.0, 3000.0, 1000.0, QString() }; // 无名 → 兜底
      QString error;
      const QVector<PaleoMapBook::Tile> tiles = PaleoMapBook::tilesFromAreas( areas, &error );
      QVERIFY2( error.isEmpty(), qPrintable( error ) );
      QCOMPARE( tiles.size(), 3 );
      QCOMPARE( tiles.at( 0 ).name, QStringLiteral( "north" ) );
      QCOMPARE( tiles.at( 2 ).name, QStringLiteral( "area_2" ) );
      for ( int i = 0; i < tiles.size(); ++i )
        QCOMPARE( tiles.at( i ).index, i );

      QString badError;
      QVector<PaleoMapBook::Area> bad;
      bad << areas.at( 0 ) << PaleoMapBook::Area{ 0.0, 0.0, 0.0, 0.0, QStringLiteral( "empty" ) };
      QVERIFY( PaleoMapBook::tilesFromAreas( bad, &badError ).isEmpty() );
      QVERIFY2( badError.contains( QStringLiteral( "empty" ) ), qPrintable( badError ) );
    }

    void variableSubstitutionCoversTileAndDate()
    {
      const QVector<PaleoMapBook::Tile> tiles = grid3x3();
      PaleoMapBook::TileContext context;
      context.tile = tiles.at( 4 );
      context.book = QStringLiteral( "工区A" );
      context.horizon = QStringLiteral( "D61" );
      context.date = QStringLiteral( "2026-10-02" );
      const QVariantMap vars = PaleoMapBook::tileVariables( context );
      QCOMPARE( vars.keys().size(), PaleoMapBook::variableNames().size() );

      QString error;
      const QString text = PaleoMapBook::applyVariables(
        QStringLiteral( "%{book}/%{horizon}/%{tile_label}/%{center_x}/%{date}" ), vars, &error );
      QVERIFY2( error.isEmpty(), qPrintable( error ) );
      QCOMPARE( text, QStringLiteral( "工区A/D61/R2C2/1500/2026-10-02" ) );

      const QStringList refs = PaleoMapBook::referencedVariables(
        QStringLiteral( "%{tile_name}%{tile_name}%{extent}" ), &error );
      QVERIFY( error.isEmpty() );
      QCOMPARE( refs, QStringList( { QStringLiteral( "tile_name" ), QStringLiteral( "extent" ) } ) );
    }

    void missingVariableFailsLoudly()
    {
      const QVector<PaleoMapBook::Tile> tiles = grid3x3();
      PaleoMapBook::TileContext context;
      context.tile = tiles.at( 0 );
      const QVariantMap vars = PaleoMapBook::tileVariables( context );

      QString error;
      const QString out = PaleoMapBook::applyVariables( QStringLiteral( "%{nope}" ), vars, &error );
      QVERIFY( out.isEmpty() );           // 不静默留空串当成功
      QVERIFY2( error.contains( QStringLiteral( "nope" ) ), qPrintable( error ) );

      error.clear();
      const QString unclosed =
          PaleoMapBook::applyVariables( QStringLiteral( "tile_%{tile_index" ), vars, &error );
      QVERIFY( unclosed.isEmpty() );
      QVERIFY2( error.contains( QStringLiteral( "未闭合" ) ), qPrintable( error ) );
    }

    void tileLayoutSlotsAndPngRoundTrip()
    {
      QgsProject project;
      PaleoMapBookLayout::TileSpec spec;
      spec.title = QStringLiteral( "工区A R1C1" );
      spec.footer = QStringLiteral( "R1C1 · 中心 500, 500 · 工程坐标" );
      spec.extent = QgsRectangle( 0.0, 0.0, 1000.0, 1000.0 );

      QString error;
      QgsPrintLayout *layout = PaleoMapBookLayout::buildTileLayout( &project, spec, &error );
      QVERIFY2( layout != nullptr, qPrintable( error ) );

      // 固定模板位：标题/地图/图例/比例尺/指北针/页脚一个都不能少。
      for ( const QString &id : { QStringLiteral( "title" ), QStringLiteral( "map" ),
                                  QStringLiteral( "legend" ), QStringLiteral( "scalebar" ),
                                  QStringLiteral( "northArrow" ), QStringLiteral( "footer" ) } )
        QVERIFY2( layout->itemById( id ) != nullptr, qPrintable( id ) );
      auto *map = qobject_cast<QgsLayoutItemMap *>( layout->itemById( QStringLiteral( "map" ) ) );
      QVERIFY( map != nullptr );
      QCOMPARE( map->extent(), QgsRectangle( 0.0, 0.0, 1000.0, 1000.0 ) );
      // 范围原样输出的同时，地图框不得越出版面可用区（A4 横版 217 × 152 mm）。
      QVERIFY( map->sizeWithUnits().width() <= 217.0 + 1e-6 );
      QVERIFY( map->sizeWithUnits().height() <= 152.0 + 1e-6 );

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "tile_1_1.png" ) );
      const auto out = PaleoMapBookLayout::renderLayout( layout, path,
                                                         PaleoLayoutExport::Format::Png, 300.0 );
      delete layout;
      QVERIFY2( out.ok, qPrintable( out.error ) );
      QCOMPARE( out.path, path );
      QVERIFY( out.bytes > 0 );
      // A4 横版 300dpi ≈ 3508 × 2480 px。
      QVERIFY( qAbs( out.width - 3508 ) <= 3 );
      QVERIFY( qAbs( out.height - 2480 ) <= 3 );

      QFile file( path );
      QVERIFY( file.open( QIODevice::ReadOnly ) );
      QCOMPARE( file.read( 8 ), QByteArray( "\x89PNG\x0D\x0A\x1A\x0A", 8 ) );
      file.close();
      const QImage image( path );
      QVERIFY( !image.isNull() );
    }

    void montageCombinesThreeItemKinds()
    {
      QTemporaryDir dir;
      const QString section = makeSnapshot( dir.path(), QStringLiteral( "section.png" ),
                                            QSize( 320, 160 ) );
      const QString well = makeSnapshot( dir.path(), QStringLiteral( "well.png" ),
                                         QSize( 240, 180 ) );

      QgsProject project;
      PaleoMapBookLayout::MontageSpec spec;
      spec.title = QStringLiteral( "工区A D61 组合页" );
      spec.mapCaption = QStringLiteral( "平面图（厚度）" );
      spec.sectionCaption = QStringLiteral( "剖面快照" );
      spec.wellCaption = QStringLiteral( "连井小图" );
      spec.mapExtent = QgsRectangle( 0.0, 0.0, 3000.0, 3000.0 );
      spec.sectionImage = section;
      spec.wellImage = well;

      QString error;
      QgsPrintLayout *layout = PaleoMapBookLayout::buildMontageLayout( &project, spec, &error );
      QVERIFY2( layout != nullptr, qPrintable( error ) );

      // ≥3 类图项组合：地图项（平面图）+ 图片项 ×2（剖面/连井）+ 标签（标题/小标题）。
      const QStringList types = contentItemTypes( layout );
      QVERIFY2( types.contains( QStringLiteral( "QgsLayoutItemMap" ) ), types.join( ',' ).toUtf8() );
      QVERIFY2( types.contains( QStringLiteral( "QgsLayoutItemPicture" ) ),
                types.join( ',' ).toUtf8() );
      QVERIFY2( types.contains( QStringLiteral( "QgsLayoutItemLabel" ) ),
                types.join( ',' ).toUtf8() );
      QVERIFY2( types.size() >= 3, types.join( ',' ).toUtf8() );
      QVERIFY( qobject_cast<QgsLayoutItemPicture *>(
                 layout->itemById( QStringLiteral( "sectionSnapshot" ) ) ) != nullptr );
      QVERIFY( qobject_cast<QgsLayoutItemPicture *>(
                 layout->itemById( QStringLiteral( "wellPanel" ) ) ) != nullptr );

      // 竖版 A4 下两栏不得出页、不得重叠（几何由页面尺寸推导，不写死右栏 x）。
      const QRectF pageRect = layout->pageCollection()->page( 0 )->sceneBoundingRect();
      for ( QgsLayoutItem *item : layout->pageCollection()->itemsOnPage( 0 ) )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue;
        const QRectF r = item->sceneBoundingRect();
        QVERIFY2( r.left() >= -0.5 && r.top() >= -0.5 &&
                  r.right() <= pageRect.width() + 0.5 &&
                  r.bottom() <= pageRect.height() + 0.5,
                  qPrintable( QStringLiteral( "%1 出页：x=%2 y=%3 w=%4 h=%5" )
                                .arg( item->id() ).arg( r.left() ).arg( r.top() )
                                .arg( r.width() ).arg( r.height() ) ) );
      }
      QgsLayoutItem *planItem = layout->itemById( QStringLiteral( "planMap" ) );
      QgsLayoutItem *sectionItem = layout->itemById( QStringLiteral( "sectionSnapshot" ) );
      QVERIFY( planItem != nullptr && sectionItem != nullptr );
      QVERIFY2( planItem->sceneBoundingRect().right() <=
                  sectionItem->sceneBoundingRect().left() + 0.5,
                qPrintable( QStringLiteral( "左栏右边界 %1 压过右栏左边界 %2" )
                              .arg( planItem->sceneBoundingRect().right() )
                              .arg( sectionItem->sceneBoundingRect().left() ) ) );

      const QString path = dir.filePath( QStringLiteral( "montage.png" ) );
      const auto out = PaleoMapBookLayout::renderLayout( layout, path,
                                                         PaleoLayoutExport::Format::Png, 200.0 );
      delete layout;
      QVERIFY2( out.ok, qPrintable( out.error ) );
      QVERIFY( out.bytes > 0 );
      QVERIFY( out.width > 0 && out.height > 0 );
      QFile file( path );
      QVERIFY( file.open( QIODevice::ReadOnly ) );
      QCOMPARE( file.read( 8 ), QByteArray( "\x89PNG\x0D\x0A\x1A\x0A", 8 ) );
      file.close();
    }

    void batchQueueExportsEveryTileAndWritesManifest()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_3x3" );
      request.outputDir = dir.path();
      request.tiles = grid3x3();
      request.dpi = 150.0;
      request.withIndexPage = true;
      request.date = QStringLiteral( "2026-10-02" );

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.total, 9 );
      QCOMPARE( result.succeeded, 9 );
      QCOMPARE( result.failed, 0 );
      QCOMPARE( result.cancelled, 0 );
      QCOMPARE( result.files.size(), 9 );
      QVERIFY( result.complete() );

      // 落盘目录结构钉死：<out>/<book>/{pages,index,manifest.json}
      QVERIFY( QDir( dir.filePath( QStringLiteral( "book_3x3/pages" ) ) ).exists() );
      QVERIFY( QDir( dir.filePath( QStringLiteral( "book_3x3/index" ) ) ).exists() );
      QVERIFY( QFile::exists( dir.filePath( QStringLiteral( "book_3x3/manifest.json" ) ) ) );
      for ( const QString &file : result.files )
      {
        QVERIFY2( QFile::exists( file ), qPrintable( file ) );
        QVERIFY( QFileInfo( file ).size() > 0 );
        QVERIFY2( file.contains( QStringLiteral( "/pages/" ) ), qPrintable( file ) );
        QVERIFY2( file.endsWith( QStringLiteral( ".png" ) ), qPrintable( file ) );
      }
      QVERIFY( !result.indexPage.isEmpty() );
      QVERIFY( QFile::exists( result.indexPage ) );

      // manifest 是账：条目数、成功数、失败明细都在里面。
      QFile manifest( result.manifest );
      QVERIFY( manifest.open( QIODevice::ReadOnly ) );
      const QJsonObject root = QJsonDocument::fromJson( manifest.readAll() ).object();
      manifest.close();
      QCOMPARE( root.value( QStringLiteral( "book" ) ).toString(), QStringLiteral( "book_3x3" ) );
      QCOMPARE( root.value( QStringLiteral( "tiles" ) ).toInt(), 9 );
      QCOMPARE( root.value( QStringLiteral( "succeeded" ) ).toInt(), 9 );
      QCOMPARE( root.value( QStringLiteral( "pages" ) ).toArray().size(), 9 );
      QCOMPARE( root.value( QStringLiteral( "failures" ) ).toArray().size(), 0 );
      QCOMPARE( root.value( QStringLiteral( "generated" ) ).toString(), QStringLiteral( "2026-10-02" ) );
    }

    void batchQueueSingleFailureDoesNotKillTheRest()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      QVector<PaleoMapBook::Tile> tiles = grid3x3();
      const QString doomedName = tiles.at( 4 ).name;
      tiles[4].extent = PaleoMapBook::Area{ 500.0, 500.0, 500.0, 500.0 }; // 退化范围 → 版面拒绝

      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_partial" );
      request.outputDir = dir.path();
      request.tiles = tiles;
      request.dpi = 150.0;
      request.withIndexPage = true;

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.total, 9 );
      QCOMPARE( result.succeeded, 8 ); // 坏的那一版没拖死其余
      QCOMPARE( result.failed, 1 );
      QCOMPARE( result.cancelled, 0 );
      QCOMPARE( result.files.size(), 8 );
      QVERIFY( !result.complete() );

      // 失败明细可查：格号 + 阶段 + 原因。
      QCOMPARE( result.failures.size(), 1 );
      const PaleoMapBookQueue::Failure failure = result.failures.first();
      QCOMPARE( failure.tile, doomedName );
      QCOMPARE( failure.stage, QStringLiteral( "layout" ) );
      QVERIFY( !failure.error.isEmpty() );
      QVERIFY2( result.summary().contains( QStringLiteral( "8" ) ), qPrintable( result.summary() ) );

      QFile manifest( result.manifest );
      QVERIFY( manifest.open( QIODevice::ReadOnly ) );
      const QJsonObject root = QJsonDocument::fromJson( manifest.readAll() ).object();
      manifest.close();
      QCOMPARE( root.value( QStringLiteral( "failures" ) ).toArray().size(), 1 );
      QCOMPARE( root.value( QStringLiteral( "failures" ) )
                  .toArray().first().toObject()
                  .value( QStringLiteral( "stage" ) ).toString(),
                QStringLiteral( "layout" ) );
    }

    void batchQueueCancelStopsOnlyTheRemaining()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      // (a) 预演上限：前 3 版照出，其余记 cancelled。
      PaleoMapBookQueue::Request preview;
      preview.book = QStringLiteral( "book_preview" );
      preview.outputDir = dir.path();
      preview.tiles = grid3x3();
      preview.dpi = 150.0;
      preview.previewLimit = 3;
      const PaleoMapBookQueue::Result partial = exporter.run( nullptr, preview );
      QCOMPARE( partial.succeeded, 3 );
      QCOMPARE( partial.cancelled, 6 );
      QCOMPARE( partial.failed, 0 );
      QCOMPARE( partial.files.size(), 3 );

      // (b) 协作式取消：开跑前已 requestCancel → 一版都不出，全记 cancelled
      //（不谎报成功）。
      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_cancel" );
      request.outputDir = dir.path();
      request.tiles = grid3x3();
      request.dpi = 150.0;
      PaleoTask *task = tasks.start( QStringLiteral( "mapbook-cancel" ),
                                     []( PaleoTask * ) { return QString(); } );
      QVERIFY( task != nullptr );
      task->requestCancel();
      const PaleoMapBookQueue::Result cancelled = exporter.run( task, request );
      QCOMPARE( cancelled.total, 9 );
      QCOMPARE( cancelled.succeeded, 0 );
      QCOMPARE( cancelled.cancelled, 9 );
      QVERIFY( cancelled.files.isEmpty() );
    }

    void batchQueueRejectsBadSetupBeforeTouchingDisk()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;

      PaleoMapBookQueue::Request badPattern;
      badPattern.book = QStringLiteral( "book_bad" );
      badPattern.outputDir = dir.path();
      badPattern.tiles = grid3x3();
      badPattern.titlePattern = QStringLiteral( "%{not_a_variable}" );
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );
      const PaleoMapBookQueue::Result result = exporter.run( nullptr, badPattern );
      QCOMPARE( result.succeeded, 0 );
      QCOMPARE( result.failed, 9 );
      QCOMPARE( result.failures.size(), 1 );
      QCOMPARE( result.failures.first().stage, QStringLiteral( "prepare" ) );
      QVERIFY( !QDir( dir.filePath( QStringLiteral( "book_bad" ) ) ).exists() );
    }

    void batchQueueRegistersArtifactsInCatalog()
    {
      QTemporaryDir dir;
      QVERIFY2( dir.isValid(), qPrintable( dir.errorString() ) );
      DataCatalog catalog;
      QVERIFY2( catalog.open( dir.path() ), qPrintable( catalog.openError() ) );

      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_reg" );
      request.outputDir = dir.path();
      request.tiles = grid3x3().mid( 0, 2 );
      request.dpi = 150.0;
      request.catalog = &catalog;
      request.projectDir = dir.path();

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.succeeded, 2 );
      QCOMPARE( result.registered.size(), 2 );
      for ( const QString &managed : result.registered )
      {
        QVERIFY2( QFile::exists( managed ), qPrintable( managed ) );
        QVERIFY2( managed.contains( QStringLiteral( "artifacts/output/" ) ), qPrintable( managed ) );
      }

      // 入库元数据必须如实：PNG 册不许被印成 pdf。
      QCOMPARE( result.registeredIds.size(), 2 );
      for ( const QString &id : result.registeredIds )
        QCOMPARE( catalog.assetById( id ).format, QStringLiteral( "png" ) );
    }

    void panelOnlyEmitsSignals()
    {
      QTemporaryDir dir;
      PaleoMapBookPanel panel;
      QSignalSpy started( &panel, &PaleoMapBookPanel::batchRequested );
      QSignalSpy cancelled( &panel, &PaleoMapBookPanel::cancelRequested );

      panel.setBookName( QStringLiteral( "book_ui" ) );
      panel.setOutputDir( dir.path() );
      panel.setArea( workArea() );
      panel.setGrid( 3, 4 );
      QCOMPARE( panel.bookName(), QStringLiteral( "book_ui" ) );
      QCOMPARE( panel.cols(), 3 );
      QCOMPARE( panel.rows(), 4 );
      QCOMPARE( panel.previewTiles().size(), 12 );

      // 「全部」在界面上是特殊值 0，传到队列必须是 -1（0 会被当成一版不出）。
      QSpinBox *preview = panel.findChild<QSpinBox *>( QStringLiteral( "mapbookPreviewLimit" ) );
      QVERIFY( preview != nullptr );
      preview->setValue( 0 );
      QCOMPARE( panel.previewLimit(), -1 );
      preview->setValue( 4 );
      QCOMPARE( panel.previewLimit(), 4 );

      // 参数不全 → 不发信号（视图不做业务判断，只拒发并留一行日志）。
      panel.setOutputDir( QString() );
      QPushButton *start = panel.findChild<QPushButton *>( QStringLiteral( "mapbookStart" ) );
      QVERIFY( start != nullptr );
      start->click();
      QCOMPARE( started.count(), 0 );

      panel.setOutputDir( dir.path() );
      start->click();
      QCOMPARE( started.count(), 1 ); // 视图只发信号，编排在功能层

      // 取消键只在忙碌态可用（禁用按钮的 click 不发信号）：先进入忙碌态再点。
      panel.setBusy( true );
      QPushButton *cancel = panel.findChild<QPushButton *>( QStringLiteral( "mapbookCancel" ) );
      QVERIFY( cancel != nullptr );
      QVERIFY( cancel->isEnabled() );
      cancel->click();
      QCOMPARE( cancelled.count(), 1 );

      panel.setProgress( 2, 12 );
      QCOMPARE( panel.findChild<QProgressBar *>( QStringLiteral( "mapbookProgress" ) )->value(), 2 );
      panel.setBusy( false );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QgisRuntime::defaultPrefixPath() ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestMapBook tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_mapbook.moc"
