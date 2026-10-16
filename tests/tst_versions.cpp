#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>
#include <QSemaphore>
#include <QScopeGuard>
#include "../src/services/paleotaskservice.h"
#include "../src/metadata/mapsnapshot.h"
#include "../src/catalog/datacatalog.h"

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgiseditingservice.h"
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

    // #222：提交失败时编辑缓冲保留供重试/回滚，撤销历史也必须保留——
    // 失败路径不得执行成功路径的 undoStack()->clear() 收尾。
    void failedCommitKeepsUndoHistory()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString gpkg = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies_undo.gpkg" ) ) );
        QVERIFY( !gpkg.isEmpty() );

        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "faciesundo.D61" );
        decl.horizon = QStringLiteral( "D61" );
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg );
        QString err;
        QVERIFY2( f.layers.declare( decl, &err ), qPrintable( err ) );
        auto *vl = qobject_cast<QgsVectorLayer *>( f.layers.instantiate( decl.layerId, &err ) );
        QVERIFY2( vl != nullptr, qPrintable( err ) );

        QVERIFY( vl->startEditing() );
        QgsFeature nf( vl->fields() );
        nf.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "POLYGON((2 2,3 2,3 3,2 2))" ) ) );
        QVERIFY( vl->addFeature( nf ) );
        const int undoBefore = vl->undoStack()->count();
        QVERIFY( undoBefore > 0 );

        vl->setAllowCommit( false ); // 模拟提交失败（磁盘满 / 锁 / 约束拒绝）
        const MapVersion v = f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err );
        QCOMPARE( v.version, 0 );
        QVERIFY( !err.isEmpty() );
        QVERIFY( vl->isEditable() );                       // 编辑缓冲保留
        QCOMPARE( vl->undoStack()->count(), undoBefore );  // 撤销历史保留

        // 解除故障后重试成功，成功路径照常清栈。
        vl->setAllowCommit( true );
        err.clear();
        const MapVersion retry = f.controller.saveVersion( QStringLiteral( "D61" ), QVariantMap(), &err );
        QVERIFY2( retry.version == 1, qPrintable( err ) );
        QCOMPARE( vl->undoStack()->count(), 0 );
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
            (void)pf.open( QIODevice::ReadOnly );
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

    void savedBytesAndPdfSurviveWorkingChanges() {
        Fixture f; QVERIFY(f.init()); QString error;
        const auto gpkg = makeFaciesGpkg(f.dir.filePath("working.gpkg"));
        const auto pdf = makePdf(f.dir.filePath("working.pdf"));
        LayerDeclaration decl; decl.layerId = "facies.D61"; decl.horizon = "D61";
        decl.type = "vector"; decl.source = gpkg + "|layername=facies_polygons";
        QVERIFY(f.layers.declare(decl, &error));

        const QString pdfSha = DataCatalog::sha256FileHex(pdf);
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", pdfSha, &error));
        const auto saved = f.controller.saveVersion("D61", {}, &error);
        QVERIFY2(saved.version > 0, qPrintable(error));
        const auto manifest = QJsonDocument::fromJson(saved.provenance.toUtf8()).object();
        QCOMPARE(manifest.value("snapshot_schema").toString(), QString("paleo_map_snapshot_v1"));
        QCOMPARE(manifest.value("saved_layers").toArray().size(), 1);
        const QString gpkgSha = manifest.value("saved_layers").toArray().first().toObject().value("sha256").toString();
        QFile changedPdf(pdf); QVERIFY(changedPdf.open(QIODevice::WriteOnly | QIODevice::Truncate));
        changedPdf.write("later edit"); changedPdf.close();
        // #499：declare/saveVersion 路径会实例化 QgsVectorLayer 读取 gpkg，图层经
        // 移除路径走 DeferredDelete——事件循环「回到最外层」前对象仍在，Windows 上
        // 就锁住 gpkg（Linux 允许 unlink 打开的 fd，故 linux 车道掩盖了该平台差异）。
        // 真实应用里事件循环持续运行、句柄自然释放，**不是产品泄漏**；测试需按 Qt
        // 删除语义等它落地，而不是放宽「文件必须被删除」这个断言。
        // 注意：QTRY/qWait 内部的 processEvents 默认**不处理** DeferredDelete，
        // 必须显式 sendPostedEvents；实测落地后同一句 QFile::remove 立即返回 true。
        // #499：saveVersion 的快照捕获（MapVersionController::captureVersion）会
        // instantiate 该层位的 vector 图层并 addMapLayer 进工程——实例活着的 OGR
        // provider 持有 gpkg 只读句柄，Windows 上就删不掉（Linux 允许 unlink 打开的
        // fd，故 linux 车道掩盖了这个平台差异）。真实序列里用户在源文件消失前会先把
        // 图层从工程移除，测试按同一序列补这一步。注意 purge 不能靠
        // QgsProject::removeMapLayer(f.layers.layer(id))——实例可能不在 m_instances
        // （no-op），removeDeclarations 按工程内 paleoLayerId 遍历、同步删图层，更稳。
        // publish 读的是已保存版本的 saved_layers 快照（frozen），不依赖当前声明，
        // 故本用例后续断言语义不变。
        // #499：saveVersion 的快照捕获（MapVersionController::captureVersion）会
        // instantiate 该层位的 vector 图层并 addMapLayer 进工程——实例活着的 OGR
        // provider 持有 gpkg 只读句柄，Windows 上文件就删不掉（Linux 允许 unlink
        // 打开的 fd，故 linux 车道掩盖了这个平台差异）。真实序列里用户在源文件消失
        // 前会先把图层从工程移除，测试按同一序列补这一步。
        // 两处细节都是必需的（实测踩过）：
        //  ① 用 removeDeclarations 而非 removeMapLayer(layers.layer(id))——后者在
        //     实例未被服务登记时是 no-op；
        //  ② QgsProject::removeMapLayers 走 deleteLater，对象析构前 OGR 句柄不释放，
        //     须显式 sendPostedEvents 处理 DeferredDelete（processEvents/qWait 默认
        //     不处理它，QTRY 重试因此无效）。
        QString dropError;
        QVERIFY2(f.layers.removeDeclarations({QStringLiteral("facies.D61")}, &dropError),
                 qPrintable(dropError));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QVERIFY(QFile::remove(gpkg));
        const QString summary = R"({"wells_total":1,"covered":1,"missing":[],"rows":[{"kind":"residual","residual_ms":1}]})";
        const auto published = f.controller.publish("D61", summary, &error);
        QVERIFY2(!published.isEmpty(), qPrintable(error));
        QCOMPARE(DataCatalog::sha256FileHex(QDir(published).filePath("layout.pdf")), pdfSha);
        const auto files = QDir(published).entryList({"*.gpkg"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QCOMPARE(DataCatalog::sha256FileHex(QDir(published).filePath(files.first())), gpkgSha);
        QVERIFY(QFileInfo::exists(QDir(published).filePath("evidence-snapshot.json")));
    }

    // #439：三参数旧调用 saveVersion(horizon,"{}") 不写 snapshot_schema；
    // 随后 publish 仍按调用方 decls 复制图层文件。
    void oldCallSaveThenPublishStillCopiesDecls() {
        Fixture f; QVERIFY(f.init()); QString error;
        const auto gpkg = makeFaciesGpkg(f.dir.filePath("legacy.gpkg"));
        QVERIFY(!gpkg.isEmpty());
        const auto pdf = makePdf(f.dir.filePath("legacy.pdf"));
        QVERIFY(!pdf.isEmpty());
        const QString pdfSha = DataCatalog::sha256FileHex(pdf);
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", pdfSha, &error));

        // 三参数旧调用：不传 layers。
        const auto saved = f.versions.saveVersion("D61", QStringLiteral("{}"), &error);
        QVERIFY2(saved.version > 0, qPrintable(error));
        const auto manifest = QJsonDocument::fromJson(saved.provenance.toUtf8()).object();
        QVERIFY(!manifest.contains("snapshot_schema"));
        QVERIFY(!manifest.contains("saved_layers"));

        LayerDeclaration decl; decl.layerId = "facies.D61"; decl.horizon = "D61";
        decl.type = "vector"; decl.source = gpkg + "|layername=facies_polygons";
        const QString summary = R"({"wells_total":1,"covered":1,"missing":[],"rows":[{"kind":"residual","residual_ms":1}]})";
        const auto published = f.versions.publish("D61", {decl}, summary, &error);
        QVERIFY2(!published.isEmpty(), qPrintable(error));
        const auto files = QDir(published).entryList({"*.gpkg"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QCOMPARE(DataCatalog::sha256FileHex(QDir(published).filePath(files.first())),
                 DataCatalog::sha256FileHex(gpkg));
        // 旧发布路径按原文件名复制 PDF（不是 layout.pdf）。
        const auto pdfs = QDir(published).entryList({"*.pdf"}, QDir::Files);
        QCOMPARE(pdfs.size(), 1);
        QCOMPARE(DataCatalog::sha256FileHex(QDir(published).filePath(pdfs.first())), pdfSha);
    }

    void missingFrozenBytesRejectPublicationAndCleanStaging() {
        Fixture f; QVERIFY(f.init()); QString error;
        // #439：冻结标记依赖传入图层；本用例需声明图层才会走不可变清单。
        const auto gpkg = makeFaciesGpkg(f.dir.filePath("map.gpkg"));
        LayerDeclaration decl; decl.layerId = "facies.D61"; decl.horizon = "D61";
        decl.type = "vector"; decl.source = gpkg + "|layername=facies_polygons";
        QVERIFY(f.layers.declare(decl, &error));
        const auto pdf = makePdf(f.dir.filePath("map.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &error));
        const auto saved = f.controller.saveVersion("D61", {}, &error);
        QVERIFY(saved.version > 0);
        const auto manifest = QJsonDocument::fromJson(saved.provenance.toUtf8()).object();
        QCOMPARE(manifest.value("snapshot_schema").toString(), QString("paleo_map_snapshot_v1"));
        const auto frozenPdf = f.dir.filePath(manifest.value("saved_pdf").toObject().value("path").toString());
        QFile::setPermissions(frozenPdf, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QVERIFY(QFile::remove(frozenPdf));
        const QString summary = R"({"wells_total":1,"covered":1,"missing":[],"rows":[{"kind":"residual","residual_ms":1}]})";
        QVERIFY(f.controller.publish("D61", summary, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("快照")));
        QCOMPARE(f.versions.latest("D61").state, QString("Editing"));
        QVERIFY(QDir(f.dir.filePath("result/D61")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void corruptedSavedManifestCannotFallBackToLiveFiles() {
        Fixture f; QVERIFY(f.init()); QString error;
        const auto gpkg = makeFaciesGpkg(f.dir.filePath("corrupt-map.gpkg"));
        LayerDeclaration decl; decl.layerId = "facies.D61"; decl.horizon = "D61";
        decl.type = "vector"; decl.source = gpkg + "|layername=facies_polygons";
        QVERIFY(f.layers.declare(decl, &error));
        const auto pdf = makePdf(f.dir.filePath("map.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &error));
        const auto saved = f.controller.saveVersion("D61", {}, &error); QVERIFY(saved.version > 0);
        auto manifest = QJsonDocument::fromJson(saved.provenance.toUtf8()).object();
        QVERIFY(manifest.contains("snapshot_schema"));
        manifest.insert("saved_layers", "corrupt");
        auto db = QSqlDatabase::database("paleo_mapversions_" + QString::number(qHash(f.metaPath())));
        QSqlQuery update(db); update.prepare("UPDATE map_versions SET provenance=? WHERE id=?");
        update.addBindValue(QString::fromUtf8(QJsonDocument(manifest).toJson(QJsonDocument::Compact)));
        update.addBindValue(saved.id); QVERIFY(update.exec());
        const QString summary = R"({"wells_total":1,"covered":1,"missing":[],"rows":[{"kind":"residual","residual_ms":1}]})";
        QVERIFY(f.controller.publish("D61", summary, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("损坏")));
        QVERIFY(!QFileInfo::exists(f.dir.filePath("result/D61/v1")));
        QCOMPARE(f.versions.latest("D61").state, QString("Editing"));
    }
    void corruptedVersionEvidenceIsNotCommittedAsEmpty() {
        Fixture f; QVERIFY(f.init()); QString error;
        DataCatalog catalog; QVERIFY(catalog.open(f.dir.path())); f.controller.setCatalog(&catalog);
        const auto path = makeFaciesGpkg(f.dir.filePath("facies.gpkg")); QVERIFY(!path.isEmpty());
        CatalogAsset asset; asset.id = "asset-source"; asset.type = "facies_polygons";
        QVERIFY(catalog.addAsset(asset, &error));
        CatalogVersion version; version.id = "version-source"; version.assetId = asset.id;
        version.stage = "DERIVED"; version.path = path; version.managed = false;
        version.sha256 = DataCatalog::sha256FileHex(path);
        version.extra = {{"layer_id", "facies.D61"}, {"horizon", "D61"}, {"evidence", "broken JSON"}};
        QVERIFY(catalog.addVersion(version, &error));
        LayerDeclaration layer; layer.layerId = "facies.D61"; layer.horizon = "D61";
        layer.type = "vector"; layer.source = path + "|layername=facies_polygons";
        QVERIFY(f.layers.declare(layer, &error));
        QVERIFY(f.controller.saveVersion("D61", {}, &error).version == 0);
        QVERIFY(error.contains(QStringLiteral("证据字段损坏")));
        QCOMPARE(catalog.versionById(version.id).extra.value("evidence").toString(), QString("broken JSON"));
        QCOMPARE(f.versions.currentVersion("D61"), 0);
    }

    void lostCatalogRejectsBoundSnapshotPublication() {
        Fixture f; QVERIFY(f.init()); QString error;
        auto catalog = std::make_unique<DataCatalog>(); QVERIFY(catalog->open(f.dir.path()));
        f.controller.setCatalog(catalog.get());
        const auto path = makeFaciesGpkg(f.dir.filePath("facies.gpkg"));
        CatalogAsset asset; asset.id = "asset-source"; asset.type = "facies_polygons";
        QVERIFY(catalog->addAsset(asset, &error));
        CatalogVersion version; version.id = "version-source"; version.assetId = asset.id;
        version.stage = "DERIVED"; version.managed = false; version.path = path;
        version.sha256 = DataCatalog::sha256FileHex(path);
        version.extra = {{"layer_id", "facies.D61"}, {"horizon", "D61"}};
        QVERIFY(catalog->addVersion(version, &error));
        LayerDeclaration layer; layer.layerId = "facies.D61"; layer.horizon = "D61";
        layer.type = "vector"; layer.source = path + "|layername=facies_polygons";
        QVERIFY(f.layers.declare(layer, &error));
        const auto pdf = makePdf(f.dir.filePath("map.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &error));
        QVERIFY2(f.controller.saveVersion("D61", {}, &error).version > 0, qPrintable(error));
        catalog.reset();
        const QString summary = R"({"wells_total":1,"covered":1,"missing":[],"rows":[{"kind":"residual","residual_ms":1}]})";
        QVERIFY(f.controller.publish("D61", summary, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("资料目录不可用")));
        QCOMPARE(f.versions.latest("D61").state, QString("Editing"));
        QVERIFY(!QFileInfo::exists(f.dir.filePath("result/D61/v1")));
    }
    void publicationSqlFailureRollsBackFilesAndCanRetry() {
        Fixture f; QVERIFY(f.init()); QString error;
        const auto pdf = makePdf(f.dir.filePath("map.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &error));
        const auto saved = f.controller.saveVersion("D61", {}, &error); QVERIFY(saved.version > 0);
        auto db = QSqlDatabase::database("paleo_mapversions_" + QString::number(qHash(f.metaPath())));
        QSqlQuery trigger(db);
        QVERIFY(trigger.exec("CREATE TRIGGER refuse_publish BEFORE UPDATE OF state ON map_versions WHEN NEW.state='Published' BEGIN SELECT RAISE(ABORT,'publication refused'); END"));
        const QString summary = R"({"wells_total":1,"covered":1,"missing":[],"rows":[{"kind":"residual","residual_ms":1}]})";
        QVERIFY(f.controller.publish("D61", summary, &error).isEmpty());
        QVERIFY(error.contains("publication refused"));
        QCOMPARE(f.versions.latest("D61").state, QString("Editing"));
        QVERIFY(QDir(f.dir.filePath("result/D61")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(trigger.exec("DROP TRIGGER refuse_publish")); error.clear();
        QVERIFY2(!f.controller.publish("D61", summary, &error).isEmpty(), qPrintable(error));
        QCOMPARE(f.versions.latest("D61").state, QString("Published"));
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

    // ---- Issue #27: saveVersion 写队列路由与 dual-key busy token 彻底释放 ----
    void saveVersionRoutesThroughWriteQueueAndReleasesAllTokens()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.controller.setProjectStore( &f.store );
        QgisEditingService editSvc( &f.store );
        f.controller.setEditingService( &editSvc );

        const QString gpkg = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies27.gpkg" ) ) );
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

        // 1. 通过 editSvc 开启编辑会话（对 d.layerId 和 vl->id() 建立 busy 标记）
        QVERIFY( editSvc.beginEdit( vl, &err ) );
        QVERIFY( vl->isEditable() );
        QVERIFY( f.store.layerBusy( decl.layerId ) );

        QVariantMap prov;
        prov.insert( QStringLiteral( "step" ), QStringLiteral( "test27" ) );

        // 2. 模拟写队列被置为只读（如工程被另一实例持锁降级模式）
        f.store.setReadOnly( true );
        MapVersion vFail = f.controller.saveVersion( QStringLiteral( "D61" ), prov, &err );
        // 写队列拒绝保存版本
        QCOMPARE( vFail.version, 0 );
        QVERIFY( !err.isEmpty() );

        // 提交失败且图层仍在编辑：edit 锁仍属于这次会话，不能按图层 ID 清掉。
        QVERIFY( vl->isEditable() );
        QString busy;
        QVERIFY( f.store.layerBusy( decl.layerId, &busy ) );
        QVERIFY( busy.startsWith( QStringLiteral( "edit" ) ) );
        if ( vl->id() != decl.layerId )
            QVERIFY( !f.store.layerBusy( vl->id() ) );

        // 3. 恢复可写后正常保存版本，验证通过写队列完成保存并清空 busy 标记
        f.store.setReadOnly( false );
        f.store.markLayerBusy( decl.layerId, QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        f.store.markLayerBusy( vl->id(), QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        QVERIFY( f.store.layerBusy( decl.layerId ) );
        QVERIFY( vl->isEditable() );

        MapVersion vSuccess = f.controller.saveVersion( QStringLiteral( "D61" ), prov, &err );
        QVERIFY2( vSuccess.version == 1, qPrintable( err ) );
        QVERIFY( !vl->isEditable() );
        QVERIFY( !f.store.layerBusy( decl.layerId ) );
        QVERIFY( !f.store.layerBusy( vl->id() ) );
    }

    // ---- Challenger 2 Adversarial Stress Test: Genuine Write Queue Serialization ----
    void adversarialWriteQueueGenuineSerialization()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.controller.setProjectStore( &f.store );

        const QString gpkg = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies_ser.gpkg" ) ) );
        QVERIFY( !gpkg.isEmpty() );

        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "facies.D61_ser" );
        decl.horizon = QStringLiteral( "D61" );
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg );
        decl.group = QStringLiteral( "05_PaleoMap" );
        QString err;
        QVERIFY( f.layers.declare( decl, &err ) );

        std::atomic<bool> holdQueue{ true };
        std::atomic<bool> queueActive{ false };

        // 1. 模拟另一线程正在持有写队列
        std::thread blockerThread( [&]() {
            f.store.enqueueWrite( [&]() -> PaleoProjectStore::WriteResult {
                queueActive = true;
                while ( holdQueue.load() )
                {
                    QThread::msleep( 10 );
                }
                return { true, QString() };
            } );
        } );

        QTRY_VERIFY( queueActive.load() );

        // 2. 启动计时器，并在 250ms 后释放写队列
        QElapsedTimer timer;
        timer.start();

        std::thread releaserThread( [&]() {
            QThread::msleep( 250 );
            holdQueue = false;
        } );

        // 3. 在主线程调用 saveVersion：必须等待写队列释放后方能进入执行
        QVariantMap prov;
        prov.insert( QStringLiteral( "step" ), QStringLiteral( "serialization_test" ) );
        QString saveErr;
        MapVersion v = f.controller.saveVersion( QStringLiteral( "D61" ), prov, &saveErr );
        const qint64 elapsed = timer.elapsed();

        blockerThread.join();
        releaserThread.join();

        // 4. 严谨断言：saveVersion 耗时必须 >= 200ms，证明其确实在写队列中等待锁释放，绝未绕过！
        QVERIFY2( elapsed >= 200,
                  qPrintable( QStringLiteral( "saveVersion bypassed queue! Elapsed: %1 ms" ).arg( elapsed ) ) );
        QVERIFY( v.version > 0 );
        QCOMPARE( f.versions.latest( QStringLiteral( "D61" ) ).version, 1 );
    }

    // ---- Challenger 2 Adversarial Stress Test: Multi-layer Partial Commit Failure Token Cleanup ----
    void adversarialMultiLayerPartialCommitFailureReleasesAllTokens()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.controller.setProjectStore( &f.store );
        QgisEditingService editSvc( &f.store );
        f.controller.setEditingService( &editSvc );

        const QString gpkg1 = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies_p1.gpkg" ) ) );
        const QString gpkg2 = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies_p2.gpkg" ) ) );
        const QString gpkg3 = makeFaciesGpkg( f.dir.filePath( QStringLiteral( "facies_p3.gpkg" ) ) );
        QVERIFY( !gpkg1.isEmpty() && !gpkg2.isEmpty() && !gpkg3.isEmpty() );

        LayerDeclaration d1, d2, d3, dGhost;
        d1.layerId = QStringLiteral( "layer1.D61" );
        d1.horizon = QStringLiteral( "D61" );
        d1.type = QStringLiteral( "vector" );
        d1.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg1 );

        d2.layerId = QStringLiteral( "layer2.D61" );
        d2.horizon = QStringLiteral( "D61" );
        d2.type = QStringLiteral( "vector" );
        d2.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg2 );

        d3.layerId = QStringLiteral( "layer3.D61" );
        d3.horizon = QStringLiteral( "D61" );
        d3.type = QStringLiteral( "vector" );
        d3.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( gpkg3 );

        // 注册一个损坏/不存在的数据源作为幽灵图层
        dGhost.layerId = QStringLiteral( "layerGhost.D61" );
        dGhost.horizon = QStringLiteral( "D61" );
        dGhost.type = QStringLiteral( "vector" );
        dGhost.source = QStringLiteral( "/nonexistent/path.gpkg|layername=none" );

        QString err;
        QVERIFY( f.layers.declare( d1, &err ) );
        QVERIFY( f.layers.declare( d2, &err ) );
        QVERIFY( f.layers.declare( d3, &err ) );
        QVERIFY( f.layers.declare( dGhost, &err ) );

        auto *vl1 = qobject_cast<QgsVectorLayer *>( f.layers.instantiate( d1.layerId, &err ) );
        auto *vl2 = qobject_cast<QgsVectorLayer *>( f.layers.instantiate( d2.layerId, &err ) );
        auto *vl3 = qobject_cast<QgsVectorLayer *>( f.layers.instantiate( d3.layerId, &err ) );
        QVERIFY( vl1 && vl2 && vl3 );

        // 开始编辑会话
        QVERIFY( editSvc.beginEdit( vl1, &err ) );
        QVERIFY( editSvc.beginEdit( vl2, &err ) );
        QVERIFY( editSvc.beginEdit( vl3, &err ) );

        // 建立 dual-key busy token
        f.store.markLayerBusy( d1.layerId, QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        f.store.markLayerBusy( vl1->id(), QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        f.store.markLayerBusy( d2.layerId, QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        f.store.markLayerBusy( vl2->id(), QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        f.store.markLayerBusy( d3.layerId, QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        f.store.markLayerBusy( vl3->id(), QStringLiteral( "edit" ), QStringLiteral( "editing" ) );
        f.store.markLayerBusy( dGhost.layerId, QStringLiteral( "edit" ), QStringLiteral( "ghost editing" ) );

        // 模拟 vl2 提交失败（利用 QGIS 原生 setAllowCommit(false)）
        vl2->setAllowCommit( false );

        // 执行 saveVersion
        QVariantMap prov;
        prov.insert( QStringLiteral( "step" ), QStringLiteral( "partial_failure_test" ) );
        MapVersion v = f.controller.saveVersion( QStringLiteral( "D61" ), prov, &err );

        // 断言：由于 layer2 提交失败，saveVersion 应当失败返回版本 0
        QCOMPARE( v.version, 0 );
        QVERIFY( !err.isEmpty() );

        // 已提交的层释放双键。仍在编辑的 layer2 保留自己的 edit 锁。
        // 无法实例化的幽灵层没有活会话，孤立 edit 标记必须释放，避免卡住后续保存。
        QVERIFY( !vl1->isEditable() );
        QVERIFY( !f.store.layerBusy( d1.layerId ) );
        QVERIFY( !f.store.layerBusy( vl1->id() ) );
        QVERIFY( vl2->isEditable() );
        QVERIFY( f.store.layerBusy( d2.layerId ) );
        QVERIFY( f.store.layerBusy( vl2->id() ) );
        QVERIFY( !vl3->isEditable() );
        QVERIFY( !f.store.layerBusy( d3.layerId ) );
        QVERIFY( !f.store.layerBusy( vl3->id() ) );
        QVERIFY( !f.store.layerBusy( dGhost.layerId ) );
    }
    void asyncSaveFreezesAllDeclaredProviderViewsAndPublishesSavedBytes()
    {
        Fixture f; QVERIFY(f.init());
        PaleoTaskService tasks; tasks.setMaxWorkerThreads(1); f.controller.setTaskService(&tasks);
        QString err;
        const auto gpkg = makeFaciesGpkg(f.dir.filePath("facies-async.gpkg")); QVERIFY(!gpkg.isEmpty());
        LayerDeclaration first; first.layerId = "facies.D61"; first.horizon = "D61"; first.type = "vector";
        first.source = gpkg + "|layername=facies_polygons";
        LayerDeclaration hidden = first; hidden.layerId = "reference.D61";
        QVERIFY(f.layers.declare(first, &err)); QVERIFY(f.layers.declare(hidden, &err));
        auto *one = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(first.layerId, &err));
        auto *empty = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(hidden.layerId, &err));
        QVERIFY(one && empty); QVERIFY(empty->setSubsetString("fid < 0"));
        const auto pdf = makePdf(f.dir.filePath("async-layout.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &err));
        QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
        QVERIFY2(f.controller.startSaveVersion("D61", {{"test", "async"}}, &err), qPrintable(err));
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
        QCOMPARE(finished.size(), 1); QVERIFY2(finished[0][2].toBool(), qPrintable(finished[0][4].toString()));
        const auto version = f.versions.latest("D61"); QCOMPARE(version.version, 1);
        const auto frozen = QJsonDocument::fromJson(version.provenance.toUtf8()).toVariant().toMap();
        QCOMPARE(frozen.value("saved_layers").toList().size(), 2);
        QSet<QString> paths;
        for (const auto &entry : frozen.value("saved_layers").toList()) {
            const auto row = entry.toMap(); const auto file = f.dir.filePath(row.value("path").toString());
            QgsVectorLayer saved(file + row.value("source_suffix").toString(), "saved", "ogr");
            QVERIFY(saved.isValid()); QCOMPARE(saved.featureCount(), row.value("layer_id").toString() == first.layerId ? qint64(1) : qint64(0));
            paths.insert(file); QCOMPARE(DataCatalog::sha256FileHex(file), row.value("sha256").toString());
        }
        QCOMPARE(paths.size(), 2); // Distinct provider subsets are never deduplicated by URI alone.
        QVERIFY(one->startEditing()); QgsFeature added(one->fields());
        added.setGeometry(QgsGeometry::fromWkt("POLYGON((2 0,3 0,3 1,2 0))"));
        QVERIFY(one->addFeature(added)); QVERIFY(one->commitChanges()); QCOMPARE(one->featureCount(), qint64(2));
        const QString summary = "{\"wells_total\":1,\"covered\":1,\"missing\":[],\"rows\":[{\"kind\":\"residual\",\"residual_ms\":1}]}";
        QVERIFY2(f.controller.startPublish("D61", summary, &err), qPrintable(err));
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
        QCOMPARE(finished.size(), 2); QVERIFY2(finished[1][2].toBool(), qPrintable(finished[1][4].toString()));
        QVERIFY(f.versions.isPublished("D61"));
        const auto published = f.versions.latest("D61");
        QCOMPARE(published.provenance, version.provenance);
        for (const auto &entry : frozen.value("saved_layers").toList()) {
            const auto row = entry.toMap(); const auto immutable = f.dir.filePath(row.value("path").toString());
            // Snapshot filenames are opaque. Locate the corresponding copied immutable bytes by hash.
            int matches = 0;
            for (const auto &name : QDir(published.publishedPath).entryList(QDir::Files))
                if (DataCatalog::sha256FileHex(QDir(published.publishedPath).filePath(name)) == DataCatalog::sha256FileHex(immutable)) ++matches;
            QCOMPARE(matches, 1);
        }
        QVERIFY(tasks.shutdown());
    }

    void asyncSnapshotCancellationAndProjectEpochDoNotWriteVersions_data()
    {
        QTest::addColumn<bool>("changeProject");
        QTest::newRow("user-cancel") << false;
        QTest::newRow("project-change") << true;
    }
    void asyncSnapshotCancellationAndProjectEpochDoNotWriteVersions()
    {
        QFETCH(bool, changeProject);
        Fixture f; QVERIFY(f.init()); f.controller.setProjectStore(&f.store);
        PaleoTaskService tasks; tasks.setMaxWorkerThreads(1); f.controller.setTaskService(&tasks);
        const auto gate = std::make_shared<QSemaphore>(); const auto entered = std::make_shared<QSemaphore>();
        tasks.start("hold-worker", [gate, entered](PaleoTask *) { entered->release(); gate->acquire(); return QString(); });
        const auto release = qScopeGuard([&] { gate->release(); });
        QTRY_VERIFY(entered->available() > 0);
        QString err;
        QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
        QSignalSpy busy(&f.controller, &MapVersionController::snapshotBusyChanged);
        QVERIFY2(f.controller.startSaveVersion("D61", {{"test", "cancel"}}, &err), qPrintable(err));
        QVERIFY(f.controller.snapshotBusy());
        if (changeProject) f.controller.setCatalog(nullptr); else f.controller.cancelSnapshot();
        gate->release();
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
        QCOMPARE(busy.size(), 2); QVERIFY(busy[0][0].toBool()); QVERIFY(!busy[1][0].toBool());
        QCOMPARE(f.versions.currentVersion("D61"), 0);
        QCOMPARE(QDir(f.dir.filePath("artifacts/map-versions")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        if (changeProject) QCOMPARE(finished.size(), 0); // Old completion must not reset new same-horizon UI.
        else { QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][2].toBool()); QVERIFY(finished[0][3].toBool()); }
        QVERIFY(tasks.shutdown());
    }

    void projectCloseAbandonsInFlightSnapshot()
    {
        Fixture f; QVERIFY(f.init()); f.controller.setProjectStore(&f.store);
        PaleoTaskService tasks; tasks.setMaxWorkerThreads(1); f.controller.setTaskService(&tasks);
        const auto gate = std::make_shared<QSemaphore>(); const auto entered = std::make_shared<QSemaphore>();
        tasks.start("hold-worker", [gate, entered](PaleoTask *) { entered->release(); gate->acquire(); return QString(); });
        const auto release = qScopeGuard([&] { gate->release(); });
        QTRY_VERIFY(entered->available() > 0);
        QString err;
        QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
        QVERIFY2(f.controller.startSaveVersion("D61", {{"test", "close"}}, &err), qPrintable(err));
        QVERIFY(f.controller.snapshotBusy());
        f.controller.abandonForProjectClose();
        gate->release();
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
        QCOMPARE(f.versions.currentVersion("D61"), 0);
        QCOMPARE(QDir(f.dir.filePath("artifacts/map-versions")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        QCOMPARE(finished.size(), 0);
        QVERIFY(tasks.shutdown());
    }

    void asyncPublicationFailureCleansFilesAndCanRetry()
    {
        Fixture f; QVERIFY(f.init());
        PaleoTaskService tasks; tasks.setMaxWorkerThreads(1); f.controller.setTaskService(&tasks);
        QString err; const auto pdf = makePdf(f.dir.filePath("async-failure.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &err));
        QVERIFY(f.controller.saveVersion("D61", {}, &err).version > 0);
        const QString connection = "async-publication-failure";
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(f.metaPath()); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("CREATE TRIGGER refuse_async_publish BEFORE UPDATE OF state ON map_versions WHEN NEW.state='Published' BEGIN SELECT RAISE(ABORT,'test publish failure'); END"));
            QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
            const QString summary = "{\"wells_total\":1,\"covered\":1,\"missing\":[],\"rows\":[{\"kind\":\"residual\",\"residual_ms\":1}]}";
            QVERIFY2(f.controller.startPublish("D61", summary, &err), qPrintable(err));
            QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
            QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][2].toBool()); QVERIFY(!finished[0][4].toString().isEmpty());
            QVERIFY(!f.versions.isPublished("D61"));
            QCOMPARE(QDir(f.dir.filePath("result/D61")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
            QVERIFY(q.exec("DROP TRIGGER refuse_async_publish")); err.clear();
            QVERIFY2(f.controller.startPublish("D61", summary, &err), qPrintable(err));
            QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
            QCOMPARE(finished.size(), 2); QVERIFY2(finished[1][2].toBool(), qPrintable(finished[1][4].toString()));
            QVERIFY(f.versions.isPublished("D61")); db.close();
        }
        QSqlDatabase::removeDatabase(connection); QVERIFY(tasks.shutdown());
    }

    void preparedSnapshotRejectsTamperedBytesAndCancellationLeavesNoDirectory()
    {
        Fixture f; QVERIFY(f.init()); QString err;
        const auto pdf = makePdf(f.dir.filePath("prepare.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &err));
        auto input = f.versions.snapshotInputForSave("D61", "{}", {}, &err); QVERIFY(input.valid);
        const auto cancelled = prepareMapSnapshot(input, [] { return true; });
        QVERIFY(cancelled.cancelled); QCOMPARE(f.versions.currentVersion("D61"), 0);
        // #444：取消路径不得留暂存目录；禁止用 discardPreparedMapSnapshot 掩盖泄漏。
        QVERIFY(cancelled.directory.isEmpty() || !QFileInfo::exists(cancelled.directory));
        QCOMPARE(QDir(f.dir.filePath("artifacts/map-versions")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        auto prepared = prepareMapSnapshot(input); QVERIFY2(prepared.isValid(), qPrintable(prepared.error));
        const auto stagingBefore = prepared.directory;
        QVERIFY(QFileInfo::exists(stagingBefore));
        const auto target = prepared.attestations.first().toMap().value("path").toString();
        QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile modified(target); QVERIFY(modified.open(QIODevice::Append)); QCOMPARE(modified.write("changed"), qint64(7)); modified.close();
        QCOMPARE(f.versions.commitPreparedVersion(prepared, &err).version, 0); QVERIFY(!err.isEmpty());
        QCOMPARE(f.versions.currentVersion("D61"), 0);
        // 失败后测试主动清理以便 QTemporaryDir 结束；取消路径已在上面无 discard 断言。
        if (QFileInfo::exists(stagingBefore)) discardPreparedMapSnapshot(prepared);
    }

    void horizonlessCitationBlocksPurgeAndPublish() {
        Fixture f; QVERIFY(f.init()); QString error;
        DataCatalog catalog; QVERIFY(catalog.open(f.dir.path())); f.controller.setCatalog(&catalog);
        const QString sourcePath = f.dir.filePath("manual-note.txt");
        QFile source(sourcePath); QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("horizonless manual source"), qint64(25)); source.close();
        CatalogAsset asset; asset.id = "asset-manual"; asset.type = "document";
        QVERIFY(catalog.addAsset(asset, &error));
        CatalogVersion version; version.id = "ver-manual"; version.assetId = asset.id;
        version.stage = "RAW"; version.managed = false; version.path = sourcePath;
        version.sha256 = DataCatalog::sha256FileHex(sourcePath);
        QVERIFY(catalog.addVersion(version, &error));
        const QString evidence = QString::fromUtf8(QJsonDocument(QJsonArray{QJsonObject{
            {"id", "ev-manual"}, {"role", "REFERENCES"}, {"type", "manual"},
            {"source_version", "ver-manual"}, {"text", "horizonless manual"}}}).toJson(QJsonDocument::Compact));
        QgsVectorLayer memory(QStringLiteral("polygon?crs=EPSG:4326"), QStringLiteral("facies"), QStringLiteral("memory"));
        QVERIFY(memory.dataProvider()->addAttributes({QgsField("facies_evidence", QMetaType::QString)}));
        memory.updateFields();
        QgsFeature feature(memory.fields());
        feature.setAttribute("facies_evidence", evidence);
        feature.setGeometry(QgsGeometry::fromWkt(QStringLiteral("POLYGON((0 0,1 0,1 1,0 0))")));
        QgsFeatureList citedFeatures;
        citedFeatures << feature;
        QVERIFY(memory.dataProvider()->addFeatures(citedFeatures));
        const QString gpkg = f.dir.filePath("cited-facies.gpkg");
        QgsVectorFileWriter::SaveVectorOptions options; options.driverName = "GPKG"; options.layerName = "facies_polygons";
        QString writerError;
        QCOMPARE(QgsVectorFileWriter::writeAsVectorFormatV3(&memory, gpkg, QgsProject::instance()->transformContext(), options, &writerError),
                 QgsVectorFileWriter::WriterError::NoError);
        LayerDeclaration layer; layer.layerId = "facies.D61"; layer.horizon = "D61"; layer.type = "vector";
        layer.source = gpkg + "|layername=facies_polygons";
        QVERIFY(f.layers.declare(layer, &error));
        QVERIFY(f.layers.instantiate(layer.layerId, &error));
        const auto pdf = makePdf(f.dir.filePath("cited-map.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &error));
        const auto saved = f.controller.saveVersion("D61", {}, &error);
        QVERIFY2(saved.version > 0, qPrintable(error));
        const auto cited = QJsonDocument::fromJson(saved.provenance.toUtf8()).object().value("cited_sources").toArray();
        QCOMPARE(cited.size(), 1);
        QCOMPARE(cited.at(0).toObject().value("version_id").toString(), QString("ver-manual"));
        QCOMPARE(cited.at(0).toObject().value("horizon").toString(), QString());
        QVERIFY(!catalog.removeAsset("asset-manual", &error));
        QVERIFY(error.contains(QStringLiteral("cited by map result")));
        QCOMPARE(catalog.versionById("ver-manual").id, QString("ver-manual"));
        QVERIFY(QFileInfo::exists(sourcePath));
        QVERIFY(catalog.updateVersionExtra("ver-manual", {{"retained_by_map_results", QVariantList{}}}, &error));
        error.clear();
        QVERIFY2(catalog.removeAsset("asset-manual", &error), qPrintable(error));
        QVERIFY(catalog.versionById("ver-manual").id.isEmpty());
        const QString summary = QStringLiteral("{\"wells_total\":1,\"covered\":1,\"missing\":[],\"rows\":[{\"kind\":\"residual\",\"residual_ms\":1}]}");
        QVERIFY(f.controller.publish("D61", summary, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("来源版本已缺失")));
        QCOMPARE(f.versions.latest("D61").state, QString("Editing"));
        QVERIFY(!QFileInfo::exists(f.dir.filePath("result/D61/v1")));
    }
    void unlinkingCitationKeepsTheSourceAsset() {
        Fixture f; QVERIFY(f.init()); QString error;
        DataCatalog catalog; QVERIFY(catalog.open(f.dir.path())); f.controller.setCatalog(&catalog);
        const QString sourcePath = f.dir.filePath("manual-note.txt");
        QFile source(sourcePath); QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("horizonless manual source"), qint64(25)); source.close();
        CatalogAsset asset; asset.id = "asset-manual"; asset.type = "document";
        QVERIFY(catalog.addAsset(asset, &error));
        CatalogVersion version; version.id = "ver-manual"; version.assetId = asset.id;
        version.stage = "RAW"; version.managed = false; version.path = sourcePath;
        version.sha256 = DataCatalog::sha256FileHex(sourcePath);
        QVERIFY(catalog.addVersion(version, &error));
        QJsonObject record{{"id", "ev-manual"}, {"role", "REFERENCES"}, {"type", "manual"},
                           {"source_version", "ver-manual"}, {"text", "horizonless manual"}};
        const auto encode = [](const QJsonObject &row) {
            return QString::fromUtf8(QJsonDocument(QJsonArray{row}).toJson(QJsonDocument::Compact));
        };
        QgsVectorLayer memory(QStringLiteral("polygon?crs=EPSG:4326"), QStringLiteral("facies"), QStringLiteral("memory"));
        QVERIFY(memory.dataProvider()->addAttributes({QgsField("facies_evidence", QMetaType::QString)}));
        memory.updateFields();
        QgsFeature created(memory.fields());
        created.setAttribute("facies_evidence", encode(record));
        created.setGeometry(QgsGeometry::fromWkt(QStringLiteral("POLYGON((0 0,1 0,1 1,0 0))")));
        QgsFeatureList createdFeatures;
        createdFeatures << created;
        QVERIFY(memory.dataProvider()->addFeatures(createdFeatures));
        const QString gpkg = f.dir.filePath("unlink-facies.gpkg");
        QgsVectorFileWriter::SaveVectorOptions options; options.driverName = "GPKG"; options.layerName = "facies_polygons";
        QString writerError;
        QCOMPARE(QgsVectorFileWriter::writeAsVectorFormatV3(&memory, gpkg, QgsProject::instance()->transformContext(), options, &writerError),
                 QgsVectorFileWriter::WriterError::NoError);
        LayerDeclaration declaration; declaration.layerId = "facies.D61"; declaration.horizon = "D61";
        declaration.type = "vector"; declaration.source = gpkg + "|layername=facies_polygons";
        QVERIFY(f.layers.declare(declaration, &error));
        auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(declaration.layerId, &error));
        QVERIFY(layer);
        const auto pdf = makePdf(f.dir.filePath("unlink-map.pdf"));
        QVERIFY(f.versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &error));
        QVERIFY2(f.controller.saveVersion("D61", {}, &error).version == 1, qPrintable(error));
        QVERIFY(layer->startEditing());
        QgsFeature current; QVERIFY(layer->getFeatures().nextFeature(current));
        record.insert("unlinked", true);
        QVERIFY(layer->changeAttributeValue(current.id(), layer->fields().indexOf("facies_evidence"), encode(record)));
        const auto second = f.controller.saveVersion("D61", {}, &error);
        QVERIFY2(second.version == 2, qPrintable(error));
        bool stillCited = false;
        for (const auto &entry : QJsonDocument::fromJson(second.provenance.toUtf8()).object().value("cited_sources").toArray())
            stillCited = stillCited || entry.toObject().value("version_id").toString() == "ver-manual";
        QVERIFY(!stillCited);
        QVERIFY(QFileInfo::exists(sourcePath));
        QCOMPARE(catalog.versionById("ver-manual").id, QString("ver-manual"));
        const auto retained = catalog.versionById("ver-manual").extra.value("retained_by_map_results");
        QStringList tokens = retained.toStringList();
        if (tokens.isEmpty()) for (const auto &entry : retained.toList()) tokens << entry.toString();
        QVERIFY(tokens.contains(QStringLiteral("D61/1")));
        QVERIFY(!catalog.removeAsset("asset-manual", &error));
        QVERIFY(QFileInfo::exists(sourcePath));
        QCOMPARE(catalog.versionById("ver-manual").id, QString("ver-manual"));
    }
    void publicationCrashAfterRenameIsCompletedOnReopen() {
        if (qEnvironmentVariableIntValue("PALEO_PUBLISH_CRASH_CHILD") == 1) {
            const QString root = qEnvironmentVariable("PALEO_PUBLISH_DIR");
            QVERIFY(QFileInfo(root).isDir());
            QgisProjectService projectSvc;
            PaleoProjectStore store;
            LayerManifest manifest(root + "/project.sqlite");
            QgisLayerService layers(&projectSvc, &manifest);
            MapVersionStore versions(root + "/project.sqlite");
            MapVersionController controller(&versions, &layers);
            QVERIFY(projectSvc.createProject(root + "/proj.qgz"));
            QVERIFY(manifest.open());
            store.setProjectPaths(root + "/proj.qgz", root + "/proj.gpkg", root + "/project.sqlite");
            QString error; QVERIFY2(versions.open(&error), qPrintable(error));
            const auto pdf = makePdf(root + "/map.pdf");
            QVERIFY(versions.recordLayoutProduct("D61", pdf, "pdf-asset", DataCatalog::sha256FileHex(pdf), &error));
            QVERIFY(controller.saveVersion("D61", {}, &error).version > 0);
            versions.setPublicationRenameProbe([] { ::_exit(86); });
            const QString summary = QStringLiteral("{\"wells_total\":1,\"covered\":1,\"missing\":[],\"rows\":[{\"kind\":\"residual\",\"residual_ms\":1}]}");
            controller.publish("D61", summary, &error);
            QFAIL("publication returned without the crash probe");
            return;
        }
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QProcess child;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("PALEO_PUBLISH_CRASH_CHILD", "1");
        env.insert("PALEO_PUBLISH_DIR", dir.path());
        env.insert("QT_QPA_PLATFORM", "offscreen");
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::SeparateChannels);
        child.start(QCoreApplication::applicationFilePath(), {"publicationCrashAfterRenameIsCompletedOnReopen"});
        QVERIFY2(child.waitForFinished(180000), "publication crash child timed out");
        if (child.exitCode() != 86) {
            qWarning().noquote() << child.readAllStandardOutput();
            qWarning().noquote() << child.readAllStandardError();
        }
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 86);
        MapVersionStore reopened(dir.filePath("project.sqlite"));
        QString error; QVERIFY2(reopened.open(&error), qPrintable(error));
        QCOMPARE(reopened.latest("D61").state, QString("Published"));
        const QString published = reopened.publishedPath("D61");
        QVERIFY(QFileInfo(published).isDir());
        QVERIFY(QFileInfo::exists(QDir(published).filePath("evidence-snapshot.json")));
        MapVersionStore again(dir.filePath("project.sqlite"));
        QVERIFY2(again.open(&error), qPrintable(error));
        QCOMPARE(again.publishedPath("D61"), published);
        QVERIFY(QFileInfo(published).isDir());
        const QString summary = QStringLiteral("{\"wells_total\":1,\"covered\":1,\"missing\":[],\"rows\":[{\"kind\":\"residual\",\"residual_ms\":1}]}");
        QVERIFY(again.publish("D61", {}, summary, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("当前版本已发布")));
        QVERIFY(QFileInfo(published).isDir());
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
