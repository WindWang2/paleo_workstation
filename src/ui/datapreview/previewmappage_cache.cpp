// 层：视图
#include "previewmappage.h"
#include "../../qgis/previewrendercache.h"

#include <QEvent>
#include <QLabel>
#include <QTimer>

void PreviewMapPage::setupRenderCache()
{
  m_cacheSettleTimer = new QTimer( this );
  m_cacheSettleTimer->setSingleShot( true );
  m_cacheSettleTimer->setInterval( 300 );
  connect( m_cacheSettleTimer, &QTimer::timeout, this, &PreviewMapPage::storeSettledSnapshot );
  connect( m_canvas, &PreviewMapCanvas::renderStarted, this, &PreviewMapPage::onRenderStarted );
  connect( m_canvas, &PreviewMapCanvas::extentChanged, this, &PreviewMapPage::discardStaleSnapshot );
  connect( m_canvas, &PreviewMapCanvas::renderCanceled, this, [this] {
    m_cacheSettleTimer->stop();
    m_renderMatchesIdentity = false;
    m_renderLabel->clear();
    discardStaleSnapshot();
  } );
  connect( m_canvas, &PreviewMapCanvas::renderCompleted, this, [this] {
    m_snapshotKey.clear(); // The canvas has replaced the overlay with its live frame.
    if ( m_renderMatchesIdentity ) {
      m_completedKey = currentRenderCacheKey();
      m_cacheSettleTimer->start();
    }
  } );
}

void PreviewMapPage::setRenderCacheIdentity( const QString &assetId, const QString &versionId )
{
  if ( m_cacheAssetId == assetId && m_cacheVersionId == versionId ) return;
  m_cacheSettleTimer->stop();
  m_renderMatchesIdentity = false;
  m_completedKey.clear();
  m_awaitingFirstSettledFrame = !assetId.isEmpty();
  m_canvas->installEventFilter( this );
  m_cacheAssetId = assetId;
  m_cacheVersionId = versionId;
  discardStaleSnapshot();
}

bool PreviewMapPage::eventFilter( QObject *watched, QEvent *event )
{
  if ( watched == m_canvas && m_awaitingFirstSettledFrame &&
       ( event->type() == QEvent::Resize || event->type() == QEvent::Show ) ) {
    // Let the canvas finish propagating its final size to QGIS map settings.
    QTimer::singleShot( 0, this, [this] {
      if ( m_awaitingFirstSettledFrame && m_canvas->isVisible() ) primeRenderCache();
    } );
  }
  return QWidget::eventFilter( watched, event );
}

void PreviewMapPage::onRenderStarted()
{
  m_renderLabel->setText( QObject::tr( "渲染中…" ) );
  m_cacheSettleTimer->stop();
  m_renderMatchesIdentity = !m_cacheAssetId.isEmpty();
  // Tab insertion can prime against a 100x30 placeholder. QGIS now has the
  // actual viewport and map extent, and the new frame has not yet been painted.
  if ( m_awaitingFirstSettledFrame && m_canvas->isVisible() )
    primeRenderCache();
}

void PreviewMapPage::primeRenderCache()
{
  discardStaleSnapshot();
  if ( !m_snapshotKey.isEmpty() && m_canvas->overlayVisible() ) return;
  const QString key = currentRenderCacheKey();
  if ( key.isEmpty() )
    return;
  const QImage cached = PreviewRenderCache::instance().lookup( key );
  if ( !cached.isNull() )
  {
    m_snapshotKey = key;
    m_canvas->showPreviewOverlay( cached ); // 命中：缓存图立即上屏（D6.2）
  }
}

void PreviewMapPage::discardStaleSnapshot()
{
  if ( !m_snapshotKey.isEmpty() && m_snapshotKey != currentRenderCacheKey() ) {
    m_canvas->hidePreviewOverlay();
    m_snapshotKey.clear();
  }
}
