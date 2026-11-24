// 层：测试壳
#include "services/pythonsandbox_windows_view_logic.h"

#include <QtTest>

using namespace paleo::python::windows_view_logic;

// #959：Windows 受控 Python 私有视图与 journal 恢复逻辑的跨平台单元测试。
// 覆盖 moniker/摘要校验、相对路径越界防护、profile/lease/staging/input/ready
// 记录 fail-closed 契约、owner 态机恢复决策与 LRU 视图逐出策略。
class TestPythonSandboxViewJournal : public QObject
{
  Q_OBJECT
private slots:
  void monikerAndHexDigestValidation();
  void relativePathAndReservedNames();
  void stagingNameAndLeaseFileRoundTrip();
  void profileJournalRecordStrictValidation();
  void viewLeaseAndMarkerRecordsStrictValidation();
  void recoveryActionStateMachine();
  void viewEvictionProtectsBlockedAndKeepsNewest();
};

void TestPythonSandboxViewJournal::monikerAndHexDigestValidation()
{
  const QString okSha(64, QLatin1Char('a'));
  QCOMPARE(hexDigest64OrEmpty(okSha), okSha);
  QVERIFY(hexDigest64OrEmpty(QString(63, QLatin1Char('a'))).isEmpty());
  QVERIFY(hexDigest64OrEmpty(QString(64, QLatin1Char('A'))).isEmpty());
  QVERIFY(hexDigest64OrEmpty(QString(64, QLatin1Char('g'))).isEmpty());

  const QString okMoniker = QStringLiteral("PaleoPython.") + QString(32, QLatin1Char('f'));
  QVERIFY(isValidSandboxMoniker(okMoniker));
  QVERIFY(!isValidSandboxMoniker(QStringLiteral("PaleoPython.") + QString(32, QLatin1Char('F'))));
  QVERIFY(!isValidSandboxMoniker(QStringLiteral("Other.") + QString(32, QLatin1Char('a'))));
  QVERIFY(!isValidSandboxMoniker(QStringLiteral("../PaleoPython.") + QString(32, QLatin1Char('a'))));
  QVERIFY(!isValidSandboxMoniker(QStringLiteral("PaleoPython.") + QString(31, QLatin1Char('a'))));
}

void TestPythonSandboxViewJournal::relativePathAndReservedNames()
{
  QCOMPARE(relativePathUnderRoot(QStringLiteral("C:/views/abc"),
                                 QStringLiteral("C:/views/abc/lib/python.exe")),
           QStringLiteral("lib/python.exe"));
  QVERIFY(relativePathUnderRoot(QStringLiteral("C:/views/abc"),
                                QStringLiteral("C:/views/abcdef/x")).isEmpty());
  QVERIFY(relativePathUnderRoot(QStringLiteral("C:/views/abc"),
                                QStringLiteral("C:/views/abc/../escape")).isEmpty());
  QVERIFY(relativePathUnderRoot(QStringLiteral("C:/views/abc"),
                                QStringLiteral("C:/views/abc/a\\b")).isEmpty());
  QVERIFY(isReservedViewEntryName(QStringLiteral("view.json")));
  QVERIFY(isReservedViewEntryName(QStringLiteral("STAGING.JSON")));
  QVERIFY(!isReservedViewEntryName(QStringLiteral("runtime-manifest.json")));
}

void TestPythonSandboxViewJournal::stagingNameAndLeaseFileRoundTrip()
{
  const QString sha = QString(64, QLatin1Char('9'));
  const QString moniker = QStringLiteral("PaleoPython.0123456789abcdef0123456789abcdef");
  QCOMPARE(buildLeaseFileName(sha, 4242, 133700000000000000ULL, moniker),
           sha + QStringLiteral(".4242-133700000000000000.") + moniker + QStringLiteral(".json"));

  const QString staging =
      sha + QStringLiteral(".4242-133700000000000000.") + moniker + QStringLiteral(".staging");
  const ParsedStagingName parsed = parseStagingDirName(staging);
  QVERIFY(parsed.valid);
  QCOMPARE(parsed.manifestSha, sha);
  QCOMPARE(parsed.hostPid, quint64(4242));
  QCOMPARE(parsed.hostCreated100ns, quint64(133700000000000000ULL));
  QCOMPARE(parsed.moniker, moniker);

  QVERIFY(!parseStagingDirName(sha + QStringLiteral(".0-123.") + moniker + QStringLiteral(".staging")).valid);
  QVERIFY(!parseStagingDirName(sha + QStringLiteral(".4242-0.") + moniker + QStringLiteral(".staging")).valid);
  QVERIFY(!parseStagingDirName(QStringLiteral("not-a-staging-dir")).valid);
}

void TestPythonSandboxViewJournal::profileJournalRecordStrictValidation()
{
  const QString moniker = QStringLiteral("PaleoPython.0123456789abcdef0123456789abcdef");
  const QJsonObject valid{
      {QStringLiteral("schema"), QLatin1String(kProfileJournalSchema)},
      {QStringLiteral("moniker"), moniker},
      {QStringLiteral("hostPid"), qint64(1024)},
      {QStringLiteral("hostCreated100ns"), QStringLiteral("133700000000000000")},
  };
  QString outMoniker;
  OwnerIdentity outOwner;
  QVERIFY(validateProfileJournalRecord(valid, moniker + QStringLiteral(".json"), &outMoniker, &outOwner));
  QCOMPARE(outMoniker, moniker);
  QVERIFY(outOwner.valid);
  QCOMPARE(outOwner.hostPid, quint64(1024));

  // File name mismatch, extra keys, bad schema, or invalid PID must fail closed.
  QVERIFY(!validateProfileJournalRecord(valid, QStringLiteral("other.json")));
  QJsonObject extraKey = valid;
  extraKey.insert(QStringLiteral("arbitraryPath"), QStringLiteral("C:/Windows"));
  QVERIFY(!validateProfileJournalRecord(extraKey, moniker + QStringLiteral(".json")));
  QJsonObject badPid = valid;
  badPid.insert(QStringLiteral("hostPid"), 0);
  QVERIFY(!validateProfileJournalRecord(badPid, moniker + QStringLiteral(".json")));
}

