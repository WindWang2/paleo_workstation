// 层：QGIS 封装
#pragma once
#include <QList>
#include <QObject>
#include <QPointer>

#include <qgsfeatureid.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>

class QgsPointLocator;
class QgsVectorLayer;

// QgisTopologicalIndex —— PaleoVertexTool 的拓扑邻域索引（mapping 主线2）。
//
// 底座 = QgsPointLocator（CORE_EXPORT，libqgis_core）：每参与层一个实例，
// 不带目标 CRS 变换（索引坐标 = 层自身 CRS）；verticesInRect / edgesInRect
// 给出 (fid, dense vertexNr, 层坐标) 候选。共点/共边的精确判定
//（qgsDoubleNear）仍由调用面完成——本类只负责把「全要素线性扫描」换成
// R-tree 邻域候选集。
//
// 失效策略（QgsPointLocator 对编辑会话的内部跟踪不可依赖：提交时临时 fid
// 重排、回滚无逐要素信号）：
//   · 每层独立 dirty 位——geometryChanged / featureAdded / featureDeleted /
//     afterRollBack / editingStarted / editingStopped 任一触发即整层标脏；
//     图层析构摘除条目；
//   · 惰性重建：查询时发现脏 → 销毁旧 locator 整体重建（init 走 relaxed
//     任务线程建 R-tree，随后 waitForIndexingFinished 有界等待——本仓库
//     相图层规模为数百要素，毫秒级；超大层的分帧接缝见
//     docs/progress/mapping.md 遗留节）。
//
// 跨层拓扑（同 CRS 相邻编辑层）：setLayers 登记参与层；查询 includeOthers
// 时遍历全部参与层索引。参与层须与 scope 层同 CRS 且可写——由登记面把关，
// 本类查询前再做一次 CRS 防御跳过，不做坐标事后折算。
class QgisTopologicalIndex : public QObject
{
    Q_OBJECT
  public:
    explicit QgisTopologicalIndex( QObject *parent = nullptr );
    ~QgisTopologicalIndex() override;

    // scope 层 = 主查询层（查询坐标即该层 CRS）；others = 跨层参与层。
    // 重复登记保持既有条目与脏位（只增量增删），可每手势调用。
    void setLayers( QgsVectorLayer *scope, const QList<QgsVectorLayer *> &others );

    struct VertexHit
    {
        QgsVectorLayer *layer = nullptr;
        QgsFeatureId fid = 0;
        int vertexNr = -1; // dense numbering（与 QgsGeometry 顶点号同轴）
        QgsPointXY pos;    // 该层 CRS
    };
    struct EdgeHit
    {
        QgsVectorLayer *layer = nullptr;
        QgsFeatureId fid = 0;
        int vertexNr = -1; // 边首顶点（dense；插点位置 = vertexNr + 1）
        QgsPointXY p1;     // 边两端点，该层 CRS
        QgsPointXY p2;
    };

    // radius（scope 层坐标单位）邻域内的顶点候选；includeOthers 加上参与层。
    QList<VertexHit> verticesNear( const QgsPointXY &pos, double radius, bool includeOthers );
    // rect 范围内的边候选；includeOthers 同上。
    QList<EdgeHit> edgesNear( const QgsRectangle &rect, bool includeOthers );

    // 索引健康面（测试/诊断）：层已建且未标脏。
    bool isLayerIndexed( QgsVectorLayer *layer ) const;
    int indexedLayerCount() const;

  private:
    struct LayerEntry
    {
        QPointer<QgsVectorLayer> layer;
        QgsPointLocator *locator = nullptr; // owned
        bool dirty = true;
    };

    LayerEntry *entryFor( QgsVectorLayer *layer );
    void ensureReady( LayerEntry &e );
    void markDirty( const QPointer<QgsVectorLayer> &layer );
    void watchLayer( QgsVectorLayer *layer );

    QPointer<QgsVectorLayer> m_scope; // 主查询层（查询坐标即其 CRS）
    QList<LayerEntry> m_entries;      // m_scope 恒为首条
};
