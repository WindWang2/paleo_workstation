#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/mapversioncontroller.h"

// wave/mapping-pipeline 阶段E — 版本状态机（PALEO_QGIS_PLAN §41.7/§1223 最小语义）：
//   保存版本 = 编辑会话 commit → 版本号递增 + provenance；commit 后 undo 栈清空，
//   undo 不跨版本边界。
//   发布 = result/ 快照（gpkg + 布局产物）→ Published；快照只读；继续编辑产生
//   下一版本，不回写已发布快照。发布门 = 该层位已能导出 PDF。
class TestVersions : public QObject
{
    Q_OBJECT

  private:
    struct Fixture
    {
        QTemporaryDir dir;
        QgisProjectService projectSvc;
        PaleoProjectStore store;
        LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
        QgisLayerService layers{ &projectSvc, &manifest };
        MapVersionStore versions{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
        MapVersionController controller{ &versions, &layers };

        QString metaPath() const { return dir.filePath( QStringLiteral( "project.sqlite" ) ); }

        bool init()
        {
            if ( !dir.isValid() )
                return false;
            if ( !projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
                return false;
            if ( !manifest.open() )
                return false;
            store.setProjectPaths( dir.filePath( QStringLiteral( "proj.qgz" ) ),
                                   dir.filePath( QStringLiteral( "proj.gpkg" ) ),
                                   metaPath() );
            QString err;
            return versions.open( &err );
        }
    };

    // Writable GPKG with one polygon, declared as facies.<h>.
    static QString makeFaciesGpkg( const QString &path )
    {
        QgsVectorLayer mem( QStringLiteral( "polygon?crs=EPSG:4326" ),
                            QStringLiteral( "facies" ), QStringLiteral( "memory" ) );
        QgsFeature f( mem.fields() );
        f.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "POLYGON((0 0,1 0,1 1,0 0))" ) ) );
        mem.dataProvider()->addFeatures( QgsFeatureList() << f );
        QgsVectorFileWriter::SaveVectorOptions opts;
        opts.driverName = QStringLiteral( "GPKG" );
        opts.layerName = QStringLiteral( "facies_polygons" );
        QString errStr;
        const auto res = QgsVectorFileWriter::writeAsVectorFormatV3(
            &mem, path, QgsProject::instance()->transformContext(), opts, &errStr );
        if ( res != QgsVectorFileWriter::WriterError::NoError )
            return QString();
        return path;
    }

    static QString makePdf( const QString &path )
    {
        QFile f( path );
        if ( !f.open( QIODevice::WriteOnly ) )
            return QString();
        f.write( "%PDF-1.4 fake d61 map\n" );
        f.close();
        return QFile::exists( path ) ? path : QString();
    }

  private slots:

    void saveVersionCommitsAndClearsUndo()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString gpkg = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies.gpkg" ) ) );
        QVERIFY( !gpkg.isEmpty() );

        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "facies.D61" );
        decl.horizon = QStringLiteral( "D61" );
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg );
        decl.group = QStringLiteral( "05_PaleoMap" );
        QString err;
        QVERIFY2( f.layers.declare( decl, &err ), qPrintable( err ) );

        auto *vl = qobject_cast<QgsVectorLayer *>( f.layers.instantiate(
            QStringLiteral( "facies.D61" ), &err ) );
        QVERIFY2( vl != nullptr, qPrintable( err ) );

        // Uncommitted edits sit on the layer's undo stack.
        QVERIFY( vl->startEditing() );
        QgsFeature nf( vl->fields() );
        nf.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "POLYGON((2 2,3 2,3 3,2 2))" ) ) );
        QVERIFY( vl->addFeature( nf ) );
        QVERIFY( vl->undoStack()->count() > 0 );

        QVariantMap provenance;
        provenance.insert( QStringLiteral( "steps" ),
                           QStringLiteral( "tops-thickness->constraint-idw->polygonize" ) );
        provenance.insert( QStringLiteral( "cell_size" ), 100.0 );

        const MapVersion v1 = f.controller.saveVersion( QStringLiteral( "D61" ), provenance, &err );
        QVERIFY2( v1.version == 1, qPrintable( err ) );
        // commit 清空 undo 栈（QGIS 原生），undo 不跨版本边界。
        QCOMPARE( vl->undoStack()->count(), 0 );
        QVERIFY( !vl->isEditable() );

        // 再次保存 → 版本号递增。
        const MapVersion v2 = f.controller.saveVersion( QStringLiteral( "D61" ), provenance, &err );
        QCOMPARE( v2.version, 2 );
        QCOMPARE( f.versions.currentVersion( QStringLiteral( "D61" ) ), 2 );
        QVERIFY( v2.provenance.contains( QStringLiteral( "constraint-idw" ) ) );
        QCOMPARE( v2.state, QStringLiteral( "Editing" ) );

        // 重开库：版本历史持久。
        MapVersionStore reopened( f.metaPath() );
        QString reopenErr;
        QVERIFY( reopened.open( &reopenErr ) );
        QCOMPARE( reopened.currentVersion( QStringLiteral( "D61" ) ), 2 );
        QCOMPARE( reopened.versions( QStringLiteral( "D61" ) ).size(), 2 );
    }

    // 清单读失败 → saveVersion/publish 都失败，不出版本、不发空快照。
    void manifestReadFailureFailsVersionOps()
    {
        Fixture f;
        QVERIFY( f.init() );
        QFile blocker( f.dir.filePath( QStringLiteral( "blocker" ) ) );
        QVERIFY( blocker.open( QIODevice::WriteOnly ) );
        blocker.close();
        LayerManifest broken( f.dir.filePath( QStringLiteral( "blocker/m.sqlite" ) ) );
        QgisLayerService brokenLayers( nullptr, &broken );
        MapVersionController ctl( &f.versions, &brokenLayers );
        QString err;
        const MapVersion v = ctl.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err );
        QVERIFY( v.version <= 0 );
        QVERIFY( !err.isEmpty() );
        err.clear();
        QVERIFY( ctl.publish( QStringLiteral( "D61" ), QString(), &err ).isEmpty() );
        QVERIFY( !err.isEmpty() );
    }

    // 残差摘要完整性判定 — 发布门与按钮 tooltip 共用这一份口径。
    void residualSummaryCompleteness()
    {
        int covered = -1, total = -1;
        QVERIFY( !MapVersionStore::residualSummaryComplete( QString(), &covered, &total ) );
        QVERIFY( !MapVersionStore::residualSummaryComplete(
            QStringLiteral( R"({"wells_total":3,"covered":2,"missing":[]})" ),
            &covered, &total ) );
        QCOMPARE( total, 3 );
        QCOMPARE( covered, 2 );
        QVERIFY( !MapVersionStore::residualSummaryComplete(
            QStringLiteral( R"({"wells_total":3,"covered":3,"missing":["A3"]})" ) ) );
        QVERIFY( !MapVersionStore::residualSummaryComplete(
            QStringLiteral( R"({"wells_total":0,"covered":0,"missing":[]})" ) ) ); // 无井不算覆盖
        QVERIFY( MapVersionStore::residualSummaryComplete(
            QStringLiteral( R"({"wells_total":3,"covered":3,"missing":[]})" ) ) );
    }

    // D10（PROJECT_AREA_PLAN L1077 批准「≥15/20 numeric」）：数值残差行数下限。
    // 参数换算：真工区 20 井 → 绝对 15；小工区按同一比例 3/4 向下取整、至少
    // 1 口（既有 3 井 2 数值的通过用例不受影响）。数值行 = rows[].kind=="residual"。
    void numericResidualFloorArithmetic()
    {
        QCOMPARE( MapVersionStore::requiredNumericResiduals( 20 ), 15 ); // 15/20 批准口径
        QCOMPARE( MapVersionStore::requiredNumericResiduals( 3 ), 2 );  // 3/4 向下取整
        QCOMPARE( MapVersionStore::requiredNumericResiduals( 4 ), 3 );
        QCOMPARE( MapVersionStore::requiredNumericResiduals( 1 ), 1 );  // 至少 1 口
        QCOMPARE( MapVersionStore::requiredNumericResiduals( 0 ), 0 );  // 无井由完备性检查拒
        QCOMPARE( MapVersionStore::numericResidualCount(
                      QStringLiteral( R"({"wells_total":20,"covered":20,"missing":[],"rows":)"
                                      R"([{"kind":"residual","residual_ms":1.0},)"
                                      R"({"kind":"reason","reason":"x"},)"
                                      R"({"kind":"residual","residual_ms":-2.0}]})" ) ),
                  2 );
        QCOMPARE( MapVersionStore::numericResidualCount( QString() ), 0 );
        QCOMPARE( MapVersionStore::numericResidualCount(
                      QStringLiteral( R"({"wells_total":2,"covered":2,"missing":[]})" ) ),
                  0 );
    }

    // D10 发布门：20 井 14 数值 → 拒（写明差几口）；15 数值 → 放行。
    void publishGateNumericResidualFloor()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString gpkg = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies.gpkg" ) ) );
        QVERIFY( !gpkg.isEmpty() );
        const QString pdf = makePdf( f.dir.filePath( QStringLiteral( "D61_map.pdf" ) ) );
        QVERIFY( !pdf.isEmpty() );

        // 20 口井的完整摘要：前 numeric 口数值，其余 reason（完备但数值不足）。
        const auto summaryWith = []( int numeric ) {
            QString rows;
            for ( int i = 0; i < 20; ++i )
            {
                if ( i > 0 )
                    rows += QLatin1Char( ',' );
                if ( i < numeric )
                    rows += QStringLiteral( R"({"well_id":"w%1","name":"W%1","kind":"residual","residual_ms":%2})" )
                                .arg( i )
                                .arg( i % 2 ? 3.5 : -1.25 );
                else
                    rows += QStringLiteral( R"({"well_id":"w%1","name":"W%1","kind":"reason","reason":"无时深表"})" )
                                .arg( i );
            }
            return QStringLiteral( R"({"wells_total":20,"covered":20,"missing":[],"rows":[%1]})" ).arg( rows );
        };

        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "facies.D61" );
        decl.horizon = QStringLiteral( "D61" );
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg );
        decl.group = QStringLiteral( "05_PaleoMap" );
        QString err;
        QVERIFY2( f.layers.declare( decl, &err ), qPrintable( err ) );
        QVERIFY2( f.versions.recordLayoutProduct( QStringLiteral( "D61" ), pdf,
                                                  QStringLiteral( "ast-pdf-2" ),
                                                  QStringLiteral( "cafebabe" ), &err ),
                  qPrintable( err ) );
        QVERIFY2( f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err ).version == 1,
                  qPrintable( err ) );

        // 14/20 数值 → 拒；原因可读：写明数值残差、实况与门槛。
        err.clear();
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), summaryWith( 14 ), &err ).isEmpty() );
        QVERIFY2( !err.isEmpty(), "insufficient numeric residuals must refuse" );
        QVERIFY( err.contains( QStringLiteral( "数值残差" ) ) );
        QVERIFY( err.contains( QStringLiteral( "14" ) ) );
        QVERIFY( err.contains( QStringLiteral( "15" ) ) );
        QVERIFY( !f.versions.isPublished( QStringLiteral( "D61" ) ) );

        // 15/20 数值 → 放行（快照落盘）。
        err.clear();
        const QString snap =
            f.controller.publish( QStringLiteral( "D61" ), summaryWith( 15 ), &err );
        QVERIFY2( !snap.isEmpty(), qPrintable( err ) );
        QVERIFY( f.versions.isPublished( QStringLiteral( "D61" ) ) );
    }

    void publishGateSnapshotAndImmutability()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString gpkg = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies.gpkg" ) ) );
        QVERIFY( !gpkg.isEmpty() );
        const QString pdf = makePdf( f.dir.filePath( QStringLiteral( "D61_map.pdf" ) ) );
        QVERIFY( !pdf.isEmpty() );

        // 完整残差摘要 = 每口井都有残差或原因（3 井：2 残差 + 1 原因）。
        const QString completeSummary = QStringLiteral(
            R"({"wells_total":3,"covered":3,"missing":[],"rows":)"
            R"([{"well_id":"well-1","name":"A1","kind":"residual","residual_ms":12.3},)"
            R"({"well_id":"well-2","name":"A2","kind":"reason","reason":"无 D61 分层"},)"
            R"({"well_id":"well-3","name":"A3","kind":"residual","residual_ms":-4.2}]})" );
        const QString incompleteSummary = QStringLiteral(
            R"({"wells_total":3,"covered":2,"missing":["A3"],"rows":[]})" );

        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "facies.D61" );
        decl.horizon = QStringLiteral( "D61" );
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg );
        decl.group = QStringLiteral( "05_PaleoMap" );
        QString err;
        QVERIFY2( f.layers.declare( decl, &err ), qPrintable( err ) );

        // 门检 1：无版本 → 拒。
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), completeSummary, &err ).isEmpty() );
        QVERIFY( !err.isEmpty() );
        QVERIFY2( f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err ).version == 1,
                  qPrintable( err ) );

        // 门检 2：版本行上没有 PDF 资产 → 拒（先查 PDF 再查残差）。
        err.clear();
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), completeSummary, &err ).isEmpty() );
        QVERIFY( err.contains( QStringLiteral( "PDF" ) ) );

        // 登记产物后还须保存版本 —— 行上的 PDF 引用靠 saveVersion 继承，
        // 光登记不改已存在的版本行。
        QVERIFY2( f.versions.recordLayoutProduct( QStringLiteral( "D61" ), pdf,
                                                  QStringLiteral( "ast-pdf-1" ),
                                                  QStringLiteral( "deadbeefcafe" ), &err ),
                  qPrintable( err ) );
        QVERIFY( f.versions.hasLayoutProduct( QStringLiteral( "D61" ) ) );
        QCOMPARE( f.versions.latestLayoutProduct( QStringLiteral( "D61" ) ), pdf );
        err.clear();
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), completeSummary, &err ).isEmpty() );
        QVERIFY( err.contains( QStringLiteral( "PDF" ) ) );

        // 门检 3：残差覆盖不全 → 拒（写明缺几口井 + 下一步）。
        QVERIFY2( f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err ).version == 2,
                  qPrintable( err ) );
        {
            const MapVersion v2 = f.versions.latest( QStringLiteral( "D61" ) );
            QCOMPARE( v2.pdfAssetId, QStringLiteral( "ast-pdf-1" ) ); // save 继承了产物引用
            QCOMPARE( v2.pdfSha256, QStringLiteral( "deadbeefcafe" ) );
        }
        err.clear();
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), incompleteSummary, &err ).isEmpty() );
        QVERIFY( err.contains( QStringLiteral( "先在验证页运行验证" ) ) );

        // 完整摘要 → 发布成功：result/ 快照存在且只读；摘要冻结进版本行。
        err.clear();
        const QString snap1 =
            f.controller.publish( QStringLiteral( "D61" ), completeSummary, &err );
        QVERIFY2( !snap1.isEmpty(), qPrintable( err ) );
        QVERIFY( QFileInfo( snap1 ).isDir() );
        QVERIFY( snap1.contains( QStringLiteral( "result/D61/v2" ) ) );
        {
            const MapVersion pub = f.versions.latest( QStringLiteral( "D61" ) );
            QCOMPARE( pub.state, QStringLiteral( "Published" ) );
            QCOMPARE( pub.residualSummary, completeSummary );
        }

        // 快照内容：facies gpkg 拷贝 + 布局 PDF，全部只读。
        const QStringList entries = QDir( snap1 ).entryList( QDir::Files );
        QVERIFY( entries.size() >= 2 );
        for ( const QString &name : entries )
        {
            const QFileInfo info( QDir( snap1 ).filePath( name ) );
            QVERIFY2( info.size() > 0, qPrintable( name ) );
            QVERIFY2( !info.isWritable(), qPrintable( QStringLiteral( "snapshot file writable: %1" ).arg( name ) ) );
        }
        QVERIFY( QDir( snap1 ).entryList( QDir::Files ).join( QLatin1Char( ',' ) ).contains( QStringLiteral( ".pdf" ) ) );

        QVERIFY( f.versions.isPublished( QStringLiteral( "D61" ) ) );
        QCOMPARE( f.versions.publishedPath( QStringLiteral( "D61" ) ), snap1 );

        // 已发布再发布 → 拒绝（先保存新版本）。
        err.clear();
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), completeSummary, &err ).isEmpty() );
        QVERIFY( err.contains( QStringLiteral( "已发布" ) ) );

        // 继续编辑产生下一版本；发布 v3，v2 快照原样保留。
        const QByteArray v1PdfBytes = [] ( const QString &dir ) {
            QFile pf( QDir( dir ).filePath( QDir( dir ).entryList( { QStringLiteral( "*.pdf" ) }, QDir::Files ).first() ) );
            pf.open( QIODevice::ReadOnly );
            const QByteArray b = pf.readAll();
            pf.close();
            return b;
        }( snap1 );

        QVERIFY2( f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err ).version == 3,
                  qPrintable( err ) );
        QVERIFY( !f.versions.isPublished( QStringLiteral( "D61" ) ) ); // 新版本回到 Editing
        // 版本 3 也继承了 PDF 产物引用（asset id + sha 抄行进新版本行）。
        QCOMPARE( f.versions.latest( QStringLiteral( "D61" ) ).pdfAssetId,
                  QStringLiteral( "ast-pdf-1" ) );
        err.clear();
        const QString snap2 =
            f.controller.publish( QStringLiteral( "D61" ), completeSummary, &err );
        QVERIFY2( !snap2.isEmpty(), qPrintable( err ) );
        QVERIFY( snap2.contains( QStringLiteral( "result/D61/v3" ) ) );

        // v1 快照未被回写：PDF 内容不变且仍只读。
        QFile pf1( QDir( snap1 ).filePath( QDir( snap1 ).entryList( { QStringLiteral( "*.pdf" ) }, QDir::Files ).first() ) );
        QVERIFY( pf1.open( QIODevice::ReadOnly ) );
        QCOMPARE( pf1.readAll(), v1PdfBytes );
        pf1.close();
        QVERIFY( !QFileInfo( pf1.fileName() ).isWritable() );
    }

    // 旧 schema 库（无 pdf_asset_id/pdf_sha256/residual_summary 列）：
    // open() 逐列补建；旧行读成未发布（state 归一 Editing），且读/升级本身
    // 不回写旧行（§177/§260 前向兼容纪律）。
    void oldSchemaRowsLoadUnpublished()
    {
        QTemporaryDir dir;
        QVERIFY( dir.isValid() );
        const QString dbPath = dir.filePath( QStringLiteral( "old.sqlite" ) );
        const QString conn = QStringLiteral( "old_schema_seed" );
        {
            QSqlDatabase db = QSqlDatabase::addDatabase( QStringLiteral( "QSQLITE" ), conn );
            db.setDatabaseName( dbPath );
            QVERIFY( db.open() );
            QSqlQuery q( db );
            QVERIFY( q.exec( QStringLiteral(
                "CREATE TABLE map_versions(id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "horizon TEXT NOT NULL,version INTEGER NOT NULL,"
                "provenance TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'Editing',"
                "published_path TEXT,created_utc TEXT NOT NULL)" ) ) );
            QVERIFY( q.exec( QStringLiteral(
                "INSERT INTO map_versions(horizon,version,provenance,state,published_path,created_utc)"
                " VALUES('D61',1,'{}','Published','result/D61/v1','2020-01-01T00:00:00Z')" ) ) );
        }
        QSqlDatabase::removeDatabase( conn );

        MapVersionStore store( dbPath );
        QString err;
        QVERIFY2( store.open( &err ), qPrintable( err ) );
        const MapVersion v = store.latest( QStringLiteral( "D61" ) );
        QCOMPARE( v.version, 1 );
        QCOMPARE( v.state, QStringLiteral( "Editing" ) ); // 无 PDF 资产 → 读成未发布
        QVERIFY( v.pdfAssetId.isEmpty() );
        QVERIFY( v.pdfSha256.isEmpty() );
        QVERIFY( v.residualSummary.isEmpty() );
        QCOMPARE( v.publishedPath, QStringLiteral( "result/D61/v1" ) ); // 行未被回写
        QVERIFY( !store.isPublished( QStringLiteral( "D61" ) ) );

        // 升级/读取不得回写旧行：raw state 仍是 Published。
        {
            QSqlDatabase db = QSqlDatabase::addDatabase( QStringLiteral( "QSQLITE" ), conn );
            db.setDatabaseName( dbPath );
            QVERIFY( db.open() );
            QSqlQuery q( db );
            QVERIFY( q.exec( QStringLiteral(
                "SELECT state,pdf_asset_id FROM map_versions WHERE horizon='D61'" ) ) );
            QVERIFY( q.next() );
            QCOMPARE( q.value( 0 ).toString(), QStringLiteral( "Published" ) );
            QVERIFY( q.value( 1 ).isNull() ); // 新列补上但旧行保持 NULL
        }
        QSqlDatabase::removeDatabase( conn );
    }
};

int main( int argc, char *argv[] )
{
    if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
        qputenv( "QT_QPA_PLATFORM", "offscreen" );
    QgsApplication app( argc, argv, false );
    app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
    app.initQgis();
    TestVersions tc;
    const int rc = QTest::qExec( &tc, argc, argv );
    QgsApplication::exitQgis();
    return rc;
}

#include "tst_versions.moc"
