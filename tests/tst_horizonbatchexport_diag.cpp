#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/qgis/layoutexport.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/standardelements.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/horizonbatchexport.h"
#include "../src/workflow/mapexport.h"

#include <qgsfeature.h>
#include <qgsfillsymbol.h>
#include <qgsgeometry.h>
#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutundostack.h>
#include <qgsmaplayer.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgspolygon.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsvectorlayer.h>

// 方向 25 M4：按层位组批量出图——计数=层位组数（Oracle 2）、文件名规则
// （工程_层位_日期）、catalog OUTPUT 登记、版面批后复原。
// 纪律同 tst_layoutdesigner_full：QgsProject 堆分配并故意泄漏。
class TestHorizonBatchExport : public QObject
{
  Q_OBJECT

  private:
    QgsPrintLayout *makeFigureLayout()
    {
      auto *project = new QgsProject(); // leaked on purpose
      auto *layout = new QgsPrintLayout( project );
      layout->initializeDefaults();
      PaleoStandardElements::populateFigureLayout(
          layout, PaleoStandardElements::FigureKind::WellPosition,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() );
      // 定框后设真实范围（复原断言需要一个非退化的初始范围）。
      QList<QgsLayoutItemMap *> maps;
      layout->layoutItems( maps );
      if ( !maps.isEmpty() )
        maps.first()->setExtent( QgsRectangle( 0, 0, 2000, 1400 ) );
      return layout;
    }

    // 纯色多边形层（红填充无描边），覆盖 0..1000 × 0..800——像素一致性断言
    // 的渲染源（内部纯色，避开反锯齿边缘）。
    QgsVectorLayer *makeSolidLayer()
    {
      auto *layer = new QgsVectorLayer( QStringLiteral( "Polygon?crs=EPSG:3857" ),
                                        QStringLiteral( "solid" ),
                                        QStringLiteral( "memory" ) );
      QgsFeature feature;
      feature.setGeometry( QgsGeometry::fromWkt(
          QStringLiteral( "POLYGON((0 0, 1000 0, 1000 800, 0 800, 0 0))" ) ) );
      layer->dataProvider()->addFeature( feature );
      layer->setRenderer( new QgsSingleSymbolRenderer( QgsFillSymbol::createSimple(
                              { { QStringLiteral( "color" ), QStringLiteral( "255,0,0,255" ) },
                                { QStringLiteral( "outline_style" ), QStringLiteral( "no" ) } } )
                              .release() ) );
      layer->updateExtents();
      return layer;
    }

    // 内存多边形层：整幅一个纯色矩形（像素一致性断言用得到时也可复用）。
    QgsVectorLayer *makeLayer( const QString &name )
    {
      auto *layer = new QgsVectorLayer( QStringLiteral( "Polygon?crs=EPSG:3857" ), name,
                                        QStringLiteral( "memory" ) );
      layer->updateExtents();
      return layer;
    }

  private slots:
    void countsMatchHorizonGroupsAndRestoresLayout()
    {
      QTemporaryDir projectDir;
      QVERIFY( projectDir.isValid() );
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );

      std::unique_ptr<QgsPrintLayout> layout( makeFigureLayout() );

      // 三个层位组（各带图层）。
      QMap<QString, QList<QgsMapLayer *>> groups;
      QList<QgsMapLayer *> keepAlive; // 内存层生命周期：批处理期间必须存活
      for ( const QString &h : { QStringLiteral( "SB1" ), QStringLiteral( "SB2" ),
                                 QStringLiteral( "SB3" ) } )
      {
        auto *layer = makeLayer( h + QStringLiteral( "_layer" ) );
        keepAlive.append( layer );
        groups.insert( h, { layer } );
      }

