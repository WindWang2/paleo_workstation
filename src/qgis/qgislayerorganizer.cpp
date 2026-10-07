// 层：QGIS 封装
#include "qgislayerorganizer.h"

#include "layervocabulary.h"
#include "qgislayerservice.h"
#include "qgisprojectservice.h"
#include "../domain/mappinghorizons.h"

#include <QHash>
#include <QSet>

#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgslayertreelayer.h>
#include <qgslayertreenode.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>

namespace
{
  // 层位组绑定凭据（头文件注释）；用户改组名后仍能认回。
  const QString kHorizonProp = QStringLiteral( "paleoHorizon" );

  QgsProject *organizerProject( QgisProjectService *svc )
  {
    if ( svc )
      return svc->project();
    return QgsProject::instance();
  }

  QString paleoIdOf( const QgsMapLayer *layer )
  {
    return layer ? layer->customProperty( QStringLiteral( "paleoLayerId" ) ).toString()
                 : QString();
  }

  bool isHorizonGroup( QgsLayerTreeNode *node )
  {
    return node && QgsLayerTree::isGroup( node )
           && node->customProperty( kHorizonProp ).isValid();
  }

  QString groupHorizon( QgsLayerTreeNode *node )
  {
    return isHorizonGroup( node ) ? node->customProperty( kHorizonProp ).toString()
                                  : QString();
  }

  // 层位组序：编图层位名单序（浅→深，AreaRules 序即用户心智序）；名单外
  // 层位按名排尾。mappingHorizons() 无工程绑定时返回内置默认——可测。
  int horizonOrder( const QString &horizon )
  {
    const QStringList horizons = mappingHorizons();
    const int idx = horizons.indexOf( horizon );
    return idx >= 0 ? idx : horizons.size();
  }

  // 搬移：clone+remove+insert 保勾选态/自定义属性/图层引用（与
  // LayerTreePanel::moveCurrentNode 同一 QGIS 内部搬移语义）。
  // index 是「原节点已摘除」坐标下的目标位——同父重排先摘再插，
  // 跨父先插后摘（下标在目标组自身坐标系内，天然无自引用偏移）。
  void moveNode( QgsLayerTreeGroup *from, QgsLayerTreeNode *node,
                 QgsLayerTreeGroup *to, int index )
  {
    if ( !from || !node || !to )
      return;
    QgsLayerTreeNode *placed = node->clone();
    if ( from == to )
    {
      from->removeChildNode( node );
      to->insertChildNode( index, placed );
    }
    else
    {
      to->insertChildNode( index, placed );
      from->removeChildNode( node );
    }
  }
} // namespace

QgisLayerOrganizer::QgisLayerOrganizer( QgisProjectService *projectSvc,
                                        QgisLayerService *layerSvc, QObject *parent )
  : QObject( parent )
  , m_projectSvc( projectSvc )
  , m_layerSvc( layerSvc )
{
  QgsProject *proj = organizerProject( m_projectSvc );
  if ( !proj )
    return;

  // 新节点先落树根顶部（addMapLayer 默认），归位在同一调用栈内完成——
  // 画布桥/树模型只见终态，无闪烁。拖拽（InternalMove=mimeData 反序列化
  // 克隆+删源）不触发 legendLayersAdded——用户手动摆位不被接管。
  connect( proj, &QgsProject::legendLayersAdded, this,
           [this]( const QList<QgsMapLayer *> &layers ) {
             for ( QgsMapLayer *layer : layers )
               placeLayer( layer );
           } );
  // 声明落单：层位声明先建占位组（§37 懒加载——未实例化层位在树里有名）。
  if ( m_layerSvc )
  {
    connect( m_layerSvc, &QgisLayerService::layerDeclared, this,
             [this]( const QString & ) { ensureHorizonGroups(); } );
    // 兜底：instantiate 尾信号再归位一次（任何在 addMapLayer 后才盖章
    // paleoLayerId 的旧路径也能被收编；幂等，已在位即零开销）。
    connect( m_layerSvc, &QgisLayerService::layerInstantiated, this,
             [this]( const QString &layerId ) {
               if ( QgsMapLayer *layer = m_layerSvc->layer( layerId ) )
                 placeLayer( layer );
             } );
  }
  connect( proj, &QgsProject::layersRemoved, this,
           [this]( const QStringList & ) { pruneVacantGroups(); } );
}

