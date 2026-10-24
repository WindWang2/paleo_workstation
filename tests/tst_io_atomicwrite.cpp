// review-fix-dataio：io/atomicwrite.h（QSaveFile 原子整写 helper）行为回归。
// 钉住三条承诺：正常写/覆盖内容逐字节正确且无暂存残留；短写/磁盘满必须报
// 失败（旧写点忽略 write() 返回值会在此返回 true）；失败时旧文件保持完好。
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../src/io/atomicwrite.h"

class TestIoAtomicWrite : public QObject
{
  Q_OBJECT
private slots:
  void writesNewFileAndReplacesExisting();
  void mkdirsCreatesParent();
  void diskFullIsReportedAsFailure();
  void failureKeepsOriginalIntact();
  void emptyPathRejected();
};

static QByteArray slurp( const QString &p )
{
  QFile f( p );
  if ( !f.open( QIODevice::ReadOnly ) )
    return QByteArray( "<unreadable>" );
  return f.readAll();
}

void TestIoAtomicWrite::writesNewFileAndReplacesExisting()
{
  QTemporaryDir tmp;
  QVERIFY( tmp.isValid() );
  const QString p = tmp.filePath( QStringLiteral( "a.json" ) );
  QString err;
  QVERIFY2( paleo::writeFileAtomic( p, "first", &err ), qPrintable( err ) );
  QCOMPARE( slurp( p ), QByteArray( "first" ) );
  QVERIFY2( paleo::writeJsonAtomic( p, QJsonDocument( QJsonObject{ { "k", 1 } } ), &err,
                                    QJsonDocument::Compact ),
            qPrintable( err ) );
  QCOMPARE( slurp( p ), QByteArray( "{\"k\":1}" ) );
  // 只剩目标文件，无 QSaveFile 暂存残留
  QCOMPARE( QDir( tmp.path() ).entryList( QDir::Files | QDir::Hidden ),
            QStringList{ QStringLiteral( "a.json" ) } );
}

void TestIoAtomicWrite::mkdirsCreatesParent()
{
  QTemporaryDir tmp;
  const QString p = tmp.filePath( QStringLiteral( "x/y/z.bin" ) );
  QString err;
  QVERIFY( !paleo::writeFileAtomic( p, "z", &err ) ); // 默认不建目录
  QVERIFY( !err.isEmpty() );
  QVERIFY2( paleo::writeFileAtomic( p, "z", &err, true ), qPrintable( err ) );
  QCOMPARE( slurp( p ), QByteArray( "z" ) );
}

void TestIoAtomicWrite::diskFullIsReportedAsFailure()
{
#ifndef Q_OS_LINUX
  QSKIP( "需要 /dev/full" );
#else
  if ( !QFileInfo::exists( QStringLiteral( "/dev/full" ) ) )
    QSKIP( "no /dev/full" );
  // 对照：旧写法（QFile 打开后忽略 write() 返回值）在磁盘满时「成功」。
  {
    QFile legacy( QStringLiteral( "/dev/full" ) );
    QVERIFY( legacy.open( QIODevice::WriteOnly ) );
    const QByteArray big( 1 << 16, 'x' );
    // 大块直写在 write() 即失败，小块在 flush() 失败——两者都必须查。
    const bool legacyOk = legacy.write( big ) == big.size() && legacy.flush();
    QVERIFY( !legacyOk ); // 数据确实没落下
  }
  QString err;
  QVERIFY( !paleo::writeFileAtomic( QStringLiteral( "/dev/full" ), QByteArray( 1 << 16, 'x' ), &err ) );
  QVERIFY( !err.isEmpty() );
#endif
}

void TestIoAtomicWrite::failureKeepsOriginalIntact()
{
#ifdef Q_OS_WIN
  QSKIP( "POSIX 目录权限语义" );
#else
  QTemporaryDir tmp;
  const QString dir = tmp.filePath( QStringLiteral( "ro" ) );
  QVERIFY( QDir().mkpath( dir ) );
  const QString p = dir + QStringLiteral( "/keep.json" );
  QVERIFY( paleo::writeFileAtomic( p, "original" ) );
  QFile::setPermissions( dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner );
  QFile probe( dir + QStringLiteral( "/probe" ) );
  if ( probe.open( QIODevice::WriteOnly ) ) // root 等无视权限的环境
  {
    probe.close();
    probe.remove();
    QFile::setPermissions( dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner );
    QSKIP( "目录权限不生效（root？）" );
  }
  QString err;
  const bool ok = paleo::writeFileAtomic( p, "replacement", &err );
  QFile::setPermissions( dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner );
  QVERIFY( !ok );
  QVERIFY( !err.isEmpty() );
  QCOMPARE( slurp( p ), QByteArray( "original" ) );
#endif
}

void TestIoAtomicWrite::emptyPathRejected()
{
  QString err;
  QVERIFY( !paleo::writeFileAtomic( QString(), "x", &err ) );
  QVERIFY( !err.isEmpty() );
}

QTEST_GUILESS_MAIN( TestIoAtomicWrite )
#include "tst_io_atomicwrite.moc"
