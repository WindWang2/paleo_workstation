#pragma once
#include <QDir>
#include <QFile>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <cmath>

#include <gdal.h>
#include <cpl_conv.h>

#include <qgsapplication.h>
#include <qgsexception.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsnativealgorithms.h>
#include <qgspointxy.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingregistry.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>

#include "../src/algorithms/paleoalgorithms.h"

// ---------------------------------------------------------------------------
// AlgorithmTestBase — 自建 C++ 算法测试 harness（wave3/model-hardening；
// TODOS P1「算法测试框架自建」）。
//
// QGIS 官方算法测试基座（Python 的 AlgorithmsTestBase + YAML 期望文件）在
// C++-only 约束下不可用；这个头文件给所有 tests/ 下的算法用例提供同一套
// 薄原语，覆盖官方基座的三个核心能力：
//
//   1. 调算法        run("paleo:paleo_constraint_idw", params, &log)
//                    —— 参数是 QVariantMap（QgsMapLayer 用
//                    QVariant::fromValue(layer) 塞入，输出用文件路径字符串）；
//   2. 栅格比较      compareRasters(a, b, tol, &diff)
//                    —— 尺寸一致 + 逐像元 |a-b|<=tol（双方 nodata 视为相等）；
//   3. 矢量比较      compareVectors(a, b, "facies_polygons", xyTol, &msg)
//                    —— 要素数一致 + 按序几何近似（顶点数一致、逐顶点
//                       xy 距离 <= xyTol）。
//
// 用法（QTest 类里组合而非继承——QTest 的槽必须在自己声明的类里）：
//
//   class TestMyAlgo : public QObject
//   {
//     Q_OBJECT
//     QTemporaryDir mDir;
//     AlgorithmTestBase mHarness{ mDir.path() };   // 组合
//   private slots:
//     void myCase()
//     {
//       AlgorithmTestBase::ensurePaleoProvider();
//       const QString in = mHarness.makeRaster("in.tif", 3, 3, px);
//       auto *layer = new QgsRasterLayer(in, "in", "gdal");
//       QVariantMap params;
//       params.insert("INPUT", QVariant::fromValue(static_cast<QgsMapLayer*>(layer)));
//       params.insert("OUTPUT", mDir.filePath("out.tif"));
//       QString log;
//       const QVariantMap res = AlgorithmTestBase::run(
//           "paleo:paleo_isopach", params, &log);
//       QVERIFY2(!res.isEmpty(), qPrintable(log));
//       QVERIFY(AlgorithmTestBase::compareRasters(
//           mDir.filePath("out.tif"), mDir.filePath("want.tif"), 1e-4));
//       delete layer;
//     }
//   };
//
// main() 需要照 tst_algorithm_harness.cpp 的样子先 QgsApplication::initQgis()
// （offscreen 下 Processing registry 才可用）。
// 现有用户：tst_algorithm_harness.cpp（≥3 算法回归）。欢迎新用例复用。
// ---------------------------------------------------------------------------
class AlgorithmTestBase
{
  public:
    explicit AlgorithmTestBase( const QString &scratchDir )
      : mDir( scratchDir )
    {
      if ( mDir.isEmpty() )
        mDir = QDir::tempPath();
    }

    // ---- provider ----

    // 幂等注册 Paleo provider（"paleo"）。
    static void ensurePaleoProvider()
    {
      if ( !QgsApplication::processingRegistry()->providerById( QStringLiteral( "paleo" ) ) )
        QgsApplication::processingRegistry()->addProvider( new PaleoProvider() );
    }

    // 幂等注册 QGIS 原生 C++ provider（"native"，QgsNativeAlgorithms——
    // qgis_analysis 库，无需 Python）。QGIS 4 的 initQgis() 不会自动注册
    // 任何 processing provider；要用 native:* 算法（距井距离/hub、
    // zonal statistics 等）必须先调这个（docs/ALGORITHM_AUDIT.md §1）。
    static void ensureNativeAlgorithms()
    {
      if ( !QgsApplication::processingRegistry()->providerById( QStringLiteral( "native" ) ) )
        QgsApplication::processingRegistry()->addProvider( new QgsNativeAlgorithms() );
    }