// ---- 秩与分区 ----

int QgisLayerOrganizer::groupRank( const QString &group )
{
  const QString canonical = PaleoLayerVocabulary::canonicalize( group );
  const QString root = PaleoLayerVocabulary::groupRoot( canonical );
  // 树内自上而下 = canonical 词表自下而上（01_Base 底 → 07_Validation 顶）。
  static const QStringList topDown = {
      QStringLiteral( "07_Validation" ),    QStringLiteral( "06_Reference" ),
      QStringLiteral( "05_PaleoMap" ),      QStringLiteral( "04_SingleFactor" ),
      PaleoLayerVocabulary::kConstraintsGroup,   QStringLiteral( "02_Prediction" ),
      QStringLiteral( "01_Base" ),          QStringLiteral( "00_Data" ) };
  const int idx = topDown.indexOf( root );
  const int base = idx < 0 ? int( topDown.size() ) * 10 : idx * 10;
  // 子路径（04_SingleFactor/Contours）排在同根组平级成员之前：其产物是
  // 注释/成图层，压在因素栅格与约束线上。
  return canonical.contains( QLatin1Char( '/' ) ) ? base - 1 : base;
}

QgisLayerOrganizer::Zone QgisLayerOrganizer::zoneFor( const QString &horizon,
                                                    const QString &group )
{
  if ( !horizon.isEmpty() )
    return Zone::HorizonGroup;
  if ( PaleoLayerVocabulary::groupRoot( PaleoLayerVocabulary::canonicalize( group ) )
       == QLatin1String( "00_Data" ) )
    return Zone::SharedData;
  return Zone::SharedFlat;
}

// ---- 归位 ----

void QgisLayerOrganizer::placeLayer( QgsMapLayer *layer )
{
  QgsProject *proj = organizerProject( m_projectSvc );
  if ( !proj || !layer || !m_layerSvc )
    return;
  const QString paleoId = paleoIdOf( layer );
  if ( paleoId.isEmpty() )
    return;

  const QVector<LayerDeclaration> decls = m_layerSvc->declared();
  const LayerDeclaration *decl = nullptr;
  for ( const LayerDeclaration &d : decls )
    if ( d.layerId == paleoId )
    {
      decl = &d;
      break;
    }
  if ( !decl )
    return; // 无声明：不接管摆放

  QgsLayerTree *root = proj->layerTreeRoot();
  QgsLayerTreeLayer *node = root ? root->findLayer( layer->id() ) : nullptr;
  if ( !node )
    return;

  QgsLayerTreeGroup *targetParent = nullptr;
  int targetIndex = -1;
  desiredSlot( *decl, node, &targetParent, &targetIndex );
  if ( !targetParent || targetIndex < 0 )
    return;

  auto *currentParent = qobject_cast<QgsLayerTreeGroup *>( node->parent() );
  if ( !currentParent )
    return;
  if ( currentParent == targetParent
       && targetParent->children().indexOf( node ) == targetIndex )
    return; // 已在位——归位幂等

  moveNode( currentParent, node, targetParent, targetIndex );
}

