#include "types.h"

#include <algorithm>

// domain/ — pure Qt value types, NO Qgs* (§25).
// QVariantMap persistence conventions:
//   - keys are lower_snake_case ("layer_id", "target_facies_code", ...)
//   - QColor stored as hex string via name(QColor::HexArgb) -> "#AARRGGBB"
//   - enums stored as ints
//   - absent optional fields deserialize to header defaults

static QString colorToString( const QColor &c )
{
  return c.isValid() ? c.name( QColor::HexArgb ) : QString();
}

static QColor colorFromString( const QVariant &v )
{
  const QString s = v.toString();
  return s.isEmpty() ? QColor() : QColor::fromString( s );
}

QVariantMap Horizon::toMap() const
{
  return {
    { QStringLiteral( "id" ),    id },
    { QStringLiteral( "name" ),  name },
    { QStringLiteral( "order" ), order },
    { QStringLiteral( "color" ), colorToString( color ) },
  };
}

Horizon Horizon::fromMap( const QVariantMap &m )
{
  Horizon h;
  h.id    = m.value( QStringLiteral( "id" ) ).toString();
  h.name  = m.value( QStringLiteral( "name" ) ).toString();
  h.order = m.value( QStringLiteral( "order" ), 0 ).toInt();
  h.color = colorFromString( m.value( QStringLiteral( "color" ) ) );
  return h;
}

QVariantMap Facies::toMap() const
{
  return {
    { QStringLiteral( "code" ),  code },
    { QStringLiteral( "name" ),  name },
    { QStringLiteral( "color" ), colorToString( color ) },
  };
}

Facies Facies::fromMap( const QVariantMap &m )
{
  Facies f;
  f.code  = m.value( QStringLiteral( "code" ), 0 ).toInt();
  f.name  = m.value( QStringLiteral( "name" ) ).toString();
  f.color = colorFromString( m.value( QStringLiteral( "color" ) ) );
  return f;
}

QVariantMap Constraint::toMap() const
{
  return {
    { QStringLiteral( "id" ),                 id },
    { QStringLiteral( "type" ),               type },
    { QStringLiteral( "wkt" ),                wkt },
    { QStringLiteral( "target_facies_code" ), targetFaciesCode },
    { QStringLiteral( "weight" ),             weight },
  };
}

Constraint Constraint::fromMap( const QVariantMap &m )
{
  Constraint c;
  c.id               = m.value( QStringLiteral( "id" ) ).toString();
  c.type             = m.value( QStringLiteral( "type" ) ).toString();
  c.wkt              = m.value( QStringLiteral( "wkt" ) ).toString();
  c.targetFaciesCode = m.value( QStringLiteral( "target_facies_code" ), -1 ).toInt();
  c.weight           = m.value( QStringLiteral( "weight" ), 1.0 ).toDouble();
  return c;
}

QVariantMap DataAsset::toMap() const
{
  return {
    { QStringLiteral( "id" ),      id },
    { QStringLiteral( "kind" ),    kind },
    { QStringLiteral( "uri" ),     uri },
    { QStringLiteral( "crs" ),     crs },
    { QStringLiteral( "horizon" ), horizon },
  };
}

DataAsset DataAsset::fromMap( const QVariantMap &m )
{
  DataAsset a;
  a.id      = m.value( QStringLiteral( "id" ) ).toString();
  a.kind    = m.value( QStringLiteral( "kind" ) ).toString();
  a.uri     = m.value( QStringLiteral( "uri" ) ).toString();
  a.crs     = m.value( QStringLiteral( "crs" ) ).toString();
  a.horizon = m.value( QStringLiteral( "horizon" ) ).toString();
  return a;
}

QVariantMap ValidationIssue::toMap() const
{
  QVariantMap m = {
    { QStringLiteral( "severity" ),     static_cast<int>( severity ) },
    { QStringLiteral( "code" ),         code },
    { QStringLiteral( "message" ),      message },
    { QStringLiteral( "layer_id" ),     layerId },
    { QStringLiteral( "horizon" ),      horizon },
    { QStringLiteral( "wkt_location" ), wktLocation },
  };
  if ( !wellId.isEmpty() ) // mapping-pipeline fields stay optional for old rows
    m.insert( QStringLiteral( "well_id" ), wellId );
  if ( !details.isEmpty() )
    m.insert( QStringLiteral( "details" ), details );
  return m;
}

ValidationIssue ValidationIssue::fromMap( const QVariantMap &m )
{
  ValidationIssue v;
  const int sev = m.value( QStringLiteral( "severity" ), static_cast<int>( Warning ) ).toInt();
  v.severity    = ( sev >= Info && sev <= Error )
                    ? static_cast<Severity>( sev )
                    : Warning;
  v.code        = m.value( QStringLiteral( "code" ) ).toString();
  v.message     = m.value( QStringLiteral( "message" ) ).toString();
  v.layerId     = m.value( QStringLiteral( "layer_id" ) ).toString();
  v.horizon     = m.value( QStringLiteral( "horizon" ) ).toString();
  v.wktLocation = m.value( QStringLiteral( "wkt_location" ) ).toString();
  v.wellId      = m.value( QStringLiteral( "well_id" ) ).toString();
  v.details     = m.value( QStringLiteral( "details" ) ).toMap();
  return v;
}

// §34 release semantics — "maj.min.patch[-label]", label omitted when empty.
QString Version::toString() const
{
  QString s = QStringLiteral( "%1.%2.%3" ).arg( major ).arg( minor ).arg( patch );
  if ( !label.isEmpty() )
    s += QLatin1Char( '-' ) + label;
  return s;
}

Version Version::parse( const QString &s, bool *ok )
{
  Version v;
  bool good = false;

  const int dash = s.indexOf( QLatin1Char( '-' ) );
  const QString core = dash < 0 ? s : s.left( dash );
  const QStringList parts = core.split( QLatin1Char( '.' ) );

  if ( parts.size() == 3 )
  {
    const auto isDigits = []( const QString &p ) {
      return !p.isEmpty() &&
             std::all_of( p.begin(), p.end(), []( QChar c ) { return c.isDigit(); } );
    };
    bool okMaj = false, okMin = false, okPat = false;
    const int maj = parts.at( 0 ).toInt( &okMaj );
    const int min = parts.at( 1 ).toInt( &okMin );
    const int pat = parts.at( 2 ).toInt( &okPat );
    if ( isDigits( parts.at( 0 ) ) && isDigits( parts.at( 1 ) ) && isDigits( parts.at( 2 ) ) &&
         okMaj && okMin && okPat )
    {
      v.major = maj;
      v.minor = min;
      v.patch = pat;
      if ( dash >= 0 )
        v.label = s.mid( dash + 1 );
      good = true;
    }
  }

  if ( ok )
    *ok = good;
  return v;
}