    // ---- run ----

    // 按 id 调算法（如 "paleo:paleo_isopach"）。算法抛出的 QgsProcessingException
    // 被捕获并把文本写进 *log（结果回空 map）——测试用 QVERIFY2(!res.isEmpty(),
    // qPrintable(log)) 断言失败面。算法不存在 → log 说明并回空。
    static QVariantMap run( const QString &algorithmId, const QVariantMap &params,
                            QString *log = nullptr )
    {
      ensurePaleoProvider();
      const QgsProcessingAlgorithm *alg =
          QgsApplication::processingRegistry()->algorithmById( algorithmId );
      if ( !alg )
      {
        if ( log )
          *log = QStringLiteral( "algorithm not found: %1" ).arg( algorithmId );
        return {};
      }
      QgsProcessingContext ctx;
      QgsProcessingFeedback fb;
      bool ok = false;
      QVariantMap res;
      try
      {
        // catchExceptions=false：失败以 QgsProcessingException 抛出——比
        // 静默空 map 好断言；run() 内部自带 prepare/runPrepared/postProcess。
        res = alg->run( params, ctx, &fb, &ok, QVariantMap(), false );
      }
      catch ( const QgsException &e )
      {
        if ( log )
          *log = QStringLiteral( "%1 failed: %2" ).arg( algorithmId, e.what() );
        return {};
      }
      catch ( const std::exception &e )
      {
        if ( log )
          *log = QStringLiteral( "%1 failed: %2" )
                     .arg( algorithmId, QString::fromUtf8( e.what() ) );
        return {};
      }
      catch ( ... )
      {
        if ( log )
          *log = QStringLiteral( "%1 failed with an unknown exception" ).arg( algorithmId );
        return {};
      }
      if ( !ok )
      {
        if ( log )
          *log = QStringLiteral( "%1 failed without an exception: %2" )
                     .arg( algorithmId,
                           fb.textLog().isEmpty() ? QStringLiteral( "(no feedback text)" )
                                                  : fb.textLog() );
        return {};
      }
      return res;
    }

    // ---- raster IO ----

