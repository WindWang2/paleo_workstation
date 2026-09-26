#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../src/metadata/paleoprojectstore.h"

// data/commit-coord（D 包）：PaleoProjectStore::commitAll 的 journal +
// 幂等 + 有序提交。测试只驱动仪表化回调（不依赖真 .qgz/.gpkg 内容），
// 记档目录为 <临时工程>/artifacts/metadata/commit_journal/<opId>.json。
// 中途崩溃用「手工落一条半成品记档」模拟——与进程崩在写序中途等价。
class TestCommitCoord : public QObject
{
  Q_OBJECT

  static QString journalDirFor( const QString &projectDir )
  {
    return projectDir + QStringLiteral( "/artifacts/metadata/commit_journal" );
  }

  static QString journalPathFor( const QString &projectDir, const QString &opId )
  {
    return journalDirFor( projectDir ) + QLatin1Char( '/' ) + opId +
           QStringLiteral( ".json" );
  }

  static QJsonObject journalJson( const QString &path )
  {
    QFile f( path );
    if ( !f.open( QIODevice::ReadOnly ) )
      return {};
    const QJsonDocument doc = QJsonDocument::fromJson( f.readAll() );
    return doc.isObject() ? doc.object() : QJsonObject{};
  }

  static QString journalStage( const QString &projectDir, const QString &opId )
  {
    return journalJson( journalPathFor( projectDir, opId ) )
        .value( QStringLiteral( "stage" ) )
        .toString();
  }

  // 直接落一条 journal 文件——模拟上次进程在写序中途崩溃留下的记档。
  static void plantJournal( const QString &projectDir, const QString &opId,
                            const QString &stage, const QString &digest )
  {
    QDir().mkpath( journalDirFor( projectDir ) );
    QJsonObject o;
    o.insert( QStringLiteral( "journal_version" ), 1 );
    o.insert( QStringLiteral( "op_id" ), opId );
    o.insert( QStringLiteral( "stage" ), stage );
    o.insert( QStringLiteral( "input_digest" ), digest );
    QFile f( journalPathFor( projectDir, opId ) );
    if ( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      f.write( QJsonDocument( o ).toJson( QJsonDocument::Indented ) );
  }

  static bool writeFile( const QString &path, const QByteArray &bytes )
  {
    QFile f( path );
    if ( !f.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      return false;
    return f.write( bytes ) == bytes.size();
  }

  static QByteArray readFile( const QString &path )
  {
    QFile f( path );
    return f.open( QIODevice::ReadOnly ) ? f.readAll() : QByteArray();
  }

  static PaleoProjectStore::WriteResult ok()
  {
    return { true, QString() };
  }

  static PaleoProjectStore::WriteResult fail( const QString &error )
  {
    return { false, error };
  }

  // 在 dir 下绑好工程路径（qgz/gpkg/meta 都是名义路径，内容由各用例决定）。
  static PaleoProjectStore *makeStore( QObject *parent, const QString &projectDir )
  {
    auto *store = new PaleoProjectStore( parent );
    const QString qgz = projectDir + QStringLiteral( "/proj.qgz" );
    store->setProjectPaths( qgz, projectDir + QStringLiteral( "/proj.gpkg" ),
                            qgz + QStringLiteral( ".project.sqlite" ) );
    return store;
  }

private slots:
  // 成功 op：queued → catalog_done → qgz_done → complete 全程记档，
  // 且保留与 saveAll 相同的 .qgz 备份保护。
  void successfulCommitJournalsLifecycle()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString qgz = dir.filePath( QStringLiteral( "proj.qgz" ) );
    QVERIFY( writeFile( qgz, "qgz-v1" ) ); // 预存 qgz → 触发 .bak 备份分支
    PaleoProjectStore store;
    store.setProjectPaths( qgz, dir.filePath( QStringLiteral( "proj.gpkg" ) ),
                           qgz + QStringLiteral( ".project.sqlite" ) );

    const QString jPath = journalPathFor( dir.path(), QStringLiteral( "op-1" ) );
    QStringList order;
    QString stageAtCatalog, stageAtQgz;
    const auto res = store.commitAll(
        QStringLiteral( "op-1" ), QStringLiteral( "digest-A" ),
        [&]() -> PaleoProjectStore::WriteResult {
          order << QStringLiteral( "catalog" );
          // catalog 单元执行期间记档应停在 queued（尚未推进）。
          stageAtCatalog = journalJson( jPath ).value( QStringLiteral( "stage" ) ).toString();
          return ok();
        },
        [&]() -> PaleoProjectStore::WriteResult {
          order << QStringLiteral( "qgz" );
          // qgz 单元执行期间 catalog 阶段必须已记档完成。
          stageAtQgz = journalJson( jPath ).value( QStringLiteral( "stage" ) ).toString();
          return writeFile( qgz, "qgz-v2" ) ? ok() : fail( QStringLiteral( "write failed" ) );
        } );

    QVERIFY2( res.ok, qPrintable( res.error ) );
    QCOMPARE( order, QStringList() << QStringLiteral( "catalog" )
                                   << QStringLiteral( "qgz" ) );
    QCOMPARE( stageAtCatalog, QStringLiteral( "queued" ) );
    QCOMPARE( stageAtQgz, QStringLiteral( "catalog_done" ) );
    // 终态：记档标记 complete（不删除——complete 记档是幂等重入的凭据）。
    QCOMPARE( journalStage( dir.path(), QStringLiteral( "op-1" ) ),
              QStringLiteral( "complete" ) );
    // .qgz 备份保护仍在，且 .bak 是上一版内容。
    QVERIFY( QFile::exists( qgz + QStringLiteral( ".bak" ) ) );
    QCOMPARE( readFile( qgz + QStringLiteral( ".bak" ) ), QByteArray( "qgz-v1" ) );
    QCOMPARE( readFile( qgz ), QByteArray( "qgz-v2" ) );
  }

  // 顺序不变量：catalog 提交恒先于 qgz 写（仪表化回调记录执行序）。
  void catalogCommitPrecedesQgzWrite()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore store;
    store.setProjectPaths( dir.filePath( QStringLiteral( "proj.qgz" ) ),
                           dir.filePath( QStringLiteral( "proj.gpkg" ) ),
                           dir.filePath( QStringLiteral( "proj.qgz.project.sqlite" ) ) );

    QStringList order;
    const auto res = store.commitAll(
        QStringLiteral( "op-order" ), QStringLiteral( "d" ),
        [&]() -> PaleoProjectStore::WriteResult {
          order << QStringLiteral( "catalog" );
          return ok();
        },
        [&]() -> PaleoProjectStore::WriteResult {
          order << QStringLiteral( "qgz" );
          return ok();
        } );
    QVERIFY2( res.ok, qPrintable( res.error ) );
    QCOMPARE( order, QStringList() << QStringLiteral( "catalog" )
                                   << QStringLiteral( "qgz" ) );
  }