// 受管/非受管分区：Unmanaged（无声明）节点对摆放透明——既不参与区界也
// 不被挤位，用户把手动图层摆在哪它就在哪。
void QgisLayerOrganizer::desiredSlot( const LayerDeclaration &decl,
                                      const QgsLayerTreeLayer *self,
                                      QgsLayerTreeGroup **parent, int *index )
{
  QgsProject *proj = organizerProject( m_projectSvc );
  QgsLayerTree *root = proj ? proj->layerTreeRoot() : nullptr;
  *parent = nullptr;
  *index = -1;
  if ( !root || !m_layerSvc )
    return;

  const QVector<LayerDeclaration> decls = m_layerSvc->declared();
  QHash<QString, LayerDeclaration> declById;
  declById.reserve( decls.size() );
  for ( const LayerDeclaration &d : decls )
    declById.insert( d.layerId, d );
  auto zoneOfNode = [&declById]( QgsLayerTreeNode *n ) -> Zone {
    if ( isHorizonGroup( n ) )
      return Zone::HorizonGroup;
    auto *ln = qobject_cast<QgsLayerTreeLayer *>( n );
    if ( !ln )
      return Zone::Unmanaged;
    const auto it = declById.constFind( paleoIdOf( ln->layer() ) );
    if ( it == declById.constEnd() )
      return Zone::Unmanaged;
    return zoneFor( it.value().horizon, it.value().group );
  };

  const Zone zone = zoneFor( decl.horizon, decl.group );
  QList<QgsLayerTreeNode *> kids = root->children();
  kids.removeAll( const_cast<QgsLayerTreeLayer *>( self ) ); // 自排除：下标按摘除后坐标

  if ( zone == Zone::SharedData )
  {
    *parent = root;
    // 置顶区：插在首个「受管的非置顶」节点之前（层位组/共享平铺都收）。
    // Unmanaged 跳过——手动层摆在受管区带内不挡归位，也不被挤位。
    for ( int i = 0; i < kids.size(); ++i )
    {
      const Zone z = zoneOfNode( kids.at( i ) );
      if ( z == Zone::HorizonGroup || z == Zone::SharedFlat )
      {
        *index = i;
        return;
      }
    }
    *index = kids.size();
    return;
  }

  if ( zone == Zone::SharedFlat )
  {
    *parent = root;
    // 共享平铺区在全部层位组之下：越过 SharedData/HorizonGroup/Unmanaged，
    // 在本区成员上按 groupRank 找首个秩更大者之前插入。
    const int rank = groupRank( decl.group );
    for ( int i = 0; i < kids.size(); ++i )
    {
      QgsLayerTreeNode *n = kids.at( i );
      if ( zoneOfNode( n ) != Zone::SharedFlat )
        continue;
      const auto it = declById.constFind(
          paleoIdOf( qobject_cast<QgsLayerTreeLayer *>( n )->layer() ) );
      if ( it != declById.constEnd() && groupRank( it.value().group ) > rank )
      {
        *index = i;
        return;
      }
    }
    *index = kids.size();
    return;
  }

  // HorizonGroup：目标父组 = 该层位的占位/实组。
  QgsLayerTreeGroup *grp = horizonGroup( decl.horizon );
  if ( !grp )
    return;
  *parent = grp;
  const int rank = groupRank( decl.group );
  QList<QgsLayerTreeNode *> members = grp->children();
  members.removeAll( const_cast<QgsLayerTreeLayer *>( self ) );
  for ( int i = 0; i < members.size(); ++i )
  {
    auto *ln = qobject_cast<QgsLayerTreeLayer *>( members.at( i ) );
    if ( !ln )
      continue; // 组内子组（用户拖入）不参与秩位
    const auto it = declById.constFind( paleoIdOf( ln->layer() ) );
    if ( it == declById.constEnd() )
      continue;
    if ( groupRank( it.value().group ) > rank )
    {
      *index = i;
      return;
    }
  }
  *index = members.size();
}