void TestPythonSandboxViewJournal::viewLeaseAndMarkerRecordsStrictValidation()
{
  const QString sha = QString(64, QLatin1Char('c'));
  const QJsonObject lease{
      {QStringLiteral("schema"), QLatin1String(kViewLeaseSchema)},
      {QStringLiteral("manifestSha256"), sha},
      {QStringLiteral("viewPath"), QStringLiteral("runtime-views/") + sha},
      {QStringLiteral("hostPid"), qint64(888)},
      {QStringLiteral("hostCreated100ns"), QStringLiteral("987654321")},
  };
  QString outSha;
  OwnerIdentity outOwner;
  QVERIFY(validateViewLeaseRecord(lease, &outSha, &outOwner));
  QCOMPARE(outSha, sha);
  QCOMPARE(outOwner.hostPid, quint64(888));

  // Arbitrary absolute path inside viewPath must be rejected (fail-closed).
  QJsonObject badPath = lease;
  badPath.insert(QStringLiteral("viewPath"), QStringLiteral("C:/Users/victim"));
  QVERIFY(!validateViewLeaseRecord(badPath));

  const QJsonObject staging{
      {QStringLiteral("schema"), QLatin1String(kViewMarkerSchema)},
      {QStringLiteral("state"), QStringLiteral("staging")},
      {QStringLiteral("manifestSha256"), sha},
      {QStringLiteral("hostPid"), qint64(888)},
      {QStringLiteral("hostCreated100ns"), QStringLiteral("987654321")},
  };
  QVERIFY(validateStagingMarkerRecord(staging));

  const QJsonObject inputMarker{
      {QStringLiteral("schema"), QLatin1String(kViewMarkerSchema)},
      {QStringLiteral("state"), QStringLiteral("input")},
      {QStringLiteral("hostPid"), qint64(888)},
      {QStringLiteral("hostCreated100ns"), QStringLiteral("987654321")},
  };
  QVERIFY(validateInputMarkerRecord(inputMarker));

  const QJsonObject readyView{
      {QStringLiteral("schema"), QLatin1String(kViewMarkerSchema)},
      {QStringLiteral("state"), QStringLiteral("ready")},
      {QStringLiteral("manifestSha256"), sha},
      {QStringLiteral("files"), 12},
      {QStringLiteral("treeSize"), 4096},
      {QStringLiteral("hostPid"), qint64(888)},
      {QStringLiteral("hostCreated100ns"), QStringLiteral("987654321")},
  };
  QVERIFY(validateReadyViewRecord(readyView, sha));
  QVERIFY(!validateReadyViewRecord(readyView, QString(64, QLatin1Char('d'))));
}

void TestPythonSandboxViewJournal::recoveryActionStateMachine()
{
  // Dead host and finished self-owned stale records are reclaimed.
  QCOMPARE(decideResourceRecovery(OwnerState::Dead, true), RecoveryAction::Remove);
  QCOMPARE(decideResourceRecovery(OwnerState::AliveSelfIdle, true), RecoveryAction::Remove);
  // Active leases (other host or active self) are preserved.
  QCOMPARE(decideResourceRecovery(OwnerState::AliveOther, true), RecoveryAction::Keep);
  QCOMPARE(decideResourceRecovery(OwnerState::AliveSelfActive, true), RecoveryAction::Keep);
  // Corrupt record or unknown owner must fail closed (never blindly delete).
  QCOMPARE(decideResourceRecovery(OwnerState::Dead, false), RecoveryAction::FailClosed);
  QCOMPARE(decideResourceRecovery(OwnerState::Unknown, true), RecoveryAction::FailClosed);
}

void TestPythonSandboxViewJournal::viewEvictionProtectsBlockedAndKeepsNewest()
{
  const QString sha1 = QString(64, QLatin1Char('1'));
  const QString sha2 = QString(64, QLatin1Char('2'));
  const QString sha3 = QString(64, QLatin1Char('3'));
  const QString sha4 = QString(64, QLatin1Char('4'));
  const QList<CachedViewCandidate> candidates{
      {sha1, QStringLiteral("/views/") + sha1, 100},
      {sha2, QStringLiteral("/views/") + sha2, 200},
      {sha3, QStringLiteral("/views/") + sha3, 300},
      {sha4, QStringLiteral("/views/") + sha4, 400},
      {QStringLiteral("invalid-sha"), QStringLiteral("/views/invalid"), 50},
  };
  // sha1 is actively leased (blocked), so among the 3 idle views (sha2, sha3, sha4)
  // only the oldest idle view (sha2) should be evicted when maxCached=2.
  const QStringList evicted = selectViewsToEvict(candidates, {sha1}, 2);
  QCOMPARE(evicted, QStringList{QStringLiteral("/views/") + sha2});
}

QTEST_GUILESS_MAIN(TestPythonSandboxViewJournal)
#include "tst_pythonsandbox_view_journal.moc"
