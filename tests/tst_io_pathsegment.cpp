// review-fix-dataio：io/pathsegment.h safeFileNameComponent——外部层位名嵌入
// 文件名前的净化。结果必须恒为 DataCatalog::isSafePathSegment 意义下的单个
// 安全段；已安全的名字原样保留（既有产物文件名不变）。
#include <QtTest>

#include "../src/io/pathsegment.h"

class TestIoPathSegment : public QObject
{
  Q_OBJECT
private slots:
  void safeNamesUnchanged_data();
  void safeNamesUnchanged();
  void unsafeNamesNeutralised_data();
  void unsafeNamesNeutralised();
};

void TestIoPathSegment::safeNamesUnchanged_data()
{
  QTest::addColumn<QString>( "name" );
  QTest::newRow( "ascii" ) << QStringLiteral( "T4" );
  QTest::newRow( "cjk" ) << QStringLiteral( "沙三段_上" );
  QTest::newRow( "dot" ) << QStringLiteral( "Es3.1" );
  QTest::newRow( "space" ) << QStringLiteral( "Top K1" );
}

void TestIoPathSegment::safeNamesUnchanged()
{
  QFETCH( QString, name );
  QCOMPARE( paleo::safeFileNameComponent( name ), name );
}

void TestIoPathSegment::unsafeNamesNeutralised_data()
{
  QTest::addColumn<QString>( "name" );
  QTest::newRow( "traversal" ) << QStringLiteral( "T4/../../x" );
  QTest::newRow( "backslash" ) << QStringLiteral( "..\\..\\evil" );
  QTest::newRow( "dotdot" ) << QStringLiteral( ".." );
  QTest::newRow( "tripledot" ) << QStringLiteral( "..." );
  QTest::newRow( "dot" ) << QStringLiteral( "." );
  QTest::newRow( "empty" ) << QString();
  QTest::newRow( "nul" ) << QString( QStringLiteral( "a" ) + QChar( 0 ) + QStringLiteral( "b" ) );
  QTest::newRow( "newline" ) << QStringLiteral( "a\nb" );
  QTest::newRow( "drive" ) << QStringLiteral( "C:evil" );
  QTest::newRow( "winreserved" ) << QStringLiteral( "a*b?c\"d<e>f|g" );
}

void TestIoPathSegment::unsafeNamesNeutralised()
{
  QFETCH( QString, name );
  const QString out = paleo::safeFileNameComponent( name );
  QVERIFY( !out.isEmpty() );
  QVERIFY( out != QLatin1String( "." ) );
  QVERIFY2( !out.contains( QLatin1String( ".." ) ), qPrintable( out ) );
  for ( const QChar c : out )
  {
    QVERIFY2( QStringLiteral( "/\\:*?\"<>|" ).indexOf( c ) < 0, qPrintable( out ) );
    QVERIFY( c.unicode() >= 0x20 && c.unicode() != 0x7F );
  }
}

QTEST_GUILESS_MAIN( TestIoPathSegment )
#include "tst_io_pathsegment.moc"
