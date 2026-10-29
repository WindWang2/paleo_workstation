// 层：视图
#include <qgsfeedback.h>

// ---------------------------------------------------------------------------
// Compat shim: the frozen paleolocatorfilters.h declares fetchResults() taking
// QgsLocatorFeedback*, but QGIS 4.2 renamed that type to QgsFeedback — the
// base-class signature in qgslocatorfilter.h is
//   fetchResults( const QString &, const QgsLocatorContext &, QgsFeedback * )
// Aliasing keeps the header's `override` declarations valid. This must appear
// before the header include in every TU that pulls it in (see tst_locator.cpp;
// AUTOMOC-generated moc files need the same shim via -include or a header fix).
// ---------------------------------------------------------------------------
using QgsLocatorFeedback = QgsFeedback;

#include "paleolocatorfilters.h"

#include <qgsfeature.h>
#include <qgsfeatureid.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>
#include <qgslocatormodel.h>
#include <qgslocatorwidget.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QVariantMap>

namespace
{
  // Cap emitted results so a broad query over a big layer can't flood the
  // locator model.
  constexpr int MAX_RESULTS = 50;

  bool matches( const QString &candidate, const QString &query )
  {
    return candidate.contains( query, Qt::CaseInsensitive );
  }
}

// ---------------------------------------------------------------------------
// WellLocatorFilter — search well ids (and a conventional "name" field when
// present) in the wells point layer; trigger selects + zooms + flashes.
// ---------------------------------------------------------------------------

WellLocatorFilter::WellLocatorFilter( WellLayerProvider provider, QgsMapCanvas *canvas, QObject *parent )
  : QgsLocatorFilter( parent )
  , m_provider( std::move( provider ) )
  , m_canvas( canvas )
{
}

namespace
{
  // wave/ux-polish：结果排序/分组——score 反映匹配强度（精确 > 前缀 > 包含），
  // group 走各 filter 的 displayName（井位/层位各一节，QgsLocatorModel 自动
  // 出节头；键盘 ↑↓ 导航由 QgsLocatorWidget 原生提供）。
  double matchScore( const QString &candidate, const QString &term )
  {
    const QString c = candidate.trimmed();
    const QString t = term.trimmed();
    if ( c.compare( t, Qt::CaseInsensitive ) == 0 )
      return 1.0;
    if ( c.startsWith( t, Qt::CaseInsensitive ) )
      return 0.9;
    return 0.6;
  }
} // namespace

void WellLocatorFilter::fetchResults( const QString &string, const QgsLocatorContext &context,
                                      QgsLocatorFeedback *feedback )
{
  Q_UNUSED( context )
  if ( string.trimmed().isEmpty() || !m_provider )
    return;

  const QPair<QgsVectorLayer *, QString> resolved = m_provider();
  QgsVectorLayer *layer = resolved.first;
  const QString idField = resolved.second;
  if ( !layer || !layer->isValid() || idField.isEmpty() )
    return;

  // Display-substring match against the declared id field, plus a
  // conventional "name" field when the layer carries one.
  QStringList matchFields;
  if ( layer->fields().lookupField( idField ) >= 0 )
    matchFields << idField;
  const QString nameField = QStringLiteral( "name" );
  if ( !matchFields.contains( nameField, Qt::CaseInsensitive )
       && layer->fields().lookupField( nameField ) >= 0 )
    matchFields << nameField;
  if ( matchFields.isEmpty() )
    return;

  QgsFeatureRequest request;
  request.setSubsetOfAttributes( matchFields, layer->fields() );

  QgsFeatureIterator it = layer->getFeatures( request );
  QgsFeature f;
  int emitted = 0;
  while ( it.nextFeature( f ) )
  {
    if ( feedback && feedback->isCanceled() )
      break;

    bool hit = false;
    for ( const QString &field : matchFields )
    {
      if ( matches( f.attribute( field ).toString(), string ) )
      {
        hit = true;
        break;
      }
    }
    if ( !hit )
      continue;

    QgsLocatorResult result;
    result.filter = this;
    result.displayString = f.attribute( idField ).toString();
    if ( result.displayString.isEmpty() )
      result.displayString = FID_TO_STRING( f.id() );
    result.group = displayName();
    result.groupScore = 1.0;
    result.score = 0.6; // 兜底；命中字段打分见下
    for ( const QString &field : matchFields )
      if ( matches( f.attribute( field ).toString(), string ) )
        result.score = qMax( result.score, matchScore( f.attribute( field ).toString(), string ) );
    // 次级说明：name 字段有值且不同于显示串时作为 description。
    if ( matchFields.contains( QStringLiteral( "name" ) ) )
    {
      const QString nm = f.attribute( QStringLiteral( "name" ) ).toString();
      if ( !nm.isEmpty() && nm != result.displayString )
        result.description = nm;
    }

    QVariantMap data;
    data.insert( QStringLiteral( "fid" ), FID_TO_NUMBER( f.id() ) );
    data.insert( QStringLiteral( "wkt" ),
                 f.hasGeometry() ? f.geometry().asWkt() : QString() );
    result.setUserData( data );
    emit resultFetched( result );

    if ( ++emitted >= MAX_RESULTS )
      break;
  }
}