QgsLayerTreeGroup *QgisLayerOrganizer::horizonGroup( const QString &horizon )
{
  QgsProject *proj = organizerProject( m_projectSvc );
  QgsLayerTree *root = proj ? proj->layerTreeRoot() : nullptr;
  if ( !root || horizon.isEmpty() )
    return nullptr;

  // 绑定凭据是 paleoHorizon 属性（用户改组名不丢）；无属性的同名组不收养
  // ——与用户自建组井水不犯河水，不抢命名。
  const QList<QgsLayerTreeNode *> kids = root->children();
  for ( QgsLayerTreeNode *n : kids )
    if ( groupHorizon( n ) == horizon )
      return qobject_cast<QgsLayerTreeGroup *>( n );

  auto *grp = new QgsLayerTreeGroup( horizon );
  grp->setCustomProperty( kHorizonProp, horizon );

  // 插入位：SharedData 区之后，按层位序插进层位组带；撞上 SharedFlat
  // 受管层即收。Unmanaged 跳过——手动层不被挤位。
  const int order = horizonOrder( horizon );
  QSet<QString> sharedIds;
  if ( m_layerSvc )
    for ( const LayerDeclaration &d : m_layerSvc->declared() )
      if ( zoneFor( d.horizon, d.group ) == Zone::SharedData )
        sharedIds.insert( d.layerId );

  int insertAt = kids.size();
  for ( int i = 0; i < kids.size(); ++i )
  {
    QgsLayerTreeNode *n = kids.at( i );
    if ( isHorizonGroup( n ) )
    {
      const QString other = groupHorizon( n );
      const int otherOrder = horizonOrder( other );
      if ( otherOrder > order
           || ( otherOrder == order && other.localeAwareCompare( horizon ) > 0 ) )
      {
        insertAt = i;
        break;
      }
      continue;
    }
    auto *ln = qobject_cast<QgsLayerTreeLayer *>( n );
    if ( ln && sharedIds.contains( paleoIdOf( ln->layer() ) ) )
      continue;
    if ( ln && m_layerSvc )
    {
      // 有声明但非置顶共享 = SharedFlat 受管层 → 组带不越过它；无声明的
      // Unmanaged 直接跳过（手动层不参与区界）。
      const QString pid = paleoIdOf( ln->layer() );
      bool managed = false;
      if ( !pid.isEmpty() )
        for ( const LayerDeclaration &d : m_layerSvc->declared() )
          if ( d.layerId == pid )
          {
            managed = true;
            break;
          }
      if ( managed )
      {
        insertAt = i;
        break;
      }
      continue;
    }
    // 组节点（无 paleoHorizon 的用户自建组）/ 空层节点：Unmanaged——
    // 透明跳过，不挤位。
    continue;
  }
  root->insertChildNode( insertAt, grp );
  grp->setItemVisibilityChecked( true ); // 新组默认勾选——不遮挡后到的子层
  return grp;
}

// ---- 全量规整 / 剪枝 ----

void QgisLayerOrganizer::reorganize()
{
  QgsProject *proj = organizerProject( m_projectSvc );
  if ( !proj || !proj->layerTreeRoot() || !m_layerSvc )
    return;
  ensureHorizonGroups();
  // 按声明序逐个归位：同秩稳定，收敛于声明序而非散乱现状。
  const QVector<LayerDeclaration> decls = m_layerSvc->declared();
  for ( const LayerDeclaration &d : decls )
    if ( QgsMapLayer *layer = m_layerSvc->layer( d.layerId ) )
      placeLayer( layer );
  pruneVacantGroups();
}

void QgisLayerOrganizer::ensureHorizonGroups()
{
  QgsProject *proj = organizerProject( m_projectSvc );
  if ( !proj || !proj->layerTreeRoot() || !m_layerSvc )
    return;
  for ( const LayerDeclaration &d : m_layerSvc->declared() )
    if ( !d.horizon.isEmpty() )
      horizonGroup( d.horizon ); // find-or-create：已存在的零开销
}

void QgisLayerOrganizer::pruneVacantGroups()
{
  QgsProject *proj = organizerProject( m_projectSvc );
  QgsLayerTree *root = proj ? proj->layerTreeRoot() : nullptr;
  if ( !root || !m_layerSvc )
    return;

  QSet<QString> declaredHorizons;
  for ( const LayerDeclaration &d : m_layerSvc->declared() )
    if ( !d.horizon.isEmpty() )
      declaredHorizons.insert( d.horizon );

  const QList<QgsLayerTreeNode *> kids = root->children();
  for ( QgsLayerTreeNode *n : kids )
  {
    if ( !isHorizonGroup( n ) )
      continue;
    auto *grp = qobject_cast<QgsLayerTreeGroup *>( n );
    // 有声明 → 占位组留名；无声明且无子节点 → 剪掉。
    if ( grp->children().isEmpty()
         && !declaredHorizons.contains( groupHorizon( grp ) ) )
      root->removeChildNode( grp );
  }
}
