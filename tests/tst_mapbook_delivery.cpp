#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <QPdfDocument>

#include "../src/catalog/datacatalog.h"
#include "../src/qgis/mapbooklayout.h"
#include "../src/qgis/mapbookrender.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/mapbook.h"
#include "../src/workflow/mapbookqueue.h"
#include "../src/workflow/mapexport.h"

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsfeature.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgslayoutpagecollection.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

// P5 交付闭环测试：逐页真实状态（ok/failed/cancelled 同源守恒）、冻结输入
// 说明（manifest inputs 段）、目录分页与全部索引文件回传校验、装配失败与
// 页失败的区分。全部确定性断言（manifest JSON/文件存在性/文件头），不靠
// 截图；既有快照/取消/蒙太奇覆盖仍在 tst_mapbook，此处只测本次新行为。
class TestMapBookDelivery : public QObject
{
    Q_OBJECT

  private:
    QVector<PaleoMapBook::Tile> grid( int cols, int rows ) const
    {
      PaleoMapBook::GridRequest request;
      request.area = PaleoMapBook::Area{ 0.0, 0.0, 3000.0, 3000.0 };
      request.cols = cols;
      request.rows = rows;
      QString error;
      const QVector<PaleoMapBook::Tile> tiles = PaleoMapBook::buildGrid( request, &error );
      if ( tiles.isEmpty() )
        qWarning( "grid: %s", qPrintable( error ) ); // 非 void 夹具：失败交给调用点断言
      return tiles;
    }

    QJsonObject readManifest( const QString &path )
    {
      QFile file( path );
      if ( !file.open( QIODevice::ReadOnly ) )
      {
        qWarning( "manifest unreadable: %s", qPrintable( path ) ); // 非 void 夹具
        return {};
      }
      return QJsonDocument::fromJson( file.readAll() ).object();
    }

    // 页状态计数与 Result 计数守恒（manifest/Result/UI 同源的数学面）。
    void assertPageAccounting( const QJsonObject &manifest,
                               const PaleoMapBookQueue::Result &result )
    {
      const QJsonArray pages = manifest.value( QStringLiteral( "pages" ) ).toArray();
      QCOMPARE( pages.size(), result.total );
      QCOMPARE( pages.size(), result.pages.size() );
      int ok = 0, failed = 0, cancelled = 0;
      for ( const QJsonValue &value : pages )
      {
        const QString status = value.toObject().value( QStringLiteral( "status" ) ).toString();
        if ( status == QLatin1String( "ok" ) )
          ++ok;
        else if ( status == QLatin1String( "failed" ) )
          ++failed;
        else if ( status == QLatin1String( "cancelled" ) )
          ++cancelled;
        else
          QVERIFY2( false, qPrintable( QStringLiteral( "未知页状态：%1" ).arg( status ) ) );
      }
      QCOMPARE( ok, result.succeeded );
      QCOMPARE( failed, result.failed );
      QCOMPARE( cancelled, result.cancelled );
      QCOMPARE( ok + failed + cancelled, result.total );
    }

