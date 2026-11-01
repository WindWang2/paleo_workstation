// 层：视图
#include "undogrouprouter.h"
#include <QApplication>
#include <QUndoGroup>
#include <QUndoStack>
#include <QWidget>

PaleoUndoGroupRouter::PaleoUndoGroupRouter( QObject *parent )
  : QObject( parent ), mGroup( new QUndoGroup( this ) )
{
  if ( qApp )
    connect( qApp, &QApplication::focusChanged, this,
             [this]( QWidget *, QWidget *now ) { handleFocusChanged( now ); } );
}

PaleoUndoGroupRouter::~PaleoUndoGroupRouter()
{
  // 组析构不删栈（栈归各自 owner），但须先摘除，避免栈析构时回访已死的组。
  const auto stacks = mGroup->stacks();
  for ( QUndoStack *s : stacks )
    mGroup->removeStack( s );
}

void PaleoUndoGroupRouter::registerScope( QWidget *scope, QUndoStack *stack )
{
  if ( !scope )
    return;
  const bool known = mScopes.contains( scope );
  QUndoStack *previous = known ? mScopes.value( scope ).data() : nullptr;
  mScopes.insert( scope, stack );
  if ( stack && !mGroup->stacks().contains( stack ) )
    mGroup->addStack( stack );
  if ( previous && previous != stack )
    detachStackIfUnused( previous );
  if ( !known )
    connect( scope, &QObject::destroyed, this, [this, scope] { unregisterScope( scope ); } );
  if ( mActiveScope == scope )
    mGroup->setActiveStack( stack );
}

void PaleoUndoGroupRouter::unregisterScope( QWidget *scope )
{
  const auto it = mScopes.find( scope );
  if ( it == mScopes.end() )
    return;
  QUndoStack *stack = it.value().data();
  mScopes.erase( it );
  if ( mActiveScope == scope || mActiveScope.isNull() )
  {
    mActiveScope = nullptr;
    mGroup->setActiveStack( nullptr );
  }
  if ( stack )
    detachStackIfUnused( stack );
}

QUndoStack *PaleoUndoGroupRouter::activeStack() const
{
  return mGroup->activeStack();
}

void PaleoUndoGroupRouter::handleFocusChanged( QWidget *now )
{
  for ( QWidget *w = now; w; w = w->parentWidget() )
  {
    const auto it = mScopes.constFind( w );
    if ( it == mScopes.constEnd() )
      continue;
    mActiveScope = w;
    mGroup->setActiveStack( it.value().data() );
    return;
  }
  // 未登记控件：保持当前活动栈（见头文件口径）。
}

void PaleoUndoGroupRouter::detachStackIfUnused( QUndoStack *stack )
{
  for ( const auto &s : std::as_const( mScopes ) )
    if ( s.data() == stack )
      return;
  if ( mGroup->activeStack() == stack )
    mGroup->setActiveStack( nullptr );
  mGroup->removeStack( stack );
}
