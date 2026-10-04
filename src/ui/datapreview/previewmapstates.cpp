// 层：视图
#include "previewmapstates.h"

#include "../../catalog/datacatalog.h"
#include "../paleotheme.h"

#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QVBoxLayout>

namespace
{
  QLabel *captionLabel( const QString &text, QWidget *parent )
  {
    auto *l = new QLabel( text, parent );
    QFont f = l->font();
    f.setPointSize(PaleoTheme::tokens().labelPt);
    l->setFont( f );
    PaleoTheme::applyThemedStyleSheet(
        l, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
    l->setWordWrap( true );
    return l;
  }
} // namespace

namespace PreviewMapStates
{

QWidget *buildErrorPage( const QString &title, const QString &detail, QWidget *parent,
                         const QString &retryText, const std::function<void()> &onRetry )
{
  auto *box = new QWidget( parent );
  auto *lay = new QVBoxLayout( box );
  lay->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);
  lay->addStretch( 1 );

  auto *titleLbl = new QLabel( title, box );
  titleLbl->setObjectName( QStringLiteral( "stateText" ) );
  titleLbl->setAlignment( Qt::AlignCenter );
  titleLbl->setWordWrap( true );
  PaleoTheme::applyThemedStyleSheet( titleLbl, [] {
    return QStringLiteral( "color: %1; font-weight: 600;" )
        .arg( PaleoTheme::tokens().errorText.name() );
  } );
  lay->addWidget( titleLbl );

  if ( !detail.isEmpty() )
  {
    auto *detailLbl = new QLabel( detail, box );
    detailLbl->setObjectName( QStringLiteral( "previewErrorDetail" ) );
    detailLbl->setAlignment( Qt::AlignCenter );
    detailLbl->setWordWrap( true );
    PaleoTheme::applyThemedStyleSheet(
        detailLbl, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
    lay->addWidget( detailLbl );
  }
  if ( !retryText.isEmpty() )
  {
    auto *btn = new QPushButton( retryText, box );
    btn->setObjectName( QStringLiteral( "retryBtn" ) );
    if ( onRetry )
      QObject::connect( btn, &QPushButton::clicked, box, [onRetry] { onRetry(); } );
    lay->addWidget( btn, 0, Qt::AlignHCenter );
  }
  lay->addStretch( 1 );

  auto *page = new QWidget( parent );
  page->setObjectName( QStringLiteral( "previewMapErrorPage" ) );
  auto *pageLay = new QVBoxLayout( page );
  pageLay->setContentsMargins( 0, 0, 0, 0 );
  auto *frame = new QFrame( page );
  frame->setObjectName(QStringLiteral("previewErrorCard"));
  frame->setFrameShape( QFrame::StyledPanel );
  PaleoTheme::applyThemedStyleSheet( frame, [] {
    return PaleoTheme::metricStyleSheet(QStringLiteral(
                           // QLabel 也继承 QFrame；卡片边框只作用于外框。
                           "QFrame#previewErrorCard { background: %1; border: 1px solid %2;"
                           " border-radius: {rounded.md}px; }" ))
        .arg( PaleoTheme::tokens().errorBg.name(), PaleoTheme::tokens().errorText.name() );
  } );
  auto *frameLay = new QVBoxLayout( frame );
  frameLay->setContentsMargins(PaleoTheme::tokens().spacingLg, PaleoTheme::tokens().spacingLg, PaleoTheme::tokens().spacingLg, PaleoTheme::tokens().spacingLg);
  frameLay->addWidget( box );
  pageLay->addWidget( frame, 0, Qt::AlignCenter );
  return page;
}

QWidget *buildUnsupportedPage( const QString &typeName, QWidget *parent )
{
  auto *page = new QWidget( parent );
  page->setObjectName( QStringLiteral( "previewUnsupportedPage" ) );
  auto *lay = new QVBoxLayout( page );
  lay->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);
  lay->addStretch( 1 );

  auto *title = new QLabel( QObject::tr("不支持预览这个类型") +
                            ( typeName.isEmpty() ? QString() : QStringLiteral("：%1").arg(typeName) ),
                            page );
  title->setObjectName( QStringLiteral( "stateText" ) );
  title->setAlignment( Qt::AlignCenter );
  PaleoTheme::applyThemedStyleSheet(
      title, [] { return PaleoTheme::sectionTitleStyleSheet(); } );
  lay->addWidget( title );