      // 版面原状（断言复原用）。
      QList<QgsLayoutItemMap *> maps;
      layout->layoutItems( maps );
      QVERIFY( !maps.isEmpty() );
      auto *titleLabel = qobject_cast<QgsLayoutItemLabel *>(
          layout->itemById( QStringLiteral( "title" ) ) );
      QVERIFY( titleLabel != nullptr );
      const QString titleBackup = titleLabel->text();
      const QgsRectangle extentBackup = maps.first()->extent();
      const int layerCountBackup = maps.first()->layers().size();

      PaleoHorizonBatchExport::Request request;
      request.layout = layout.get();
      request.projectName = QStringLiteral( "demo 工程" ); // 含空格/中文——文件名消毒对象
      request.horizonLayers = groups;
      request.catalog = &catalog;
      request.projectDir = projectDir.path();
      request.dpi = 96.0;
      request.format = PaleoHorizonBatchExport::Format::Png;
      request.date = QStringLiteral( "20261004" );

      const auto result = PaleoHorizonBatchExport::run( request );

      // Oracle 2：计数 = 层位组数。
      QCOMPARE( result.total, 3 );
      QCOMPARE( result.succeeded, 3 );
      QCOMPARE( result.failed, 0 );
      QCOMPARE( result.horizons.size(), 3 );

      // 文件名规则 + 受管落位 + catalog 登记。
      for ( const auto &outcome : result.horizons )
      {
        QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
        QVERIFY( !outcome.assetId.isEmpty() );
        QVERIFY( outcome.sha256.size() == 64 );
        QVERIFY( QFile::exists( outcome.file ) );
        const CatalogAsset asset = catalog.assetById( outcome.assetId );
        QCOMPARE( asset.type, QStringLiteral( "document" ) );
        QCOMPARE( asset.format, QStringLiteral( "png" ) );
        const CatalogVersion ver = catalog.versionBySha256( outcome.sha256 );
        QCOMPARE( ver.stage, QStringLiteral( "OUTPUT" ) );
        QVERIFY( ver.fileName.startsWith( QStringLiteral( "demo_工程_" ) ) );
        QVERIFY( ver.fileName.contains( QStringLiteral( "20261004" ) ) );
        QVERIFY( ver.fileName.endsWith( QStringLiteral( ".png" ) ) );
        QCOMPARE( ver.sha256, outcome.sha256 );
      }

      // 版面复原：标题文本与地图项状态回到批前（逐字段）。
      QCOMPARE( titleLabel->text(), titleBackup );
      QCOMPARE( maps.first()->layers().size(), layerCountBackup );
      QVERIFY( qAbs( maps.first()->extent().width() - extentBackup.width() ) < 1e-6 );
      QVERIFY( qAbs( maps.first()->extent().height() - extentBackup.height() ) < 1e-6 );

