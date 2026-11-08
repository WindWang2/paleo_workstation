// 层：视图
#pragma once
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class QUndoGroup;
class QUndoStack;
class QWidget;

struct PaleoUndoStep
{
    bool available = false;
    QString text;
};

// A view adapter for histories whose commands can fail and must remain retryable.
struct PaleoUndoTarget
{
    std::function<PaleoUndoStep( bool undo )> describe;
    std::function<bool( bool undo )> step;
};

// Selects one visible focus scope. Native stacks retain their QUndoGroup;
// callback targets keep their original execution and failure semantics.
class PaleoUndoGroupRouter : public QObject
{
    Q_OBJECT
  public:
    explicit PaleoUndoGroupRouter( QObject *parent = nullptr );
    ~PaleoUndoGroupRouter() override;

    QUndoGroup *group() const { return mGroup; }
    void registerScope( QWidget *scope, QUndoStack *stack, PaleoUndoTarget target = {} );
    void unregisterScope( QWidget *scope );
    QUndoStack *activeStack() const;
    void setCommandChrome( QWidget *chrome );
    void setWritable( bool writable );
    void resetFocus();
    void clearScopeFocus( QWidget *scope );
    void handleFocusChanged( QWidget *now );
    PaleoUndoStep describe( bool undo ) const;
    bool step( bool undo );

  signals:
    void targetChanged();

  protected:
    bool eventFilter( QObject *watched, QEvent *event ) override;

  private:
    struct Scope
    {
        QPointer<QUndoStack> stack;
        PaleoUndoTarget target;
        QMetaObject::Connection destroyed;
    };
    void activate( QWidget *scope );
    void detachStackIfUnused( QUndoStack *stack );

    QUndoGroup *mGroup = nullptr; // owned (QObject child)
    QHash<QWidget *, Scope> mScopes;
    QPointer<QWidget> mActiveScope;
    QPointer<QWidget> mCommandChrome;
    bool mWritable = true;
};
