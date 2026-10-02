// 层：数据
#include "singlefactorrequest.h"

#include <QCryptographicHash>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

void appendString( const QString &text, QByteArray *out )
{
  out->append( '"' );
  const QByteArray utf8 = text.toUtf8();
  for ( unsigned char c : utf8 )
  {
    switch ( c )
    {
      case '"':
        out->append( "\\\"" );
        break;
      case '\\':
        out->append( "\\\\" );
        break;
      case '\b':
        out->append( "\\b" );
        break;
      case '\f':
        out->append( "\\f" );
        break;
      case '\n':
        out->append( "\\n" );
        break;
      case '\r':
        out->append( "\\r" );
        break;
      case '\t':
        out->append( "\\t" );
        break;
      default:
        if ( c < 0x20 )
          out->append( QStringLiteral( "\\u%1" ).arg( c, 4, 16, QLatin1Char( '0' ) ).toLatin1() );
        else
          out->append( static_cast<char>( c ) );
        break;
    }
  }
  out->append( '"' );
}

bool appendCanonical( const QVariant &value, QByteArray *out, QString *error )
{
  if ( !value.isValid() || value.isNull() )
  {
    out->append( "null" );
    return true;
  }
  const int type = value.typeId();
  if ( type == QMetaType::Bool )
  {
    out->append( value.toBool() ? "true" : "false" );
    return true;
  }
  if ( type == QMetaType::QString )
  {
    appendString( value.toString(), out );
    return true;
  }
  if ( type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong ||
       type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float )
  {
    const double number = value.toDouble();
    if ( !std::isfinite( number ) )
    {
      if ( error )
        *error = QStringLiteral( "parameter hash rejects non-finite numbers" );
      return false;
    }
    char buffer[64];
    const std::to_chars_result formatted =
        std::to_chars( buffer, buffer + sizeof( buffer ), number, std::chars_format::general, 17 );
    if ( formatted.ec != std::errc() )
    {
      if ( error )
        *error = QStringLiteral( "parameter number format failed" );
      return false;
    }
    out->append( buffer, static_cast<int>( formatted.ptr - buffer ) );
    return true;
  }
  if ( type == QMetaType::QVariantList )
  {
    out->append( '[' );
    const QVariantList list = value.toList();
    for ( int i = 0; i < list.size(); ++i )
    {
      if ( i > 0 )
        out->append( ',' );
      if ( !appendCanonical( list.at( i ), out, error ) )
        return false;
    }
    out->append( ']' );
    return true;
  }
  if ( type == QMetaType::QVariantMap )
  {
    out->append( '{' );
    const QVariantMap map = value.toMap();
    QStringList keys = map.keys();
    std::sort( keys.begin(), keys.end(), []( const QString &a, const QString &b ) {
      return a.toUtf8() < b.toUtf8();
    } );
    bool first = true;
    for ( const QString &key : keys )
    {
      if ( !first )
        out->append( ',' );
      first = false;
      appendString( key, out );
      out->append( ':' );
      if ( !appendCanonical( map.value( key ), out, error ) )
        return false;
    }
    out->append( '}' );
    return true;
  }
  if ( error )
    *error = QStringLiteral( "parameter hash cannot encode %1" ).arg( QLatin1String( value.typeName() ) );
  return false;
}

} // namespace

ParameterHash parameterHash( const QVariantMap &parameters )
{
  ParameterHash result;
  if ( !appendCanonical( QVariant( parameters ), &result.canonical, &result.error ) )
    return result;
  result.sha256 = QString::fromLatin1(
      QCryptographicHash::hash( result.canonical, QCryptographicHash::Sha256 ).toHex() );
  result.ok = true;
  return result;
}

} // namespace paleo::singlefactor
