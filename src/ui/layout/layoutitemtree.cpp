// 层：视图
#include "layoutitemtree.h"

#include <QHeaderView>
#include <QTreeView>
#include <QVBoxLayout>

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutmodel.h>

PaleoLayoutItemTree::PaleoLayoutItemTree( QWidget *parent )
  : QWidget( parent )
{
  setObjectName( QStringLiteral( "PaleoLayoutItemTree" ) );
  setWindowTitle( tr( "元素树" ) );

  m_view = new QTreeView( this );
  m_view->setObjectName( QStringLiteral( "layoutItemTreeView" ) );
  m_view->setRootIsDecorated( false );
  m_view->setUniformRowHeights( true );
  m_view->setAlternatingRowColors( false );
  m_view->header()->setStretchLastSection( true );
  m_view->header()->setVisible( false ); // 三列窄表：列头不占地方

  auto *root = new QVBoxLayout( this );
  root->setContentsMargins( 0, 0, 0, 0 );
  root->setSpacing( 0 );
  root->addWidget( m_view );

  // selectionChanged 的连接在 attach() 里做——模型未挂时 selectionModel()
  // 为空，构造期连不上（QAbstractItemView 的选择模型随模型建）。
  connect( m_view, &QTreeView::doubleClicked, this,
           [this]( const QModelIndex &index )
           {
             if ( !m_layout || !m_layout->itemsModel() )
               return;
             QgsLayoutItem *item = m_layout->itemsModel()->itemFromIndex( index );
             if ( item )
               emit itemShowOptions( item );
           } );
}

void PaleoLayoutItemTree::attach( QgsLayout *layout )
{
  m_layout = layout;
  QAbstractItemModel *model = layout ? layout->itemsModel() : nullptr;
  if ( m_view->model() == model )
    return; // 同模型重挂：不动（避免清用户的高亮）

  // 换模型前断开 selectionModel 信号（旧模型销毁会带走 selectionModel）。
  if ( m_view->selectionModel() )
    disconnect( m_view->selectionModel(), nullptr, this, nullptr );
  m_view->setModel( model );
  if ( m_view->selectionModel() )
    connect( m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
             [this]( const QItemSelection &, const QItemSelection & )
             {
               if ( m_syncing || !m_layout )
                 return;
               emit itemActivated( currentItem() );
             } );

  // 可见性/锁定两列收窄，项名列吃满余宽。
  m_view->resizeColumnToContents( 0 );
  m_view->resizeColumnToContents( 1 );
}

QgsLayoutItem *PaleoLayoutItemTree::currentItem() const
{
  if ( !m_layout || !m_layout->itemsModel() || !m_view->selectionModel() )
    return nullptr;
  const QModelIndex index = m_view->selectionModel()->currentIndex();
  if ( !index.isValid() )
    return nullptr;
  return m_layout->itemsModel()->itemFromIndex( index );
}

void PaleoLayoutItemTree::highlightItem( QgsLayoutItem *item )
{
  if ( !m_layout || !m_layout->itemsModel() || !m_view->selectionModel() )
    return;
  const QSignalBlocker block( m_view->selectionModel() );
  m_syncing = true;
  if ( !item )
  {
    m_view->selectionModel()->clearSelection();
    m_view->selectionModel()->setCurrentIndex( QModelIndex(), QItemSelectionModel::NoUpdate );
  }
  else
  {
    const QModelIndex index = m_layout->itemsModel()->indexForItem( item );
    if ( index.isValid() )
    {
      // select(...|Current) 不改 currentIndex——树面板读的是 currentIndex，
      // 必须显式 setCurrentIndex（否则 currentItem() 停在旧行/无效行）。
      m_view->selectionModel()->select( index, QItemSelectionModel::ClearAndSelect );
      m_view->selectionModel()->setCurrentIndex( index, QItemSelectionModel::NoUpdate );
      m_view->scrollTo( index );
    }
    else
    {
      m_view->selectionModel()->clearSelection();
      m_view->selectionModel()->setCurrentIndex( QModelIndex(), QItemSelectionModel::NoUpdate );
    }
  }
  m_syncing = false;
}