  const QStringList supported = supportedPreviewTypes();
  lay->addWidget( captionLabel( QObject::tr("当前支持预览的类型"), page ) );
  for ( const QString &s : supported )
  {
    auto *row = new QLabel( QStringLiteral( "· %1" ).arg( s ), page );
    PaleoTheme::applyThemedStyleSheet(
        row, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
    lay->addWidget( row, 0, Qt::AlignHCenter );
  }
  lay->addStretch( 1 );
  return page;
}

namespace
{
// QTemporaryDir 不是 QObject——用小持有件把生命周期挂到预览宿主上。
class TempDirHolder : public QObject
{
  public:
    explicit TempDirHolder( QObject *parent )
      : QObject( parent )
      , dir( std::make_shared<QTemporaryDir>() )
    {
    }
    std::shared_ptr<QTemporaryDir> dir;
};
} // namespace

QPair<QString, QString> stageGeorefPairIfNeeded( const QString &imagePath,
                                                 const QString &sourceImagePath,
                                                 QObject *parent )
{
  const QString localWld = detectWorldFile( imagePath );
  if ( !localWld.isEmpty() )
    return { imagePath, localWld }; // 托管副本身旁已有边车
  if ( sourceImagePath.isEmpty() )
    return { imagePath, QString() };
  const QString srcWld = detectWorldFile( sourceImagePath );
  if ( srcWld.isEmpty() )
    return { imagePath, QString() };
  auto *holder = new TempDirHolder( parent );
  const QString dirPath = holder->dir->path();
  const QString baseName = QFileInfo( imagePath ).fileName();
  const QString stagedImg = QDir( dirPath ).filePath( baseName );
  const QString stagedWld = QDir( dirPath ).filePath(
      QFileInfo( baseName ).completeBaseName() + QLatin1Char( '.' ) +
      QFileInfo( srcWld ).suffix() );
  if ( !QFile::copy( imagePath, stagedImg ) || !QFile::copy( srcWld, stagedWld ) )
    return { imagePath, QString() };
  return { stagedImg, stagedWld };
}

QWidget *buildBigRasterHintBar( const QString &hint, QWidget *parent )
{
  auto *bar = new QWidget( parent );
  bar->setObjectName( QStringLiteral( "previewBigRasterHint" ) );
  PaleoTheme::applyThemedStyleSheet( bar, [] {
    return PaleoTheme::metricStyleSheet(QStringLiteral( "background: %1; border: 1px solid %2;"
                           " border-radius: {rounded.sm}px;" ))
        .arg( PaleoTheme::tokens().warningBg.name(),
              PaleoTheme::tokens().warning.name() );
  } );
  auto *lay = new QHBoxLayout( bar );
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
  auto *lbl = new QLabel( hint, bar );
  PaleoTheme::applyThemedStyleSheet(
      lbl, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
  lbl->setWordWrap( true );
  lay->addWidget( lbl );
  return bar;
}

QString detectWorldFile( const QString &imagePath )
{
  if ( imagePath.isEmpty() )
    return QString();
  const QFileInfo fi( imagePath );
  if ( !fi.exists() )
    return QString();
  const QString base = fi.absolutePath() + QLatin1Char( '/' ) + fi.completeBaseName();
  const QString dirOnly = fi.absolutePath();
  // 扩展名换 .wld 的惯例（png.wld）+ 各格式专属（.pgw/.jgw/.tfw/.hpw/.bpw）。
  const QStringList candidates = {
      dirOnly + QLatin1Char( '/' ) + fi.fileName() + QStringLiteral( ".wld" ),
      base + QStringLiteral( ".wld" ),
      base + QStringLiteral( ".pgw" ),
      base + QStringLiteral( ".jgw" ),
      base + QStringLiteral( ".tfw" ),
      base + QStringLiteral( ".hpw" ),
      base + QStringLiteral( ".bpw" ),
  };
  for ( const QString &c : candidates )
    if ( QFileInfo::exists( c ) )
      return c;
  return QString();
}

QVector<QPair<QString, QString>> siblingMappableAssets(
    DataCatalog *catalog, const QString &anchorPath,
    const std::function<QString( const CatalogVersion & )> &resolveAbs )
{
  QVector<QPair<QString, QString>> out;
  if ( !catalog || anchorPath.isEmpty() || !resolveAbs )
    return out;
  const QDir anchorDir = QFileInfo( anchorPath ).absoluteDir();
  const QStringList mappableTypes = { QStringLiteral( "geojson" ),
                                      QStringLiteral( "image_reference" ),
                                      QStringLiteral( "horizon" ) };
  for ( const CatalogAsset &asset : catalog->assets() )
  {
    if ( asset.id.isEmpty() || asset.displayName.isEmpty() )
      continue;
    if ( !mappableTypes.contains( asset.type ) )
      continue;
    const CatalogVersion v = catalog->currentVersion( asset.id );
    if ( v.path.isEmpty() )
      continue;
    // 同目录判据：资产当前版本的绝对路径所在目录 == anchor 目录。
    const QString abs = resolveAbs( v );
    if ( abs.isEmpty() )
      continue;
    const QDir d = QFileInfo( abs ).absoluteDir();
    if ( d != anchorDir )
      continue;
    if ( QFileInfo::exists( abs ) )
      out.append( { asset.id, asset.displayName } );
  }
  return out;
}

QStringList supportedPreviewTypes()
{
  return { QObject::tr("well_log — 测井曲线（多曲线叠合/综合柱状图）"),
           QObject::tr("well_head — 井位表（信息卡 + 井位图）"),
           QObject::tr("well_stratification — 分层表（表格 + 井位落图）"),
           QObject::tr("time_depth — 时深表（曲线）"),
           QObject::tr("horizon — 层位（栅格地图 + 等值线 + 剖面分析）"),
           QObject::tr("seismic — 地震（测线剖面/三维/时间切片）"),
           QObject::tr("image_reference — 平面图（配准后栅格上图，否则图片查看器）"),
           QObject::tr("geojson — 相图/矢量（分类渲染地图 + 属性表）"),
           QObject::tr("document — 文档（PDF 预览）") };
}

} // namespace PreviewMapStates

// ---------------------------------------------------------------- session --

namespace
{
struct AssetMemory
{
  QVector<PreviewStateMemory::Bookmark> bookmarks;
  QStringList tocOrder;
  QHash<QString, PreviewStateMemory::TocLayerState> tocStates;
};
QHash<QString, AssetMemory> &memoryStore()
{
  static QHash<QString, AssetMemory> store;
  return store;
}
} // namespace

QVector<PreviewStateMemory::Bookmark> PreviewStateMemory::bookmarks( const QString &assetKey )
{
  return memoryStore().value( assetKey ).bookmarks;
}

void PreviewStateMemory::addBookmark( const QString &assetKey, const Bookmark &bm )
{
  if ( bm.name.isEmpty() || bm.extent.isEmpty() )
    return;
  AssetMemory &m = memoryStore()[assetKey];
  // 同名覆盖（重建视图后重新保存的同名书签不翻倍）。
  for ( int i = 0; i < m.bookmarks.size(); ++i )
    if ( m.bookmarks.at( i ).name == bm.name )
    {
      m.bookmarks[i] = bm;
      return;
    }
  m.bookmarks.append( bm );
}

bool PreviewStateMemory::removeBookmark( const QString &assetKey, const QString &name )
{
  AssetMemory &m = memoryStore()[assetKey];
  for ( int i = 0; i < m.bookmarks.size(); ++i )
    if ( m.bookmarks.at( i ).name == name )
    {
      m.bookmarks.removeAt( i );
      return true;
    }
  return false;
}

QHash<QString, PreviewStateMemory::TocLayerState> PreviewStateMemory::tocStates( const QString &assetKey )
{
  return memoryStore().value( assetKey ).tocStates;
}

QStringList PreviewStateMemory::tocOrder( const QString &assetKey )
{
  return memoryStore().value( assetKey ).tocOrder;
}

void PreviewStateMemory::saveToc( const QString &assetKey, const QStringList &orderTopToBottom,
                                  const QHash<QString, TocLayerState> &states )
{
  AssetMemory &m = memoryStore()[assetKey];
  m.tocOrder = orderTopToBottom;
  m.tocStates = states;
}

void PreviewStateMemory::removeTocLayer( const QString &assetKey, const QString &layerName )
{
  AssetMemory &m = memoryStore()[assetKey];
  m.tocOrder.removeAll( layerName );
  m.tocStates.remove( layerName );
}

void PreviewStateMemory::clearAsset( const QString &assetKey )
{
  memoryStore().remove( assetKey );
}

void PreviewStateMemory::clearAll()
{
  memoryStore().clear();
}
