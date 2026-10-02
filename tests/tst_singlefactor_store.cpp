// 层：数据（测试壳位于 tests/，被测对象为约束参数存储）
#include <QtTest/QtTest>

#include "io/constraintstore.h"

#include <gdal.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

class SingleFactorStoreTests : public QObject
{
  Q_OBJECT
  private slots:
    void extendedParametersReopen();
    void readOnlyEnqueueCreatesNothing();
    void missingFileIsNotCreated();
};

void SingleFactorStoreTests::extendedParametersReopen()
{
  GDALAllRegister();
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "constraints.gpkg" ) );
  ConstraintStore store( path );

  QString err;
  QVERIFY2( store.append( QStringLiteral( "D61" ), QStringLiteral( "old" ),
                           QStringLiteral( "LINESTRING(0 0, 1 0)" ), QStringLiteral( "break_line" ), 0, &err ),
            qPrintable( err ) );
  const QString json = QStringLiteral( "{\"schemaVersion\":1,\"semantic\":\"interpretive_boundary\",\"softStrength\":0.35}" );
  QVERIFY2( store.appendExtended( QStringLiteral( "D61" ), QStringLiteral( "soft" ),
                                   QStringLiteral( "LINESTRING(2 0, 2 4)" ),
                                   QStringLiteral( "interpretive_boundary" ), 0, json, 1, &err ),
            qPrintable( err ) );

  const QVector<QVariantMap> first = store.load( QStringLiteral( "D61" ) );
  QCOMPARE( first.size(), 2 );
  QVERIFY( !first[0].contains( QStringLiteral( "params_json" ) ) );
  QCOMPARE( first[1].value( QStringLiteral( "params_json" ) ).toString(), json );
  QCOMPARE( first[1].value( QStringLiteral( "schema_version" ) ).toInt(), 1 );
  QCOMPARE( first[1].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "interpretive_boundary" ) );

  const QString updated = QStringLiteral( "{\"schemaVersion\":1,\"semantic\":\"interpretive_boundary\",\"softStrength\":0.2}" );
  QVERIFY2( store.updateParameters( QStringLiteral( "soft" ), updated, 1, QString(), &err ), qPrintable( err ) );
  QVERIFY2( store.updateParameters( QStringLiteral( "soft" ), updated, 1, QString(), &err ), qPrintable( err ) );
  const QVector<QVariantMap> again = store.load( QStringLiteral( "D61" ) );
  QCOMPARE( again[1].value( QStringLiteral( "params_json" ) ).toString(), updated );
  QCOMPARE( again[0].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "break_line" ) );

  QVERIFY( !store.updateParameters( QStringLiteral( "missing" ), updated, 1, QString(), &err ) );
  QCOMPARE( store.load( QStringLiteral( "D61" ) ).size(), 2 );

  const QFileDevice::Permissions original = QFile::permissions( path );
  QVERIFY( QFile::setPermissions( path, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther ) );
  QVERIFY( !store.updateParameters( QStringLiteral( "soft" ), QStringLiteral( "{}" ), 1, QStringLiteral( "break_line" ),
                                     &err ) );
  QVERIFY( QFile::setPermissions( path, original ) );
  const QVector<QVariantMap> after = store.load( QStringLiteral( "D61" ) );
  QCOMPARE( after[1].value( QStringLiteral( "params_json" ) ).toString(), updated );
  QCOMPARE( after[1].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "interpretive_boundary" ) );
}

void SingleFactorStoreTests::readOnlyEnqueueCreatesNothing()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "absent.gpkg" ) );
  ConstraintStore store( path, []( const ConstraintStore::WriteFn & ) {
    return PaleoProjectStore::WriteResult{ false, QStringLiteral( "read only" ) };
  } );
  QString err;
  QVERIFY( !store.appendExtended( QStringLiteral( "D61" ), QStringLiteral( "a" ),
                                   QStringLiteral( "LINESTRING(0 0, 1 1)" ), QStringLiteral( "break_line" ), 0,
                                   QStringLiteral( "{}" ), 1, &err ) );
  QCOMPARE( err, QStringLiteral( "read only" ) );
  QVERIFY( !QFile::exists( path ) );
}

void SingleFactorStoreTests::missingFileIsNotCreated()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "missing.gpkg" ) );
  ConstraintStore store( path );
  QString err;
  QVERIFY( !store.updateParameters( QStringLiteral( "a" ), QStringLiteral( "{}" ), 1, QString(), &err ) );
  QVERIFY( !QFile::exists( path ) );
  QVERIFY( err.contains( QStringLiteral( "does not exist" ) ) );
}

QTEST_MAIN( SingleFactorStoreTests )
#include "tst_singlefactor_store.moc"
