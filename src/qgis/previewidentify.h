// 层：QGIS 封装
#pragma once

#include <QHash>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>

class QgsMapLayer;
class QgsSpatialIndex;
class QgsVectorLayer;
class QgsRasterLayer;

// qgis/previewidentify — 预览画布 identify 的查询核心（P2 D2.5/D7.x/D6.4）。
//
// 点选/框选 → 命中要素列表（矢量属性 + 栅格取值）。矢量查询带按层缓存
// 的 QgsSpatialIndex（D6.4：大矢量层二次查询不再全表扫）；栅格点取值给
// 最近邻与双线性插值两档（D7.4）。
struct PreviewIdentifyResult
{
  QgsMapLayer *layer = nullptr;
  QString layerName;

  // 矢量命中
  QgsFeatureId featureId = -1;
  QVariantMap attributes; // 字段名 → 值（已按层字段表翻译）
  QgsGeometry geometry;

  // 栅格命中（isRaster=true 时有效）
  bool isRaster = false;
  int rasterBand = 1;
  double rasterValueNearest = 0.0;
  double rasterValueBilinear = 0.0;
  bool rasterValid = false;
};

class PreviewIdentifyCore : public QObject
{
    Q_OBJECT
  public:
    explicit PreviewIdentifyCore( QObject *parent = nullptr );
    ~PreviewIdentifyCore() override;

    // 点选：容差矩形（局部网格米制）内的矢量要素 + 该点栅格值。
    QVector<PreviewIdentifyResult> identifyPoint( const QList<QgsMapLayer *> &layers,
                                                  const QgsPointXY &point,
                                                  double toleranceMapUnits );
    // 框选：矩形相交要素（每层上限 maxPerLayer，按要素 id 截断）。
    QVector<PreviewIdentifyResult> identifyRect( const QList<QgsMapLayer *> &layers,
                                                 const QgsRectangle &rect,
                                                 int maxPerLayer = 50 );

    // D6.4：按层空间索引缓存（层析构自动清理；dropIndex 供换源后手工失效）。
    QgsSpatialIndex *ensureIndex( QgsVectorLayer *layer );
    void dropIndex( QgsMapLayer *layer );
    int indexCacheSize() const;

    // 栅格点取值（D7.4）：nearest=最近邻；bilinear 出参为双线性插值。
    static bool sampleRaster( QgsRasterLayer *layer, const QgsPointXY &point, int band,
                              double *nearest, double *bilinear );

  private:
    void hookLayerDestruction( QgsVectorLayer *layer );

    QHash<QgsVectorLayer *, QSharedPointer<QgsSpatialIndex>> m_indexCache;
};