  // 幂等：journal=complete 的同 opId 重入 → no-op 成功，回调不再执行。
  void replayedCompleteOpIsNoOp()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );
    const QString opId = QStringLiteral( "op-done" );

    int catCalls = 0, qgzCalls = 0;
    auto res = store->commitAll(
        opId, QStringLiteral( "digest-A" ),
        [&]() -> PaleoProjectStore::WriteResult { ++catCalls; return ok(); },
        [&]() -> PaleoProjectStore::WriteResult { ++qgzCalls; return ok(); } );
    QVERIFY2( res.ok, qPrintable( res.error ) );
    QCOMPARE( catCalls, 1 );
    QCOMPARE( qgzCalls, 1 );

    res = store->commitAll(
        opId, QStringLiteral( "digest-A" ),
        [&]() -> PaleoProjectStore::WriteResult { ++catCalls; return ok(); },
        [&]() -> PaleoProjectStore::WriteResult { ++qgzCalls; return ok(); } );
    QVERIFY2( res.ok, qPrintable( res.error ) );
    QCOMPARE( catCalls, 1 ); // 未重跑
    QCOMPARE( qgzCalls, 1 ); // 未重跑
    QCOMPARE( journalStage( dir.path(), opId ), QStringLiteral( "complete" ) );
  }

  // 中阶段续跑：catalog_done 记档 + 相同摘要 → 跳过 catalog，只补 qgz。
  void midStageJournalResumesRemainingUnits()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );
    const QString opId = QStringLiteral( "op-resume" );
    plantJournal( dir.path(), opId, QStringLiteral( "catalog_done" ),
                  QStringLiteral( "digest-A" ) );

    int catCalls = 0, qgzCalls = 0;
    const auto res = store->commitAll(
        opId, QStringLiteral( "digest-A" ),
        [&]() -> PaleoProjectStore::WriteResult { ++catCalls; return ok(); },
        [&]() -> PaleoProjectStore::WriteResult { ++qgzCalls; return ok(); } );
    QVERIFY2( res.ok, qPrintable( res.error ) );
    QCOMPARE( catCalls, 0 );  // catalog 已完成 → 不重跑
    QCOMPARE( qgzCalls, 1 );
    QCOMPARE( journalStage( dir.path(), opId ), QStringLiteral( "complete" ) );
  }

  // honest failure：同 opId 但输入摘要不同 → 拒绝续跑，记档原样保留。
  void mismatchedDigestRefusedHonestly()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );
    const QString opId = QStringLiteral( "op-mismatch" );
    plantJournal( dir.path(), opId, QStringLiteral( "catalog_done" ),
                  QStringLiteral( "digest-A" ) );

    int calls = 0;
    const auto res = store->commitAll(
        opId, QStringLiteral( "digest-B" ),
        [&]() -> PaleoProjectStore::WriteResult { ++calls; return ok(); },
        [&]() -> PaleoProjectStore::WriteResult { ++calls; return ok(); } );
    QVERIFY( !res.ok );
    QVERIFY2( res.error.contains( opId ), qPrintable( res.error ) );
    QCOMPARE( calls, 0 );
    QCOMPARE( journalStage( dir.path(), opId ),
              QStringLiteral( "catalog_done" ) ); // 记档未被推进
  }

  // catalog 单元失败：qgz 不执行，记档停在 queued，recover 能报告它。
  void catalogFailureStopsBeforeQgz()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );
    const QString opId = QStringLiteral( "op-catfail" );

    int qgzCalls = 0;
    const auto res = store->commitAll(
        opId, QStringLiteral( "d" ),
        [&]() -> PaleoProjectStore::WriteResult {
          return fail( QStringLiteral( "catalog boom" ) );
        },
        [&]() -> PaleoProjectStore::WriteResult { ++qgzCalls; return ok(); } );
    QVERIFY( !res.ok );
    QVERIFY2( res.error.contains( QStringLiteral( "boom" ) ), qPrintable( res.error ) );
    QCOMPARE( qgzCalls, 0 );
    QCOMPARE( journalStage( dir.path(), opId ), QStringLiteral( "queued" ) );

    const QVector<PaleoProjectStore::CommitOp> pending =
        store->recoverCommitJournal();
    QCOMPARE( pending.size(), 1 );
    QCOMPARE( pending.first().opId, opId );
    QCOMPARE( pending.first().stage, QStringLiteral( "queued" ) );
  }

  // qgz 单元失败：catalog 已落库、记档停在 catalog_done——可恢复态；
  // 同 opId+同摘要重入 → catalog 不重跑、只补 qgz → complete。
  void qgzFailureIsRecoverableAndResumable()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );
    const QString opId = QStringLiteral( "op-qgzfail" );

    int catCalls = 0, qgzCalls = 0;
    auto res = store->commitAll(
        opId, QStringLiteral( "d" ),
        [&]() -> PaleoProjectStore::WriteResult { ++catCalls; return ok(); },
        [&]() -> PaleoProjectStore::WriteResult {
          ++qgzCalls;
          return fail( QStringLiteral( "qgz boom" ) );
        } );
    QVERIFY( !res.ok );
    QCOMPARE( catCalls, 1 );
    QCOMPARE( qgzCalls, 1 );
    QCOMPARE( journalStage( dir.path(), opId ),
              QStringLiteral( "catalog_done" ) );

    // 续跑：catalog 不重跑，qgz 这次成功 → 记档推进到 complete。
    res = store->commitAll(
        opId, QStringLiteral( "d" ),
        [&]() -> PaleoProjectStore::WriteResult { ++catCalls; return ok(); },
        [&]() -> PaleoProjectStore::WriteResult { ++qgzCalls; return ok(); } );
    QVERIFY2( res.ok, qPrintable( res.error ) );
    QCOMPARE( catCalls, 1 ); // 仍为 1——已完成单元不重跑
    QCOMPARE( qgzCalls, 2 );
    QCOMPARE( journalStage( dir.path(), opId ), QStringLiteral( "complete" ) );
    QVERIFY( store->recoverCommitJournal().isEmpty() );
  }

  // 崩溃恢复扫描：queued/catalog_done/corrupt 一律报告，complete 不报。
  void recoverCommitJournalReportsUnfinishedOnly()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );

    plantJournal( dir.path(), QStringLiteral( "op-a" ),
                  QStringLiteral( "queued" ), QStringLiteral( "d" ) );
    plantJournal( dir.path(), QStringLiteral( "op-b" ),
                  QStringLiteral( "qgz_done" ), QStringLiteral( "d" ) );
    plantJournal( dir.path(), QStringLiteral( "op-c" ),
                  QStringLiteral( "complete" ), QStringLiteral( "d" ) );
    // 损坏记档：不是 JSON——同属未完成，要如实报出来而不是跳过。
    QVERIFY( writeFile( journalPathFor( dir.path(), QStringLiteral( "op-bad" ) ),
                        QByteArray( "this is not json{" ) ) );

    const QVector<PaleoProjectStore::CommitOp> ops =
        store->recoverCommitJournal();
    QCOMPARE( ops.size(), 3 );
    QStringList ids;
    QString badStage;
    for ( const auto &op : ops )
    {
      ids << op.opId;
      if ( op.opId == QStringLiteral( "op-bad" ) )
        badStage = op.stage;
    }
    QVERIFY( ids.contains( QStringLiteral( "op-a" ) ) );
    QVERIFY( ids.contains( QStringLiteral( "op-b" ) ) );
    QVERIFY( ids.contains( QStringLiteral( "op-bad" ) ) );
    QVERIFY( !ids.contains( QStringLiteral( "op-c" ) ) );
    QCOMPARE( badStage, QStringLiteral( "corrupt" ) );
  }

  // 执行前校验：不安全 opId / 未设工程路径 → 如实报错、不落记档。
  void preflightValidationFailsHonestly()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );
    int calls = 0;
    const auto bump = [&]() -> PaleoProjectStore::WriteResult {
      ++calls;
      return ok();
    };

    auto res = store->commitAll( QStringLiteral( "../escape" ),
                                 QStringLiteral( "d" ), bump, bump );
    QVERIFY( !res.ok );
    QCOMPARE( calls, 0 );

    PaleoProjectStore unbound; // 从未 setProjectPaths
    res = unbound.commitAll( QStringLiteral( "op-x" ), QStringLiteral( "d" ),
                             bump, bump );
    QVERIFY( !res.ok );
    QVERIFY( !res.error.isEmpty() );
    QCOMPARE( calls, 0 );
    QVERIFY( unbound.recoverCommitJournal().isEmpty() );
  }

  void journalPruneCapsComplete()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore *store = makeStore( this, dir.path() );

    const auto okUnit = []() -> PaleoProjectStore::WriteResult { return ok(); };
    for ( int i = 0; i < 5; ++i )
      QVERIFY( store->commitAll( QStringLiteral( "op-%1" ).arg( i ),
                                 QStringLiteral( "d" ), okUnit, okUnit ).ok );
    // qgz 单元失败 → 记档停在 catalog_done：未完成面，清理不得碰它。
    const auto failUnit = []() -> PaleoProjectStore::WriteResult {
      return fail( QStringLiteral( "x" ) );
    };
    QVERIFY( !store->commitAll( QStringLiteral( "op-stuck" ),
                                QStringLiteral( "d" ), okUnit, failUnit ).ok );

    const QString jdir = store->commitJournalDir();
    QCOMPARE( store->pruneCommitJournal( 2 ), 3 );
    QCOMPARE( QDir( jdir ).entryList(
                  QStringList{ QStringLiteral( "*.json" ) }, QDir::Files ).size(), 3 );

    // keepComplete=0：complete 全清，op-stuck（未完成）仍必须保留上报。
    QCOMPARE( store->pruneCommitJournal( 0 ), 2 );
    QVERIFY( QFile::exists( jdir + QStringLiteral( "/op-stuck.json" ) ) );
    const QVector<PaleoProjectStore::CommitOp> unfinished =
        store->recoverCommitJournal();
    QCOMPARE( unfinished.size(), 1 );
    QCOMPARE( unfinished.front().opId, QStringLiteral( "op-stuck" ) );

    // 无 journal 目录 / 未绑工程：如实回 0，不崩。
    PaleoProjectStore bare;
    QCOMPARE( bare.pruneCommitJournal(), 0 );
  }
};

QTEST_MAIN( TestCommitCoord )
#include "tst_commitcoord.moc"