void WellLocatorFilter::triggerResult( const QgsLocatorResult &result )
{
  const QVariantMap data = result.userData().toMap();
  if ( !data.contains( QStringLiteral( "fid" ) ) || !m_provider )
    return;
  const QgsFeatureId fid = data.value( QStringLiteral( "fid" ) ).toLongLong();

  QgsVectorLayer *layer = m_provider().first;
  if ( !layer || !layer->isValid() )
    return;

  const QgsFeatureIds ids{ fid };
  layer->selectByIds( ids );
  if ( m_canvas )
  {
    // QGIS 4.2: zoomToFeatureExtent() takes a bare QgsRectangle; the
    // layer+ids entry point is zoomToFeatureIds().
    m_canvas->zoomToFeatureIds( layer, ids );
    m_canvas->flashFeatureIds( layer, ids );
    m_canvas->refresh();
  }
}

// ---------------------------------------------------------------------------
// HorizonLocatorFilter — contains-match over manifest horizon ids; trigger
// switches the active horizon via the injected callback.
// ---------------------------------------------------------------------------

HorizonLocatorFilter::HorizonLocatorFilter( HorizonListProvider provider, ActivateFn activate, QObject *parent )
  : QgsLocatorFilter( parent )
  , m_provider( std::move( provider ) )
  , m_activate( std::move( activate ) )
{
}

void HorizonLocatorFilter::fetchResults( const QString &string, const QgsLocatorContext &context,
                                         QgsLocatorFeedback *feedback )
{
  Q_UNUSED( context )
  if ( string.trimmed().isEmpty() || !m_provider )
    return;

  int emitted = 0;
  const QStringList ids = m_provider();
  for ( const QString &id : ids )
  {
    if ( feedback && feedback->isCanceled() )
      break;
    if ( !matches( id, string ) )
      continue;

    QgsLocatorResult result;
    result.filter = this;
    result.displayString = id;
    result.group = displayName();
    result.groupScore = 1.0;
    result.score = matchScore( id, string );
    result.setUserData( id );
    emit resultFetched( result );

    if ( ++emitted >= MAX_RESULTS )
      break;
  }
}

void HorizonLocatorFilter::triggerResult( const QgsLocatorResult &result )
{
  if ( m_activate )
    m_activate( result.userData().toString() );
}

// ---------------------------------------------------------------------------
// IssueLocatorFilter — match validation issue codes/messages; trigger hands a
// reconstructed IssueRef to the injected locate callback.
// ---------------------------------------------------------------------------

IssueLocatorFilter::IssueLocatorFilter( IssueProvider provider, LocateFn locate, QObject *parent )
  : QgsLocatorFilter( parent )
  , m_provider( std::move( provider ) )
  , m_locate( std::move( locate ) )
{
}

