// 层：视图
#include "layouttemplates.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QDomDocument>
#include <QFileInfo>
#include "../notifications/paleonotify.h"

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempage.h>
#include <qgslayoutpagecollection.h>
#include <qgsmaplayer.h>
#include <qgsprintlayout.h>
#include <qgsreadwritecontext.h>
#include <qgsrectangle.h>

#include "../../qgis/standardelements.h"

namespace
{
  struct BuiltinTemplate
  {
    const char *key;
    const char *title; // tr() source string
  };

  // Order is part of the contract (tests pin it).
  const BuiltinTemplate kBuiltinTemplates[] =
  {
    { "a4_landscape", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A4 横向（297 × 210 mm）" ) },
    { "a4_portrait", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A4 纵向（210 × 297 mm）" ) },
    { "a3_landscape", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A3 横向（420 × 297 mm）" ) },
    { "a3_portrait", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A3 纵向（297 × 420 mm）" ) },
    { "a0_landscape", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A0 横向（1189 × 841 mm）" ) },
  };

  // 内容模板的页面变体（复合键的右段）。
  const char *kFigurePageKeys[] = { "a4_landscape", "a4_portrait", "a3_landscape", "a3_portrait" };

  const BuiltinTemplate *builtinFor( const QString &key )
  {
    for ( const BuiltinTemplate &entry : kBuiltinTemplates )
    {
      if ( key == QLatin1String( entry.key ) )
        return &entry;
    }
    return nullptr;
  }

  // 复合键解析："well_position@a4_landscape" → kind + 页面规格。
  bool parseFigureKey( const QString &key, PaleoStandardElements::FigureKind *kind,
                       PaleoStandardElements::PageSetup *setup )
  {
    const int at = key.indexOf( QLatin1Char( '@' ) );
    if ( at <= 0 || !kind || !setup )
      return false;
    if ( !PaleoStandardElements::figureKindFromKey( key.left( at ), kind ) )
      return false;

    const QString pageKey = key.mid( at + 1 );
    *setup = PaleoStandardElements::PageSetup{};
    if ( pageKey == QLatin1String( "a4_landscape" ) )
    {
      setup->sizeName = QStringLiteral( "A4" );
      setup->landscape = true;
    }
    else if ( pageKey == QLatin1String( "a4_portrait" ) )
    {
      setup->sizeName = QStringLiteral( "A4" );
      setup->landscape = false;
    }
    else if ( pageKey == QLatin1String( "a3_landscape" ) )
    {
      setup->sizeName = QStringLiteral( "A3" );
      setup->landscape = true;
    }
    else if ( pageKey == QLatin1String( "a3_portrait" ) )
    {
      setup->sizeName = QStringLiteral( "A3" );
      setup->landscape = false;
    }
    else
      return false;
    return true;
  }

  // Content items = everything except the paper items.
  int contentItemCount( QgsLayout *layout )
  {
    QList<QgsLayoutItem *> items;
    layout->layoutItems( items );
    int count = 0;
    for ( QgsLayoutItem *item : items )
    {
      if ( !dynamic_cast<QgsLayoutItemPage *>( item ) )
        ++count;
    }
    return count;
  }

  QWidget *dialogParent( QObject *anchor )
  {
    return anchor ? qobject_cast<QWidget *>( anchor->parent() ) : nullptr;
  }
}

// ---------------------------------------------------------------------------
// PaleoLayoutTemplates
// ---------------------------------------------------------------------------

PaleoLayoutTemplates::PaleoLayoutTemplates( QObject *parent )
  : QObject( parent )
{
  m_saveAction = new QAction( tr( "另存为模板(&T)…" ), this );
  m_saveAction->setObjectName( QStringLiteral( "actionSaveLayoutTemplate" ) );
  connect( m_saveAction, &QAction::triggered, this, [this]
  {
    QgsLayout *layout = m_layoutProvider ? m_layoutProvider() : nullptr;
    if ( !layout )
    {
      emit templateFinished( QString(), false );
      return;
    }
    const QString path = QFileDialog::getSaveFileName( dialogParent( this ), tr( "保存版面模板" ),
                                                       QString(), tr( "QGIS 版面模板 (*.qpt)" ) );
    if ( path.isEmpty() )
      return; // user canceled — no signal
    saveTemplate( layout, path );
  } );

  m_loadAction = new QAction( tr( "从模板加载(&L)…" ), this );
  m_loadAction->setObjectName( QStringLiteral( "actionLoadLayoutTemplate" ) );
  connect( m_loadAction, &QAction::triggered, this, [this]
  {
    QgsLayout *layout = m_layoutProvider ? m_layoutProvider() : nullptr;
    if ( !layout )
    {
      emit templateFinished( QString(), false );
      return;
    }
    const QString path = QFileDialog::getOpenFileName( dialogParent( this ), tr( "加载版面模板" ),
                                                       QString(), tr( "QGIS 版面模板 (*.qpt)" ) );
    if ( path.isEmpty() )
      return; // user canceled — no signal
    loadTemplate( layout, path );
  } );
}

QStringList PaleoLayoutTemplates::builtinKeys()
{
  QStringList keys;
  for ( const BuiltinTemplate &entry : kBuiltinTemplates )
    keys << QLatin1String( entry.key );
  return keys;
}

QStringList PaleoLayoutTemplates::figureBuiltinKeys()
{
  const QStringList kinds = { QStringLiteral( "well_position" ),
                              QStringLiteral( "single_factor" ),
                              QStringLiteral( "facies" ) };
  QStringList keys;
  for ( const QString &kind : kinds )
    for ( const char *page : kFigurePageKeys )
      keys << kind + QLatin1Char( '@' ) + QLatin1String( page );
  return keys;
}

QString PaleoLayoutTemplates::builtinTitle( const QString &key )
{
  // 复合键：「井位图 · A4 横向」。
  PaleoStandardElements::FigureKind kind;
  PaleoStandardElements::PageSetup setup;
  if ( parseFigureKey( key, &kind, &setup ) )
  {
    const QString page = setup.landscape ? tr( "横向" ) : tr( "纵向" );
    return tr( "%1 · %2 %3" ).arg( PaleoStandardElements::figureKindTitle( kind ),
                                   setup.sizeName, page );
  }
  const BuiltinTemplate *entry = builtinFor( key );
  return entry ? tr( entry->title ) : QString();
}

void PaleoLayoutTemplates::setTemplatesDir( const QString &dir )
{
  m_templatesDir = dir;
}

QString PaleoLayoutTemplates::templatesDir() const
{
  if ( !m_templatesDir.isEmpty() )
    return m_templatesDir;

  const QByteArray env = qgetenv( "PALEO_LAYOUT_TEMPLATES_DIR" );
  if ( !env.isEmpty() )
    return QDir( QString::fromLocal8Bit( env ) ).absolutePath();

  if ( QCoreApplication::instance() )
  {
    QDir dir( QCoreApplication::applicationDirPath() );
    for ( int level = 0; level <= 3; ++level )
    {
      if ( dir.exists( QStringLiteral( "docs/templates" ) ) )
        return dir.absoluteFilePath( QStringLiteral( "docs/templates" ) );
      if ( !dir.cdUp() )
        break;
    }
  }
  return QString();
}

PaleoLayoutTemplates::TemplateOutcome PaleoLayoutTemplates::saveTemplate( QgsLayout *layout, const QString &path )
{
  TemplateOutcome outcome;
  if ( !layout )
  {
    outcome.error = tr( "没有可保存为模板的版面。" );
    emit templateFinished( QString(), false );
    return outcome;
  }
  if ( path.isEmpty() )
  {
    outcome.error = tr( "未指定模板文件。" );
    emit templateFinished( QString(), false );
    return outcome;
  }

  outcome.itemCount = contentItemCount( layout );
  QgsReadWriteContext context;
  if ( !layout->saveAsTemplate( path, context ) )
  {
    outcome.ok = false;
    outcome.error = tr( "无法写入模板文件 %1。" ).arg( path );
    emit templateFinished( path, false );
    return outcome;
  }

  outcome.ok = true;
  emit templateFinished( path, true );
  return outcome;
}

PaleoLayoutTemplates::TemplateOutcome PaleoLayoutTemplates::loadTemplate( QgsLayout *layout, const QString &path )
{
  return loadFromFile( layout, path );
}

PaleoLayoutTemplates::TemplateOutcome PaleoLayoutTemplates::loadFromFile( QgsLayout *layout, const QString &path )
{
  TemplateOutcome outcome;
  if ( !layout )
  {
    outcome.error = tr( "没有可加载模板的版面。" );
    emit templateFinished( QString(), false );
    return outcome;
  }

  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
  {
    outcome.error = tr( "无法读取模板文件 %1。" ).arg( path );
    emit templateFinished( path, false );
    return outcome;
  }

  QDomDocument document;
  const QDomDocument::ParseResult parseResult = document.setContent( &file, QDomDocument::ParseOption::UseNamespaceProcessing );
  if ( !parseResult )
  {
    outcome.error = tr( "模板文件 %1 不是有效的 XML（第 %2 行：%3）。" )
                      .arg( path )
                      .arg( parseResult.errorLine )
                      .arg( parseResult.errorMessage );
    emit templateFinished( path, false );
    return outcome;
  }

  QgsReadWriteContext context;
  bool ok = false;
  layout->loadFromTemplate( document, context, /*clearExisting=*/true, &ok );
  if ( !ok )
  {
    outcome.error = tr( "无法将模板 %1 加载到版面。" ).arg( path );
    emit templateFinished( path, false );
    return outcome;
  }

  outcome.ok = true;
  outcome.itemCount = contentItemCount( layout );
  emit templateFinished( path, true );
  return outcome;
}

PaleoLayoutTemplates::TemplateOutcome PaleoLayoutTemplates::applyBuiltin( QgsLayout *layout, const QString &key )
{
  TemplateOutcome outcome;

  // 内容模板（复合键）：骨架走代码工厂，不落 .qpt。
  PaleoStandardElements::FigureKind kind;
  PaleoStandardElements::PageSetup setup;
  if ( parseFigureKey( key, &kind, &setup ) )
  {
    auto *printLayout = dynamic_cast<QgsPrintLayout *>( layout );
    if ( !printLayout )
    {
      outcome.error = tr( "内容模板需要 QgsPrintLayout 版面。" );
      emit templateFinished( QString(), false );
      return outcome;
    }
    // 套模板保留地图项当前图层与范围（换骨架不换数据）。
    QgsLayoutItemMap *map = nullptr;
    QList<QgsLayoutItemMap *> maps;
    layout->layoutItems( maps );
    if ( !maps.isEmpty() )
      map = maps.first();
    const QList<QgsMapLayer *> layers = map ? map->layers() : QList<QgsMapLayer *>();
    const QgsRectangle extent = map ? map->extent() : QgsRectangle();

    QString err;
    if ( !PaleoStandardElements::populateFigureLayout( printLayout, kind, setup, layers, extent, &err ) )
    {
      outcome.error = err;
      emit templateFinished( QString(), false );
      return outcome;
    }
    outcome.ok = true;
    outcome.itemCount = contentItemCount( layout );
    emit templateFinished( key, true );
    return outcome;
  }

  if ( !builtinFor( key ) )
  {
    outcome.error = tr( "未知的内置模板「%1」。" ).arg( key );
    emit templateFinished( QString(), false );
    return outcome;
  }

  const QString dir = templatesDir();
  if ( dir.isEmpty() )
  {
    outcome.error = tr( "未配置内置模板目录。" );
    emit templateFinished( QString(), false );
    return outcome;
  }

  const QString path = QDir( dir ).absoluteFilePath( key + QStringLiteral( ".qpt" ) );
  if ( !QFile::exists( path ) )
  {
    outcome.error = tr( "在 %2 中找不到内置模板 %1。" ).arg( key, dir );
    emit templateFinished( path, false );
    return outcome;
  }

  return loadFromFile( layout, path );
}

QAction *PaleoLayoutTemplates::saveAsTemplateAction() { return m_saveAction; }
QAction *PaleoLayoutTemplates::loadFromTemplateAction() { return m_loadAction; }

void PaleoLayoutTemplates::setLayoutProvider( const std::function<QgsLayout *()> &provider )
{
  m_layoutProvider = provider;
}

QAction *PaleoLayoutTemplates::applyBuiltinAction( const QString &key )
{
  if ( builtinTitle( key ).isEmpty() ) // page keys + figure composite keys
    return nullptr;

  const QString title = builtinTitle( key );
  auto *action = new QAction( title, this );
  action->setObjectName( QStringLiteral( "actionApplyBuiltin_%1" ).arg( key ) );
  connect( action, &QAction::triggered, this, [this, key]
  {
    QgsLayout *layout = m_layoutProvider ? m_layoutProvider() : nullptr;
    if ( !layout )
    {
      emit templateFinished( QString(), false );
      return;
    }
    // 内置模板走 loadFromTemplate(clearExisting=true)——一键清空版面。
    // 有内容项时先确认（程序化 applyBuiltin() 不确认：调用方已决断）。
    const int existing = contentItemCount( layout );
    if ( existing > 0 )
    {
      if ( !PaleoNotify::ask(
             dialogParent( this ), tr( "应用内置模板" ),
             tr( "应用模板「%1」将清空当前版面的 %2 个内容项，是否继续？" )
               .arg( builtinTitle( key ) )
               .arg( existing ),
             PaleoNotify::AskButtons::OkCancel, PaleoNotify::AskDefault::Reject ) )
        return;
    }
    applyBuiltin( layout, key );
  } );
  return action;
}
