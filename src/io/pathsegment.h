#pragma once
// 层：数据
// 把外部来源的名字（层位名、井名等，来自导入的分层表/解释文件）嵌进文件名
// 前的净化：任何路径分隔符、".."、Windows 保留字符与控制字符都替换为 '_'，
// 保证结果是单个安全路径段（满足 DataCatalog::isSafePathSegment 的口径，且
// 在 Windows 上也是合法文件名字符）。已安全的名字原样返回——既有产物文件名
// 不变。仅依赖 QtCore。
#include <QString>

namespace paleo {

inline QString safeFileNameComponent( const QString &raw )
{
  QString out;
  out.reserve( raw.size() );
  for ( const QChar c : raw )
  {
    const ushort u = c.unicode();
    const bool bad = u < 0x20 || u == 0x7F || c == QLatin1Char( '/' ) ||
                     c == QLatin1Char( '\\' ) || c == QLatin1Char( ':' ) ||
                     c == QLatin1Char( '*' ) || c == QLatin1Char( '?' ) ||
                     c == QLatin1Char( '"' ) || c == QLatin1Char( '<' ) ||
                     c == QLatin1Char( '>' ) || c == QLatin1Char( '|' );
    out.append( bad ? QLatin1Char( '_' ) : c );
  }
  while ( out.contains( QLatin1String( ".." ) ) )
    out.replace( QLatin1String( ".." ), QLatin1String( "_." ) );
  if ( out.isEmpty() || out == QLatin1String( "." ) )
    out = QStringLiteral( "_" );
  return out;
}

} // namespace paleo