void IssueLocatorFilter::fetchResults( const QString &string, const QgsLocatorContext &context,
                                       QgsLocatorFeedback *feedback )
{
  Q_UNUSED( context )
  if ( string.trimmed().isEmpty() || !m_provider )
    return;

  int emitted = 0;
  const QList<IssueRef> issues = m_provider();
  for ( const IssueRef &ref : issues )
  {
    if ( feedback && feedback->isCanceled() )
      break;
    if ( !matches( ref.code, string ) && !matches( ref.message, string ) )
      continue;

    QgsLocatorResult result;
    result.filter = this;
    result.displayString = ref.message.isEmpty()
                             ? ref.code
                             : QStringLiteral( "%1 — %2" ).arg( ref.code, ref.message );

    QVariantMap data;
    data.insert( QStringLiteral( "code" ), ref.code );
    data.insert( QStringLiteral( "message" ), ref.message );
    data.insert( QStringLiteral( "layerId" ), ref.layerId );
    data.insert( QStringLiteral( "wkt" ), ref.wkt );
    result.setUserData( data );
    emit resultFetched( result );

    if ( ++emitted >= MAX_RESULTS )
      break;
  }
}

void IssueLocatorFilter::triggerResult( const QgsLocatorResult &result )
{
  if ( !m_locate )
    return;

  const QVariantMap data = result.userData().toMap();
  IssueRef ref;
  ref.code = data.value( QStringLiteral( "code" ) ).toString();
  ref.message = data.value( QStringLiteral( "message" ) ).toString();
  ref.layerId = data.value( QStringLiteral( "layerId" ) ).toString();
  ref.wkt = data.value( QStringLiteral( "wkt" ) ).toString();
  m_locate( ref );
}

// ---------------------------------------------------------------------------
// 键盘出口：QgsLocatorLineEdit 把 Tab/Shift+Tab 留给补全与下一条结果，Esc 又
// 只收结果列表——键盘用户进到搜索框就离不开。判据看结果行数而不是浮窗可见
// 性：QGIS 会在聚焦后 ~110ms 再把浮窗弹出来，且空查询时列表里摆的是「可用
// 过滤器」伪结果行，不算命中。空闲（框内无文本且无真实结果行）时 Tab/
// Shift+Tab 放行到焦点链前/后一个可聚焦控件；有结果行或文本时仍归 QGIS。
// Esc 把焦点还给进入前的控件（没有则向后走一格）——只有可见列表里有真实
// 结果行时才归 QGIS 收表。
// ---------------------------------------------------------------------------
namespace
{
  // 析构安全（#775）：Qt 在 QLineEdit 析构途中仍会向已安装的 eventFilter 投递
  // 事件（实测 delete 聚焦编辑框/销毁窗口时有 FocusAboutToChange、FocusOut、
  // Hide、WindowDeactivate、Destroy 等到达过滤器，且已到 ~QWidget 阶段——对象
  // 动态类型随析构链降级）。此刻任何按 QLineEdit 静态类型的成员访问都是 UB
  // （UBSan vptr：member call on address ... does not point to an object of
  // type 'QLineEdit'），QPointer<QLineEdit> 的下转型比较也在同一片雷区。
  // 对策：m_edit 只以 QObject* 做地址比较；真要碰编辑框成员前先用 inherits()
  // 确认动态类型仍在 QgsLocatorLineEdit——inherits 是 QObject 的非虚拟成员，
  // 沿元对象链走，任何析构阶段都能安全调用，且半析构时必为 false。
  class LocatorKeyboardExit : public QObject
  {
    public:
      LocatorKeyboardExit(QgsLocatorWidget *locator, QLineEdit *edit)
        : QObject(locator), m_edit(edit)
      {
        edit->installEventFilter(this);
        QObject::connect(qApp, &QApplication::focusChanged, this,
                         [this](QWidget *old, QWidget *now) {
                           if (now == m_edit && old && old != m_edit)
                             m_prev = old;
                         });
      }

