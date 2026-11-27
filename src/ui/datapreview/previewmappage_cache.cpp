// 层：视图
#include "previewmappage.h"
#include "../../qgis/previewrendercache.h"

#include <QEvent>
#include <QLabel>
#include <QTimer>

void PreviewMapPage::setRenderCacheIdentity( const QString &assetId, const QString &versionId )
{
  m_cacheSettleTimer->stop();
  m_awaitingFirstSettledFrame = true;
  m_canvas->installEventFilter( this );
  m_cacheAssetId = assetId;
  m_cacheVersionId = versionId;
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
  // Tab insertion can prime against a 100x30 placeholder. QGIS now has the
  // actual viewport and map extent, and the new frame has not yet been painted.
  if ( m_awaitingFirstSettledFrame && m_canvas->isVisible() )
    primeRenderCache();
}

void PreviewMapPage::primeRenderCache()
{
  const QString key = currentRenderCacheKey();
  if ( key.isEmpty() )
    return;
  const QImage cached = PreviewRenderCache::instance().lookup( key );
  if ( !cached.isNull() )
    m_canvas->showPreviewOverlay( cached ); // 命中：缓存图立即上屏（D6.2）
}
