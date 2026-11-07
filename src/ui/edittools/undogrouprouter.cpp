// 层：视图
#include "undogrouprouter.h"
#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QUndoGroup>
#include <QUndoStack>
#include <QWidget>

PaleoUndoGroupRouter::PaleoUndoGroupRouter( QObject *parent )
  : QObject( parent ), mGroup( new QUndoGroup( this ) )
{
  connect( mGroup, &QUndoGroup::indexChanged, this, &PaleoUndoGroupRouter::targetChanged );
  connect( mGroup, &QUndoGroup::activeStackChanged, this, &PaleoUndoGroupRouter::targetChanged );
  connect( mGroup, &QUndoGroup::canUndoChanged, this, &PaleoUndoGroupRouter::targetChanged );
  connect( mGroup, &QUndoGroup::canRedoChanged, this, &PaleoUndoGroupRouter::targetChanged );
  connect( mGroup, &QUndoGroup::undoTextChanged, this, &PaleoUndoGroupRouter::targetChanged );
  connect( mGroup, &QUndoGroup::redoTextChanged, this, &PaleoUndoGroupRouter::targetChanged );
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

void PaleoUndoGroupRouter::registerScope( QWidget *scope, QUndoStack *stack, PaleoUndoTarget target )
{
  if ( !scope )
    return;
  Scope &entry = mScopes[scope];
  QUndoStack *previous = entry.stack;
  entry.stack = stack;
  entry.target = std::move( target );
  if ( !entry.destroyed )
  {
    entry.destroyed = connect( scope, &QObject::destroyed, this,
                              [this, scope] { unregisterScope( scope ); } );
    scope->installEventFilter( this );
  }
  if ( stack && !mGroup->stacks().contains( stack ) )
    mGroup->addStack( stack );
  if ( previous && previous != stack )
    detachStackIfUnused( previous );
  if ( mActiveScope == scope )
    activate( scope );
  else if ( QWidget *focus = QApplication::focusWidget();
            focus && ( focus == scope || scope->isAncestorOf( focus ) ) )
    handleFocusChanged( focus );
}

void PaleoUndoGroupRouter::unregisterScope( QWidget *scope )
{
  const auto it = mScopes.find( scope );
  if ( it == mScopes.end() )
    return;
  QUndoStack *stack = it->stack;
  disconnect( it->destroyed );
  mScopes.erase( it );
  if ( mActiveScope == scope || mActiveScope.isNull() )
    resetFocus();
  if ( stack )
    detachStackIfUnused( stack );
}

QUndoStack *PaleoUndoGroupRouter::activeStack() const
{
  return mGroup->activeStack();
}

void PaleoUndoGroupRouter::handleFocusChanged( QWidget *now )
{
  if ( !now )
  {
    // Popup menus can temporarily take the keyboard without a focus widget.
    QMetaObject::invokeMethod( this, [this] {
      if ( !QApplication::focusWidget() )
      {
        if ( QWidget *popup = QApplication::activePopupWidget() )
          handleFocusChanged( popup );
        else
          resetFocus();
      }
    }, Qt::QueuedConnection );
    return;
  }
  for ( QWidget *w = now; w; w = w->parentWidget() )
  {
    if ( mScopes.contains( w ) )
    {
      activate( w->isVisible() && w->isEnabled() ? w : nullptr );
      return;
    }
    if ( w == mCommandChrome && mActiveScope && mActiveScope->window() == w->window() )
      return;
    if ( w->isWindow() )
      break;
  }
  auto *menu = qobject_cast<QMenu *>( now->window() );
  if ( menu && menu->parentWidget() && mActiveScope &&
       menu->parentWidget()->window() == mActiveScope->window() )
    return;
  resetFocus();
}

void PaleoUndoGroupRouter::setCommandChrome( QWidget *chrome )
{
  mCommandChrome = chrome;
}

void PaleoUndoGroupRouter::setWritable( bool writable )
{
  mWritable = writable;
  emit targetChanged();
}

void PaleoUndoGroupRouter::resetFocus()
{
  activate( nullptr );
}

void PaleoUndoGroupRouter::clearScopeFocus( QWidget *scope )
{
  if ( mActiveScope == scope )
    resetFocus();
}

void PaleoUndoGroupRouter::activate( QWidget *scope )
{
  mActiveScope = scope;
  const auto it = mScopes.constFind( scope );
  mGroup->setActiveStack( it != mScopes.constEnd() ? it->stack.data() : nullptr );
  emit targetChanged();
}

bool PaleoUndoGroupRouter::eventFilter( QObject *watched, QEvent *event )
{
  if ( watched == mActiveScope.data() && ( event->type() == QEvent::Hide ||
                                          event->type() == QEvent::EnabledChange ) )
    resetFocus();
  return QObject::eventFilter( watched, event );
}

PaleoUndoStep PaleoUndoGroupRouter::describe( bool undo ) const
{
  if ( !mWritable || !mActiveScope || !mActiveScope->isVisible() || !mActiveScope->isEnabled() )
    return {};
  const auto it = mScopes.constFind( mActiveScope );
  if ( it != mScopes.constEnd() && it->target.describe )
    return it->target.describe( undo );
  if ( mGroup->activeStack() )
    return undo ? PaleoUndoStep{mGroup->canUndo(), mGroup->undoText()}
                : PaleoUndoStep{mGroup->canRedo(), mGroup->redoText()};
  return {};
}

bool PaleoUndoGroupRouter::step( bool undo )
{
  if ( !describe( undo ).available )
    return false;
  const auto it = mScopes.constFind( mActiveScope );
  const auto execute = it != mScopes.constEnd() ? it->target.step : nullptr;
  if ( execute )
    return execute( undo );
  if ( mGroup->activeStack() )
  {
    undo ? mGroup->undo() : mGroup->redo();
    return true;
  }
  return false;
}

void PaleoUndoGroupRouter::detachStackIfUnused( QUndoStack *stack )
{
  for ( const auto &s : std::as_const( mScopes ) )
    if ( s.stack == stack )
      return;
  if ( mGroup->activeStack() == stack )
    mGroup->setActiveStack( nullptr );
  mGroup->removeStack( stack );
}
