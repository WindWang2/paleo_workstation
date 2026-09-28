#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgscolorramp.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

#include <gdal.h>
#include <cpl_conv.h>

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/factorcontour.h"
#include "../src/qgis/factorstylewriter.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisstyleservice.h"
#include "../src/services/singlefactordef.h"
#include "../src/workflow/workflows.h"

// m2(B) 单因素图页 — 真实栈（tst_workflows 模式：临时 manifest+catalog+真实
// QGIS 引导）。覆盖：注册表 processingAlgId 在运行时可用（algorithmIds 核实）、
// generateFactor 声明进 04_SingleFactor（layerId/group/type/styleRef）、
// 重生成幂等、色带 .qml 落盘且可被 QgisStyleService::applyStyle 应用、
// 等值线产出真 LineString vector layer（gdal:contour 不可用 → GDAL C API 降级）。
class TestFactorWorkflow : public QObject
{
  Q_OBJECT

  private:
    // 与 tst_workflows 同构的真实服务栈（临时目录）。
    struct Fixture
    {
      QTemporaryDir dir;
      DataCatalog catalog;
      QgisProjectService projectSvc;
      PaleoProjectStore store;
      LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
      QgisLayerService layers{ &projectSvc, &manifest };
      QgisProcessingService proc{ &store };
    };

