// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>

#include "../metadata/layermanifest.h"

class QgsLayerTree;
class QgsLayerTreeGroup;
class QgsLayerTreeLayer;
class QgsMapLayer;
class QgsProject;
class QgisLayerService;
class QgisProjectService;

// 图层树布局器（图层管理器重设计）：声明驱动的树位摆放，与图层树模型无关。
//
// 用户契约（goal/layer-manager）：
//   · 测区范围与测井数据单独成层、置顶共享（树最上 = 最后画 = 压在所有
//     编图内容之上——井位/边界是基准参考叠加层）；
//   · 地层（层位）构成组——同一层位的全部产层收进以该层位命名的组节点，
//     组内按 canonical 组序自上而下（07_Validation 最上 → 00_Data 最下，
//     后者承接 horizon.<H> 层位底图栅格）；
//   · 共享的非数据产层（horizon="" 且组非 00_Data）平铺在树根的层位组
//     之下，仍按 canonical 序排；
//   · 无声明的图层/组（用户手工加层、参考窗、自建组）对摆放透明——
//     不被移动也不被挤位；摆放只管辖有 manifest 声明的层。
//
// 层位组节点的自定义属性 paleoHorizon=<层位名> 是布局器与组的绑定凭据
//（随 .qgs/.qgz 持久化）——用户改组名不丢绑定，占位组（层位已声明、
// 图层未实例化，§37 懒加载）也靠它在树里留名。
//
// 摆放时机：QgsProject::legendLayersAdded（节点已落树）→ 归位；
// projectOpened（工程读完、manifest 已重绑）→ reorganize() 规整存量
// 平铺树并补齐层位占位组；layersRemoved → 剪掉「无声明且无子节点」的
// 空层位组（有声明的空组是占位节点，保留）。
//
// 归位用 clone+insert+remove（与 LayerTreePanel::moveCurrentNode 同套
// QGIS 内部搬移语义）——勾选态/自定义属性/图层引用全保留，不会因重排
// 丢显示信息。
class QgisLayerOrganizer : public QObject
{
    Q_OBJECT
  public:
    // projectSvc 可为空（测试裸挂 QgsProject::instance()）；layerSvc 是
    // 声明回查唯一来源，为空时本类只保证「什么都不动」。
    QgisLayerOrganizer( QgisProjectService *projectSvc, QgisLayerService *layerSvc,
                        QObject *parent = nullptr );

    // 全量规整：按声明把所有受管节点归位 + 补齐层位占位组 + 剪空组。
    // 幂等——工程打开后、声明批量回灌后、测试里都可直调。
    void reorganize();

    // canonical 组 → 自上而下的秩（07_Validation 最上 … 00_Data 最下，
    // 未知组垫底）；同根组的子路径（04_SingleFactor/Contours）排在该根组
    // 平级成员之前——等值线/制图产物压在因素栅格之上。
    static int groupRank( const QString &group );

    // 树根区段：SharedData（置顶 00_Data）/ HorizonGroup / SharedFlat
    //（树底层位组之下）/ Unmanaged（无声明，不参与摆放）。
    enum class Zone { SharedData, HorizonGroup, SharedFlat, Unmanaged };
    static Zone zoneFor( const QString &horizon, const QString &group );

    // 自定义绘制顺序生效时（「显示预测点位」会启用它），新落树的图层会被
    // QGIS 追加到序列表尾 = 画在最底。按树序给它找位：插到树序中位于它之下
    // 的首个已入序图层之前——相面（05_PaleoMap）压在同层位的相栅格
    //（02_Prediction）之上、仍在置顶共享数据之下。未启用自定义序时不动。
    // 返回是否改了序。
    static bool slotIntoCustomOrder( QgsLayerTree *root, QgsMapLayer *layer );

  private:
    // decls 非空时复用调用方已取好的声明表（reorganize 批量归位一次取
    // 表）；为空时现场 declared()——单事件路径语义与旧实现一致。
    void placeLayer( QgsMapLayer *layer, const QVector<LayerDeclaration> *decls = nullptr );
    // 同 placeLayer 的取表约定：decls 非空复用调用方快照（reorganize 一次
    // 取表贯穿三步）；为空现场 declared()（单信号路径）。
    void ensureHorizonGroups( const QVector<LayerDeclaration> *decls = nullptr );
    void pruneVacantGroups( const QVector<LayerDeclaration> *decls = nullptr );
    // 找到或创建层位组；返回带 paleoHorizon 属性的组节点（树根直挂）。
    QgsLayerTreeGroup *horizonGroup( const QString &horizon,
                                     const QVector<LayerDeclaration> &decls );
    // 目标父组 + 目标插入下标；parent=nullptr 表示不动。self 从扫描中
    // 排除——返回下标按「self 已摘除」坐标计（同父重排不自我引用）。
    void desiredSlot( const LayerDeclaration &decl, const QgsLayerTreeLayer *self,
                      const QVector<LayerDeclaration> &decls,
                      QgsLayerTreeGroup **parent, int *index );

    QPointer<QgisProjectService> m_projectSvc;
    QPointer<QgisLayerService> m_layerSvc;
};
