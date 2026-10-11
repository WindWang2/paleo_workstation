// 层：QGIS 封装
#include "previewrendercache.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <qgsrectangle.h>

#include <cmath>

namespace
{
QString sanitizeKey( const QString &key )
{
  QString out;
  for ( const QChar &c : key )
    out.append( c.isLetterOrNumber() ? c : QLatin1Char( '_' ) );
  return out;
}
} // namespace

PreviewRenderCache &PreviewRenderCache::instance()
{
  static PreviewRenderCache cache;
  return cache;
}

PreviewRenderCache::PreviewRenderCache()
{
  // 磁盘层：QTemporaryDir（进程退出自动清——「QTemporaryDir 级目录」口径，
  // 不在工程目录/用户配置里留痕）。
  m_dir = new QTemporaryDir( QDir::temp().absoluteFilePath( QStringLiteral( "paleo-preview-cache-XXXXXX" ) ) );
}

QString PreviewRenderCache::makeKey( const QString &assetId, const QString &versionId,
                                     const QgsRectangle &extent, int widthPx, int heightPx )
{
  // 范围按毫米、尺寸按 8px 取整：轻微 resize/平移抖动共享同一条目。
  // 定点格式（'f',0）：默认 'g' 只有 6 位有效数字——公里/UTM 量级坐标下
  // 相差数米的 extent 会碰撞成同一 key，把别的范围的快照当本范围上屏；
  // 科学计数法还会把 'e+07' 混进磁盘文件名。
  return QStringLiteral( "%1|%2|%3,%4,%5,%6|%7x%8" )
      .arg( sanitizeKey( assetId ), sanitizeKey( versionId ) )
      .arg( QString::number( std::round( extent.xMinimum() * 1000.0 ), 'f', 0 ),
            QString::number( std::round( extent.yMinimum() * 1000.0 ), 'f', 0 ),
            QString::number( std::round( extent.width() * 1000.0 ), 'f', 0 ),
            QString::number( std::round( extent.height() * 1000.0 ), 'f', 0 ) )
      .arg( widthPx / 8 * 8 )
      .arg( heightPx / 8 * 8 );
}

QImage PreviewRenderCache::lookup( const QString &key )
{
  const auto mem = m_mem.constFind( key );
  if ( mem != m_mem.constEnd() )
  {
    // LRU touch
    m_lru.removeAll( key );
    m_lru.append( key );
    return mem.value();
  }
  const QString path = m_dir->filePath( key + QStringLiteral( ".png" ) );
  if ( !QFile::exists( path ) )
    return QImage();
  QImage img( path );
  if ( img.isNull() )
    return QImage();
  store( key, img ); // 磁盘命中 → 提内存 LRU
  return img;
}

void PreviewRenderCache::store( const QString &key, const QImage &image )
{
  if ( key.isEmpty() || image.isNull() )
    return;
  m_mem.insert( key, image );
  m_lru.removeAll( key );
  m_lru.append( key );
  while ( m_mem.size() > kMemLimit && !m_lru.isEmpty() ) // D6.6 逐最远
  {
    const QString oldest = m_lru.takeFirst();
    m_mem.remove( oldest );
  }
  image.save( m_dir->filePath( key + QStringLiteral( ".png" ) ), "PNG" );
}

QString PreviewRenderCache::diskDirPath() const
{
  return m_dir->path();
}

void PreviewRenderCache::clearMemoryForTesting()
{
  m_mem.clear();
  m_lru.clear();
}
