#include "layouttemplates.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QDomDocument>
#include <QFileInfo>

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitempage.h>
#include <qgslayoutpagecollection.h>
#include <qgsreadwritecontext.h>

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
    { "a4_landscape", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A4 Landscape (297 × 210 mm)" ) },
    { "a4_portrait", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A4 Portrait (210 × 297 mm)" ) },
    { "a0_landscape", QT_TRANSLATE_NOOP( "PaleoLayoutTemplates", "A0 Landscape (1189 × 841 mm)" ) },
  };

  const BuiltinTemplate *builtinFor( const QString &key )
  {
    for ( const BuiltinTemplate &entry : kBuiltinTemplates )
    {
      if ( key == QLatin1String( entry.key ) )
        return &entry;
    }
    return nullptr;
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
  m_saveAction = new QAction( tr( "Save as &Template…" ), this );
  m_saveAction->setObjectName( QStringLiteral( "actionSaveLayoutTemplate" ) );
  connect( m_saveAction, &QAction::triggered, this, [this]
  {
    QgsLayout *layout = m_layoutProvider ? m_layoutProvider() : nullptr;
    if ( !layout )
    {
      emit templateFinished( QString(), false );
      return;
    }
    const QString path = QFileDialog::getSaveFileName( dialogParent( this ), tr( "Save Layout Template" ),
                                                       QString(), tr( "QGIS layout template (*.qpt)" ) );
    if ( path.isEmpty() )
      return; // user canceled — no signal
    saveTemplate( layout, path );
  } );

  m_loadAction = new QAction( tr( "&Load from Template…" ), this );
  m_loadAction->setObjectName( QStringLiteral( "actionLoadLayoutTemplate" ) );
  connect( m_loadAction, &QAction::triggered, this, [this]
  {
    QgsLayout *layout = m_layoutProvider ? m_layoutProvider() : nullptr;
    if ( !layout )
    {
      emit templateFinished( QString(), false );
      return;
    }
    const QString path = QFileDialog::getOpenFileName( dialogParent( this ), tr( "Load Layout Template" ),
                                                       QString(), tr( "QGIS layout template (*.qpt)" ) );
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

QString PaleoLayoutTemplates::builtinTitle( const QString &key )
{
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
    outcome.error = tr( "No layout to save as a template." );
    emit templateFinished( QString(), false );
    return outcome;
  }
  if ( path.isEmpty() )
  {
    outcome.error = tr( "No template file given." );
    emit templateFinished( QString(), false );
    return outcome;
  }

  outcome.itemCount = contentItemCount( layout );
  QgsReadWriteContext context;
  if ( !layout->saveAsTemplate( path, context ) )
  {
    outcome.ok = false;
    outcome.error = tr( "Could not write the template file %1." ).arg( path );
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
    outcome.error = tr( "No layout to load the template into." );
    emit templateFinished( QString(), false );
    return outcome;
  }

  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
  {
    outcome.error = tr( "Could not read the template file %1." ).arg( path );
    emit templateFinished( path, false );
    return outcome;
  }

  QDomDocument document;
  const QDomDocument::ParseResult parseResult = document.setContent( &file, QDomDocument::ParseOption::UseNamespaceProcessing );
  if ( !parseResult )
  {
    outcome.error = tr( "The template file %1 is not valid XML (line %2: %3)." )
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
    outcome.error = tr( "Could not load the template %1 into the layout." ).arg( path );
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
  if ( !builtinFor( key ) )
  {
    outcome.error = tr( "Unknown built-in template '%1'." ).arg( key );
    emit templateFinished( QString(), false );
    return outcome;
  }

  const QString dir = templatesDir();
  if ( dir.isEmpty() )
  {
    outcome.error = tr( "The built-in templates directory is not configured." );
    emit templateFinished( QString(), false );
    return outcome;
  }

  const QString path = QDir( dir ).absoluteFilePath( key + QStringLiteral( ".qpt" ) );
  if ( !QFile::exists( path ) )
  {
    outcome.error = tr( "The built-in template %1 was not found in %2." ).arg( key, dir );
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
  const BuiltinTemplate *entry = builtinFor( key );
  if ( !entry )
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
    applyBuiltin( layout, key );
  } );
  return action;
}
