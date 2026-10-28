// PW-075 / PW-126：project.paleo 成员路径越界与 formatVersion 缺失的判定。
#include "metadata/paleoprojectfile.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TestProjectFileSafety : public QObject
{
  Q_OBJECT

private:
  static QString write(const QTemporaryDir &dir, const QByteArray &json)
  {
    const QString path = dir.filePath(QStringLiteral("project.paleo"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    f.write(json);
    return path;
  }

private slots:
  void nativeBackendRoundTripAndUnknownBackendRejected()
  {
    QTemporaryDir dir;
    auto file=projectFileForQgz(dir.filePath("project.qgz"));
    file.dataBackend="data_service_versioned";
    file.catalog="artifacts/metadata/catalog.sqlite";
    QString error; QVERIFY2(writeProjectFile(dir.path(),file,&error),qPrintable(error));
    bool ok=false; const auto read=readProjectFile(dir.filePath("project.paleo"),&ok,&error);
    QVERIFY2(ok,qPrintable(error)); QCOMPARE(read.dataBackend,file.dataBackend);
    QCOMPARE(read.catalog,file.catalog); QCOMPARE(read.projectId,file.projectId);
    const auto path=write(dir,R"({"format":"paleo-project","formatVersion":1,"dataBackend":"future-incompatible","members":{}})");
    readProjectFile(path,&ok,&error); QVERIFY(!ok); QVERIFY(error.contains("backend"));
  }
  void relativeMembersAccepted()
  {
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","formatVersion":1,
      "members":{"qgz":"a.qgz","catalog":"artifacts/metadata/catalog.json","gpkg":"a.gpkg"}})");
    bool ok = false;
    QString err;
    const PaleoProjectFile f = readProjectFile(p, &ok, &err);
    QVERIFY2(ok, qPrintable(err));
    QCOMPARE(f.gpkg, QStringLiteral("a.gpkg"));
  }

  void escapingMembersRejected_data()
  {
    QTest::addColumn<QByteArray>("member");
    QTest::newRow("dotdot") << QByteArray("../../etc/x.gpkg");
    QTest::newRow("absolute") << QByteArray("/etc/x.gpkg");
    QTest::newRow("drive") << QByteArray("C:/x.gpkg");
    QTest::newRow("backslash-dotdot") << QByteArray("sub\\\\..\\\\..\\\\x.gpkg");
  }
  void escapingMembersRejected()
  {
    QFETCH(QByteArray, member);
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","formatVersion":1,"members":{"gpkg":")" +
                                     member + R"("}})");
    bool ok = true;
    QString err;
    readProjectFile(p, &ok, &err);
    QVERIFY(!ok);
    QVERIFY2(err.contains(QStringLiteral("escapes")), qPrintable(err));
  }

  void missingVersionIsNotNewer()
  {
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","members":{}})");
    bool ok = true;
    QString err;
    readProjectFile(p, &ok, &err);
    QVERIFY(!ok);
    QVERIFY2(!err.contains(QStringLiteral("newer")), qPrintable(err));
    QVERIFY(err.contains(QStringLiteral("missing or invalid")));
  }

  void newerVersionStillReportedAsNewer()
  {
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","formatVersion":7,"members":{}})");
    bool ok = true;
    QString err;
    readProjectFile(p, &ok, &err);
    QVERIFY(!ok);
    QVERIFY(err.contains(QStringLiteral("newer")));
  }
};

QTEST_GUILESS_MAIN(TestProjectFileSafety)
#include "tst_projectfile_safety.moc"