    static bool initFixture( Fixture &f )
    {
      if ( !f.dir.isValid() )
        return false;
      if ( !f.catalog.open( f.dir.path() ) )
        return false;
      if ( !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
        return false;
      if ( !f.manifest.open() )
        return false;
      return true;
    }

    // 三个 z 值井点（tst_workflows 同款 fixture），wells.* 前缀供生成链解析。
    static bool writePointsGeoJson( const QString &path )
    {
      QFile f( path );
      if ( !f.open( QIODevice::WriteOnly ) )
        return false;
      f.write( "{\"type\":\"FeatureCollection\",\"features\":["
               "{\"type\":\"Feature\",\"properties\":{\"z\":0.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,0.0]}},"
               "{\"type\":\"Feature\",\"properties\":{\"z\":8.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[4.0,0.0]}},"
               "{\"type\":\"Feature\",\"properties\":{\"z\":4.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,4.0]}}"
               "]}" );
      f.close();
      return QFile::exists( path ) && QFileInfo( path ).size() > 0;
    }

    static LayerDeclaration decl( const QString &layerId, const QString &horizon,
                                  const QString &type, const QString &source,
                                  const QString &group = QStringLiteral( "00_Test" ) )
    {
      LayerDeclaration d;
      d.layerId = layerId;
      d.horizon = horizon;
      d.type = type;
      d.source = source;
      d.group = group;
      return d;
    }

    static const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
    {
      const QVector<LayerDeclaration> decls = layers.declared();
      for ( const LayerDeclaration &d : decls )
        if ( d.layerId == layerId )
          return new LayerDeclaration( d );
      return nullptr;
    }

    static bool derivedVersionRegistered( DataCatalog &catalog, const QString &assetType,
                                          const QString &absolutePath )
    {
      for ( const CatalogAsset &a : catalog.assets() )
      {
        if ( a.type != assetType )
          continue;
        for ( const CatalogVersion &v : catalog.versionsForAsset( a.id ) )
          if ( absolutePath.contains( QStringLiteral( "artifacts/derived/" ) ) &&
               absolutePath.endsWith( QLatin1Char( '/' ) + v.fileName ) &&
               absolutePath.contains( v.id ) && !v.sha256.isEmpty() )
            return true;
      }
      return false;
    }

    // 标准前置：井点声明 + 绑 catalog 的 ConstraintWorkflow。
    static bool setupWells( Fixture &f, QString *err )
    {
      const QString ptsPath = f.dir.filePath( QStringLiteral( "wells.geojson" ) );
      if ( !writePointsGeoJson( ptsPath ) )
        return false;
      return f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                      QStringLiteral( "vector" ), ptsPath ),
                               err );
    }

  private:
    // 主线6：3×3 同网格构造面栅格（GTiff，GDAL C API——同 tst_algorithms 的
    // makeRaster 形）。
    static QString makeSurfaceRaster( const QString &path, const QVector<float> &px )
    {
      GDALDriverH drv = GDALGetDriverByName( "GTiff" );
      GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), 3, 3, 1, GDT_Float32, nullptr );
      if ( !ds )
        return QString();
      const double gt[6] = { 0.0, 1.0, 0.0, 3.0, 0.0, -1.0 };
      GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
      GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
      const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, 3, 3,
                                       const_cast<float *>( px.constData() ), 3, 3,
                                       GDT_Float32, 0, 0 );
      GDALClose( ds );
      return err == CE_None ? path : QString();
    }

  private slots:
    void initTestCase()
    {
      QVERIFY( QgsApplication::instance() != nullptr );
      Fixture f;
      QVERIFY( initFixture( f ) );
      QVERIFY( f.proc.paleoAlgorithmIds().contains( QStringLiteral( "paleo:paleo_constraint_idw" ) ) );
    }

    void cleanup()
    {
      QgsProject::instance()->removeAllMapLayers();
    }

    // 注册表 processingAlgId 必须真实可用（运行时核实）。
    // 附核实结论：C++ 嵌入运行时 Processing 注册表只含 paleo:*（native/gdal
    // 等不自动注册；gdal:contour 属 Python provider），等值线因此走
    // GDAL C API 降级（FactorContourService）——见 contourDeclaresVectorLayer。
    void registryAlgorithmsAvailableAtRuntime()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      const QStringList ids = f.proc.algorithmIds();
      QVERIFY2( ids.contains( QStringLiteral( "paleo:paleo_constraint_idw" ) ),
                "paleo:paleo_constraint_idw must be registered" );
      // 主线6 分级：已接入引擎（IDW/isopach）运行时必须注册；冻结契约的
      // blocked 引擎（welldist 距离变换 / confidence 置信度面）恰相反——
      // 注册了反而说明契约被人抢注，生成链的显式拒绝就失真了。
      for ( const SingleFactorDefinition &d : SingleFactorRegistry::builtins() )
      {
        if ( d.processingAlgId == SingleFactorContracts::welldistEngineId()
             || d.processingAlgId == SingleFactorContracts::confidenceEngineId() )
        {
          QVERIFY2( !ids.contains( d.processingAlgId ),
                    qPrintable( QStringLiteral( "blocked engine %1 must NOT be registered yet" )
                                    .arg( d.processingAlgId ) ) );
          continue;
        }
        QVERIFY2( ids.contains( d.processingAlgId ),
                  qPrintable( QStringLiteral( "%1 not registered at runtime" ).arg( d.processingAlgId ) ) );
      }
    }

    void styleWriterPresets()
    {
      for ( const SingleFactorDefinition &d : SingleFactorRegistry::builtins() )
      {
        QgsColorRamp *ramp = FactorStyleWriter::rampFor( d.factorId );
        QVERIFY2( ramp != nullptr, qPrintable( QStringLiteral( "no ramp for %1" ).arg( d.factorId ) ) );
        delete ramp;
        const QVariantMap desc = FactorStyleWriter::presetDescription( d.factorId );
        QVERIFY( desc.contains( QStringLiteral( "color1" ) ) );
        QVERIFY( desc.contains( QStringLiteral( "color2" ) ) );
      }
      QVERIFY( FactorStyleWriter::rampFor( QStringLiteral( "nope" ) ) == nullptr );
      QVERIFY( FactorStyleWriter::presetDescription( QStringLiteral( "nope" ) ).isEmpty() );
      // 空 styleDir → 不落盘（内存描述路径）。
      QString err;
      QVERIFY( FactorStyleWriter::writeStyleQml( QStringLiteral( "poro" ),
                                                 QStringLiteral( "x.tif" ), QString(), &err )
                   .isEmpty() );
      QVERIFY( !err.isEmpty() );
    }

    // 生成链：井点 → paleo 插值 → declare factor.<h>.<fid> 进 04_SingleFactor
    // + 色带 .qml 落盘 + DERIVED 版本登记。
    void generateFactorDeclaresAndStyles()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;
      QVERIFY2( setupWells( f, &err ), qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QSignalSpy generated( &wf, &ConstraintWorkflow::factorGenerated );

      QVariantMap params;
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "cellSize" ), 1.0 );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ),
                                   params, &err ),
                qPrintable( err ) );
      QCOMPARE( generated.count(), 1 );
      QCOMPARE( generated.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
      QCOMPARE( generated.at( 0 ).at( 1 ).toString(), QStringLiteral( "sandthick" ) );
      QCOMPARE( generated.at( 0 ).at( 2 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );

      const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY2( d != nullptr, "factor declaration missing" );
      QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
      QCOMPARE( d->type, QStringLiteral( "raster" ) );
      QCOMPARE( d->group, QStringLiteral( "04_SingleFactor" ) );
      QCOMPARE( d->styleRef, QStringLiteral( "factor_sandthick" ) );
      QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
      QVERIFY2( d->source.contains( QStringLiteral( "artifacts/derived/" ) ),
                qPrintable( d->source ) );
      QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "single_factor_raster" ), d->source ),
                qPrintable( d->source ) );
      delete d;

      // 色带 .qml 落盘 <projectDir>/styles/，且能被 QgisStyleService 应用到
      // 实例化出来的栅格层。
      const QString stylesDir = f.dir.filePath( QStringLiteral( "styles" ) );
      const QString qml = QDir( stylesDir ).filePath( QStringLiteral( "factor_sandthick.qml" ) );
      QVERIFY2( QFile::exists( qml ), qPrintable( qml ) );

      QgsMapLayer *layer = f.layers.instantiate( QStringLiteral( "factor.T1.sandthick" ), &err );
      QVERIFY2( layer != nullptr, qPrintable( err ) );
      QgisStyleService styleSvc;
      styleSvc.setStylesRoot( stylesDir );
      QVERIFY2( styleSvc.applyStyle( layer, QStringLiteral( "factor_sandthick" ), &err ),
                qPrintable( err ) );
    }

    // 重生成同 factorId → 同 layerId upsert（幂等：清单单行，无重复声明）。
    void regenerateIsIdempotent()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;
      QVERIFY2( setupWells( f, &err ), qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QSignalSpy generated( &wf, &ConstraintWorkflow::factorGenerated );

      QVariantMap params;
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "poro" ), params, &err ),
                qPrintable( err ) );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "poro" ), params, &err ),
                qPrintable( err ) );
      QCOMPARE( generated.count(), 2 );
      QCOMPARE( generated.at( 1 ).at( 2 ).toString(), QStringLiteral( "factor.T1.poro" ) );

      int declarations = 0;
      const QVector<LayerDeclaration> decls = f.layers.declared();
      for ( const LayerDeclaration &d : decls )
        if ( d.layerId == QStringLiteral( "factor.T1.poro" ) )
          ++declarations;
      QCOMPARE( declarations, 1 ); // upsert，不重复
    }

    // 等值线（§12 GIS LineString）：gdal:contour 在嵌入运行时不可用
    // （Python provider）→ GDAL C API 降级产出真 vector layer，声明进
    // 04_SingleFactor/Contours 子组。
    void contourDeclaresVectorLayer()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;
      QVERIFY2( setupWells( f, &err ), qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QVariantMap params;
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ),
                                   params, &err ),
                qPrintable( err ) );

      QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );
      QVERIFY2( wf.generateContours( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                     1.0, &err ),
                qPrintable( err ) );
      QCOMPARE( contours.count(), 1 );
      QCOMPARE( contours.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
      QCOMPARE( contours.at( 0 ).at( 1 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
      QCOMPARE( contours.at( 0 ).at( 2 ).toString(), QStringLiteral( "contours.T1.sandthick" ) );

      const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "contours.T1.sandthick" ) );
      QVERIFY2( d != nullptr, "contour declaration missing" );
      QCOMPARE( d->type, QStringLiteral( "vector" ) );
      QCOMPARE( d->group, QStringLiteral( "04_SingleFactor/Contours" ) );
      QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
      delete d;

      QgsMapLayer *layer = f.layers.instantiate( QStringLiteral( "contours.T1.sandthick" ), &err );
      QVERIFY2( layer != nullptr, qPrintable( err ) );
      auto *vl = qobject_cast<QgsVectorLayer *>( layer );
      QVERIFY( vl != nullptr );
      QVERIFY2( vl->isValid(), qPrintable( vl->error().message() ) );
      QCOMPARE( vl->geometryType(), Qgis::GeometryType::Line );
      int features = 0;
      QgsFeature feat;
      QgsFeatureIterator it = vl->getFeatures();
      while ( it.nextFeature( feat ) )
        ++features;
      QVERIFY2( features > 0, "contour layer must hold real LineString features" );
      QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "contour_lines" ),
                                          vl->source().section( QLatin1Char( '|' ), 0, 0 ) ),
                "contour GPKG should register a DERIVED version" );
    }

    // 失败路径：未知因素 id / 无井点 / 坏间距 / 未声明图层 / 非栅格输入。

  private slots:
    // ---- mapping 主线6：非 IDW 引擎分级 ----
    void strathickIsopachEngineRunsAndDeclares()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;

      // 顶/底构造面：top = base + 常量厚度（3 网格逐值 5/3/…）。
      const QVector<float> topPx = { 12.0f, 11.0f, 10.0f, 9.0f, 8.0f, 7.0f, 6.0f, 5.0f, 4.0f };
      const QVector<float> basePx = { 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f };
      const QString topPath = makeSurfaceRaster( f.dir.filePath( QStringLiteral( "top.tif" ) ), topPx );
      const QString basePath = makeSurfaceRaster( f.dir.filePath( QStringLiteral( "base.tif" ) ), basePx );
      QVERIFY2( !topPath.isEmpty() && !basePath.isEmpty(), "surface rasters failed" );
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "horizon.T1.top" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "raster" ), topPath ),
                                  &err ),
                qPrintable( err ) );
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "horizon.T1.base" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "raster" ), basePath ),
                                  &err ),
                qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QSignalSpy generated( &wf, &ConstraintWorkflow::factorGenerated );

      QVariantMap params;
      params.insert( QStringLiteral( "topLayerId" ), QStringLiteral( "horizon.T1.top" ) );
      params.insert( QStringLiteral( "baseLayerId" ), QStringLiteral( "horizon.T1.base" ) );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "strathick" ),
                                   params, &err ),
                qPrintable( err ) );
      QCOMPARE( generated.count(), 1 );
      QCOMPARE( generated.at( 0 ).at( 1 ).toString(), QStringLiteral( "strathick" ) );
      QCOMPARE( generated.at( 0 ).at( 2 ).toString(), QStringLiteral( "factor.T1.strathick" ) );

      const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "factor.T1.strathick" ) );
      QVERIFY2( d != nullptr, "strathick declaration missing" );
      QCOMPARE( d->type, QStringLiteral( "raster" ) );
      QCOMPARE( d->group, QStringLiteral( "04_SingleFactor" ) );
      QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
      QVERIFY2( d->source.contains( QStringLiteral( "artifacts/derived/" ) ),
                qPrintable( d->source ) );
      delete d;

      // 缺顶/底参数 → 工作流侧显式拒绝（页面空清单兜底）。
      err.clear();
      QVERIFY( !wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "strathick" ),
                                   QVariantMap(), &err ) );
      QVERIFY2( err.contains( QStringLiteral( "topLayerId" ) ), qPrintable( err ) );
    }

    void blockedEnginesRefuseExplicitly()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;
      QVERIFY2( setupWells( f, &err ), qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );

      // welldist/confidence：冻结契约引擎显式拒绝（不静默降级 IDW）。
      for ( const char *factorId : { "welldist", "confidence" } )
      {
        err.clear();
        QVERIFY2( !wf.generateFactor( QStringLiteral( "T1" ), QString::fromLatin1( factorId ),
                                      QVariantMap(), &err ),
                  "blocked engine must refuse" );
        QVERIFY2( err.contains( QStringLiteral( "尚未接入" ) ), qPrintable( err ) );
        QVERIFY2( err.contains( QString::fromLatin1( factorId ) ), qPrintable( err ) );
        QVERIFY( findDecl( f.layers, QStringLiteral( "factor.T1.%1" ).arg(
                               QString::fromLatin1( factorId ) ) ) == nullptr );
      }
    }

    void failurePaths()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;
      QVERIFY2( setupWells( f, &err ), qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );

      QVERIFY( !wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "ghost" ),
                                   QVariantMap(), &err ) );
      QVERIFY( !err.isEmpty() );
      err.clear();

      QVERIFY( !wf.generateFactor( QStringLiteral( "T9" ), QStringLiteral( "poro" ),
                                   QVariantMap(), &err ) ); // 无井点图层
      QVERIFY( !err.isEmpty() );
      err.clear();

      QVERIFY( !wf.generateContours( QStringLiteral( "T1" ),
                                     QStringLiteral( "factor.T1.poro" ), 0.0, &err ) );
      QVERIFY( !err.isEmpty() );
      err.clear();

      QVERIFY( !wf.generateContours( QStringLiteral( "T1" ),
                                     QStringLiteral( "factor.undeclared" ), 5.0, &err ) );
      QVERIFY( !err.isEmpty() );
      err.clear();

      // 井点（vector）当等值线输入 → 如实拒绝。
      QVERIFY( !wf.generateContours( QStringLiteral( "T1" ), QStringLiteral( "wells.T1" ),
                                     5.0, &err ) );
      QVERIFY( !err.isEmpty() );
    }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestFactorWorkflow tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_factorworkflow.moc"
