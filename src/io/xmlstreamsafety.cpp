// 层：数据
#include "xmlstreamsafety.h"

namespace paleo::io::xmlsafe
{

namespace
{
QString dtdRejection( const QString &source )
{
  const QString reason =
      QStringLiteral( "XML 含 DTD/实体声明，为防实体扩展攻击已拒收" );
  return source.isEmpty() ? reason : QStringLiteral( "%1：%2" ).arg( source, reason );
}
} // namespace

bool rejectDtdToken( QXmlStreamReader::TokenType token, const QString &source, QString *error )
{
  if ( token != QXmlStreamReader::DTD )
    return false;
  if ( error )
    *error = dtdRejection( source );
  return true;
}

bool advanceToRootElement( QXmlStreamReader &reader, const QString &source, QString *error )
{
  while ( !reader.atEnd() )
  {
    const QXmlStreamReader::TokenType token = reader.readNext();
    if ( rejectDtdToken( token, source, error ) )
      return false;
    if ( token == QXmlStreamReader::StartElement )
      return true;
  }
  if ( error )
    *error = reader.hasError() ? parseErrorMessage( reader, source )
                               : QStringLiteral( "%1：没有根元素（空文档）" ).arg( source );
  return false;
}

QString parseErrorMessage( const QXmlStreamReader &reader, const QString &source )
{
  const QString located = QStringLiteral( "XML 解析错误（行 %1:列 %2）：%3" )
                              .arg( reader.lineNumber() )
                              .arg( reader.columnNumber() )
                              .arg( reader.errorString() );
  return source.isEmpty() ? located : QStringLiteral( "%1：%2" ).arg( source, located );
}

} // namespace paleo::io::xmlsafe
