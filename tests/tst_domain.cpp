#include <QtTest>

#include "../src/domain/types.h"

// domain/ value types — pure Qt, no QGIS init needed (§25).
// Contract: every field roundtrips through QVariantMap (project.sqlite JSON
// persistence), colors keep alpha (HexArgb), enums serialize as ints, absent
// optional fields fall back to header defaults.
class TestDomainTypes : public QObject
{
  Q_OBJECT
private slots:
  void horizonRoundtripPreservesAllFields()
  {
    Horizon h;
    h.id    = QStringLiteral( "T1" );
    h.name  = QStringLiteral( "顶面" );
    h.order = 3;
    h.color = QColor( 200, 30, 40, 128 ); // non-opaque alpha must survive

    const QVariantMap m = h.toMap();
    QCOMPARE( m.value( QStringLiteral( "id" ) ).toString(),    h.id );
    QCOMPARE( m.value( QStringLiteral( "name" ) ).toString(),  h.name );
    QCOMPARE( m.value( QStringLiteral( "order" ) ).toInt(),    h.order );
    QCOMPARE( m.value( QStringLiteral( "color" ) ).toString(), h.color.name( QColor::HexArgb ) );

    const Horizon r = Horizon::fromMap( m );
    QCOMPARE( r.id,    h.id );
    QCOMPARE( r.name,  h.name );
    QCOMPARE( r.order, h.order );
    QCOMPARE( r.color, h.color );
    QCOMPARE( r.color.alpha(), 128 );
    QCOMPARE( r.toMap(), m ); // map-level fixpoint
  }

  void faciesRoundtripPreservesAllFields()
  {
    Facies f;
    f.code  = 6;
    f.name  = QStringLiteral( "河道" );
    f.color = QColor( 10, 220, 60, 77 );

    const QVariantMap m = f.toMap();
    QCOMPARE( m.value( QStringLiteral( "code" ) ).toInt(),     f.code );
    QCOMPARE( m.value( QStringLiteral( "color" ) ).toString(), f.color.name( QColor::HexArgb ) );

    const Facies r = Facies::fromMap( m );
    QCOMPARE( r.code,          f.code );
    QCOMPARE( r.name,          f.name );
    QCOMPARE( r.color,         f.color );
    QCOMPARE( r.color.alpha(), 77 );
    QCOMPARE( r.toMap(), m );
  }

  void constraintRoundtripPreservesAllFields()
  {
    Constraint c;
    c.id               = QStringLiteral( "C-9" );
    c.type             = QStringLiteral( "line" );
    c.wkt              = QStringLiteral( "LINESTRING(0 0, 10 10)" );
    c.targetFaciesCode = 4;
    c.weight           = 2.5;

    const Constraint r = Constraint::fromMap( c.toMap() );
    QCOMPARE( r.id,               c.id );
    QCOMPARE( r.type,             c.type );
    QCOMPARE( r.wkt,              c.wkt );
    QCOMPARE( r.targetFaciesCode, c.targetFaciesCode );
    QCOMPARE( r.weight,           c.weight );
  }

  void constraintMissingOptionalFieldsGetDefaults()
  {
    // Minimal map: only the required string fields present.
    const QVariantMap m {
      { QStringLiteral( "id" ),   QStringLiteral( "C-1" ) },
      { QStringLiteral( "type" ), QStringLiteral( "point" ) },
      { QStringLiteral( "wkt" ),  QStringLiteral( "POINT(5 5)" ) },
    };
    const Constraint c = Constraint::fromMap( m );
    QCOMPARE( c.id,               QStringLiteral( "C-1" ) );
    QCOMPARE( c.targetFaciesCode, -1 );   // header default
    QCOMPARE( c.weight,           1.0 );  // header default
  }

  void dataAssetRoundtripPreservesAllFields()
  {
    DataAsset a;
    a.id      = QStringLiteral( "well-01" );
    a.kind    = QStringLiteral( "well" );
    a.uri     = QStringLiteral( "/data/wells/w01.las" );
    a.crs     = QStringLiteral( "EPSG:4326" );
    a.horizon = QStringLiteral( "T1" );

    const DataAsset r = DataAsset::fromMap( a.toMap() );
    QCOMPARE( r.id,      a.id );
    QCOMPARE( r.kind,    a.kind );
    QCOMPARE( r.uri,     a.uri );
    QCOMPARE( r.crs,     a.crs );
    QCOMPARE( r.horizon, a.horizon );

    // Empty optional horizon must stay empty.
    a.horizon.clear();
    QCOMPARE( DataAsset::fromMap( a.toMap() ).horizon, QString() );
  }

