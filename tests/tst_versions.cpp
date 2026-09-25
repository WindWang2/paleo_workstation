#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    void publishGateSnapshotAndImmutability()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString gpkg = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies.gpkg" ) ) );
        QVERIFY( !gpkg.isEmpty() );
        const QString pdf = makePdf( f.dir.filePath( QStringLiteral( "D61_map.pdf" ) ) );
        QVERIFY( !pdf.isEmpty() );

        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "facies.D61" );
        decl.horizon = QStringLiteral( "D61" );
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg );
        decl.group = QStringLiteral( "05_PaleoMap" );
        QString err;
        QVERIFY2( f.layers.declare( decl, &err ), qPrintable( err ) );

        // 发布门前置：无版本 / 无布局产物 → 干净失败。
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), &err ).isEmpty() );
        QVERIFY( !err.isEmpty() );
        QVERIFY2( f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err ).version == 1,
                  qPrintable( err ) );
        err.clear();
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), &err ).isEmpty() );
        QVERIFY( err.contains( QStringLiteral( "布局产物" ) ) );

        // 记录布局产物后发布成功：result/ 快照存在且只读。
        QVERIFY2( f.versions.recordLayoutProduct( QStringLiteral( "D61" ), pdf, &err ),
                  qPrintable( err ) );
        QVERIFY( f.versions.hasLayoutProduct( QStringLiteral( "D61" ) ) );
        err.clear();
        const QString snap1 = f.controller.publish( QStringLiteral( "D61" ), &err );
        QVERIFY2( !snap1.isEmpty(), qPrintable( err ) );
        QVERIFY( QFileInfo( snap1 ).isDir() );
        QVERIFY( snap1.contains( QStringLiteral( "result/D61/v1" ) ) );

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
        QVERIFY( f.controller.publish( QStringLiteral( "D61" ), &err ).isEmpty() );
        QVERIFY( err.contains( QStringLiteral( "已发布" ) ) );

        // 继续编辑产生下一版本；发布 v2，v1 快照原样保留。
        const QByteArray v1PdfBytes = [] ( const QString &dir ) {
            QFile pf( QDir( dir ).filePath( QDir( dir ).entryList( { QStringLiteral( "*.pdf" ) }, QDir::Files ).first() ) );
            pf.open( QIODevice::ReadOnly );
            const QByteArray b = pf.readAll();
            pf.close();
            return b;
        }( snap1 );

        QVERIFY2( f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err ).version == 2,
                  qPrintable( err ) );
        QVERIFY( !f.versions.isPublished( QStringLiteral( "D61" ) ) ); // 新版本回到 Editing
        err.clear();
        const QString snap2 = f.controller.publish( QStringLiteral( "D61" ), &err );
        QVERIFY2( !snap2.isEmpty(), qPrintable( err ) );
        QVERIFY( snap2.contains( QStringLiteral( "result/D61/v2" ) ) );

        // v1 快照未被回写：PDF 内容不变且仍只读。
        QFile pf1( QDir( snap1 ).filePath( QDir( snap1 ).entryList( { QStringLiteral( "*.pdf" ) }, QDir::Files ).first() ) );
        QVERIFY( pf1.open( QIODevice::ReadOnly ) );
        QCOMPARE( pf1.readAll(), v1PdfBytes );
        pf1.close();
        QVERIFY( !QFileInfo( pf1.fileName() ).isWritable() );
    }
};

int main( int argc, char *argv[] )
{
    if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
        qputenv( "QT_QPA_PLATFORM", "offscreen" );
    QgsApplication app( argc, argv, false );
    app.setPrefixPath( QStringLiteral( "/usr" ), true );
    app.initQgis();
    TestVersions tc;
    const int rc = QTest::qExec( &tc, argc, argv );
    QgsApplication::exitQgis();
    return rc;
}

#include "tst_versions.moc"