      // undo 历史不被批处理污染。
      QCOMPARE( layout->undoStack()->stack()->count(), 0 );
    }

    // startAsync（#349 后台批量出图路径）：任务化出图在 worker 私有工程上
    // 完成、journal 经 owner 线程发布登记、结果回包带真实文件路径；同版面
    // 重入被 busy 拒绝；完成后 busy 标记释放。修复前该路径零测试（只测
    // 同步 run()）。
    void startAsyncExportsPublishesAndClearsBusy()
    {
      QTemporaryDir projectDir;
      QVERIFY( projectDir.isValid() );
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );

      std::unique_ptr<QgsPrintLayout> layout( makeFigureLayout() );

      QMap<QString, QList<QgsMapLayer *>> groups;
      QList<QgsMapLayer *> keepAlive;
      for ( const QString &h : { QStringLiteral( "SB1" ), QStringLiteral( "SB2" ) } )
      {
        auto *layer = makeLayer( h + QStringLiteral( "_layer" ) );
        keepAlive.append( layer );
        layout->project()->addMapLayer( layer, false ); // 快照源：须在版面工程里
        groups.insert( h, { layer } );
      }

      PaleoHorizonBatchExport::Request request;
      request.layout = layout.get();
      request.projectName = QStringLiteral( "async 工程" );
      request.horizonLayers = groups;
      request.catalog = &catalog;
      request.projectDir = projectDir.path();
      request.dpi = 96.0;
      request.format = PaleoHorizonBatchExport::Format::Png;
      request.date = QStringLiteral( "20261013" );

      PaleoTaskService tasks;
      bool doneCalled = false;
      PaleoHorizonBatchExport::Result result;
      QString err;
      PaleoTask *task = PaleoHorizonBatchExport::startAsync(
          request, &tasks,
          [&doneCalled, &result]( const PaleoHorizonBatchExport::Result &r, bool ) {
            result = r;
            doneCalled = true;
          },
          &err );
      QVERIFY2( task != nullptr, qPrintable( err ) );

      // busy 重入：同版面第二发起被拒（无新任务、有原因）。
      {
        QString busyErr;
        QVERIFY( PaleoHorizonBatchExport::startAsync( request, &tasks, {}, &busyErr ) ==
                 nullptr );
        QVERIFY2( busyErr.contains( QStringLiteral( "已有批量出图" ) ),
                  qPrintable( busyErr ) );
        QCOMPARE( tasks.tasks().size(), 1 );
      }

      QSignalSpy finished( task, &PaleoTask::finished );
      QVERIFY2( finished.wait( 60000 ), "async batch export never finished" );
      QTRY_VERIFY_WITH_TIMEOUT( doneCalled, 5000 );
      QCOMPARE( task->state(), PaleoTask::State::Succeeded );

      QCOMPARE( result.total, 2 );
      QCOMPARE( result.succeeded, 2 );
      QCOMPARE( result.failed, 0 );
      for ( const auto &outcome : result.horizons )
      {
        QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
        QVERIFY2( QFile::exists( outcome.file ),
                  qPrintable( outcome.file ) ); // owner 线程发布后的真实路径
        const CatalogVersion ver =
            catalog.versionBySha256( outcome.sha256 );
        QCOMPARE( ver.stage, QStringLiteral( "OUTPUT" ) );
      }

      // busy 释放在回包内：后续可再发起。
      QVERIFY( !layout->property( "paleoBatchExportRunning" ).toBool() );
      QString err2;
      PaleoTask *second = PaleoHorizonBatchExport::startAsync(
          request, &tasks, {}, &err2 );
      QVERIFY2( second != nullptr, qPrintable( err2 ) );
      second->requestCancel();
      QSignalSpy finished2( second, &PaleoTask::finished );
      QVERIFY( finished2.wait( 60000 ) );
    }

    void emptyHorizonReportedAndMissingLayoutBails()
    {
      QTemporaryDir projectDir;
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );

      // 无版面：整体失败，逐层位入账。
      {
        QMap<QString, QList<QgsMapLayer *>> groups;
        groups.insert( QStringLiteral( "SB1" ), {} );
        PaleoHorizonBatchExport::Request request;
        request.layout = nullptr;
        request.horizonLayers = groups;
        request.catalog = &catalog;
        request.projectDir = projectDir.path();
        const auto result = PaleoHorizonBatchExport::run( request );
        QCOMPARE( result.total, 1 );
        QCOMPARE( result.failed, 1 );
        QVERIFY( !result.horizons.first().error.isEmpty() );
      }

      // 空图层集层位：如实失败（不静默跳过）；好层位照出。
      {
        std::unique_ptr<QgsPrintLayout> layout( makeFigureLayout() );
        auto *layer = makeLayer( QStringLiteral( "only" ) );
        QMap<QString, QList<QgsMapLayer *>> groups;
        groups.insert( QStringLiteral( "SB1" ), QList<QgsMapLayer *>() ); // 空
        groups.insert( QStringLiteral( "SB2" ), { layer } );
        PaleoHorizonBatchExport::Request request;
        request.layout = layout.get();
        request.projectName = QStringLiteral( "p" );
        request.horizonLayers = groups;
        request.catalog = &catalog;
        request.projectDir = projectDir.path();
        request.dpi = 96.0;
        const auto result = PaleoHorizonBatchExport::run( request );
        QCOMPARE( result.total, 2 ); // 计数 = 键数（Oracle 2 语义）
        QCOMPARE( result.succeeded, 1 );
        QCOMPARE( result.failed, 1 );
        QCOMPARE( result.horizons.first().horizon, QStringLiteral( "SB1" ) );
        QVERIFY( !result.horizons.first().ok );
        QVERIFY( result.horizons.last().ok );
        QVERIFY( !result.summary().isEmpty() );
      }

      // resolveHorizonLayers 防呆：空服务回空表。
      QVERIFY( PaleoHorizonBatchExport::resolveHorizonLayers( nullptr ).isEmpty() );
    }

    // Oracle 2 前半：导出与画布同图层渲染像素抽样一致。
    void exportMatchesCanvasPixels()
    {
      auto *project = new QgsProject(); // leaked on purpose
      QgsVectorLayer *layer = makeSolidLayer();
      project->addMapLayer( layer ); // 布局渲染管线要求图层挂工程

      // 布局：A4 横版，地图项铺满整页（无页边距 → 导出像素与画布像素同构）。
      auto *layout = new QgsPrintLayout( project );
      layout->initializeDefaults();
      const QgsRectangle extent( 0, 0, 1000, 800 );
      auto *map = PaleoStandardElements::addMainMap( layout, { layer }, QgsRectangle() );
      QVERIFY( map != nullptr );
      map->attemptResize( QgsLayoutSize( 297, 210, Qgis::LayoutUnit::Millimeters ) );
      map->attemptMove( QgsLayoutPoint( 0, 0, Qgis::LayoutUnit::Millimeters ) );
      map->setExtent( extent ); // 定框后设范围（mapbook 顺序纪律）

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "match.png" ) );
      const auto outcome = PaleoLayoutExport::exportLayout(
          layout, path, PaleoLayoutExport::Format::Png, 96.0, PaleoLayoutExport::PageRange() );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QImage exported( path );
      QVERIFY( !exported.isNull() );
      QCOMPARE( exported.format(), QImage::Format_ARGB32 );

      // 画布同口径：同一 MapSettings 管线（QgsMapCanvas 渲染走的就是它）。
      QgsMapSettings settings;
      settings.setLayers( { layer } );
      settings.setExtent( extent );
      settings.setOutputSize( exported.size() );
      settings.setOutputDpi( 96.0 );
      QgsMapRendererSequentialJob job( settings );
      job.start();
      job.waitForFinished();
      const QImage canvas = job.renderedImage();
      QCOMPARE( canvas.size(), exported.size() );

      // 5×5 内部抽样（各缩进 10%）：纯色多边形内部，两管线应逐通道一致
      //（小容差吸收文本渲染/伽马微差；几何边界的 AA 点不采样）。
      int sampled = 0;
      for ( int iy = 1; iy <= 5; ++iy )
        for ( int ix = 1; ix <= 5; ++ix )
        {
          const QPoint p( exported.width() * ix / 6, exported.height() * iy / 6 );
          const QColor a = exported.pixelColor( p );
          const QColor b = canvas.pixelColor( p );
          QVERIFY2( qAbs( a.red() - b.red() ) <= 2 &&
                        qAbs( a.green() - b.green() ) <= 2 &&
                        qAbs( a.blue() - b.blue() ) <= 2,
                    qPrintable( QStringLiteral( "(%1,%2) export=%3 canvas=%4" )
                                    .arg( p.x() )
                                    .arg( p.y() )
                                    .arg( a.name(), b.name() ) ) );
          ++sampled;
        }
      QCOMPARE( sampled, 25 );
      delete layout;
    }

    // #537：消毒后同形的层位名（"K1/q" 与 "K1 q" 的 safeSegment 都是 "K1_q"）
    // 必须拿到不同交付文件——平面命名下 QFile::copy 目标已存在即失败，且失败
    // 发生在全部渲染之后。层位序号进文件名后两者都成功，metadata.json 的
    // horizons[].files 记录实际落盘名。
    void sanitizedCollisionStillGetsUniqueDeliveries()
    {
      QTemporaryDir projectDir;
      QVERIFY( projectDir.isValid() );
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );

      std::unique_ptr<QgsPrintLayout> layout( makeFigureLayout() );
      QMap<QString, QList<QgsMapLayer *>> groups;
      QList<QgsMapLayer *> keepAlive;
      for ( const QString &h : { QStringLiteral( "K1/q" ), QStringLiteral( "K1 q" ) } )
      {
        auto *layer = makeLayer( h + QStringLiteral( "_layer" ) );
        keepAlive.append( layer );
        layout->project()->addMapLayer( layer, false ); // 快照源：须在版面工程里
        groups.insert( h, { layer } );
      }

      PaleoHorizonBatchExport::Request request;
      request.layout = layout.get();
      request.projectName = QStringLiteral( "collide 工程" );
      request.horizonLayers = groups;
      request.catalog = &catalog;
      request.projectDir = projectDir.path();
      request.dpi = 96.0;
      request.format = PaleoHorizonBatchExport::Format::Png;
      request.date = QStringLiteral( "20261018" );
      request.outputDirectory = projectDir.filePath( QStringLiteral( "batch" ) );

      // ---- DIAG replication of the worker's file pipeline ----
      {
        auto *replLayout = makeFigureLayout();
        auto *replLayer = makeLayer( QStringLiteral( "K1 q_layer" ) );
        replLayout->project()->addMapLayer( replLayer, false );
        auto replStaging =
            std::make_unique<QTemporaryDir>( projectDir.path() + "/.horizon-export-XXXXXX" );
        auto replCatalog = catalog.createStagingCopy( replStaging->path() );
        const QString replTmp =
            QDir::temp().filePath( QStringLiteral( "collide_工程_K1_q_20261018_1.png" ) );
        const auto replExported = PaleoLayoutExport::exportLayout(
            replLayout, replTmp, PaleoLayoutExport::Format::Png, 96.0,
            PaleoLayoutExport::PageRange() );
        qWarning( "DIAG repl export ok=%d err=%s files=%s", int( replExported.ok ),
                  qPrintable( replExported.error ),
                  qPrintable( replExported.files.join( QLatin1Char( ',' ) ) ) );
        QString sha, managed, regErr;
        const auto assetId = registerMapPdfAsset(
            replCatalog.get(), replStaging->path(), replTmp, &sha, &managed, &regErr,
            QStringLiteral( "png" ) );
        qWarning( "DIAG repl register assetId=%s err=%s managed=%s exists=%d",
                  qPrintable( assetId ), qPrintable( regErr ), qPrintable( managed ),
                  int( QFile::exists( managed ) ) );
        auto replExternal =
            std::make_unique<QTemporaryDir>( projectDir.filePath( ".paleo-batch-XXXXXX" ) );
        QFile src( managed );
        const bool copied =
            src.copy( replExternal->filePath( QStringLiteral( "collide_工程_K1_q_20261018_1.png" ) ) );
        qWarning( "DIAG repl copy ok=%d errorString=%s dest=%s", int( copied ),
                  qPrintable( src.errorString() ),
                  qPrintable( replExternal->filePath( QStringLiteral( "collide_工程_K1_q_20261018_1.png" ) ) ) );
        qWarning( "DIAG repl staging tree: %s",
                  qPrintable( QDir( replStaging->path() )
                                  .entryList( QDir::AllEntries | QDir::NoDotAndDotDot )
                                  .join( QLatin1Char( ',' ) ) ) );
        delete replLayout;
      }
      // ---- end DIAG replication ----

      PaleoTaskService tasks;
      bool doneCalled = false;
      PaleoHorizonBatchExport::Result result;
      PaleoTask *task = PaleoHorizonBatchExport::startAsync(
          request, &tasks,
          [&doneCalled, &result]( const PaleoHorizonBatchExport::Result &r, bool ) {
            result = r;
            doneCalled = true;
          } );
      QVERIFY2( task != nullptr, "startAsync rejected a valid request" );
      QSignalSpy finished( task, &PaleoTask::finished );
      QVERIFY2( finished.wait( 60000 ), "async batch export never finished" );
      QTRY_VERIFY_WITH_TIMEOUT( doneCalled, 5000 );
      // ---- DIAG instrumentation (temporary, diagnosis only) ----
      qWarning( "DIAG task.state=%d errorText=%s", int( task->state() ),
                qPrintable( task->errorText() ) );
      qWarning( "DIAG result.total=%d succeeded=%d failed=%d directory=%s", result.total,
                result.succeeded, result.failed, qPrintable( result.directory ) );
      for ( const auto &o : result.horizons )
        qWarning( "DIAG horizon=%s ok=%d file=%s sha=%s err=%s files=%s",
                  qPrintable( o.horizon ), int( o.ok ), qPrintable( o.file ),
                  qPrintable( o.sha256 ), qPrintable( o.error ),
                  qPrintable( o.files.join( QLatin1Char( ',' ) ) ) );
      qWarning( "DIAG projectDir entries: %s",
                qPrintable( QDir( projectDir.path() )
                                .entryList( QDir::AllEntries | QDir::NoDotAndDotDot )
                                .join( QLatin1Char( ',' ) ) ) );
      {
        const QDir artifacts( projectDir.filePath( QStringLiteral( "artifacts" ) ) );
        if ( artifacts.exists() )
          for ( const auto &fi : artifacts.entryInfoList( { QStringLiteral( "*.png" ) },
                                                          QDir::Files, QDir::Name ) )
            qWarning( "DIAG artifact: %s", qPrintable( fi.absoluteFilePath() ) );
      }
      qWarning( "DIAG outputDirectory exists=%d path=%s",
                int( QFileInfo::exists( request.outputDirectory ) ),
                qPrintable( request.outputDirectory ) );
      // ---- end DIAG ----
      QCOMPARE( task->state(), PaleoTask::State::Succeeded );
      QCOMPARE( result.succeeded, 2 );
      QCOMPARE( result.failed, 0 );

      // 交付文件名唯一，且层位序号确实进了文件名（按名排序，断言确定）。
      const auto delivered = QDir( result.directory )
                                .entryList( { QStringLiteral( "*.png" ) }, QDir::Files, QDir::Name );
      QCOMPARE( delivered.size(), 2 );
      QVERIFY2( delivered.at( 0 ) != delivered.at( 1 ), "两个层位拿到了同一交付文件名" );
      QVERIFY2( delivered.at( 0 ).endsWith( QStringLiteral( "_1.png" ) ),
                qPrintable( delivered.at( 0 ) ) );
      QVERIFY2( delivered.at( 1 ).endsWith( QStringLiteral( "_2.png" ) ),
                qPrintable( delivered.at( 1 ) ) );

      // metadata.json 的 horizons[].files 记录实际落盘名；清单本身存在。
      QFile manifest( QDir( result.directory ).filePath( QStringLiteral( "metadata.json" ) ) );
      QVERIFY2( manifest.open( QIODevice::ReadOnly ), "批量图件清单缺失" );
      const auto root = QJsonDocument::fromJson( manifest.readAll() ).object();
      QVERIFY2( root.contains( QStringLiteral( "error" ) ), "清单缺 error 字段（#541）" );
      QVERIFY2( root.contains( QStringLiteral( "cancelled" ) ), "清单缺 cancelled 字段（#541）" );
      const auto horizons = root.value( QStringLiteral( "horizons" ) ).toArray();
      QCOMPARE( horizons.size(), 2 );
      QStringList recorded;
      for ( const auto &entry : horizons )
      {
        const auto horizon = entry.toObject();
        QCOMPARE( horizon.value( QStringLiteral( "ok" ) ).toBool(), true );
        for ( const auto &file : horizon.value( QStringLiteral( "files" ) ).toArray() )
          recorded << file.toString();
      }
      QCOMPARE( recorded.size(), 2 );
      for ( const auto &name : recorded )
        QVERIFY2( QFileInfo::exists( QDir( result.directory ).filePath( name ) ),
                  qPrintable( name ) );
    }

    // #541：worker 报错（一个层位没有可出图的图层）→ 外部目录不被发布、journal
    // 不登记。发布门与 geologicalexport.cpp:214 / mapexport.cpp:287-288 对齐：
    // 只有 Succeeded 且未取消的批次才 rename 发布。清单只写进（随后销毁的）
    // 暂存目录，交付目录因此既不会被发布成半成品、也不会留下清单残片。
    void workerErrorDoesNotPublishExternalDirectory()
    {
      QTemporaryDir projectDir;
      QVERIFY( projectDir.isValid() );
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );

      std::unique_ptr<QgsPrintLayout> layout( makeFigureLayout() );
      QMap<QString, QList<QgsMapLayer *>> groups;
      QList<QgsMapLayer *> keepAlive;
      auto *good = makeLayer( QStringLiteral( "good" ) );
      keepAlive.append( good );
      layout->project()->addMapLayer( good, false ); // 快照源：须在版面工程里
      groups.insert( QStringLiteral( "AA" ), { good } );
      groups.insert( QStringLiteral( "ZZ" ), {} ); // 空图层集 → 该层位入账失败

      PaleoHorizonBatchExport::Request request;
      request.layout = layout.get();
      request.projectName = QStringLiteral( "fail 工程" );
      request.horizonLayers = groups;
      request.catalog = &catalog;
      request.projectDir = projectDir.path();
      request.dpi = 96.0;
      request.format = PaleoHorizonBatchExport::Format::Png;
      request.date = QStringLiteral( "20261018" );
      request.outputDirectory = projectDir.filePath( QStringLiteral( "batch" ) );

      PaleoTaskService tasks;
      bool doneCalled = false;
      PaleoHorizonBatchExport::Result result;
      PaleoTask *task = PaleoHorizonBatchExport::startAsync(
          request, &tasks,
          [&doneCalled, &result]( const PaleoHorizonBatchExport::Result &r, bool ) {
            result = r;
            doneCalled = true;
          } );
      QVERIFY2( task != nullptr, "startAsync rejected a valid request" );
      QSignalSpy finished( task, &PaleoTask::finished );
      QVERIFY2( finished.wait( 60000 ), "async batch export never finished" );
      QTRY_VERIFY_WITH_TIMEOUT( doneCalled, 5000 );

      QCOMPARE( task->state(), PaleoTask::State::Failed );
      QCOMPARE( result.total, 2 );
      QCOMPARE( result.succeeded, 0 );
      QCOMPARE( result.failed, 2 );
      QVERIFY2( result.directory.isEmpty(), "失败的批次不得留下交付目录" );
      QVERIFY2( !QFileInfo::exists( request.outputDirectory ),
                "worker 报错仍发布了外部目录（#541）" );
      // 已导出的那个层位：结果里留痕（ok=false + 原因），不是幽灵路径。
      for ( const auto &outcome : result.horizons )
      {
        QVERIFY2( !outcome.ok, qPrintable( outcome.horizon ) );
        QVERIFY2( !outcome.error.isEmpty(), qPrintable( outcome.horizon ) );
        QVERIFY( outcome.file.isEmpty() );
      }
      QVERIFY( catalog.assets().isEmpty() ); // journal 未登记：整批未交付
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestHorizonBatchExport tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_horizonbatchexport_diag.moc"