  void validationIssueRoundtripPreservesSeverity()
  {
    for ( const ValidationIssue::Severity sev :
          { ValidationIssue::Info, ValidationIssue::Warning, ValidationIssue::Error } )
    {
      ValidationIssue v;
      v.severity    = sev;
      v.code        = QStringLiteral( "DUP_HORIZON_NAME" );
      v.message     = QStringLiteral( "duplicate horizon name" );
      v.layerId     = QStringLiteral( "L-77" );
      v.horizon     = QStringLiteral( "T2" );
      v.wktLocation = QStringLiteral( "POINT(1 2)" );

      const QVariantMap m = v.toMap();
      QCOMPARE( m.value( QStringLiteral( "severity" ) ).toInt(), static_cast<int>( sev ) );

      const ValidationIssue r = ValidationIssue::fromMap( m );
      QCOMPARE( r.severity,    sev );
      QCOMPARE( r.code,        v.code );
      QCOMPARE( r.message,     v.message );
      QCOMPARE( r.layerId,     v.layerId );
      QCOMPARE( r.horizon,     v.horizon );
      QCOMPARE( r.wktLocation, v.wktLocation );
    }
  }

  void validationIssueMissingSeverityDefaultsToWarning()
  {
    const ValidationIssue v = ValidationIssue::fromMap(
      { { QStringLiteral( "code" ), QStringLiteral( "X" ) } } );
    QCOMPARE( v.severity, ValidationIssue::Warning );
    QCOMPARE( v.layerId, QString() );
    QCOMPARE( v.wktLocation, QString() );
  }

  void versionToStringOmitsEmptyLabel()
  {
    Version v { 1, 2, 3, QString() };
    QCOMPARE( v.toString(), QStringLiteral( "1.2.3" ) );

    v.label = QStringLiteral( "draft" );
    QCOMPARE( v.toString(), QStringLiteral( "1.2.3-draft" ) );

    QVERIFY( !v.isRelease() );
    v.label = QStringLiteral( "release" );
    QCOMPARE( v.toString(), QStringLiteral( "1.2.3-release" ) );
    QVERIFY( v.isRelease() );
  }

  void versionParseRoundtrips()
  {
    bool ok = false;

    const Version bare = Version::parse( QStringLiteral( "0.4.0" ), &ok );
    QVERIFY( ok );
    QCOMPARE( bare.major, 0 );
    QCOMPARE( bare.minor, 4 );
    QCOMPARE( bare.patch, 0 );
    QCOMPARE( bare.label, QString() );
    QVERIFY( !bare.isRelease() );

    const Version tagged = Version::parse( QStringLiteral( "1.2.3-release" ), &ok );
    QVERIFY( ok );
    QCOMPARE( tagged.major, 1 );
    QCOMPARE( tagged.minor, 2 );
    QCOMPARE( tagged.patch, 3 );
    QCOMPARE( tagged.label, QStringLiteral( "release" ) );
    QVERIFY( tagged.isRelease() );

    // parse(toString(v)) == v for both forms
    const Version draft { 2, 0, 0, QStringLiteral( "draft" ) };
    const Version rt = Version::parse( draft.toString(), &ok );
    QVERIFY( ok );
    QCOMPARE( rt.major, draft.major );
    QCOMPARE( rt.minor, draft.minor );
    QCOMPARE( rt.patch, draft.patch );
    QCOMPARE( rt.label, draft.label );
  }

  void versionParseRejectsMalformed()
  {
    bool ok = true;
    for ( const QString &bad : { QString(), QStringLiteral( "1.2" ),
                                 QStringLiteral( "1.2.3.4" ), QStringLiteral( "a.b.c" ),
                                 QStringLiteral( "1..3" ), QStringLiteral( "-draft" ) } )
    {
      Version::parse( bad, &ok );
      QVERIFY2( !ok, qPrintable( QStringLiteral( "accepted %1" ).arg( bad ) ) );
    }
  }
};

QTEST_APPLESS_MAIN( TestDomainTypes )

#include "tst_domain.moc"