    // 写 w x h Float32 GTiff（网格 gt 固定 0/1/-1 单位格）。失败回空串。
    QString makeRaster( const QString &name, int w, int h, const QVector<float> &px,
                        bool setNodata = false, double nodata = -9999.0 ) const
    {
      const QString path = QDir( mDir ).filePath( name );
      GDALDriverH drv = GDALGetDriverByName( "GTiff" );
      if ( !drv )
        return QString();
      GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr );
      if ( !ds )
        return QString();
      const double gt[6] = { 0.0, 1.0, 0.0, static_cast<double>( h ), 0.0, -1.0 };
      GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
      GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
      if ( setNodata )
        GDALSetRasterNoDataValue( band, nodata );
      const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, w, h,
                                       const_cast<float *>( px.constData() ),
                                       w, h, GDT_Float32, 0, 0 );
      GDALClose( ds );
      return err == CE_None ? path : QString();
    }

    // 读 band 1 为 Float32。失败回 false。
    static bool readRaster( const QString &path, int &w, int &h, QVector<float> &px )
    {
      GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
      if ( !ds )
        return false;
      w = GDALGetRasterXSize( ds );
      h = GDALGetRasterYSize( ds );
      px.resize( static_cast<qsizetype>( w ) * h );
      const CPLErr err = GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Read, 0, 0, w, h,
                                       px.data(), w, h, GDT_Float32, 0, 0 );
      GDALClose( ds );
      return err == CE_None;
    }

    // ---- vector factories（调用方 delete）----

    // 点图层（内存），带一个 double 属性（默认字段名 "z"）。
    static QgsVectorLayer *makePointLayer( const QString &name,
                                           const QList<QPair<QgsPointXY, double>> &points,
                                           const QString &field = QStringLiteral( "z" ) )
    {
      auto *layer = new QgsVectorLayer(
          QStringLiteral( "Point?crs=EPSG:4326&field=%1:double" ).arg( field ),
          name, QStringLiteral( "memory" ) );
      if ( !layer->isValid() )
        return layer;
      QList<QgsFeature> feats;
      for ( const auto &p : points )
      {
        QgsFeature f( layer->fields() );
        f.setGeometry( QgsGeometry::fromPointXY( p.first ) );
        f.setAttribute( field, p.second );
        feats << f;
      }
      layer->dataProvider()->addFeatures( feats );
      layer->updateExtents();
      return layer;
    }

    // 线图层（内存），每条折线一个要素。
    static QgsVectorLayer *makeLineLayer( const QString &name,
                                          const QList<QList<QgsPointXY>> &lines )
    {
      auto *layer = new QgsVectorLayer( QStringLiteral( "LineString?crs=EPSG:4326" ),
                                        name, QStringLiteral( "memory" ) );
      if ( !layer->isValid() )
        return layer;
      QList<QgsFeature> feats;
      for ( const QList<QgsPointXY> &line : lines )
      {
        QgsFeature f( layer->fields() );
        f.setGeometry( QgsGeometry::fromPolylineXY( line ) );
        feats << f;
      }
      layer->dataProvider()->addFeatures( feats );
      layer->updateExtents();
      return layer;
    }

    // ---- comparisons ----

    struct RasterDiff
    {
      int differingCells = 0;
      double maxAbsDiff = 0.0;
      QString message; // 首个不匹配的定位（尺寸不一致时说明原因）
    };

    // 逐像元容差比较：尺寸必须一致；单侧 nodata 记差异；双侧 nodata 相等。
    static bool compareRasters( const QString &pathA, const QString &pathB, double tol,
                                RasterDiff *diff = nullptr )
    {
      int wa = 0, ha = 0, wb = 0, hb = 0;
      QVector<float> a, b;
      if ( !readRaster( pathA, wa, ha, a ) )
      {
        if ( diff )
          diff->message = QStringLiteral( "cannot read %1" ).arg( pathA );
        return false;
      }
      if ( !readRaster( pathB, wb, hb, b ) )
      {
        if ( diff )
          diff->message = QStringLiteral( "cannot read %1" ).arg( pathB );
        return false;
      }
      if ( wa != wb || ha != hb )
      {
        if ( diff )
          diff->message = QStringLiteral( "grid mismatch: %1x%2 vs %3x%4" )
                              .arg( wa ).arg( ha ).arg( wb ).arg( hb );
        return false;
      }
      const int nodataSentinel = -9999; // 工程内约定 nodata（makeRaster 默认）
      bool ok = true;
      for ( int i = 0; i < a.size(); ++i )
      {
        const bool aNd = a[i] == static_cast<float>( nodataSentinel );
        const bool bNd = b[i] == static_cast<float>( nodataSentinel );
        if ( aNd && bNd )
          continue;
        const double d = std::fabs( static_cast<double>( a[i] ) - static_cast<double>( b[i] ) );
        if ( d > tol )
        {
          ok = false;
          if ( diff )
          {
            ++diff->differingCells;
            diff->maxAbsDiff = std::max( diff->maxAbsDiff, d );
            if ( diff->message.isEmpty() )
              diff->message = QStringLiteral( "cell %1 (r%2,c%3): %4 vs %5 (d=%6 > %7)" )
                                  .arg( i )
                                  .arg( i / wa )
                                  .arg( i % wa )
                                  .arg( a[i] )
                                  .arg( b[i] )
                                  .arg( d )
                                  .arg( tol );
          }
        }
      }
      return ok;
    }

    // 矢量近似比较：要素数一致 + 按序几何比较（wkb 类型一致、顶点序列数一致、
    // 逐顶点 xy 距离 <= xyTol）。layerName 非空时以 "|layername=" 打开（gpkg
    // 多层输出用；空 = OGR 首层）。结果写 *message。
    static bool compareVectors( const QString &pathA, const QString &pathB,
                                const QString &layerName, double xyTol, QString *message = nullptr )
    {
      auto openLayer = [layerName]( const QString &path ) {
        QString uri = path;
        if ( !path.contains( QLatin1String( "|layername=" ) ) && !layerName.isEmpty() )
          uri += QStringLiteral( "|layername=" ) + layerName;
        return QgsVectorLayer( uri, QStringLiteral( "cmp" ), QStringLiteral( "ogr" ) );
      };
      const QgsVectorLayer va = openLayer( pathA );
      const QgsVectorLayer vb = openLayer( pathB );
      if ( !va.isValid() )
      {
        if ( message )
          *message = QStringLiteral( "cannot open %1" ).arg( pathA );
        return false;
      }
      if ( !vb.isValid() )
      {
        if ( message )
          *message = QStringLiteral( "cannot open %1" ).arg( pathB );
        return false;
      }
      const long ca = va.featureCount();
      const long cb = vb.featureCount();
      if ( ca != cb )
      {
        if ( message )
          *message = QStringLiteral( "feature count %1 vs %2" ).arg( ca ).arg( cb );
        return false;
      }

      auto verticesOf = []( const QgsGeometry &g ) {
        QVector<QgsPointXY> out;
        const QgsAbstractGeometry *geom = g.constGet();
        if ( !geom )
          return out;
        // coordinateSequence(): [part][ring][point] —— 展平成顶点序列，
        // 多部件/带洞几何也能逐点比对。
        const QgsCoordinateSequence seq = geom->coordinateSequence();
        for ( const QVector<QgsPointSequence> &part : seq )
          for ( const QgsPointSequence &ring : part )
            for ( const QgsPoint &p : ring )
              out.append( QgsPointXY( p.x(), p.y() ) );
        return out;
      };

      QgsFeature fa, fb;
      QgsFeatureIterator ia = va.getFeatures();
      QgsFeatureIterator ib = vb.getFeatures();
      int index = 0;
      while ( ia.nextFeature( fa ) && ib.nextFeature( fb ) )
      {
        if ( fa.geometry().wkbType() != fb.geometry().wkbType() )
        {
          if ( message )
            *message = QStringLiteral( "feature %1: wkb %2 vs %3" )
                           .arg( index )
                           .arg( static_cast<int>( fa.geometry().wkbType() ) )
                           .arg( static_cast<int>( fb.geometry().wkbType() ) );
          return false;
        }
        const QVector<QgsPointXY> ptsA = verticesOf( fa.geometry() );
        const QVector<QgsPointXY> ptsB = verticesOf( fb.geometry() );
        if ( ptsA.size() != ptsB.size() )
        {
          if ( message )
            *message = QStringLiteral( "feature %1: vertex count %2 vs %3" )
                           .arg( index ).arg( ptsA.size() ).arg( ptsB.size() );
          return false;
        }
        for ( int v = 0; v < ptsA.size(); ++v )
        {
          if ( ptsA[v].distance( ptsB[v] ) > xyTol )
          {
            if ( message )
              *message = QStringLiteral( "feature %1 vertex %2: (%3,%4) vs (%5,%6)" )
                             .arg( index ).arg( v )
                             .arg( ptsA[v].x() ).arg( ptsA[v].y() )
                             .arg( ptsB[v].x() ).arg( ptsB[v].y() );
            return false;
          }
        }
        ++index;
      }
      return true;
    }

    // 要素数（-1 = 打不开）。
    static long featureCount( const QString &path, const QString &layerName = QString() )
    {
      QString uri = path;
      if ( !path.contains( QLatin1String( "|layername=" ) ) && !layerName.isEmpty() )
        uri += QStringLiteral( "|layername=" ) + layerName;
      QgsVectorLayer vl( uri, QStringLiteral( "count" ), QStringLiteral( "ogr" ) );
      return vl.isValid() ? vl.featureCount() : -1;
    }

  private:
    QString mDir;
};
