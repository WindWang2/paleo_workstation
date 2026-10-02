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
#include "../src/workflow/mapversioncontroller.h"
#include "../src/metadata/mapversionstore.h"

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
      // 主线6 + C5（wave/deepen-perf）分级：已接入引擎（IDW/isopach/welldist
      // 距离变换）运行时必须注册——welldist 已按 SingleFactorContracts 冻结
      // 契约实装（src/algorithms/distancetransform.cpp）；confidence 置信度面
      // 仍冻结（前置在 ONNX 置信度通道），注册了反而说明契约被人抢注，
      // 生成链的显式拒绝就失真了。
      for ( const SingleFactorDefinition &d : SingleFactorRegistry::builtins() )
      {
        if ( d.processingAlgId == SingleFactorContracts::confidenceEngineId() )
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

      // C5（wave/deepen-perf）：welldist 距离变换引擎已按冻结契约实装并
      // 注册——生成链走真引擎（无 FIELD，距离不需属性值），声明
      // factor.<h>.welldist 进 04_SingleFactor + DERIVED 版本登记。
      QSignalSpy generated( &wf, &ConstraintWorkflow::factorGenerated );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "welldist" ),
                                   QVariantMap(), &err ),
                qPrintable( err ) );
      QCOMPARE( generated.count(), 1 );
      QCOMPARE( generated.at( 0 ).at( 2 ).toString(), QStringLiteral( "factor.T1.welldist" ) );
      const LayerDeclaration *wd = findDecl( f.layers, QStringLiteral( "factor.T1.welldist" ) );
      QVERIFY2( wd != nullptr, "welldist factor declaration missing" );
      QVERIFY2( wd->type == QLatin1String( "raster" ) && QFile::exists( wd->source ),
                qPrintable( wd->source ) );

      // C4：资产关联盖章——层已实例化后重跑（同 layerId 幂等 upsert），生成
      // 侧把 catalog assetId 盖到图层对象（paleoAssetId；属性页读侧消费）。
      QgsMapLayer *wdLayer = f.layers.instantiate( QStringLiteral( "factor.T1.welldist" ), &err );
      QVERIFY2( wdLayer != nullptr, qPrintable( err ) );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "welldist" ),
                                   QVariantMap(), &err ),
                qPrintable( err ) );
      // 重生成换了 source（新版本产物路径）→ declare() 替换路径删掉旧层再
      // 重建：重跑前的 wdLayer 已悬空，必须重新取（#81：并行轮 SegFault /
      // 串行轮盖章为空的根因即解引用旧指针）。
      wdLayer = f.layers.layer( QStringLiteral( "factor.T1.welldist" ) );
      QVERIFY2( wdLayer != nullptr, "instantiated factor layer missing after regeneration" );
      {
        const LayerDeclaration *wd2 = findDecl( f.layers, QStringLiteral( "factor.T1.welldist" ) );
        QVERIFY2( wd2 != nullptr, "welldist declaration missing after regeneration" );
        QCOMPARE( wdLayer->source(), wd2->source ); // 缓存指向新层，而非旧层残留
        delete wd2;
      }
      const QString stampedAsset =
          wdLayer->customProperty( QStringLiteral( "paleoAssetId" ) ).toString();
      QVERIFY2( !stampedAsset.isEmpty(),
                "instantiated factor layer must carry paleoAssetId after regeneration" );
      QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "single_factor_raster" ),
                                          wd->source ),
                qPrintable( wd->source ) );

      // confidence：ONNX 只读首个输出张量（无置信度通道）——冻结契约引擎
      // 维持显式拒绝（不静默降级 IDW）。
      err.clear();
      QVERIFY2( !wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "confidence" ),
                                    QVariantMap(), &err ),
                "blocked engine must refuse" );
      QVERIFY2( err.contains( QStringLiteral( "尚未接入" ) ), qPrintable( err ) );
      QVERIFY2( err.contains( QStringLiteral( "confidence" ) ), qPrintable( err ) );
      QVERIFY( findDecl( f.layers, QStringLiteral( "factor.T1.confidence" ) ) == nullptr );
    }

    // WP3 Phase D：lineage 端到端——source 声明 → welldist 因素生成 → 派生
    // 登记（assetId/version/sha/manifest_layer_id）→ 图层实例化（paleoAssetId
    // 双向链）→ saveVersion → 重开（全新 manifest/catalog 实例）后逐跳信息
    // 仍完整。任何一跳缺信息都必须让断言红掉，不允许 silent success。
    void welldistLineageEndToEndReopen()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;
      QVERIFY2( setupWells( f, &err ), qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "welldist" ),
                                   QVariantMap(), &err ),
                qPrintable( err ) );

      // --- 跳 1：图层声明（layerId/horizon/type/group/styleRef/path）---------
      const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "factor.T1.welldist" ) );
      QVERIFY2( d != nullptr, "factor declaration missing" );
      QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
      QCOMPARE( d->type, QStringLiteral( "raster" ) );
      QCOMPARE( d->group, QStringLiteral( "04_SingleFactor" ) );
      QVERIFY2( !d->styleRef.isEmpty(), "styleRef must be recorded" );
      QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
      QVERIFY2( d->source.contains( QStringLiteral( "artifacts/derived/" ) ),
                qPrintable( d->source ) ); // managed/derived 边界

      // --- 跳 2：catalog 派生版本（assetId/version/sha/反链）----------------
      QString assetId;
      CatalogVersion derivedVersion;
      bool versionFound = false;
      for ( const CatalogAsset &a : f.catalog.assets() )
      {
        if ( a.type != QStringLiteral( "single_factor_raster" ) )
          continue;
        for ( const CatalogVersion &v : f.catalog.versionsForAsset( a.id ) )
        {
          if ( d->source.endsWith( QLatin1Char( '/' ) + v.fileName ) && d->source.contains( v.id ) )
          {
            assetId = a.id;
            derivedVersion = v;
            versionFound = true;
          }
        }
      }
      QVERIFY2( versionFound, "derived version must be registered for the factor raster" );
      QCOMPARE( derivedVersion.stage, QStringLiteral( "DERIVED" ) );
      QVERIFY( derivedVersion.managed );
      QVERIFY2( !derivedVersion.sha256.isEmpty(), "sha256 must be recorded" );
      QCOMPARE( derivedVersion.extra.value( QStringLiteral( "kind" ) ).toString(),
                QStringLiteral( "single_factor_raster" ) );
      QCOMPARE( derivedVersion.extra.value( QStringLiteral( "engine" ) ).toString(),
                SingleFactorContracts::welldistEngineId() );
      QCOMPARE( derivedVersion.extra.value( QStringLiteral( "manifest_layer_id" ) ).toString(),
                QStringLiteral( "factor.T1.welldist" ) ); // C4 权威反链

      // --- 跳 3：图层实例化（paleoAssetId/paleoLayerId 双向链）-------------
      QgsMapLayer *layer = f.layers.instantiate( QStringLiteral( "factor.T1.welldist" ), &err );
      QVERIFY2( layer != nullptr, qPrintable( err ) );
      QCOMPARE( layer->customProperty( QStringLiteral( "paleoLayerId" ) ).toString(),
                QStringLiteral( "factor.T1.welldist" ) );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "welldist" ),
                                   QVariantMap(), &err ),
                qPrintable( err ) ); // 重跑幂等补章（层已实例化）
      // 重跑可能换版本文件路径并替换实例（declare 的 replace 分支）——
      // 重新取当前实例与当前声明，不持旧指针。
      layer = f.layers.layer( QStringLiteral( "factor.T1.welldist" ) );
      QVERIFY2( layer != nullptr, "factor layer must stay instantiated after regeneration" );
      QCOMPARE( layer->customProperty( QStringLiteral( "paleoAssetId" ) ).toString(),
                assetId );
      const LayerDeclaration *d2 = findDecl( f.layers, QStringLiteral( "factor.T1.welldist" ) );
      QVERIFY2( d2 != nullptr, "declaration must survive regeneration" );
      QVERIFY2( QFile::exists( d2->source ), qPrintable( d2->source ) );

      // --- 跳 4：版本边界（saveVersion）--------------------------------------
      MapVersionStore versions( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) );
      QVERIFY2( versions.open( &err ), qPrintable( err ) );
      MapVersionController controller( &versions, &f.layers );
      controller.setProjectStore( &f.store );
      QVariantMap provenance;
      provenance.insert( QStringLiteral( "steps" ),
                         QStringLiteral( "wells->welldist-factor" ) );
      const MapVersion v = controller.saveVersion( QStringLiteral( "T1" ), provenance, &err );
      QVERIFY2( v.version >= 1, qPrintable( err ) );

      // --- 跳 5：重开（全新实例）后 lineage 仍完整 ---------------------------
      LayerManifest reopenedManifest( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) );
      QVERIFY2( reopenedManifest.open( &err ), qPrintable( err ) );
      bool declIntact = false;
      for ( const LayerDeclaration &rd : reopenedManifest.all() )
      {
        if ( rd.layerId != QStringLiteral( "factor.T1.welldist" ) )
          continue;
        declIntact = rd.horizon == QStringLiteral( "T1" ) &&
                     rd.type == QStringLiteral( "raster" ) &&
                     rd.group == QStringLiteral( "04_SingleFactor" ) &&
                     rd.source == d2->source && rd.styleRef == d2->styleRef;
      }
      QVERIFY2( declIntact, "reopened manifest must keep the factor declaration intact" );
      delete d;
      delete d2;

      DataCatalog reopenedCatalog;
      QVERIFY2( reopenedCatalog.open( f.dir.path() ), "reopen catalog over the project dir" );
      bool lineageIntact = false;
      for ( const CatalogAsset &a : reopenedCatalog.assets() )
      {
        if ( a.id != assetId )
          continue;
        for ( const CatalogVersion &v : reopenedCatalog.versionsForAsset( a.id ) )
        {
          if ( v.id != derivedVersion.id )
            continue;
          lineageIntact = v.sha256 == derivedVersion.sha256 &&
                          v.stage == QStringLiteral( "DERIVED" ) &&
                          v.extra.value( QStringLiteral( "manifest_layer_id" ) ).toString() ==
                              QStringLiteral( "factor.T1.welldist" );
        }
      }
      QVERIFY2( lineageIntact, "reopened catalog must keep asset/version/sha/reverse-link intact" );

      MapVersionStore reopenedVersions( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) );
      QVERIFY2( reopenedVersions.open( &err ), qPrintable( err ) );
      QVERIFY( reopenedVersions.currentVersion( QStringLiteral( "T1" ) ) >= 1 );
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
