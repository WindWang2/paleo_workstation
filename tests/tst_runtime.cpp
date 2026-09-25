#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>

#include "../src/qgis/qgisruntime.h"
#include "../src/metadata/paleoprojectstore.h"

// P0 spine: QgisRuntime owns init-order; PaleoProjectStore is the sole write
// choke point (§41.2) — serialization, saveAll ordering (gpkg commit -> .qgz
// backup -> .qgz write), per-layer busy registry.
class TestRuntime : public QObject
{
  Q_OBJECT
private slots:
  void runtimeInitialized()
  {
    QVERIFY( QgisRuntime::isInitialized() );
    QVERIFY2( QgisRuntime::providerCount() > 0,
              qPrintable( QString::number( QgisRuntime::providerCount() ) ) );
    QVERIFY2( QFile::exists( QgisRuntime::srsDbPath() ),
              qPrintable( QgisRuntime::srsDbPath() ) );
  }

  void initializeReturnsFalseWhenAlreadyUp()
  {
    // main() already initialized via QgisRuntime — second call must refuse.
    QVERIFY( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) );
  }

  void enqueueWriteSerializesAcrossThreads()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore store;

    QStringList order;                       // appended under the write mutex
    std::atomic<bool> aInside { false };
    std::atomic<bool> aOk { false }, bOk { false };

    // Thread A takes the write mutex first and holds it (sleep inside fn);
    // thread B waits until A is inside, then enqueues. If the mutex is real,
    // B's write can only land after A finishes -> order must be {A, B}.
    QThread *tA = QThread::create( [&] {
      const auto r = store.enqueueWrite( [&]() -> PaleoProjectStore::WriteResult {
        aInside = true;
        QThread::msleep( 50 );               // hold the lock; B must queue
        order << QStringLiteral( "A" );
        QFile f( dir.filePath( QStringLiteral( "a.out" ) ) );
        if ( !f.open( QIODevice::WriteOnly ) )
          return { false, QStringLiteral( "open a" ) };
        f.write( "A" );
        return { true, QString() };
      } );
      aOk = r.ok;
    } );
    QThread *tB = QThread::create( [&] {
      while ( !aInside.load() )
        QThread::msleep( 1 );
      const auto r = store.enqueueWrite( [&]() -> PaleoProjectStore::WriteResult {
        order << QStringLiteral( "B" );
        QFile f( dir.filePath( QStringLiteral( "b.out" ) ) );
        if ( !f.open( QIODevice::WriteOnly ) )
          return { false, QStringLiteral( "open b" ) };
        f.write( "B" );
        return { true, QString() };
      } );
      bOk = r.ok;
    } );

    tA->start();
    tB->start();
    QVERIFY( tA->wait( 10000 ) );
    QVERIFY( tB->wait( 10000 ) );
    delete tA;
    delete tB;

    QVERIFY( aOk );
    QVERIFY( bOk );
    QCOMPARE( order, ( QStringList { QStringLiteral( "A" ), QStringLiteral( "B" ) } ) );
    QCOMPARE( QFile( dir.filePath( QStringLiteral( "a.out" ) ) ).size(), qint64( 1 ) );
    QCOMPARE( QFile( dir.filePath( QStringLiteral( "b.out" ) ) ).size(), qint64( 1 ) );
  }

  void saveAllQgzFailureKeepsGpkgCommit()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );
    const QString gpkg = dir.filePath( QStringLiteral( "proj.gpkg" ) );
    const QString meta = dir.filePath( QStringLiteral( "meta.sqlite" ) );

    PaleoProjectStore store;
    store.setProjectPaths( qgz, gpkg, meta );
    QCOMPARE( store.gpkgPath(), gpkg );
    QCOMPARE( store.metaDbPath(), meta );
    QSignalSpy completedSpy( &store, &PaleoProjectStore::writeCompleted );
    QSignalSpy failedSpy( &store, &PaleoProjectStore::writeFailed );

    const auto gpkgCommit = [&]() -> PaleoProjectStore::WriteResult {
      QFile f( gpkg );
      if ( !f.open( QIODevice::WriteOnly ) )
        return { false, QStringLiteral( "gpkg open failed" ) };
      f.write( "gpkg-v1" );
      return { true, QString() };
    };
    const auto badQgz = []() -> PaleoProjectStore::WriteResult {
      return { false, QStringLiteral( "qgz write boom" ) };
    };

    const auto r = store.saveAll( gpkgCommit, badQgz );

    // Contract: .qgz failure does NOT roll back the gpkg commit — the data
    // state is authoritative; the error must surface to the caller.
    QVERIFY( !r.ok );
    QVERIFY2( r.error.contains( QStringLiteral( "boom" ) ), qPrintable( r.error ) );
    QVERIFY2( QFile::exists( gpkg ), "gpkg commit must survive qgz failure" );
    QCOMPARE( QFile( gpkg ).size(), qint64( 7 ) );
    QVERIFY( !QFile::exists( qgz + QStringLiteral( ".bak" ) ) ); // no .qgz existed to back up

    QCOMPARE( completedSpy.size(), 1 );
    QCOMPARE( completedSpy.at( 0 ).at( 0 ).toString(), gpkg );
    QCOMPARE( failedSpy.size(), 1 );
    QCOMPARE( failedSpy.at( 0 ).at( 0 ).toString(), qgz );
    QVERIFY( failedSpy.at( 0 ).at( 1 ).toString().contains( QStringLiteral( "boom" ) ) );
  }

  void saveAllBacksUpExistingQgz()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );
    const QString gpkg = dir.filePath( QStringLiteral( "proj.gpkg" ) );

    PaleoProjectStore store;
    store.setProjectPaths( qgz, gpkg, dir.filePath( QStringLiteral( "meta.sqlite" ) ) );

    const auto writeFile = []( const QString &path, const QByteArray &bytes ) -> PaleoProjectStore::WriteResult {
      QFile f( path );
      if ( !f.open( QIODevice::WriteOnly ) )
        return { false, path };
      f.write( bytes );
      return { true, QString() };
    };

    // First save: nothing to back up yet.
    auto r = store.saveAll( [&] { return writeFile( gpkg, "g1" ); },
                            [&] { return writeFile( qgz, "q1" ); } );
    QVERIFY2( r.ok, qPrintable( r.error ) );
    QVERIFY( !QFile::exists( qgz + QStringLiteral( ".bak" ) ) );

    // Second save: pre-existing .qgz is copied to .qgz.bak BEFORE rewrite.
    r = store.saveAll( [&] { return writeFile( gpkg, "g2" ); },
                       [&] { return writeFile( qgz, "q2" ); } );
    QVERIFY2( r.ok, qPrintable( r.error ) );

    QFile bak( qgz + QStringLiteral( ".bak" ) );
    QVERIFY( bak.open( QIODevice::ReadOnly ) );
    QCOMPARE( bak.readAll(), QByteArray( "q1" ) ); // backup = previous generation

    QFile cur( qgz );
    QVERIFY( cur.open( QIODevice::ReadOnly ) );
    QCOMPARE( cur.readAll(), QByteArray( "q2" ) );
  }

  void saveAllGpkgFailureAborts()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );

    PaleoProjectStore store;
    store.setProjectPaths( qgz, dir.filePath( QStringLiteral( "proj.gpkg" ) ), QString() );

    bool qgzRan = false;
    const auto r = store.saveAll(
      []() -> PaleoProjectStore::WriteResult { return { false, QStringLiteral( "gpkg busy" ) }; },
      [&]() -> PaleoProjectStore::WriteResult { qgzRan = true; return { true, QString() }; } );

    QVERIFY( !r.ok );
    QVERIFY2( r.error.contains( QStringLiteral( "busy" ) ), qPrintable( r.error ) );
    QVERIFY( !qgzRan );                        // sequence aborted before .qgz
    QVERIFY( !QFile::exists( qgz ) );
  }

  void busyRegistryIsThreadSafe()
  {
    PaleoProjectStore store;
    QString reason;
    QVERIFY( !store.layerBusy( QStringLiteral( "facies.C6" ), &reason ) );

    store.markLayerBusy( QStringLiteral( "facies.C6" ), QStringLiteral( "task-1" ), tr( "interpolation running" ) );
    QVERIFY( store.layerBusy( QStringLiteral( "facies.C6" ), &reason ) );
    QCOMPARE( reason, QStringLiteral( "task-1 — interpolation running" ) ); // "taskId — reason" contract
    store.markLayerFree( QStringLiteral( "facies.C6" ) );
    QVERIFY( !store.layerBusy( QStringLiteral( "facies.C6" ) ) );

    // Hammer the registry from threads — must not corrupt or crash.
    QList<QThread *> threads;
    for ( int i = 0; i < 8; ++i )
      threads << QThread::create( [&store, i] {
        const QString id = QStringLiteral( "L%1" ).arg( i );
        for ( int j = 0; j < 200; ++j )
        {
          store.markLayerBusy( id, QStringLiteral( "t" ), QStringLiteral( "r" ) );
          store.layerBusy( id );
          store.markLayerFree( id );
        }
      } );
    for ( QThread *t : threads )
      t->start();
    for ( QThread *t : threads )
    {
      QVERIFY( t->wait( 10000 ) );
      delete t;
    }
    QVERIFY( !store.layerBusy( QStringLiteral( "L0" ) ) );
  }
};

int main( int argc, char *argv[] )
{
  // Runtime owns the whole init sequence — no QgsApplication in test mains.
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestRuntime tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_runtime.moc"