  private slots:
    // P5-W4 验收：9 版 3 成后预演停 → 3 成功、6 取消，manifest/Result 同源，
    // 取消页带人读原因（不再被写成 failed）。
    void manifestPreservesCancelledPages()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_preview_status" );
      request.outputDir = dir.path();
      request.tiles = grid( 3, 3 );
      request.dpi = 96.0;
      request.previewLimit = 3;

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.succeeded, 3 );
      QCOMPARE( result.cancelled, 6 );
      QCOMPARE( result.failed, 0 );

      const QJsonObject manifest = readManifest( result.manifest );
      assertPageAccounting( manifest, result );

      const QJsonArray pages = manifest.value( QStringLiteral( "pages" ) ).toArray();
      for ( int i = 0; i < pages.size(); ++i )
      {
        const QJsonObject page = pages.at( i ).toObject();
        const QString status = page.value( QStringLiteral( "status" ) ).toString();
        if ( i < 3 )
        {
          QCOMPARE( status, QStringLiteral( "ok" ) );
          QVERIFY( !page.value( QStringLiteral( "file" ) ).toString().isEmpty() );
          QVERIFY( page.value( QStringLiteral( "bytes" ) ).toDouble() > 0.0 );
          QVERIFY( !page.value( QStringLiteral( "sha256" ) ).toString().isEmpty() );
          // 页文件真实存在且 sha 与落盘文件一致（账实相符）。
          const QString absolute = QDir( PaleoMapBookQueue::bookRoot( request ) )
                                     .filePath( page.value( QStringLiteral( "file" ) ).toString() );
          QVERIFY2( QFile::exists( absolute ), qPrintable( absolute ) );
          QString shaError;
          QCOMPARE( DataCatalog::sha256FileHex( absolute, &shaError ),
                    page.value( QStringLiteral( "sha256" ) ).toString() );
        }
        else
        {
          QCOMPARE( status, QStringLiteral( "cancelled" ) );
          QVERIFY( page.value( QStringLiteral( "file" ) ).toString().isEmpty() );
          QVERIFY2( !page.value( QStringLiteral( "reason" ) ).toString().isEmpty(),
                    "取消页必须留人读原因" );
          QVERIFY2( page.value( QStringLiteral( "reason" ) ).toString()
                        .contains( QStringLiteral( "预演" ) ),
                    "原因应能区分预演限额与用户取消" );
        }
      }
    }

    // P5-W4 验收：开跑前已取消 → 全册 cancelled；一版都不谎报成功。
    void cancelledBeforeStartIsFullyRecorded()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_cancel_status" );
      request.outputDir = dir.path();
      request.tiles = grid( 3, 3 );
      request.dpi = 96.0;
      PaleoTask *task = tasks.start( QStringLiteral( "mapbook-cancel-status" ),
                                     []( PaleoTask * ) { return QString(); } );
      task->requestCancel();
      const PaleoMapBookQueue::Result result = exporter.run( task, request );
      QCOMPARE( result.succeeded, 0 );
      QCOMPARE( result.cancelled, 9 );
      QVERIFY( result.files.isEmpty() );
      const QJsonObject manifest = readManifest( result.manifest );
      assertPageAccounting( manifest, result );
    }

    // P5-W4 验收：某页失败（退化范围）→ 页失败与装配成功并存，已成功页不丢；
    // 失败页 reason 可查。
    void failedPageKeepsOthersAndCarriesReason()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      QVector<PaleoMapBook::Tile> tiles = grid( 3, 3 );
      tiles[4].extent = PaleoMapBook::Area{ 500.0, 500.0, 500.0, 500.0, QString() };

      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_pagefail" );
      request.outputDir = dir.path();
      request.tiles = tiles;
      request.dpi = 96.0;

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.succeeded, 8 );
      QCOMPARE( result.failed, 1 );
      QCOMPARE( result.pages.at( 4 ).status,
                PaleoMapBookQueue::PageStatus::Failed );
      QVERIFY( !result.pages.at( 4 ).reason.isEmpty() );
      QCOMPARE( result.pages.at( 4 ).tile, tiles.at( 4 ).name );

      const QJsonObject manifest = readManifest( result.manifest );
      assertPageAccounting( manifest, result );
      const QJsonObject failedPage =
        manifest.value( QStringLiteral( "pages" ) ).toArray().at( 4 ).toObject();
      QCOMPARE( failedPage.value( QStringLiteral( "status" ) ).toString(),
                QStringLiteral( "failed" ) );
      QVERIFY( !failedPage.value( QStringLiteral( "reason" ) ).toString().isEmpty() );
    }

    // P5-W3 验收：manifest inputs 段冻结提交时刻输入——受管图层带
    // asset/version/sha，未受管层显式 managed=false + source；工程 CRS 与
    // 运行库版本入册。
    void manifestFreezesInputsAtSubmitTime()
    {
      QTemporaryDir dir;
      DataCatalog catalog;
      QVERIFY2( catalog.open( dir.path() ), qPrintable( catalog.openError() ) );

      // 造一个受管资产 + 版本（既有登记管线），让图层身份可解析。
      const QString inputPath = dir.filePath( QStringLiteral( "input.gpkg" ) );
      {
        QgsVectorLayer source( QStringLiteral( "Point" ), QStringLiteral( "src" ),
                               QStringLiteral( "memory" ) );
        source.dataProvider()->addAttributes(
          { QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ),
            QgsField( QStringLiteral( "well_name" ), QMetaType::Type::QString ),
            QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) } );
        source.updateFields();
        QgsFeature feature( source.fields() );
        feature.setAttribute( QStringLiteral( "well_id" ), QStringLiteral( "w1" ) );
        feature.setAttribute( QStringLiteral( "well_name" ), QStringLiteral( "W1" ) );
        feature.setAttribute( QStringLiteral( "factor_value" ), 0.23 );
        feature.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "POINT(1 1)" ) ) );
        QVERIFY( source.dataProvider()->addFeature( feature ) );
        QString err;
        QgsVectorFileWriter::SaveVectorOptions options;
        options.layerName = QStringLiteral( "samples" ); // GPKG 缺省取文件名——显式钉住
        QgsVectorFileWriter::writeAsVectorFormatV3(
          &source, inputPath, QgsCoordinateTransformContext(), options, &err );
        QVERIFY2( QFile::exists( inputPath ), qPrintable( err ) );
      }
      QString sha, managedCopy, registerError;
      const QString assetId = registerMapPdfAsset( &catalog, dir.path(), inputPath,
                                                    &sha, &managedCopy, &registerError,
                                                    QStringLiteral( "gpkg" ),
                                                    QStringLiteral( "T70" ) );
      QVERIFY2( !assetId.isEmpty(), qPrintable( registerError ) );
      const CatalogVersion version = catalog.currentVersion( assetId );
      QVERIFY( !version.id.isEmpty() );

      QgsProject project;
      QgsVectorLayer managedLayer( managedCopy + QStringLiteral( "|layername=samples" ),
                                   QStringLiteral( "井点因子·测试" ), QStringLiteral( "ogr" ) );
      QVERIFY2( managedLayer.isValid(),
                qPrintable( QStringLiteral( "%1 → %2" )
                              .arg( managedLayer.source(),
                                    managedLayer.dataProvider()->error().message() ) ) );
      managedLayer.setCustomProperty( QStringLiteral( "paleoAssetId" ), assetId );
      managedLayer.setCustomProperty( QStringLiteral( "paleoVersionId" ), version.id );
      QgsVectorLayer plainLayer( QStringLiteral( "Point?field=v:double" ),
                                 QStringLiteral( "临时点" ), QStringLiteral( "memory" ) );
      project.setCrs( QgsCoordinateReferenceSystem::fromEpsgId( 32650 ) );
      // 图层不入 project（栈对象所有权）；Request.layers 直接引用即可，
      // 快照按图层 id 序冻结输入说明。

      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );
      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_inputs" );
      request.outputDir = dir.path();
      request.tiles = grid( 2, 1 );
      request.dpi = 96.0;
      request.layers = { &managedLayer, &plainLayer };
      request.horizon = QStringLiteral( "T70" );
      request.catalog = &catalog;      // 受管身份 + sha 解析需要 catalog 事实
      request.projectDir = dir.path();

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.succeeded, 2 );
      const QJsonObject manifest = readManifest( result.manifest );

      const QJsonObject inputs = manifest.value( QStringLiteral( "inputs" ) ).toObject();
      QCOMPARE( inputs.value( QStringLiteral( "projectCrs" ) ).toString(),
                QStringLiteral( "EPSG:32650" ) );
      QCOMPARE( inputs.value( QStringLiteral( "horizon" ) ).toString(),
                QStringLiteral( "T70" ) );
      QVERIFY( !inputs.value( QStringLiteral( "runtime" ) )
                    .toObject().value( QStringLiteral( "qgis" ) ).toString().isEmpty() );
      const QJsonObject layout = inputs.value( QStringLiteral( "layout" ) ).toObject();
      QCOMPARE( layout.value( QStringLiteral( "orientation" ) ).toString(),
                QStringLiteral( "landscape" ) );
      QCOMPARE( layout.value( QStringLiteral( "dpi" ) ).toDouble(), 96.0 );

      const QJsonArray layers = inputs.value( QStringLiteral( "layers" ) ).toArray();
      QCOMPARE( layers.size(), 2 );
      const QJsonObject managed = layers.at( 0 ).toObject();
      QCOMPARE( managed.value( QStringLiteral( "assetId" ) ).toString(), assetId );
      QCOMPARE( managed.value( QStringLiteral( "versionId" ) ).toString(), version.id );
      QCOMPARE( managed.value( QStringLiteral( "sha256" ) ).toString(),
                version.sha256.toLower() );
      QVERIFY( managed.value( QStringLiteral( "managed" ) ).toBool() );
      const QJsonObject plain = layers.at( 1 ).toObject();
      QVERIFY( !plain.value( QStringLiteral( "managed" ) ).toBool() );
      QVERIFY( !plain.value( QStringLiteral( "source" ) ).toString().isEmpty() );
    }

    // P5-W5 验收：125 条目录全量分页（≥3 页）、次序保留、全部索引文件回传
    // 且逐文件有效；manifest index 段列全文件。
    void indexPaginationCarriesEveryEntry()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      // 25×5 = 125 版（全成功）→ 目录 125 条。
      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_bigindex" );
      request.outputDir = dir.path();
      request.tiles = grid( 25, 5 );
      request.dpi = 96.0;
      request.withIndexPage = true;

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.succeeded, 125 );

      // 125 条短条目 ≤ 每页行容量 56 → 至少 3 页；精确分页 oracle 在
      // longChineseEntriesPaginateAndPdfPageCountMatches（条目已知时页数可精算）。
      QVERIFY2( result.indexFiles.size() >= 3,
                qPrintable( QString::number( result.indexFiles.size() ) ) );
      const int expectedPages = result.indexFiles.size();
      QVERIFY( expectedPages > 1 );

      // 每个索引文件真实存在且是合法 PNG（首页存在 ≠ 全部成功）。
      for ( const QString &file : result.indexFiles )
      {
        QFileInfo info( file );
        QVERIFY2( info.exists() && info.size() > 0, qPrintable( file ) );
        QString validateError;
        QVERIFY2( PaleoMapBookLayout::validateExportedFile( file,
                                                            PaleoLayoutExport::Format::Png,
                                                            -1.0, -1.0, &validateError ),
                  qPrintable( validateError ) );
      }

      const QJsonObject manifest = readManifest( result.manifest );
      const QJsonObject index = manifest.value( QStringLiteral( "index" ) ).toObject();
      QCOMPARE( index.value( QStringLiteral( "status" ) ).toString(),
                QStringLiteral( "ok" ) );
      QCOMPARE( index.value( QStringLiteral( "files" ) ).toArray().size(), expectedPages );

      // 次序保留：manifest 页序 = 格序（Tile.index 0-based 契约），单调不乱序。
      const QJsonArray pages = manifest.value( QStringLiteral( "pages" ) ).toArray();
      QCOMPARE( pages.size(), 125 );
      for ( int i = 0; i < pages.size(); ++i )
        QCOMPARE( pages.at( i ).toObject().value( QStringLiteral( "index" ) ).toInt(), i );

      // 交付摘要显式呈现目录文件数与清单状态（部分可用可见）。
      QVERIFY2( result.deliverySummary().contains( QString::number( expectedPages ) ),
                qPrintable( result.deliverySummary() ) );
    }

    // P5-W5 验收：第二个索引 PNG 损坏 → 回读校验必须能发现（逐文件校验
    // 能力证明，不因首页存在报全部成功）。
    void damagedSiblingIndexFileIsDetectable()
    {
      // 先出一次多页索引（低 dpi 小图），然后人为截断 _2 兄弟文件。
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );
      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_sibling" );
      request.outputDir = dir.path();
      request.tiles = grid( 25, 5 );
      request.dpi = 72.0;
      request.withIndexPage = true;
      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QVERIFY( result.indexFiles.size() >= 2 );

      const QString sibling = result.indexFiles.at( 1 );
      {
        QFile file( sibling );
        QVERIFY( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        file.write( QByteArray( "\x89PNG\x0D\x0A\x1A\x0A", 8 ) ); // 只有文件头
        file.close();
      }
      QString validateError;
      QVERIFY2( !PaleoMapBookLayout::validateExportedFile(
                  sibling, PaleoLayoutExport::Format::Png, 100.0, 100.0, &validateError ),
                "截断的兄弟文件必须被回读校验拒绝" );

      // 缺失同样能发现（打开失败即失败，不冒充成功）。
      QVERIFY( QFile::remove( sibling ) );
      QVERIFY( !PaleoMapBookLayout::validateExportedFile(
                 sibling, PaleoLayoutExport::Format::Png, -1.0, -1.0, &validateError ) );
    }

    // P5-W5：长中文条目占行分页（不缩字号塞页）+ PDF 多页目录页数可核验。
    void longChineseEntriesPaginateAndPdfPageCountMatches()
    {
      QTemporaryDir dir;
      QgsProject project;

      QStringList entries;
      for ( int i = 0; i < 61; ++i )
        entries << QStringLiteral( "第%1格 · 井点因子砂地比长条目名称超长版本占行测试%1" ).arg( i );
      const int expected =
        PaleoMapBookLayout::indexPageCount( entries, false );
      QVERIFY( expected >= 2 );

      // 版面页数与纯计算一致（分页口径单一事实源）。
      PaleoMapBookLayout::IndexSpec spec;
      spec.title = QStringLiteral( "长条目目录" );
      spec.entries = entries;
      QString buildError;
      QgsPrintLayout *layout =
        PaleoMapBookLayout::buildIndexLayout( &project, spec, &buildError );
      QVERIFY2( layout != nullptr, qPrintable( buildError ) );
      QCOMPARE( layout->pageCollection()->pageCount(), expected );

      // PDF：单文件多页——页数 = 版面页数（QPdfDocument 权威核验）。
      const auto outcome =
        PaleoMapBookRender::renderIndex( &project,
                                         PaleoMapBookRender::IndexJob{
                                           spec.title, entries, false },
                                         dir.filePath( QStringLiteral( "index.pdf" ) ),
                                         PaleoLayoutExport::Format::Pdf, 96.0 );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QCOMPARE( outcome.files.size(), 1 );
      QPdfDocument pdf;
      QCOMPARE( pdf.load( outcome.path ), QPdfDocument::Error::None );
      QCOMPARE( pdf.pageCount(), expected );
    }

    // P5-W4 验收：索引失败（路径被目录占住）→ manifest index.status=failed，
    // 已成功页不丢、不列为可打开。
    void indexFailureIsExplicitInManifest()
    {
      QTemporaryDir dir;
      QgsProject project;
      PaleoTaskService tasks;
      PaleoMapBookQueue::Exporter exporter( &tasks, &project );

      PaleoMapBookQueue::Request request;
      request.book = QStringLiteral( "book_indexfail2" );
      request.outputDir = dir.path();
      request.tiles = grid( 2, 1 );
      request.dpi = 96.0;
      request.withIndexPage = true;
      QVERIFY( QDir().mkpath( dir.filePath(
          QStringLiteral( "book_indexfail2/index/book_indexfail2_index.png" ) ) ) );

      const PaleoMapBookQueue::Result result = exporter.run( nullptr, request );
      QCOMPARE( result.succeeded, 2 );
      QVERIFY( !result.complete() );
      QVERIFY( result.indexFiles.isEmpty() );

      const QJsonObject manifest = readManifest( result.manifest );
      const QJsonObject index = manifest.value( QStringLiteral( "index" ) ).toObject();
      QCOMPARE( index.value( QStringLiteral( "status" ) ).toString(),
                QStringLiteral( "failed" ) );
      QVERIFY( !index.value( QStringLiteral( "reason" ) ).toString().isEmpty() );
      QCOMPARE( index.value( QStringLiteral( "files" ) ).toArray().size(), 0 );
      // 逐页账本不受装配失败影响：页仍 ok 且文件在册。
      assertPageAccounting( manifest, result );
      QVERIFY2( result.deliverySummary().contains( QStringLiteral( "目录失败" ) ),
                qPrintable( result.deliverySummary() ) );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  // QgisRuntime 自建 GUI 档 QgsApplication（即 QApplication 级实例，面板
  // 控件可直接用）——调用方不得先构造 QCoreApplication。
  if ( !QgisRuntime::initialize() )
    qFatal( "QgisRuntime::initialize failed" );
  TestMapBookDelivery tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_mapbook_delivery.moc"