    protected:
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        // 半析构守卫：非 KeyPress 直接放行；地址只按 QObject* 比；动态类型已降级
        // （编辑框正在析构）时同样放行，绝不摸半析构对象。
        if (ev->type() != QEvent::KeyPress || !m_edit || obj != m_edit.data()
            || !obj->inherits("QgsLocatorLineEdit"))
          return false;
        auto *edit = static_cast<QLineEdit *>(obj);
        auto *ke = static_cast<QKeyEvent *>(ev);
        const bool fwd = ke->key() == Qt::Key_Tab && ke->modifiers() == Qt::NoModifier;
        const bool bwd = ke->key() == Qt::Key_Backtab; // Qt 把 Shift+Tab 投递成 Backtab
        if (fwd || bwd)
        {
          bool seen = false, visible = false;
          int rows = 0;
          resultsViewState(edit, seen, visible, rows);
          if (seen && rows == 0 && edit->text().trimmed().isEmpty())
          {
            moveFocus(edit, fwd);
            return true;
          }
          return false; // 其余情形 Tab/Shift+Tab 仍归 QGIS：补全、上/下一条结果
        }
        if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier)
        {
          bool seen = false, visible = false;
          int rows = 0;
          resultsViewState(edit, seen, visible, rows);
          // 列表可见且有结果行才归 QGIS 收；空行/隐藏浮窗 Esc 放行出框。
          if (!seen || (visible && rows > 0))
            return false;
          if (m_prev && m_prev != m_edit && m_prev->isVisible() && m_prev->isEnabled())
            m_prev->setFocus(Qt::OtherFocusReason);
          else
            moveFocus(edit, false);
          return true;
        }
        return false;
      }

    private:
      void resultsViewState(QLineEdit *edit, bool &seen, bool &visible, int &rows) const
      {
        for (QWidget *w : edit->window()->findChildren<QWidget *>())
          if (w->inherits("QgsLocatorResultsView"))
          {
            seen = true;
            visible |= w->isVisible();
            if (auto *v = qobject_cast<QAbstractItemView *>(w))
              if (auto *m = v->model())
                // 空查询时 QGIS 摆出来的是「可用过滤器」伪结果行——只数真实命中行
                //（代理模型不透传 ResultFilterName 角色，取 ResultData 载荷直读）。
                for (int r = m->rowCount() - 1; r >= 0; --r)
                {
                  const QgsLocatorResult res = m->data(m->index(r, 0),
                      static_cast<int>(QgsLocatorModel::CustomRole::ResultData))
                          .value<QgsLocatorResult>();
                  if (!res.filter || res.filter->name() != QLatin1String("filters"))
                    ++rows;
                }
          }
      }

      // focusNextPrevChild 是 protected——沿焦点链找第一个可聚焦件。
      void moveFocus(QLineEdit *edit, bool forward)
      {
        for (QWidget *w = forward ? edit->nextInFocusChain() : edit->previousInFocusChain();
             w && w != edit; w = forward ? w->nextInFocusChain() : w->previousInFocusChain())
          if ((w->focusPolicy() & Qt::TabFocus) && w->isVisibleTo(edit->window()) &&
              w->isEnabled())
          {
            w->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            break;
          }
      }

      // 见类前注释：只做地址比较，不下转型；成员访问前另有动态类型守卫。
      QPointer<QObject> m_edit;
      QPointer<QWidget> m_prev;
  };
} // namespace

void installLocatorKeyboardExit(QgsLocatorWidget *locator)
{
  if (!locator)
    return;
  for (auto *edit : locator->findChildren<QLineEdit *>())
    if (edit->inherits("QgsLocatorLineEdit"))
    {
      new LocatorKeyboardExit(locator, edit);
      return;
    }
}

// AUTOMOC: the header's Q_OBJECT classes get their metaobjects here — inside
// this TU the QgsLocatorFeedback alias above is already in scope, so the
// generated moc file compiles against the frozen header unmodified.
#include "moc_paleolocatorfilters.cpp"
