#include "helpers/workflowfixture.h"
#include "../src/qgis/layervocabulary.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgscolorramp.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsvectordataprovider.h>
#include <qgsfeatureiterator.h>
#include <qgsmaplayer.h>
#include <qgsmapsettings.h>
#include <qgsmaprendererparalleljob.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgslinesymbol.h>
#include <qgslinesymbollayer.h>
#include <qgsvectorlayerlabeling.h>
#include <qgslabelthinningsettings.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/factorcontour.h"
#include "../src/qgis/factorstylewriter.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisstyleservice.h"
#include "../src/services/jobrunner.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/singlefactordef.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/mapversioncontroller.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/metadata/mapversionstore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <functional>
#include <memory>

// m2(B) 单因素图页 — 真实栈（tst_workflows 模式：临时 manifest+catalog+真实
// QGIS 引导）。覆盖：注册表 processingAlgId 在运行时可用（algorithmIds 核实）、
// generateFactor 声明进 04_SingleFactor（layerId/group/type/styleRef）、
// 重生成幂等、色带 .qml 落盘且可被 QgisStyleService::applyStyle 应用、
// 等值线产出真 LineString vector layer（gdal:contour 不可用 → GDAL C API 降级）。

using paleo::tests::initFixture;
using paleo::tests::writePointsGeoJson;
using paleo::tests::derivedVersionRegistered;

class TestFactorWorkflow : public QObject
{
  Q_OBJECT

  private:
    using Fixture = paleo::tests::WorkflowFixture;

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

    struct PointRow
    {
      double x = 0;
      double y = 0;
      double z = 0;
    };
    struct LineRow
    {
      const char *id = "";
      const char *type = "";
      double x0 = 0;
      double y0 = 0;
      double x1 = 0;
      double y1 = 0;
    };

    static OGRSpatialReferenceH makeSpatialRef( int epsg )
    {
      if ( epsg <= 0 )
        return nullptr;
      OGRSpatialReferenceH srs = OSRNewSpatialReference( nullptr );
      if ( !srs || OSRImportFromEPSG( srs, epsg ) != OGRERR_NONE )
      {
        if ( srs )
          OSRDestroySpatialReference( srs );
        return nullptr;
      }
      OSRSetAxisMappingStrategy( srs, OAMS_TRADITIONAL_GIS_ORDER );
      return srs;
    }

    static bool writePointGpkg( const QString &path, int epsg, const QVector<PointRow> &rows )
    {
      GDALDriverH driver = GDALGetDriverByName( "GPKG" );
      if ( !driver )
        return false;
      GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
      if ( !dataset )
        return false;
      OGRSpatialReferenceH srs = makeSpatialRef( epsg );
      if ( epsg > 0 && !srs )
      {
        GDALClose( dataset );
        return false;
      }
      OGRLayerH layer = GDALDatasetCreateLayer( dataset, "wells", srs, wkbPoint, nullptr );
      if ( !layer )
      {
        if ( srs )
          OSRDestroySpatialReference( srs );
        GDALClose( dataset );
        return false;
      }
      OGRFieldDefnH field = OGR_Fld_Create( "z", OFTReal );
      OGR_L_CreateField( layer, field, true );
      OGR_Fld_Destroy( field );
      for ( const PointRow &row : rows )
      {
        OGRFeatureH feature = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
        OGR_F_SetFieldDouble( feature, 0, row.z );
        OGRGeometryH geometry = OGR_G_CreateGeometry( wkbPoint );
        OGR_G_SetPoint_2D( geometry, 0, row.x, row.y );
        OGR_F_SetGeometry( feature, geometry );
        OGR_G_DestroyGeometry( geometry );
        const OGRErr written = OGR_L_CreateFeature( layer, feature );
        OGR_F_Destroy( feature );
        if ( written != OGRERR_NONE )
        {
          if ( srs )
            OSRDestroySpatialReference( srs );
          GDALClose( dataset );
          return false;
        }
      }
      if ( srs )
        OSRDestroySpatialReference( srs );
      GDALClose( dataset );
      return QFile::exists( path );
    }

    static bool writeLineGpkg( const QString &path, int epsg, const QVector<LineRow> &rows )
    {
      GDALDriverH driver = GDALGetDriverByName( "GPKG" );
      if ( !driver )
        return false;
      GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
      if ( !dataset )
        return false;
      OGRSpatialReferenceH srs = makeSpatialRef( epsg );
      if ( epsg > 0 && !srs )
      {
        GDALClose( dataset );
        return false;
      }
      OGRLayerH layer = GDALDatasetCreateLayer( dataset, "lines", srs, wkbLineString, nullptr );
      if ( !layer )
      {
        if ( srs )
          OSRDestroySpatialReference( srs );
        GDALClose( dataset );
        return false;
      }
      OGRFieldDefnH idField = OGR_Fld_Create( "id", OFTString );
      OGR_L_CreateField( layer, idField, true );
      OGR_Fld_Destroy( idField );
      OGRFieldDefnH typeField = OGR_Fld_Create( "type", OFTString );
      OGR_L_CreateField( layer, typeField, true );
      OGR_Fld_Destroy( typeField );
      for ( const LineRow &row : rows )
      {
        OGRFeatureH feature = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
        OGR_F_SetFieldString( feature, 0, row.id );
        OGR_F_SetFieldString( feature, 1, row.type );
        OGRGeometryH geometry = OGR_G_CreateGeometry( wkbLineString );
        OGR_G_AddPoint_2D( geometry, row.x0, row.y0 );
        OGR_G_AddPoint_2D( geometry, row.x1, row.y1 );
        OGR_F_SetGeometry( feature, geometry );
        OGR_G_DestroyGeometry( geometry );
        const OGRErr written = OGR_L_CreateFeature( layer, feature );
        OGR_F_Destroy( feature );
        if ( written != OGRERR_NONE )
        {
          if ( srs )
            OSRDestroySpatialReference( srs );
          GDALClose( dataset );
          return false;
        }
      }
      if ( srs )
        OSRDestroySpatialReference( srs );
      GDALClose( dataset );
      return QFile::exists( path );
    }

    // 列号即值的 north-up 斜坡。等值线是竖线，用来摆一条不穿原线、却会穿过绕行肩带的停止线。
    static bool writeRampTiff( const QString &path, int epsg, int cols, int rows, double originX,
                               double originY, double cell )
    {
      GDALDriverH driver = GDALGetDriverByName( "GTiff" );
      if ( !driver )
        return false;
      GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
      if ( !dataset )
        return false;
      double geoTransform[6] = { originX, cell, 0.0, originY, 0.0, -cell };
      if ( GDALSetGeoTransform( dataset, geoTransform ) != CE_None )
      {
        GDALClose( dataset );
        return false;
      }
      OGRSpatialReferenceH srs = makeSpatialRef( epsg );
      if ( !srs )
      {
        GDALClose( dataset );
        return false;
      }
      char *wkt = nullptr;
      OSRExportToWkt( srs, &wkt );
      OSRDestroySpatialReference( srs );
      if ( !wkt )
      {
        GDALClose( dataset );
        return false;
      }
      const CPLErr projected = GDALSetProjection( dataset, wkt );
      CPLFree( wkt );
      if ( projected != CE_None )
      {
        GDALClose( dataset );
        return false;
      }
      GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
      GDALSetRasterNoDataValue( band, -9999.0 );
      QVector<float> row( cols );
      for ( int r = 0; r < rows; ++r )
      {
        for ( int c = 0; c < cols; ++c )
          row[c] = static_cast<float>( c );
        if ( GDALRasterIO( band, GF_Write, 0, r, cols, 1, row.data(), cols, 1, GDT_Float32, 0, 0 ) != CE_None )
        {
          GDALClose( dataset );
          return false;
        }
      }
      GDALClose( dataset );
      return QFile::exists( path );
    }

    static QString layerUri( const QString &path, const QString &layerName )
    {
      return path + QStringLiteral( "|layername=" ) + layerName;
    }

