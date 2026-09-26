#include "manifestprojection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <qgsproject.h>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

  // Stable field names — part of the persisted .qgz contract, do not rename.
  QJsonObject declToJson( const LayerDeclaration &d )
  {
    QJsonObject o;
    o.insert( QStringLiteral( "layerId" ), d.layerId );
    o.insert( QStringLiteral( "horizon" ), d.horizon );
    o.insert( QStringLiteral( "type" ), d.type );
    o.insert( QStringLiteral( "source" ), d.source );
    o.insert( QStringLiteral( "styleRef" ), d.styleRef );
    o.insert( QStringLiteral( "group" ), d.group );
    if ( !d.title.isEmpty() )
      o.insert( QStringLiteral( "title" ), d.title );
    // 'instantiated' deliberately omitted — runtime-only state (§37).
    return o;
  }

  LayerDeclaration jsonToDecl( const QJsonObject &o )
  {
    LayerDeclaration d;
    d.layerId = o.value( QStringLiteral( "layerId" ) ).toString();
    d.horizon = o.value( QStringLiteral( "horizon" ) ).toString();
    d.type = o.value( QStringLiteral( "type" ) ).toString();
    d.source = o.value( QStringLiteral( "source" ) ).toString();
    d.styleRef = o.value( QStringLiteral( "styleRef" ) ).toString();
    d.group = o.value( QStringLiteral( "group" ) ).toString();
    d.title = o.value( QStringLiteral( "title" ) ).toString();
    d.instantiated = false;
    return d;
  }
} // namespace

bool ManifestProjection::embedDeclarations( QgsProject *project,
                                            const QVector<LayerDeclaration> &decls,
                                            QString *error )
{
  if ( !project )
  {
    setError( error, QStringLiteral( "cannot embed manifest declarations into a null QgsProject" ) );
    return false;
  }

  QJsonArray arr;
  for ( const LayerDeclaration &d : decls )
    arr.append( declToJson( d ) );

  // Empty declared set still writes "[]" so a saved project carries an
  // explicit (not merely absent) empty manifest.
  const QString json = QString::fromUtf8(
    QJsonDocument( arr ).toJson( QJsonDocument::Compact ) );

  if ( !project->writeEntry( scope(), key(), json ) )
  {
    setError( error, QStringLiteral( "QgsProject::writeEntry refused the manifest payload" ) );
    return false;
  }
  return true;
}

QVector<LayerDeclaration> ManifestProjection::extractDeclarations( const QgsProject *project )
{
  QVector<LayerDeclaration> out;
  if ( !project )
    return out;

  bool ok = false;
  const QString json = project->readEntry( scope(), key(), QString(), &ok );
  if ( !ok || json.isEmpty() )
    return out;

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8(), &parseError );
  if ( parseError.error != QJsonParseError::NoError || !doc.isArray() )
    return out; // malformed payload — caller treats it as "nothing declared"

  const QJsonArray arr = doc.array();
  out.reserve( arr.size() );
  for ( const QJsonValue &v : arr )
    if ( v.isObject() )
      out.append( jsonToDecl( v.toObject() ) );
  return out;
}