    static bool findDerivedExtra( DataCatalog &catalog, const QString &assetType, const QString &absolutePath,
                                  QVariantMap *extra )
    {
      for ( const CatalogAsset &asset : catalog.assets() )
      {
        if ( asset.type != assetType )
          continue;
        for ( const CatalogVersion &version : catalog.versionsForAsset( asset.id ) )
        {
          if ( absolutePath.endsWith( QLatin1Char( '/' ) + version.fileName ) &&
               absolutePath.contains( version.id ) )
          {
            if ( extra )
              *extra = version.extra;
            return true;
          }
        }
      }
      return false;
    }

    static QString resolveProjectPath( const QString &projectDir, const QString &path )
    {
      if ( path.isEmpty() || QDir::isAbsolutePath( path ) )
        return path;
      return QDir( projectDir ).filePath( path );
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

    void explicitWellFactorExtractionAndAllMethods() {
      Fixture f; QVERIFY(initFixture(f)); QString err;
      const QString path = f.dir.filePath(QStringLiteral("factors.geojson"));
      QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(R"({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:3857"}},"features":[
        {"type":"Feature","geometry":{"type":"Point","coordinates":[0,0]},"properties":{"well_id":"A","sand":20,"gross":100,"value":999}},
        {"type":"Feature","geometry":{"type":"Point","coordinates":[3,0]},"properties":{"well_id":"B","sand":0,"gross":100,"value":999}},
        {"type":"Feature","geometry":{"type":"Point","coordinates":[0,3]},"properties":{"well_id":"C","sand":40,"gross":100,"value":999}},
        {"type":"Feature","geometry":{"type":"Point","coordinates":[3,3]},"properties":{"well_id":"D","sand":10,"gross":0,"value":999}},
        {"type":"Feature","geometry":{"type":"Point","coordinates":[1,1]},"properties":{"well_id":"E","sand":null,"gross":100,"value":999}}]})"); file.close();
      QVERIFY2(f.layers.declare(decl("wells.T1", "T1", "vector", path), &err), qPrintable(err));
      ConstraintWorkflow wf(&f.proc, &f.layers); wf.setCatalog(&f.catalog, f.dir.path());
      QVariantMap params{{"factorMode", "ratio"}, {"numeratorField", "sand"}, {"denominatorField", "gross"}, {"field", "value"}, {"cellSize", 1.0}};
      QString points; QVERIFY2(wf.extractWellFactors("T1", "sandratio", params, &err, &points), qPrintable(err));
      QCOMPARE(wf.wellFactorRows().size(), 5);
      QCOMPARE(wf.wellFactorRows()[0].toMap().value("value").toDouble(), 0.2);
      QCOMPARE(wf.wellFactorRows()[1].toMap().value("value").toDouble(), 0.0); // 真零有效
      QVERIFY(wf.wellFactorRows()[3].toMap().value("reason").toString().contains(QStringLiteral("分母为零")));
      QVERIFY(!wf.wellFactorRows()[3].toMap().contains("value")); // 不回落 value=999
      auto *samples = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(points)); QVERIFY(samples);
      QCOMPARE(samples->featureCount(), 3LL);
      {
        MappingWorkbench work(&f.layers, &f.proc, &f.projectSvc, &wf);
        work.bindCatalog(&f.catalog, f.dir.path());
        const auto version = f.catalog.versions().last();
        const QString historical = work.layerForVersion(version.id, &err);
        QVERIFY2(!historical.isEmpty(), qPrintable(err));
        auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(historical, &err));
        QVERIFY2(layer, qPrintable(err)); work.styleLayer(historical);
        QCOMPARE(layer->featureCount(), 3LL);
        QVERIFY(!layer->customProperty("paleo/faciesLabelMode").isValid());
      }
      for (const QString &method : {QStringLiteral("local_direction_idw"), QStringLiteral("local_direction_kriging"), QStringLiteral("surfer_idw")}) {
        params.insert("method", method); ConstraintWorkflow::LocalDirectionJob job;
        QVERIFY2(wf.prepareLocalDirectionJob("T1", "sandratio", params, &job, &err), qPrintable(err));
        QCOMPARE(job.field, QStringLiteral("factor_value"));
        if (method == QLatin1String("surfer_idw")) QCOMPARE(job.engineId, QStringLiteral("paleo:paleo_surfer_idw"));
        QCOMPARE(job.params.value("extraction").toMap().value("factorMode").toString(), QStringLiteral("ratio"));
        QgsVectorLayer snapshot(job.wellUri, "snapshot", "ogr"); QCOMPARE(snapshot.featureCount(), 3LL);
      }
      for (const QString &method : {QStringLiteral("kriging"), QStringLiteral("sgs")}) {
        ConstraintWorkflow::GeostatJob job;
        QVERIFY2(wf.prepareGeostatJob("T1", "sandratio", method, params, &job, &err), qPrintable(err));
        QCOMPARE(job.field, QStringLiteral("factor_value"));
      }
      const QString boundaryPath = f.dir.filePath(QStringLiteral("boundary.geojson"));
      QFile boundary(boundaryPath); QVERIFY(boundary.open(QIODevice::WriteOnly));
      boundary.write(R"({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:3857"}},"features":[{"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[-1,-1],[4,-1],[4,4],[-1,4],[-1,-1]]]},"properties":{}}]})"); boundary.close();
      QVERIFY2(f.layers.declare(decl("boundary.T1", "T1", "vector", boundaryPath), &err), qPrintable(err));
      params.insert("method", "structural_idw"); params.insert("boundaryLayerId", "boundary.T1"); params.insert("gridResolution", 16);
      QVERIFY2(wf.generateFactor("T1", "sandratio", params, &err), qPrintable(err));
      params.remove("method");
      QVERIFY2(wf.generateFactor("T1", "sandratio", params, &err), qPrintable(err));
      params.insert("factorMode", "direct"); params.insert("valueField", "sand");
      QVERIFY2(wf.extractWellFactors("T1", "sandthick", params, &err), qPrintable(err));
      QCOMPARE(wf.wellFactorRows()[0].toMap().value("value").toDouble(), 20.0);
      const auto versions = f.catalog.versions();
      QVERIFY(std::any_of(versions.cbegin(), versions.cend(), [](const CatalogVersion &v) { return v.extra.value("extraction").toMap().value("factorMode") == QStringLiteral("ratio"); }));
    }

    void interpretedSandThicknessAndMissingCoverage() {
      Fixture f; QVERIFY(initFixture(f)); QString err;
      const auto linkFile = [&](const QString &id, const QString &well, const QString &role, const QString &type, const QString &text) {
        const QString path = f.dir.filePath(id + QStringLiteral(".dat")); QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false; file.write(text.toUtf8()); file.close();
        CatalogAsset a; a.id = id; a.type = type; a.displayName = id;
        if (!f.catalog.addAsset(a, &err)) return false;
        CatalogVersion v; v.id = "v-" + id; v.assetId = id; v.path = path; v.managed = false; v.stage = "RAW"; v.versionNumber = 1;
        if (!f.catalog.addVersion(v, &err)) return false;
        EntityAssetLink l; l.entityId = well; l.entityType = "well"; l.assetId = id; l.role = role; l.isPrimary = true;
        return f.catalog.addLink(l, &err);
      };
      for (int i = 1; i <= 4; ++i) {
        const QString id = QStringLiteral("well-%1").arg(i); const QString name = QStringLiteral("A%1").arg(i);
        CatalogEntity e; e.id = id; e.entityType = "well"; e.name = name; e.hasSurface = true;
        e.surfaceX = i * 10; e.surfaceY = i * 5; e.coordinateStatus = "untransformed";
        QVERIFY2(f.catalog.addEntity(e, &err), qPrintable(err));
        QVERIFY2(linkFile("tops-" + id, id, "tops", "well_tops", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n%1 D61 100 0 0 -100 100 -99999\n%1 D62 200 0 0 -200 200 -99999\n").arg(name)), qPrintable(err));
      }
      QVERIFY2(linkFile("litho-1", "well-1", "interpretation", "well_litho_intervals",
          QStringLiteral(R"({"schema":1,"intervals":[{"wellId":"well-1","top":90,"base":130,"litho":"砂岩"},{"wellId":"well-1","top":120,"base":150,"litho":"细砂岩"},{"wellId":"well-1","top":150,"base":210,"litho":"泥岩"}]})")), qPrintable(err));
      QVERIFY2(linkFile("litho-2", "well-2", "interpretation", "well_litho_intervals",
          QStringLiteral(R"({"schema":1,"intervals":[{"wellId":"well-2","top":100,"base":140,"litho":"砂岩"}]})")), qPrintable(err));
      QVERIFY2(linkFile("litho-4", "well-4", "interpretation", "well_litho_intervals",
          QStringLiteral(R"({"schema":1,"intervals":[{"wellId":"well-4","top":100,"base":160,"litho":"砂岩"},{"wellId":"well-4","top":150,"base":200,"litho":"泥岩"}]})")), qPrintable(err));
      const QString wellHeads = f.dir.filePath(QStringLiteral("catalog-wells.geojson"));
      QVERIFY2(f.catalog.writeWellsGeoJson(wellHeads, &err), qPrintable(err));
      QVERIFY2(f.layers.declare(decl("wells.D61", "D61", "vector", wellHeads), &err), qPrintable(err));
      ConstraintWorkflow wf(&f.proc, &f.layers); wf.setCatalog(&f.catalog, f.dir.path());
      const auto fields = wf.wellFactorFields("D61", &err);
      QVERIFY(std::any_of(fields.begin(), fields.end(), [](const QVariant &v) { return v.toMap().value("id") == QStringLiteral("log_sand_thickness_md"); }));
      QVariantMap params{{"factorMode", "ratio"}, {"numeratorField", "log_sand_thickness_md"}, {"denominatorField", "log_layer_thickness_md"}};
      QVERIFY2(wf.extractWellFactors("D61", "sandratio", params, &err), qPrintable(err));
      const auto rows = wf.wellFactorRows(); QCOMPARE(rows.size(), 4);
      QCOMPARE(rows[0].toMap().value("value").toDouble(), 0.5); // 裁剪 + 并集，不重复计厚
      QVERIFY(rows[1].toMap().value("reason").toString().contains(QStringLiteral("覆盖不完整")));
      QVERIFY(rows[2].toMap().value("reason").toString().contains(QStringLiteral("缺少层段解释岩性")));
      QVERIFY(rows[3].toMap().value("reason").toString().contains(QStringLiteral("解释冲突")));
      bool parents = false;
      for (const auto &v : f.catalog.versions()) if (v.extra.value("factor_id") == QStringLiteral("sandratio"))
        parents = v.parentVersionIds.contains("v-tops-well-1") && v.parentVersionIds.contains("v-litho-1");
      QVERIFY(parents);
    }

    void contourStylesForBothSchemas() {
      for (const QString &field : {QStringLiteral("ELEV"), QStringLiteral("level")}) {
        QgsVectorLayer layer("LineString?field=" + field + ":double", "contours", "memory");
        QVERIFY(FactorStyleWriter::applyContours(&layer));
        auto *renderer = dynamic_cast<QgsSingleSymbolRenderer *>(layer.renderer()); QVERIFY(renderer);
        QCOMPARE(renderer->symbol()->symbolLayerCount(), 2);
        QCOMPARE(static_cast<QgsLineSymbolLayer *>(renderer->symbol()->symbolLayer(1))->width(), FactorStyleWriter::contourWidthMm);
        QVERIFY(layer.labelsEnabled()); QVERIFY(layer.labeling());
        const auto settings = layer.labeling()->settings();
        QVERIFY(settings.isExpression); QVERIFY(settings.fieldName.contains(field));
        QCOMPARE(settings.format().font().family(), QStringLiteral("JetBrains Mono"));
        QCOMPARE(settings.format().size(), FactorStyleWriter::contourLabelSizePt);
        QVERIFY(settings.format().buffer().enabled());
        QCOMPARE(settings.repeatDistance, FactorStyleWriter::contourLabelRepeatMm);
        QCOMPARE(settings.thinningSettings().minimumFeatureSize(), FactorStyleWriter::contourMinimumLengthMm);
        const QString qa = qEnvironmentVariable("PALEO_SINGLEFACTOR_QA_DIR");
        if (!qa.isEmpty()) {
          QDir().mkpath(qa); QTemporaryDir dir;
          const QString rasterPath = makeSurfaceRaster(dir.filePath("ramp.tif"), {0,10,20,30,50,60,70,90,100});
          QgsRasterLayer raster(rasterPath, "sandratio", "gdal"); QVERIFY(raster.isValid());
          raster.setCrs(QgsCoordinateReferenceSystem::fromEpsgId(3857)); layer.setCrs(raster.crs());
          QVERIFY(FactorStyleWriter::applyTo(&raster, "sandratio"));
          QgsFeature feature(layer.fields()); feature.setAttribute(field, 50.0);
          feature.setGeometry(QgsGeometry::fromWkt("LineString (-0.8 0.5, 1.5 1.1, 3.8 1.5)"));
          QgsFeatureList features{feature}; QVERIFY(layer.dataProvider()->addFeatures(features));
          for (bool dark : {false, true}) {
            QgsMapSettings map; map.setLayers({&layer, &raster}); map.setDestinationCrs(raster.crs());
            map.setExtent(QgsRectangle(-1, -1, 4, 4)); map.setOutputSize(QSize(640, 640));
            map.setBackgroundColor(dark ? QColor("#1B212A") : QColor("#FFFFFF"));
            QgsMapRendererParallelJob job(map); job.start(); job.waitForFinished();
            QVERIFY(job.renderedImage().save(qa + "/contours-" + field + (dark ? "-dark.png" : "-light.png")));
          }
        }
      }
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
      QVERIFY2( ids.contains( QStringLiteral( "paleo:paleo_local_direction_idw" ) ),
                "paleo:paleo_local_direction_idw must be registered" );
      QVERIFY2( ids.contains( QStringLiteral( "paleo:paleo_cartographic_work" ) ),
                "paleo:paleo_cartographic_work must be registered" );
    }

    // =======================================================================
    // 方向20：三组作业统一异步面（ConstraintWorkflow::startConstraintJob）
    //
    // 上面那些用例钉的是 9 个 prepare*/compute*/publish* 裸接口（契约未动）。
    // 这里钉统一面本身：分派正确性、忙则拒绝、取消后 publish 不执行。
    // =======================================================================

    // 泵事件直到 pred 成立（commit 段经 QueuedConnection 排在 owner 线程）。
    static bool pumpUntil( const std::function<bool()> &pred, int timeoutMs = 120000 )
    {
      QElapsedTimer clock;
      clock.start();
      while ( !pred() )
      {
        if ( clock.elapsed() > timeoutMs )
          return false;
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
        QThread::msleep( 2 );
      }
      return true;
    }

    // 断言 1：统一面把分析等值线组接上框架，行为与直接调 publish* 等价
    void unifiedJobRunsAnalysisContourThroughRunner()
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

      // 构造一份分析等值线 job（prepare 仍按现状在 owner 线程做）。
      ConstraintWorkflow::ConstraintJob job;
      job.kind = ConstraintWorkflow::ConstraintJobKind::AnalysisContour;
      job.payload = ConstraintWorkflow::AnalysisContourJob{};
      QVariantMap spec;
      spec.insert( QStringLiteral( "kind" ), QStringLiteral( "analysis_contour" ) );
      spec.insert( QStringLiteral( "horizon" ), QStringLiteral( "T1" ) );
      spec.insert( QStringLiteral( "factorLayerId" ), QStringLiteral( "factor.T1.sandthick" ) );
      spec.insert( QStringLiteral( "interval" ), 1.0 );
      job.title = QStringLiteral( "等值线 T1" );
      QVERIFY2( wf.prepareConstraintJob( job, spec, &err ), qPrintable( err ) );
      QCOMPARE( job.kind, ConstraintWorkflow::ConstraintJobKind::AnalysisContour );

      QObject dispatcher;
      PaleoTaskService svc( nullptr, &dispatcher );
      paleo::jobs::JobRunner<ConstraintWorkflow::ConstraintJob> runner( &dispatcher );
      runner.setTaskService( &svc );
      QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );

      PaleoTask *task = wf.startConstraintJob( runner, job );
      QVERIFY2( task, "统一面应受理任务" );
      QVERIFY( pumpUntil( [&] { return contours.count() > 0; } ) );

      // 与 contourDeclaresVectorLayer 同口径：声明、图层类型、组全一致
      QCOMPARE( contours.count(), 1 );
      QCOMPARE( contours.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
      QCOMPARE( contours.at( 0 ).at( 2 ).toString(), QStringLiteral( "contours.T1.sandthick" ) );
      const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "contours.T1.sandthick" ) );
      QVERIFY2( d != nullptr, "统一面产出的等值线声明缺失" );
      QCOMPARE( d->type, QStringLiteral( "vector" ) );
      QCOMPARE( d->group, QStringLiteral( "04_SingleFactor/Contours" ) );
      delete d;

      QVERIFY( !runner.busy() );
      svc.shutdown( 3000, false );
    }

    // 断言 2：忙则拒绝（现状 m_factorTask 互斥的等价物）
    void unifiedJobRejectsWhenBusy()
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

      ConstraintWorkflow::ConstraintJob job;
      job.kind = ConstraintWorkflow::ConstraintJobKind::AnalysisContour;
      job.payload = ConstraintWorkflow::AnalysisContourJob{};
      QVariantMap spec;
      spec.insert( QStringLiteral( "kind" ), QStringLiteral( "analysis_contour" ) );
      spec.insert( QStringLiteral( "horizon" ), QStringLiteral( "T1" ) );
      spec.insert( QStringLiteral( "factorLayerId" ), QStringLiteral( "factor.T1.sandthick" ) );
      spec.insert( QStringLiteral( "interval" ), 1.0 );
      job.title = QStringLiteral( "等值线 T1" );
      QVERIFY2( wf.prepareConstraintJob( job, spec, &err ), qPrintable( err ) );

      QObject dispatcher;
      PaleoTaskService svc( nullptr, &dispatcher );
      paleo::jobs::JobRunner<ConstraintWorkflow::ConstraintJob> runner( &dispatcher );
      runner.setTaskService( &svc );
      QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );

      PaleoTask *first = wf.startConstraintJob( runner, job );
      QVERIFY( first );
      // 立刻再起一代：忙则必须拒绝
      PaleoTask *second = wf.startConstraintJob( runner, job );
      QVERIFY2( !second, "忙则必须拒绝第二代" );

      QVERIFY( pumpUntil( [&] { return contours.count() > 0; } ) );
      QVERIFY( pumpUntil( [&] { return !runner.busy(); } ) );
      // 拒绝的那一代不产生额外成果
      QCOMPARE( contours.count(), 1 );
      svc.shutdown( 3000, false );
    }

    // 断言 3：取消后 publish 不执行、不发成果信号
    void unifiedJobCancelSkipsPublish()
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

      ConstraintWorkflow::ConstraintJob job;
      job.kind = ConstraintWorkflow::ConstraintJobKind::AnalysisContour;
      job.payload = ConstraintWorkflow::AnalysisContourJob{};
      QVariantMap spec;
      spec.insert( QStringLiteral( "kind" ), QStringLiteral( "analysis_contour" ) );
      spec.insert( QStringLiteral( "horizon" ), QStringLiteral( "T1" ) );
      spec.insert( QStringLiteral( "factorLayerId" ), QStringLiteral( "factor.T1.sandthick" ) );
      spec.insert( QStringLiteral( "interval" ), 1.0 );
      job.title = QStringLiteral( "等值线 T1" );
      QVERIFY2( wf.prepareConstraintJob( job, spec, &err ), qPrintable( err ) );

      QObject dispatcher;
      PaleoTaskService svc( nullptr, &dispatcher );
      paleo::jobs::JobRunner<ConstraintWorkflow::ConstraintJob> runner( &dispatcher );
      runner.setTaskService( &svc );
      QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );

      PaleoTask *task = wf.startConstraintJob( runner, job );
      QVERIFY( task );
      runner.requestCancel();
      QVERIFY( pumpUntil( [&] { return !runner.busy(); } ) );

      // 取消终态；publish 未执行 → 无成果信号、无等值线声明
      QCOMPARE( task->state(), PaleoTask::State::Cancelled );
      QCOMPARE( contours.count(), 0 );
      svc.shutdown( 3000, false );
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

    // 投影井点走本地方向插值，声明分析场；之后的制图工作场单独成层，
    // 不改分析场字节，也不能进入融合。
    void localDirectionPublishesAnalysisAndSeparateWorkField()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      const QString wellsPath = f.dir.filePath( QStringLiteral( "wells.gpkg" ) );
      const QVector<PointRow> wells{
          { 500000.0, 4000000.0, 1.0 },
          { 500400.0, 4000000.0, 8.0 },
          { 500000.0, 4000400.0, 4.0 },
      };
      QVERIFY( writePointGpkg( wellsPath, 3857, wells ) );
      QString err;
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "vector" ), layerUri( wellsPath, QStringLiteral( "wells" ) ) ),
                                  &err ),
                qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QVariantMap params;
      params.insert( QStringLiteral( "method" ), QStringLiteral( "local_direction_idw" ) );
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "cellSize" ), 100.0 );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ),
                qPrintable( err ) );

      const LayerDeclaration *factor = findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY2( factor != nullptr, "local-direction factor declaration missing" );
      QCOMPARE( factor->group, QStringLiteral( "04_SingleFactor" ) );
      QCOMPARE( factor->type, QStringLiteral( "raster" ) );
      const QString analysisPath = factor->source.section( QLatin1Char( '|' ), 0, 0 );
      QVERIFY2( QFile::exists( analysisPath ), qPrintable( analysisPath ) );

      QVariantMap extra;
      QVERIFY2( findDerivedExtra( f.catalog, QStringLiteral( "single_factor_raster" ), analysisPath, &extra ),
                "analysis version missing" );
      QCOMPARE( extra.value( QStringLiteral( "value_source" ) ).toString(), QStringLiteral( "analysis" ) );
      QCOMPARE( extra.value( QStringLiteral( "kind" ) ).toString(), QStringLiteral( "single_factor_raster" ) );
      const QString parameterHash = extra.value( QStringLiteral( "parameter_hash" ) ).toString();
      QVERIFY2( QRegularExpression( QStringLiteral( "^[0-9a-f]{64}$" ) ).match( parameterHash ).hasMatch(),
                qPrintable( parameterHash ) );
      QVERIFY( extra.value( QStringLiteral( "finite_cells" ) ).toInt() > 0 );

      const QString supportPath =
          resolveProjectPath( f.dir.path(), extra.value( QStringLiteral( "support_path" ) ).toString() );
      const QString qcPath = resolveProjectPath( f.dir.path(), extra.value( QStringLiteral( "qc_path" ) ).toString() );
      QVERIFY2( QFile::exists( supportPath ), qPrintable( supportPath ) );
      QVERIFY2( QFile::exists( qcPath ), qPrintable( qcPath ) );
      QCOMPARE( extra.value( QStringLiteral( "support_sha256" ) ).toString().toLower(),
                DataCatalog::sha256FileHex( supportPath ).toLower() );
      GDALDatasetH support = GDALOpen( supportPath.toUtf8().constData(), GA_ReadOnly );
      QVERIFY( support != nullptr );
      int hasNodata = 1;
      GDALGetRasterNoDataValue( GDALGetRasterBand( support, 1 ), &hasNodata );
      QCOMPARE( hasNodata, 0 );
      GDALClose( support );

      const QString linesPath = f.dir.filePath( QStringLiteral( "constraints.gpkg" ) );
      const QVector<LineRow> lines{
          { "stop1", "contour_stop", 499900.0, 4000200.0, 500500.0, 4000200.0 },
          { "hb1", "hard_barrier", 500100.0, 3999900.0, 500100.0, 4000500.0 },
      };
      QVERIFY( writeLineGpkg( linesPath, 3857, lines ) );
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "constraints.T1" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "vector" ), layerUri( linesPath, QStringLiteral( "lines" ) ),
                                        PaleoLayerVocabulary::kConstraintsGroup ),
                                  &err ),
                qPrintable( err ) );

      const QString analysisSha = DataCatalog::sha256FileHex( analysisPath );
      QVERIFY( !analysisSha.isEmpty() );
      QSignalSpy workSpy( &wf, &ConstraintWorkflow::cartographicWorkGenerated );
      QVERIFY2( wf.generateCartographicWork( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                             QVector<double>{ 2.0, 4.0, 6.0 }, &err ),
                qPrintable( err ) );
      QCOMPARE( DataCatalog::sha256FileHex( analysisPath ), analysisSha );
      QCOMPARE( workSpy.count(), 1 );
      QCOMPARE( workSpy.at( 0 ).at( 2 ).toString(), QStringLiteral( "cartographic.T1.sandthick" ) );

      const LayerDeclaration *work = findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick" ) );
      QVERIFY2( work != nullptr, "cartographic declaration missing" );
      QCOMPARE( work->group, QStringLiteral( "04_SingleFactor/Cartographic" ) );
      QCOMPARE( work->type, QStringLiteral( "raster" ) );
      const QString workPath = work->source.section( QLatin1Char( '|' ), 0, 0 );
      QVERIFY( workPath != analysisPath );
      QVERIFY2( QFile::exists( workPath ), qPrintable( workPath ) );

      const LayerDeclaration *factorAfter = findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY( factorAfter != nullptr );
      QCOMPARE( factorAfter->source.section( QLatin1Char( '|' ), 0, 0 ), analysisPath );
      QCOMPARE( factorAfter->group, QStringLiteral( "04_SingleFactor" ) );

      QVariantMap workExtra;
      QVERIFY2( findDerivedExtra( f.catalog, QStringLiteral( "single_factor_cartographic_work" ), workPath,
                                  &workExtra ),
                "cartographic version missing" );
      QCOMPARE( workExtra.value( QStringLiteral( "value_source" ) ).toString(),
                QStringLiteral( "cartographic_work" ) );
      QCOMPARE( workExtra.value( QStringLiteral( "kind" ) ).toString(),
                QStringLiteral( "single_factor_cartographic_work" ) );
      QCOMPARE( workExtra.value( QStringLiteral( "analysis_sha256" ) ).toString(), analysisSha );
      const QString workQc =
          resolveProjectPath( f.dir.path(), workExtra.value( QStringLiteral( "qc_path" ) ).toString() );
      QFile qcFile( workQc );
      QVERIFY2( qcFile.open( QIODevice::ReadOnly ), qPrintable( workQc ) );
      const QJsonArray ignored = QJsonDocument::fromJson( qcFile.readAll() ).object().value( QStringLiteral( "ignored" ) ).toArray();
      bool sawUnknown = false;
      for ( const QJsonValue &item : ignored )
      {
        const QString text = item.toString();
        if ( text.contains( QStringLiteral( "unknown_type" ) ) && text.contains( QStringLiteral( "hard_barrier" ) ) )
          sawUnknown = true;
      }
      QVERIFY2( sawUnknown, "hard_barrier without params_json must be listed as unknown_type" );

      CompositionWorkflow composition( &f.proc, &f.layers );
      QString fuseErr;
      QVERIFY( !composition.fuseFactors( QStringLiteral( "T1" ),
                                         { QStringLiteral( "cartographic.T1.sandthick" ) }, &fuseErr ) );
      QVERIFY2( fuseErr.contains( QStringLiteral( "解释性制图" ) ), qPrintable( fuseErr ) );

      delete factor;
      delete work;
      delete factorAfter;
    }

    // 方向23：method=surfer_idw 走断层绕行引擎，硬屏障不丢弃跨断层样本。
    void surferIdwUsesFaultPathEngineEndToEnd()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QVERIFY2( f.proc.algorithmIds().contains( QStringLiteral( "paleo:paleo_surfer_idw" ) ),
                "paleo:paleo_surfer_idw must be registered" );
      const QString wellsPath = f.dir.filePath( QStringLiteral( "wells.gpkg" ) );
      const QVector<PointRow> wells{
          { 500000.0, 4000000.0, 1.0 },
          { 500400.0, 4000000.0, 8.0 },
          { 500000.0, 4000400.0, 4.0 },
      };
      QVERIFY( writePointGpkg( wellsPath, 3857, wells ) );
      // 硬屏障（break_line 类型列）竖在两口井之间：surfer 引擎按绕行距离
      // 保留两侧样本，而不是像旧路径那样分仓丢弃。
      const QString linesPath = f.dir.filePath( QStringLiteral( "constraints.gpkg" ) );
      const QVector<LineRow> lines{
          { "fault1", "break_line", 500200.0, 3999800.0, 500200.0, 4000600.0 },
      };
      QVERIFY( writeLineGpkg( linesPath, 3857, lines ) );
      QString err;
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "vector" ), layerUri( wellsPath, QStringLiteral( "wells" ) ) ),
                                  &err ),
                qPrintable( err ) );
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "constraints.T1" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "vector" ), layerUri( linesPath, QStringLiteral( "lines" ) ) ),
                                  &err ),
                qPrintable( err ) );

      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QVariantMap params;
      params.insert( QStringLiteral( "method" ), QStringLiteral( "surfer_idw" ) );
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "cellSize" ), 100.0 );
      QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ),
                qPrintable( err ) );

      const LayerDeclaration *factor = findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY2( factor != nullptr, "surfer factor declaration missing" );
      QCOMPARE( factor->group, QStringLiteral( "04_SingleFactor" ) );
      const QString analysisPath = factor->source.section( QLatin1Char( '|' ), 0, 0 );
      QVERIFY2( QFile::exists( analysisPath ), qPrintable( analysisPath ) );

      QVariantMap extra;
      QVERIFY2( findDerivedExtra( f.catalog, QStringLiteral( "single_factor_raster" ), analysisPath, &extra ),
                "analysis version missing" );
      QCOMPARE( extra.value( QStringLiteral( "algorithm_id" ) ).toString(),
                QStringLiteral( "paleo:paleo_surfer_idw" ) );
      QVERIFY( extra.value( QStringLiteral( "finite_cells" ) ).toInt() > 0 );

      const QString qcPath = resolveProjectPath( f.dir.path(), extra.value( QStringLiteral( "qc_path" ) ).toString() );
      QFile qcFile( qcPath );
      QVERIFY2( qcFile.open( QIODevice::ReadOnly ), qPrintable( qcPath ) );
      const QJsonObject qc = QJsonDocument::fromJson( qcFile.readAll() ).object();
      const QJsonObject parameters = qc.value( QStringLiteral( "parameters" ) ).toObject();
      QCOMPARE( parameters.value( QStringLiteral( "algorithm_id" ) ).toString(),
                QStringLiteral( "paleo:paleo_surfer_idw" ) );
      QCOMPARE( parameters.value( QStringLiteral( "semantic_profile" ) ).toString(),
                QStringLiteral( "paleo_surfer_idw_v1" ) );
      QCOMPARE( parameters.value( QStringLiteral( "hard_barrier_model" ) ).toString(),
                QStringLiteral( "fault_path_metric_v1" ) );
      // 绕行模型不丢仓：约束行进入参数记录且不被记为 unknown_type。
      const QJsonArray ignored = parameters.value( QStringLiteral( "ignored" ) ).toArray();
      for ( const QJsonValue &item : ignored )
        QVERIFY2( !item.toString().contains( QStringLiteral( "fault1" ) ),
                  qPrintable( item.toString() ) );

      delete factor;
    }

    void localDirectionCrsPolicy()
    {
      const QVector<PointRow> geographic{
          { 0.0, 0.0, 1.0 },
          { 4.0, 0.0, 8.0 },
          { 0.0, 4.0, 4.0 },
      };
      const QVector<PointRow> local{
          { 500000.0, 4000000.0, 1.0 },
          { 500400.0, 4000000.0, 8.0 },
          { 500000.0, 4000400.0, 4.0 },
      };

      {
        Fixture f;
        QVERIFY( initFixture( f ) );
        const QString wellsPath = f.dir.filePath( QStringLiteral( "wells4326.gpkg" ) );
        QVERIFY( writePointGpkg( wellsPath, 4326, geographic ) );
        QString err;
        QVERIFY2( f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                          QStringLiteral( "vector" ),
                                          layerUri( wellsPath, QStringLiteral( "wells" ) ) ),
                                    &err ),
                  qPrintable( err ) );
        ConstraintWorkflow wf( &f.proc, &f.layers );
        wf.setCatalog( &f.catalog, f.dir.path() );
        QVariantMap params;
        params.insert( QStringLiteral( "method" ), QStringLiteral( "local_direction_idw" ) );
        params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
        params.insert( QStringLiteral( "cellSize" ), 1.0 );
        params.insert( QStringLiteral( "localGrid" ), true );
        QVERIFY( !wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ) );
        QVERIFY2( err.contains( QStringLiteral( "经纬度必须先投影" ) ), qPrintable( err ) );
        QVERIFY( findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) ) == nullptr );
      }

      {
        Fixture f;
        QVERIFY( initFixture( f ) );
        const QString wellsPath = f.dir.filePath( QStringLiteral( "wells_nosrs.gpkg" ) );
        QVERIFY( writePointGpkg( wellsPath, 0, local ) );
        QString err;
        QVERIFY2( f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                          QStringLiteral( "vector" ),
                                          layerUri( wellsPath, QStringLiteral( "wells" ) ) ),
                                    &err ),
                  qPrintable( err ) );
        ConstraintWorkflow wf( &f.proc, &f.layers );
        wf.setCatalog( &f.catalog, f.dir.path() );
        QVariantMap params;
        params.insert( QStringLiteral( "method" ), QStringLiteral( "local_direction_idw" ) );
        params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
        params.insert( QStringLiteral( "cellSize" ), 100.0 );
        QVERIFY( !wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ) );
        QVERIFY2( err.contains( QStringLiteral( "投影" ) ) || err.contains( QStringLiteral( "局部" ) ),
                  qPrintable( err ) );
        QVERIFY( findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) ) == nullptr );
      }

      {
        Fixture f;
        QVERIFY( initFixture( f ) );
        const QString wellsPath = f.dir.filePath( QStringLiteral( "wells_local.gpkg" ) );
        QVERIFY( writePointGpkg( wellsPath, 0, local ) );
        QString err;
        QVERIFY2( f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                          QStringLiteral( "vector" ),
                                          layerUri( wellsPath, QStringLiteral( "wells" ) ) ),
                                    &err ),
                  qPrintable( err ) );
        ConstraintWorkflow wf( &f.proc, &f.layers );
        wf.setCatalog( &f.catalog, f.dir.path() );
        QVariantMap params;
        params.insert( QStringLiteral( "method" ), QStringLiteral( "local_direction_idw" ) );
        params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
        params.insert( QStringLiteral( "cellSize" ), 100.0 );
        params.insert( QStringLiteral( "localGrid" ), true );
        QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "poro" ), params, &err ),
                  qPrintable( err ) );
        const LayerDeclaration *factor = findDecl( f.layers, QStringLiteral( "factor.T1.poro" ) );
        QVERIFY( factor != nullptr );
        QCOMPARE( factor->group, QStringLiteral( "04_SingleFactor" ) );
        QVariantMap extra;
        QVERIFY( findDerivedExtra( f.catalog, QStringLiteral( "single_factor_raster" ),
                                   factor->source.section( QLatin1Char( '|' ), 0, 0 ), &extra ) );
        QCOMPARE( extra.value( QStringLiteral( "value_source" ) ).toString(), QStringLiteral( "analysis" ) );
        QCOMPARE( extra.value( QStringLiteral( "crs_mode" ) ).toString(), QStringLiteral( "local_engineering" ) );
        delete factor;
      }
    }

    void unknownMethodDoesNotDeclare()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      QString err;
      QVERIFY2( setupWells( f, &err ), qPrintable( err ) );
      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QVariantMap params;
      // 方向18 起 kriging/sgs 是合法方法——未知样本改用不存在的词
      params.insert( QStringLiteral( "method" ), QStringLiteral( "magic_wand" ) );
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      QVERIFY( !wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "未知" ) ), qPrintable( err ) );
      QVERIFY( findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) ) == nullptr );
    }

    static QString analysisVersionId( DataCatalog &catalog, const QString &absolutePath )
    {
      for ( const CatalogAsset &asset : catalog.assets() )
      {
        if ( asset.type != QLatin1String( "single_factor_raster" ) )
          continue;
        for ( const CatalogVersion &version : catalog.versionsForAsset( asset.id ) )
        {
          if ( absolutePath.endsWith( QLatin1Char( '/' ) + version.fileName ) &&
               absolutePath.contains( version.id ) )
            return version.id;
        }
      }
      return QString();
    }

    static bool publishProjectedFactor( Fixture &f, ConstraintWorkflow *wf, QString *error )
    {
      const QString wellsPath = f.dir.filePath( QStringLiteral( "wells.gpkg" ) );
      const QVector<PointRow> wells{
          { 500000.0, 4000000.0, 1.0 },
          { 500400.0, 4000000.0, 8.0 },
          { 500000.0, 4000400.0, 4.0 },
      };
      if ( !writePointGpkg( wellsPath, 3857, wells ) )
        return false;
      if ( !f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "vector" ), layerUri( wellsPath, QStringLiteral( "wells" ) ) ),
                              error ) )
        return false;
      wf->setCatalog( &f.catalog, f.dir.path() );
      QVariantMap params;
      params.insert( QStringLiteral( "method" ), QStringLiteral( "local_direction_idw" ) );
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "cellSize" ), 100.0 );
      return wf->generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, error );
    }

    void fixedLevelsKeepAnalysisIdentity()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      ConstraintWorkflow wf( &f.proc, &f.layers );
      QString err;
      QVERIFY2( publishProjectedFactor( f, &wf, &err ), qPrintable( err ) );
      const LayerDeclaration *factor = findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY( factor != nullptr );
      const QString analysisPath = factor->source.section( QLatin1Char( '|' ), 0, 0 );
      const QString analysisSha = DataCatalog::sha256FileHex( analysisPath );
      const QString versionId = analysisVersionId( f.catalog, analysisPath );
      QVERIFY( !analysisSha.isEmpty() );
      QVERIFY( !versionId.isEmpty() );

      QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );
      QVERIFY2( wf.generateContoursAtLevels( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                             QVector<double>{ 2.0, 4.0, 6.0 }, &err ),
                qPrintable( err ) );
      QCOMPARE( contours.count(), 1 );
      QCOMPARE( contours.at( 0 ).at( 2 ).toString(), QStringLiteral( "contours.T1.sandthick" ) );
      QCOMPARE( DataCatalog::sha256FileHex( analysisPath ), analysisSha );
      QCOMPARE( analysisVersionId( f.catalog, analysisPath ), versionId );

      const LayerDeclaration *lines = findDecl( f.layers, QStringLiteral( "contours.T1.sandthick" ) );
      QVERIFY( lines != nullptr );
      QCOMPARE( lines->group, QStringLiteral( "04_SingleFactor/Contours" ) );
      const QString contourPath = lines->source.section( QLatin1Char( '|' ), 0, 0 );
      QVariantMap extra;
      QVERIFY2( findDerivedExtra( f.catalog, QStringLiteral( "contour_lines" ), contourPath, &extra ),
                "fixed-level contour version missing" );
      QCOMPARE( extra.value( QStringLiteral( "value_source" ) ).toString(), QStringLiteral( "analysis" ) );
      QCOMPARE( extra.value( QStringLiteral( "manifest_layer_id" ) ).toString(),
                QStringLiteral( "contours.T1.sandthick" ) );
      QVERIFY( extra.value( QStringLiteral( "layer_id" ) ).toString().startsWith( QStringLiteral( "product." ) ) );
      QVERIFY( !extra.value( QStringLiteral( "parent_version_ids" ) ).toList().isEmpty() );

      err.clear();
      QVERIFY( !wf.generateContours( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "正数" ) ), qPrintable( err ) );
      delete factor;
      delete lines;
    }

    void interpretiveContoursStayOffTheAnalysisField()
    {
      Fixture crossing;
      QVERIFY( initFixture( crossing ) );
      ConstraintWorkflow crossingWf( &crossing.proc, &crossing.layers );
      QString err;
      QVERIFY2( publishProjectedFactor( crossing, &crossingWf, &err ), qPrintable( err ) );
      const LayerDeclaration *factor = findDecl( crossing.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY( factor != nullptr );
      const QString analysisPath = factor->source.section( QLatin1Char( '|' ), 0, 0 );
      const QString analysisSha = DataCatalog::sha256FileHex( analysisPath );
      const QString versionId = analysisVersionId( crossing.catalog, analysisPath );
      const QString linesPath = crossing.dir.filePath( QStringLiteral( "constraints.gpkg" ) );
      QVERIFY( writeLineGpkg( linesPath, 3857,
                              { { "stop-cross", "contour_stop", 499000.0, 4000200.0, 501000.0, 4000200.0 } } ) );
      QVERIFY2( crossing.layers.declare(
                    decl( QStringLiteral( "constraints.T1" ), QStringLiteral( "T1" ), QStringLiteral( "vector" ),
                          layerUri( linesPath, QStringLiteral( "lines" ) ), PaleoLayerVocabulary::kConstraintsGroup ),
                    &err ),
                qPrintable( err ) );
      QSignalSpy crossSpy( &crossingWf, &ConstraintWorkflow::interpretiveContoursGenerated );
      QVERIFY2( crossingWf.generateInterpretiveContours( QStringLiteral( "T1" ),
                                                         QStringLiteral( "factor.T1.sandthick" ),
                                                         QVector<double>{ 2.0, 4.0, 6.0 }, &err, true ),
                qPrintable( err ) );
      QCOMPARE( crossSpy.count(), 1 );
      QCOMPARE( crossSpy.at( 0 ).at( 2 ).toString(), QStringLiteral( "cartographic.T1.sandthick.contours" ) );
      QCOMPARE( DataCatalog::sha256FileHex( analysisPath ), analysisSha );
      QCOMPARE( analysisVersionId( crossing.catalog, analysisPath ), versionId );

      const LayerDeclaration *crossWork = findDecl( crossing.layers, QStringLiteral( "cartographic.T1.sandthick" ) );
      const LayerDeclaration *crossDrawn =
          findDecl( crossing.layers, QStringLiteral( "cartographic.T1.sandthick.contours" ) );
      QVERIFY( crossWork != nullptr );
      QVERIFY( crossDrawn != nullptr );
      QCOMPARE( crossDrawn->title, QStringLiteral( "解释性等值线·砂体厚度" ) );
      QCOMPARE( crossDrawn->group, QStringLiteral( "04_SingleFactor/Cartographic" ) );
      QVariantMap crossWorkExtra;
      QVERIFY( findDerivedExtra( crossing.catalog, QStringLiteral( "single_factor_cartographic_work" ),
                                 crossWork->source.section( QLatin1Char( '|' ), 0, 0 ), &crossWorkExtra ) );
      QCOMPARE( crossWorkExtra.value( QStringLiteral( "unresolved_crossings" ) ).toInt(), 0 );
      QCOMPARE( crossWorkExtra.value( QStringLiteral( "unchanged" ) ).toBool(), false );
      QVERIFY( crossWorkExtra.value( QStringLiteral( "modified_cells" ) ).toInt() > 0 );
      QCOMPARE( crossWorkExtra.value( QStringLiteral( "analysis_sha256" ) ).toString(), analysisSha );
      QVariantMap crossLineExtra;
      QVERIFY( findDerivedExtra( crossing.catalog, QStringLiteral( "single_factor_cartographic_contour" ),
                                 crossDrawn->source.section( QLatin1Char( '|' ), 0, 0 ), &crossLineExtra ) );
      QCOMPARE( crossLineExtra.value( QStringLiteral( "value_source" ) ).toString(),
                QStringLiteral( "cartographic_work" ) );
      QCOMPARE( crossLineExtra.value( QStringLiteral( "unresolved_crossings" ) ).toInt(), 0 );
      QVERIFY( crossLineExtra.value( QStringLiteral( "parent_version_ids" ) ).toList().size() >= 2 );
      CompositionWorkflow crossingComposition( &crossing.proc, &crossing.layers );
      QString crossFuseErr;
      QVERIFY( !crossingComposition.fuseFactors( QStringLiteral( "T1" ),
                                                 { QStringLiteral( "cartographic.T1.sandthick.contours" ) },
                                                 &crossFuseErr ) );
      QVERIFY2( crossFuseErr.contains( QStringLiteral( "解释性制图" ) ), qPrintable( crossFuseErr ) );
      delete factor;
      delete crossWork;
      delete crossDrawn;

      Fixture open;
      QVERIFY( initFixture( open ) );
      ConstraintWorkflow wf( &open.proc, &open.layers );
      err.clear();
      QVERIFY2( publishProjectedFactor( open, &wf, &err ), qPrintable( err ) );
      const LayerDeclaration *openFactor = findDecl( open.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY( openFactor != nullptr );
      const QString openPath = openFactor->source.section( QLatin1Char( '|' ), 0, 0 );
      const QString openSha = DataCatalog::sha256FileHex( openPath );
      const QString openVersion = analysisVersionId( open.catalog, openPath );
      const QString farPath = open.dir.filePath( QStringLiteral( "constraints.gpkg" ) );
      QVERIFY( writeLineGpkg( farPath, 3857,
                              { { "stop-far", "contour_stop", 0.0, 9000000.0, 10.0, 9000000.0 } } ) );
      QVERIFY2( open.layers.declare(
                    decl( QStringLiteral( "constraints.T1" ), QStringLiteral( "T1" ), QStringLiteral( "vector" ),
                          layerUri( farPath, QStringLiteral( "lines" ) ), PaleoLayerVocabulary::kConstraintsGroup ),
                    &err ),
                qPrintable( err ) );
      QSignalSpy interpretive( &wf, &ConstraintWorkflow::interpretiveContoursGenerated );
      QVERIFY2( wf.generateInterpretiveContours( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                                 QVector<double>{ 2.0, 4.0, 6.0 }, &err, true ),
                qPrintable( err ) );
      QCOMPARE( interpretive.count(), 1 );
      QCOMPARE( interpretive.at( 0 ).at( 2 ).toString(), QStringLiteral( "cartographic.T1.sandthick.contours" ) );
      QCOMPARE( DataCatalog::sha256FileHex( openPath ), openSha );
      QCOMPARE( analysisVersionId( open.catalog, openPath ), openVersion );

      const LayerDeclaration *work = findDecl( open.layers, QStringLiteral( "cartographic.T1.sandthick" ) );
      const LayerDeclaration *drawn = findDecl( open.layers, QStringLiteral( "cartographic.T1.sandthick.contours" ) );
      QVERIFY( work != nullptr );
      QVERIFY( drawn != nullptr );
      QCOMPARE( work->group, QStringLiteral( "04_SingleFactor/Cartographic" ) );
      QCOMPARE( drawn->group, QStringLiteral( "04_SingleFactor/Cartographic" ) );
      QCOMPARE( drawn->title, QStringLiteral( "解释性等值线·砂体厚度" ) );
      const QString workPath = work->source.section( QLatin1Char( '|' ), 0, 0 );
      const QString drawnPath = drawn->source.section( QLatin1Char( '|' ), 0, 0 );
      QVariantMap workExtra;
      QVariantMap lineExtra;
      QVERIFY( findDerivedExtra( open.catalog, QStringLiteral( "single_factor_cartographic_work" ), workPath,
                                 &workExtra ) );
      QVERIFY( findDerivedExtra( open.catalog, QStringLiteral( "single_factor_cartographic_contour" ), drawnPath,
                                 &lineExtra ) );
      QCOMPARE( workExtra.value( QStringLiteral( "unchanged" ) ).toBool(), true );
      QCOMPARE( workExtra.value( QStringLiteral( "value_source" ) ).toString(),
                QStringLiteral( "cartographic_work" ) );
      QCOMPARE( lineExtra.value( QStringLiteral( "value_source" ) ).toString(),
                QStringLiteral( "cartographic_work" ) );
      QCOMPARE( lineExtra.value( QStringLiteral( "kind" ) ).toString(),
                QStringLiteral( "single_factor_cartographic_contour" ) );
      const QVariantList parents = lineExtra.value( QStringLiteral( "parent_version_ids" ) ).toList();
      QVERIFY( parents.size() >= 2 );
      const QString firstHash = workExtra.value( QStringLiteral( "parameter_hash" ) ).toString();
      QVERIFY( firstHash.size() == 64 );

      err.clear();
      QVERIFY( !wf.generateContoursAtLevels( QStringLiteral( "T1" ), QStringLiteral( "cartographic.T1.sandthick" ),
                                             QVector<double>{ 2.0 }, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "解释性制图" ) ), qPrintable( err ) );

      CompositionWorkflow composition( &open.proc, &open.layers );
      QString fuseErr;
      QVERIFY( !composition.fuseFactors( QStringLiteral( "T1" ),
                                         { QStringLiteral( "cartographic.T1.sandthick.contours" ) }, &fuseErr ) );
      QVERIFY2( fuseErr.contains( QStringLiteral( "解释性制图" ) ), qPrintable( fuseErr ) );

      QVERIFY2( wf.generateCartographicWork( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                             QVector<double>{ 3.0, 9.0 }, &err ),
                qPrintable( err ) );
      QCOMPARE( DataCatalog::sha256FileHex( openPath ), openSha );
      QCOMPARE( analysisVersionId( open.catalog, openPath ), openVersion );
      const LayerDeclaration *workAfter = findDecl( open.layers, QStringLiteral( "cartographic.T1.sandthick" ) );
      QVERIFY( workAfter != nullptr );
      QVariantMap afterExtra;
      QVERIFY( findDerivedExtra( open.catalog, QStringLiteral( "single_factor_cartographic_work" ),
                                 workAfter->source.section( QLatin1Char( '|' ), 0, 0 ), &afterExtra ) );
      const QString secondHash = afterExtra.value( QStringLiteral( "parameter_hash" ) ).toString();
      QVERIFY( secondHash.size() == 64 );
      QVERIFY( secondHash != firstHash );
      delete openFactor;
      delete work;
      delete drawn;
      delete workAfter;
    }

    // 横停止线穿过竖等值线并改出肩带；竖停止线与原等值线平行，不参与绕行，
    // 却会切过肩带上新的等值线。严格模式必须拒绝，且不声明制图成果。
    void strictModeRefusesUnresolvedShoulderCrossing()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
      const double originX = 500000.0;
      const double originY = 4001000.0;
      const double cell = 10.0;
      QVERIFY( writeRampTiff( rasterPath, 3857, 80, 40, originX, originY, cell ) );
      const QString analysisSha = DataCatalog::sha256FileHex( rasterPath );
      QString err;
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "factor.T1.sandthick" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "raster" ), rasterPath, QStringLiteral( "04_SingleFactor" ) ),
                                  &err ),
                qPrintable( err ) );
      const double stopY = originY - 20.0 * cell;
      // 斜线段落在 10 与 30 两级原等值线之间，不穿原线；肩带把级别 30 的线弯过来后应切开它。
      const QString linesPath = f.dir.filePath( QStringLiteral( "constraints.gpkg" ) );
      QVERIFY( writeLineGpkg(
          linesPath, 3857,
          { { "stop-across", "contour_stop", originX - cell, stopY, originX + 80.0 * cell + cell, stopY },
            { "stop-shoulder", "contour_stop", originX + 14.0 * cell, stopY + 75.0, originX + 27.0 * cell,
              stopY + 115.0 } } ) );
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "constraints.T1" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "vector" ), layerUri( linesPath, QStringLiteral( "lines" ) ),
                                        PaleoLayerVocabulary::kConstraintsGroup ),
                                  &err ),
                qPrintable( err ) );
      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QVERIFY( !wf.generateInterpretiveContours( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                                 QVector<double>{ 10.0, 30.0, 50.0 }, &err, true ) );
      QVERIFY2( err.contains( QStringLiteral( "未解决穿线" ) ), qPrintable( err ) );
      QVERIFY( findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick" ) ) == nullptr );
      QVERIFY( findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick.contours" ) ) == nullptr );
      QCOMPARE( DataCatalog::sha256FileHex( rasterPath ), analysisSha );
    }

    void staleGenerationDropsLocalDirectionPublish()
    {
      Fixture f;
      QVERIFY( initFixture( f ) );
      const QString wellsPath = f.dir.filePath( QStringLiteral( "wells.gpkg" ) );
      QVERIFY( writePointGpkg( wellsPath, 3857,
                               { { 500000.0, 4000000.0, 1.0 },
                                 { 500400.0, 4000000.0, 8.0 },
                                 { 500000.0, 4000400.0, 4.0 } } ) );
      QString err;
      QVERIFY2( f.layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                        QStringLiteral( "vector" ), layerUri( wellsPath, QStringLiteral( "wells" ) ) ),
                                  &err ),
                qPrintable( err ) );
      ConstraintWorkflow wf( &f.proc, &f.layers );
      wf.setCatalog( &f.catalog, f.dir.path() );
      QVariantMap params;
      params.insert( QStringLiteral( "field" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "cellSize" ), 200.0 );
      ConstraintWorkflow::LocalDirectionJob first;
      ConstraintWorkflow::LocalDirectionJob second;
      QVERIFY2( wf.prepareLocalDirectionJob( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &first,
                                             &err ),
                qPrintable( err ) );
      QVERIFY2( wf.computeLocalDirectionJob( &first ), qPrintable( first.error ) );
      QVERIFY2( wf.prepareLocalDirectionJob( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params, &second,
                                             &err ),
                qPrintable( err ) );
      QVERIFY2( wf.computeLocalDirectionJob( &second ), qPrintable( second.error ) );
      QVERIFY( !wf.publishLocalDirectionJob( first, &err ) );
      QVERIFY2( err.contains( QStringLiteral( "代次" ) ), qPrintable( err ) );
      QVERIFY( findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) ) == nullptr );
      QVERIFY2( wf.publishLocalDirectionJob( second, &err ), qPrintable( err ) );
      const LayerDeclaration *published = findDecl( f.layers, QStringLiteral( "factor.T1.sandthick" ) );
      QVERIFY( published != nullptr );
      delete published;
    }

    void realAreaDoesNotInventWellValues()
    {
      const QByteArray env = qgetenv( "PALEO_REAL_PROJECT_AREA" );
      if ( env.isEmpty() )
        QSKIP( "PALEO_REAL_PROJECT_AREA is unset. This skip is not an O12 pass." );
      const QString wells = QDir( QString::fromLocal8Bit( env ) ).filePath( QStringLiteral( "artifacts/layers/wells.geojson" ) );
      QFile file( wells );
      QVERIFY2( file.open( QIODevice::ReadOnly ), qPrintable( wells ) );
      const QJsonArray features = QJsonDocument::fromJson( file.readAll() ).object().value( QStringLiteral( "features" ) ).toArray();
      QVERIFY( features.size() >= 1 );
      QStringList numeric;
      for ( const QJsonValue &feature : features )
      {
        const QJsonObject properties = feature.toObject().value( QStringLiteral( "properties" ) ).toObject();
        for ( auto it = properties.begin(); it != properties.end(); ++it )
        {
          if ( it.value().isDouble() && !numeric.contains( it.key() ) )
            numeric << it.key();
        }
      }
      if ( numeric.size() < 2 )
      {
        QFAIL( qPrintable( QStringLiteral( "O12 gap: well-point numeric fields are [%1]. "
                                           "Do not invent values or treat this as a pass." )
                               .arg( numeric.join( QLatin1Char( ',' ) ) ) ) );
      }
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
  // ctest 无控制台下 QtTest 结果走 OutputDebugString，失败时看不到是哪条红；
  // 追加 -o 让结果落盘（方向20 迁移调试用）。
  QByteArray logPath = QByteArray( QT_TESTCASE_BUILDDIR ) + "/tst_factorworkflow-result.txt";
  QList<QByteArray> forwarded;
  forwarded << QByteArray( argv[0] );
  for ( int i = 1; i < argc; ++i )
    forwarded << QByteArray( argv[i] );
  forwarded << QByteArray( "-o" ) << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve( forwarded.size() );
  for ( QByteArray &a : forwarded )
    cargv << a.data();
  const int rc = QTest::qExec( &tc, cargv.size(), cargv.data() );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_factorworkflow.moc"
