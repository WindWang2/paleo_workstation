#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include "catalog/datacatalog.h"
#include "services/welllogset.h"

namespace
{
  QString writeLas(const QString &path, const QStringList &mnems, double startDepth, int rows)
  {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    QTextStream out(&file);
    out << "~Version Information\n";
    out << " VERS.                  2.0:   CWLS log ASCII Standard -VERSION 2.0\n";
    out << " WRAP.                   NO:   One line per depth step\n";
    out << "~Well Information Block\n";
    out << " STRT.M        " << QString::number(startDepth, 'f', 4) << ":\n";
    out << " STOP.M        " << QString::number(startDepth + rows - 1, 'f', 4) << ":\n";
    out << " STEP.M          1.0000:\n";
    out << " NULL.        -999.2500:\n";
    out << " WELL.         TEST_WELL:\n";
    out << "~Curve Information Block\n";
    for (const QString &mnem : mnems)
      out << " " << mnem << ".M                  :   " << mnem << "\n";
    out << "~A\n";
    for (int r = 0; r < rows; ++r)
    {
      out << QString::number(startDepth + r, 'f', 2);
      for (int c = 1; c < mnems.size(); ++c)
        out << " " << QString::number(10.0 * c + r, 'f', 2);
      out << "\n";
    }
    file.close();
    return path;
  }

  bool addWell(DataCatalog &cat, const QString &id, const QString &name, QString *err)
  {
    CatalogEntity well;
    well.id = id;
    well.entityType = QStringLiteral("well");
    well.name = name;
    return cat.addEntity(well, err);
  }

  // managed=false，path 为 LAS 绝对路径，resolvedVersionPath 原样返回。
  // unresolved 仍写入 entityId，这样 linksForEntity 能看见它——读面必须自己丢掉。
  bool seedLog(DataCatalog &cat, const QString &wellId, const QString &assetId,
               const QString &versionId, const QString &path, int ordinal, bool primary,
               int versionNumber, bool unresolved, bool withVersion, const QString &role,
               QString *err)
  {
    CatalogAsset asset;
    asset.id = assetId;
    asset.type = QStringLiteral("well_log");
    asset.format = QStringLiteral("las");
    asset.displayName = QFileInfo(path).fileName();
    if (asset.displayName.isEmpty())
      asset.displayName = assetId;
    if (!cat.addAsset(asset, err))
      return false;
    if (withVersion)
    {
      CatalogVersion version;
      version.id = versionId;
      version.assetId = assetId;
      version.stage = QStringLiteral("RAW");
      version.versionNumber = versionNumber;
      version.managed = false;
      version.path = path;
      version.fileName = asset.displayName;
      if (!cat.addVersion(version, err))
        return false;
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = wellId;
    link.assetId = assetId;
    link.role = role;
    link.isPrimary = primary;
    link.unresolved = unresolved;
    link.ordinal = ordinal;
    return cat.addLink(link, err);
  }

  const WellCurveRef *findMnemonic(const QVector<WellCurveRef> &refs, const QString &mnemonic)
  {
    for (const WellCurveRef &ref : refs)
      if (ref.mnemonic == mnemonic)
        return &ref;
    return nullptr;
  }
} // namespace

class TestWellLogSet : public QObject
{
  Q_OBJECT

private slots:
  void oracle1_ordersFilesAndUnionsCurvesWithoutDepth();
  void oracle2_renamesCrossFileDuplicatesAndFollowsPrimary();
  void oracle6_skipsMissingUnparsedAndUnresolved();
  void headerOnlyIndexStaysUnder200ms();
  void closedCatalogWarnsOnceEmptyWellIdDoesNot();
  void ordersByOrdinalVersionNumberThenVersionId();
  void readsCurrentVersionOnly();
  void doesNotElectPrimaryWhenMarkedFileIsMissing();
};

void TestWellLogSet::oracle1_ordersFilesAndUnionsCurvesWithoutDepth()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString pathA = writeLas(dir.filePath(QStringLiteral("fileA.las")),
                                 {QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("SP")},
                                 1000.0, 3);
  const QString pathB = writeLas(dir.filePath(QStringLiteral("fileB.las")),
                                 {QStringLiteral("DEPT"), QStringLiteral("RT"), QStringLiteral("NPHI")},
                                 2000.0, 3);
  QVERIFY(!pathA.isEmpty());
  QVERIFY(!pathB.isEmpty());

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-1"), QStringLiteral("W1"), &err), qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-1"), QStringLiteral("ast-a"), QStringLiteral("ver-a"),
                   pathA, 0, true, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-1"), QStringLiteral("ast-b"), QStringLiteral("ver-b"),
                   pathB, 1, false, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));

  WellLogWarnings warnings;
  const QVector<WellLogFile> files =
      WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-1"), &warnings);
  QCOMPARE(warnings.count, 0);
  QCOMPARE(files.size(), 2);
  QCOMPARE(files.at(0).path, pathA);
  QCOMPARE(files.at(1).path, pathB);
  QCOMPARE(files.at(0).versionId, QStringLiteral("ver-a"));
  QCOMPARE(files.at(1).versionId, QStringLiteral("ver-b"));
  QVERIFY(files.at(0).isPrimary);
  QVERIFY(!files.at(1).isPrimary);
  QCOMPARE(files.at(0).ordinal, 0);
  QCOMPARE(files.at(1).ordinal, 1);
  QCOMPARE(files.at(0).curveNames,
           QStringList({QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("SP")}));
  QCOMPARE(files.at(1).curveNames,
           QStringList({QStringLiteral("DEPT"), QStringLiteral("RT"), QStringLiteral("NPHI")}));

  WellLogWarnings indexWarnings;
  const QVector<WellCurveRef> index =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-1"), &indexWarnings);
  QCOMPARE(indexWarnings.count, 0);
  QCOMPARE(index.size(), 4);
  for (const WellCurveRef &ref : index)
  {
    QVERIFY(ref.mnemonic != QLatin1String("DEPT"));
    QVERIFY(ref.column > 0);
  }

  const WellCurveRef *gr = findMnemonic(index, QStringLiteral("GR"));
  const WellCurveRef *sp = findMnemonic(index, QStringLiteral("SP"));
  const WellCurveRef *rt = findMnemonic(index, QStringLiteral("RT"));
  const WellCurveRef *nphi = findMnemonic(index, QStringLiteral("NPHI"));
  QVERIFY(gr && sp && rt && nphi);
  QCOMPARE(gr->sourceVersionId, QStringLiteral("ver-a"));
  QCOMPARE(sp->sourceVersionId, QStringLiteral("ver-a"));
  QCOMPARE(rt->sourceVersionId, QStringLiteral("ver-b"));
  QCOMPARE(nphi->sourceVersionId, QStringLiteral("ver-b"));
  QCOMPARE(gr->path, pathA);
  QCOMPARE(rt->path, pathB);
  QVERIFY(gr->canonical);
  QVERIFY(sp->canonical);
  QVERIFY(!rt->canonical);
  QVERIFY(!nphi->canonical);
}

void TestWellLogSet::oracle2_renamesCrossFileDuplicatesAndFollowsPrimary()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString pathA = writeLas(dir.filePath(QStringLiteral("fileA.las")),
                                 {QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("SP")},
                                 1000.0, 3);
  const QString pathB = writeLas(dir.filePath(QStringLiteral("fileB.las")),
                                 {QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("RT"),
                                  QStringLiteral("NPHI")},
                                 2000.0, 3);
  const QString pathDup = writeLas(dir.filePath(QStringLiteral("fileDup.las")),
                                   {QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("GR")},
                                   3000.0, 2);
  QVERIFY(!pathA.isEmpty() && !pathB.isEmpty() && !pathDup.isEmpty());

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-1"), QStringLiteral("W1"), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-dup"), QStringLiteral("DUP"), &err), qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-1"), QStringLiteral("ast-a"), QStringLiteral("ver-a"),
                   pathA, 0, true, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-1"), QStringLiteral("ast-b"), QStringLiteral("ver-b"),
                   pathB, 1, false, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-dup"), QStringLiteral("ast-dup"), QStringLiteral("ver-dup"),
                   pathDup, 0, true, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));

  const QString aliasB = QStringLiteral("GR@") + QFileInfo(pathB).completeBaseName();
  const QString aliasA = QStringLiteral("GR@") + QFileInfo(pathA).completeBaseName();

  WellLogWarnings warnings;
  const QVector<WellCurveRef> index =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-1"), &warnings);
  QCOMPARE(warnings.count, 0);
  const WellCurveRef *gr = findMnemonic(index, QStringLiteral("GR"));
  const WellCurveRef *grB = findMnemonic(index, aliasB);
  QVERIFY(gr && grB);
  QVERIFY(gr->canonical);
  QCOMPARE(gr->sourceVersionId, QStringLiteral("ver-a"));
  QVERIFY(!grB->canonical);
  QCOMPARE(grB->sourceVersionId, QStringLiteral("ver-b"));
  QCOMPARE(grB->path, pathB);

  int indexB = -1;
  const QVector<EntityAssetLink> links = cat.links();
  for (int i = 0; i < links.size(); ++i)
    if (links.at(i).assetId == QLatin1String("ast-b"))
      indexB = i;
  QVERIFY(indexB >= 0);
  QVERIFY2(cat.setLinkPrimary(indexB, &err), qPrintable(err));

  WellLogWarnings flippedWarnings;
  const QVector<WellLogFile> flippedFiles =
      WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-1"), &flippedWarnings);
  QCOMPARE(flippedWarnings.count, 0);
  QCOMPARE(flippedFiles.size(), 2);
  QCOMPARE(flippedFiles.at(0).path, pathA);
  QCOMPARE(flippedFiles.at(1).path, pathB);
  QVERIFY(!flippedFiles.at(0).isPrimary);
  QVERIFY(flippedFiles.at(1).isPrimary);

  const QVector<WellCurveRef> flipped =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-1"), &flippedWarnings);
  QCOMPARE(flippedWarnings.count, 0);
  const WellCurveRef *grNow = findMnemonic(flipped, QStringLiteral("GR"));
  const WellCurveRef *grA = findMnemonic(flipped, aliasA);
  QVERIFY(grNow && grA);
  QVERIFY(grNow->canonical);
  QCOMPARE(grNow->sourceVersionId, QStringLiteral("ver-b"));
  QCOMPARE(grNow->path, pathB);
  QVERIFY(!grA->canonical);
  QCOMPARE(grA->sourceVersionId, QStringLiteral("ver-a"));
  QCOMPARE(grA->path, pathA);
  const WellCurveRef *sp = findMnemonic(flipped, QStringLiteral("SP"));
  const WellCurveRef *rt = findMnemonic(flipped, QStringLiteral("RT"));
  QVERIFY(sp && rt);
  QVERIFY(!sp->canonical);
  QVERIFY(rt->canonical);
  QCOMPARE(rt->sourceVersionId, QStringLiteral("ver-b"));

  WellLogWarnings dupFilesWarnings;
  const QVector<WellLogFile> dupFiles =
      WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-dup"), &dupFilesWarnings);
  QCOMPARE(dupFiles.size(), 1);
  QCOMPARE(dupFiles.at(0).curveNames,
           QStringList({QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("GR")}));
  QVERIFY(dupFilesWarnings.count >= 1);

  WellLogWarnings dupWarnings;
  const QVector<WellCurveRef> dupIndex =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-dup"), &dupWarnings);
  QVERIFY(dupWarnings.count >= 1);
  QCOMPARE(dupWarnings.messages, dupFilesWarnings.messages);
  QCOMPARE(dupIndex.size(), 2);
  const WellCurveRef *plain = nullptr;
  const WellCurveRef *hashed = nullptr;
  for (const WellCurveRef &ref : dupIndex)
  {
    QCOMPARE(ref.sourceVersionId, QStringLiteral("ver-dup"));
    if (ref.column == 1)
      plain = &ref;
    if (ref.column == 2)
      hashed = &ref;
  }
  QVERIFY(plain && hashed);
  QCOMPARE(plain->mnemonic, QStringLiteral("GR"));
  QVERIFY(hashed->mnemonic.contains(QLatin1Char('#')));
  QCOMPARE(hashed->mnemonic, QStringLiteral("GR#2"));
}

void TestWellLogSet::oracle6_skipsMissingUnparsedAndUnresolved()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString goodPath = writeLas(dir.filePath(QStringLiteral("good.las")),
                                    {QStringLiteral("DEPT"), QStringLiteral("GR")}, 1000.0, 2);
  const QString hiddenPath = writeLas(dir.filePath(QStringLiteral("hidden.las")),
                                      {QStringLiteral("DEPT"), QStringLiteral("SP")}, 1100.0, 2);
  const QString topsPath = writeLas(dir.filePath(QStringLiteral("tops.las")),
                                    {QStringLiteral("DEPT"), QStringLiteral("RT")}, 1200.0, 2);
  const QString missingPath = dir.filePath(QStringLiteral("missing.las"));
  const QString junkPath = dir.filePath(QStringLiteral("junk.txt"));
  QVERIFY(!goodPath.isEmpty() && !hiddenPath.isEmpty() && !topsPath.isEmpty());
  {
    QFile junk(junkPath);
    QVERIFY(junk.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(junk.write("this is not a las file\njust text\n") > 0);
  }
  QVERIFY(!QFileInfo(missingPath).exists());

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-mix"), QStringLiteral("MIX"), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-none"), QStringLiteral("NONE"), &err), qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-mix"), QStringLiteral("ast-good"), QStringLiteral("ver-good"),
                   goodPath, 0, true, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-mix"), QStringLiteral("ast-miss"), QStringLiteral("ver-miss"),
                   missingPath, 1, false, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-mix"), QStringLiteral("ast-junk"), QStringLiteral("ver-junk"),
                   junkPath, 2, false, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-mix"), QStringLiteral("ast-hidden"), QStringLiteral("ver-hidden"),
                   hiddenPath, 0, true, 1, true, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-mix"), QStringLiteral("ast-nover"), QString(),
                   QString(), 3, false, 1, false, false, QStringLiteral("well_log"), &err),
           qPrintable(err));

  QVERIFY2(seedLog(cat, QStringLiteral("well-none"), QStringLiteral("ast-unres"), QStringLiteral("ver-unres"),
                   hiddenPath, 0, true, 1, true, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-none"), QStringLiteral("ast-tops"), QStringLiteral("ver-tops"),
                   topsPath, 0, true, 1, false, true, QStringLiteral("tops"), &err),
           qPrintable(err));

  WellLogWarnings fileWarnings;
  const QVector<WellLogFile> files =
      WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-mix"), &fileWarnings);
  WellLogWarnings indexWarnings;
  const QVector<WellCurveRef> index =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-mix"), &indexWarnings);
  QCOMPARE(files.size(), 1);
  QCOMPARE(files.at(0).path, goodPath);
  QCOMPARE(files.at(0).versionId, QStringLiteral("ver-good"));
  QCOMPARE(index.size(), 1);
  QCOMPARE(index.at(0).mnemonic, QStringLiteral("GR"));
  QCOMPARE(index.at(0).sourceVersionId, QStringLiteral("ver-good"));
  QVERIFY(fileWarnings.count >= 1);
  QCOMPARE(fileWarnings.count, indexWarnings.count);
  QCOMPARE(fileWarnings.messages, indexWarnings.messages);
  QCOMPARE(fileWarnings.count, 3);
  const QString blob = fileWarnings.messages.join(QLatin1Char('\n'));
  QVERIFY(blob.contains(missingPath));
  QVERIFY(blob.contains(QStringLiteral("文件不存在")));
  QVERIFY(blob.contains(junkPath));
  QVERIFY(blob.contains(QStringLiteral("no curve definitions")));
  QVERIFY(blob.contains(QStringLiteral("ast-nover")));
  QVERIFY(!blob.contains(hiddenPath));

  WellLogWarnings noneFiles;
  WellLogWarnings noneIndex;
  QCOMPARE(WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-none"), &noneFiles).size(), 0);
  QCOMPARE(WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-none"), &noneIndex).size(), 0);
  QCOMPARE(noneFiles.count, 0);
  QCOMPARE(noneIndex.count, 0);
  QCOMPARE(noneFiles.messages.size(), 0);
  QCOMPARE(noneIndex.messages.size(), 0);
}

void TestWellLogSet::headerOnlyIndexStaysUnder200ms()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("fat.las"));
  {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray header =
        "~Version Information\n"
        " VERS. 2.0 : CWLS\n"
        " WRAP. NO : one line\n"
        "~Well Information Block\n"
        " STRT.M 1000.0 :\n"
        " STOP.M 1001.0 :\n"
        " STEP.M 1.0 :\n"
        " NULL. -999.25 :\n"
        " WELL. FAT :\n"
        "~Curve Information Block\n"
        " DEPT.M : DEPTH\n"
        " GR.GAPI : GR\n"
        "~A\n";
    QVERIFY(file.write(header) == header.size());
    const QByteArray garbage(2 * 1024 * 1024, 'x');
    QVERIFY(file.write(garbage) == garbage.size());
  }
  QVERIFY(QFileInfo(path).size() >= 2 * 1024 * 1024);

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-fat"), QStringLiteral("FAT"), &err), qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-fat"), QStringLiteral("ast-fat"), QStringLiteral("ver-fat"),
                   path, 0, true, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));

  QElapsedTimer timer;
  timer.start();
  const QVector<WellCurveRef> index =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-fat"), nullptr);
  const double ms = timer.nsecsElapsed() / 1.0e6;
  const QString msg = QStringLiteral("wellCurveIndex took %1 ms").arg(ms, 0, 'f', 3);
  QVERIFY2(ms < 200.0, qPrintable(msg));
  QCOMPARE(index.size(), 1);
  QCOMPARE(index.at(0).mnemonic, QStringLiteral("GR"));
  QCOMPARE(index.at(0).column, 1);
  QVERIFY(index.at(0).canonical);
}

void TestWellLogSet::closedCatalogWarnsOnceEmptyWellIdDoesNot()
{
  WellLogWarnings nullFiles;
  QCOMPARE(WellLogSet::wellLogFiles(nullptr, QString(), QStringLiteral("well-1"), &nullFiles).size(), 0);
  QCOMPARE(nullFiles.count, 1);
  QCOMPARE(nullFiles.messages, QStringList({QStringLiteral("catalog 未打开")}));
  WellLogWarnings nullIndex;
  QCOMPARE(WellLogSet::wellCurveIndex(nullptr, QString(), QStringLiteral("well-1"), &nullIndex).size(), 0);
  QCOMPARE(nullIndex.messages, nullFiles.messages);
  QCOMPARE(WellLogSet::wellLogFiles(nullptr, QString(), QStringLiteral("well-1"), nullptr).size(), 0);
  QCOMPARE(WellLogSet::wellCurveIndex(nullptr, QString(), QStringLiteral("well-1"), nullptr).size(), 0);

  DataCatalog closed;
  WellLogWarnings closedWarnings;
  QCOMPARE(WellLogSet::wellLogFiles(&closed, QString(), QStringLiteral("well-1"), &closedWarnings).size(), 0);
  QCOMPARE(closedWarnings.count, 1);
  QCOMPARE(closedWarnings.messages.at(0), QStringLiteral("catalog 未打开"));
  WellLogWarnings closedIndex;
  QCOMPARE(WellLogSet::wellCurveIndex(&closed, QString(), QStringLiteral("well-1"), &closedIndex).size(), 0);
  QCOMPARE(closedIndex.count, 1);

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  WellLogWarnings emptyFiles;
  QCOMPARE(WellLogSet::wellLogFiles(&cat, dir.path(), QString(), &emptyFiles).size(), 0);
  QCOMPARE(emptyFiles.count, 0);
  WellLogWarnings emptyIndex;
  QCOMPARE(WellLogSet::wellCurveIndex(&cat, dir.path(), QString(), &emptyIndex).size(), 0);
  QCOMPARE(emptyIndex.count, 0);

  QVERIFY2(addWell(cat, QStringLiteral("well-1"), QStringLiteral("W1"), &err), qPrintable(err));
  WellLogWarnings noLinkFiles;
  WellLogWarnings noLinkIndex;
  QCOMPARE(WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-1"), &noLinkFiles).size(), 0);
  QCOMPARE(WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-1"), &noLinkIndex).size(), 0);
  QCOMPARE(noLinkFiles.count, 0);
  QCOMPARE(noLinkIndex.count, 0);
}

void TestWellLogSet::ordersByOrdinalVersionNumberThenVersionId()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-1"), QStringLiteral("W1"), &err), qPrintable(err));

  struct Spec
  {
    const char *id;
    int ordinal;
    int versionNumber;
    const char *versionId;
    bool primary;
  };
  const Spec specs[] = {
      {"c", 1, 1, "ver-c", false},
      {"a", 0, 2, "ver-a", false},
      {"d", 0, 1, "ver-d", false},
      {"b", 0, 1, "ver-b", true},
  };
  for (const Spec &spec : specs)
  {
    const QString path = writeLas(dir.filePath(QStringLiteral("ord-%1.las").arg(QLatin1String(spec.id))),
                                  {QStringLiteral("DEPT"), QStringLiteral("GR")}, 1000.0, 2);
    QVERIFY(!path.isEmpty());
    QVERIFY2(seedLog(cat, QStringLiteral("well-1"),
                     QStringLiteral("ast-%1").arg(QLatin1String(spec.id)),
                     QString::fromLatin1(spec.versionId), path, spec.ordinal, spec.primary,
                     spec.versionNumber, false, true, QStringLiteral("well_log"), &err),
             qPrintable(err));
  }

  const QVector<WellLogFile> files =
      WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-1"), nullptr);
  QCOMPARE(files.size(), 4);
  QCOMPARE(files.at(0).versionId, QStringLiteral("ver-b"));
  QCOMPARE(files.at(1).versionId, QStringLiteral("ver-d"));
  QCOMPARE(files.at(2).versionId, QStringLiteral("ver-a"));
  QCOMPARE(files.at(3).versionId, QStringLiteral("ver-c"));
  QCOMPARE(files.at(0).ordinal, 0);
  QCOMPARE(files.at(3).ordinal, 1);
}

void TestWellLogSet::readsCurrentVersionOnly()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString oldPath = writeLas(dir.filePath(QStringLiteral("old.las")),
                                   {QStringLiteral("DEPT"), QStringLiteral("GR")}, 1000.0, 2);
  const QString newPath = writeLas(dir.filePath(QStringLiteral("new.las")),
                                   {QStringLiteral("DEPT"), QStringLiteral("SP")}, 2000.0, 2);
  QVERIFY(!oldPath.isEmpty() && !newPath.isEmpty());

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-1"), QStringLiteral("W1"), &err), qPrintable(err));
  CatalogAsset asset;
  asset.id = QStringLiteral("ast-1");
  asset.type = QStringLiteral("well_log");
  asset.format = QStringLiteral("las");
  asset.displayName = QStringLiteral("old.las");
  QVERIFY2(cat.addAsset(asset, &err), qPrintable(err));
  CatalogVersion older;
  older.id = QStringLiteral("ver-1");
  older.assetId = asset.id;
  older.stage = QStringLiteral("RAW");
  older.versionNumber = 1;
  older.managed = false;
  older.path = oldPath;
  older.fileName = QStringLiteral("old.las");
  QVERIFY2(cat.addVersion(older, &err), qPrintable(err));
  CatalogVersion newer;
  newer.id = QStringLiteral("ver-2");
  newer.assetId = asset.id;
  newer.stage = QStringLiteral("RAW");
  newer.versionNumber = 2;
  newer.managed = false;
  newer.path = newPath;
  newer.fileName = QStringLiteral("new.las");
  QVERIFY2(cat.addVersion(newer, &err), qPrintable(err));
  EntityAssetLink link;
  link.entityType = QStringLiteral("well");
  link.entityId = QStringLiteral("well-1");
  link.assetId = asset.id;
  link.role = QStringLiteral("well_log");
  link.isPrimary = true;
  link.ordinal = 0;
  QVERIFY2(cat.addLink(link, &err), qPrintable(err));

  WellLogWarnings warnings;
  const QVector<WellLogFile> files =
      WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-1"), &warnings);
  QCOMPARE(warnings.count, 0);
  QCOMPARE(files.size(), 1);
  QCOMPARE(files.at(0).versionId, QStringLiteral("ver-2"));
  QCOMPARE(files.at(0).path, newPath);
  QCOMPARE(files.at(0).curveNames, QStringList({QStringLiteral("DEPT"), QStringLiteral("SP")}));
  const QVector<WellCurveRef> index =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-1"), &warnings);
  QCOMPARE(warnings.count, 0);
  QCOMPARE(index.size(), 1);
  QCOMPARE(index.at(0).mnemonic, QStringLiteral("SP"));
  QCOMPARE(index.at(0).sourceVersionId, QStringLiteral("ver-2"));
  QVERIFY(findMnemonic(index, QStringLiteral("GR")) == nullptr);
}

void TestWellLogSet::doesNotElectPrimaryWhenMarkedFileIsMissing()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString pathB = writeLas(dir.filePath(QStringLiteral("fileB.las")),
                                 {QStringLiteral("DEPT"), QStringLiteral("GR")}, 1000.0, 2);
  const QString pathC = writeLas(dir.filePath(QStringLiteral("fileC.las")),
                                 {QStringLiteral("DEPT"), QStringLiteral("GR")}, 2000.0, 2);
  const QString missing = dir.filePath(QStringLiteral("gone.las"));
  QVERIFY(!pathB.isEmpty() && !pathC.isEmpty());

  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  QVERIFY2(addWell(cat, QStringLiteral("well-1"), QStringLiteral("W1"), &err), qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-1"), QStringLiteral("ast-a"), QStringLiteral("ver-a"),
                   missing, 0, true, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-1"), QStringLiteral("ast-b"), QStringLiteral("ver-b"),
                   pathB, 1, false, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));
  QVERIFY2(seedLog(cat, QStringLiteral("well-1"), QStringLiteral("ast-c"), QStringLiteral("ver-c"),
                   pathC, 2, false, 1, false, true, QStringLiteral("well_log"), &err),
           qPrintable(err));

  WellLogWarnings warnings;
  const QVector<WellLogFile> files =
      WellLogSet::wellLogFiles(&cat, dir.path(), QStringLiteral("well-1"), &warnings);
  QCOMPARE(files.size(), 2);
  QCOMPARE(files.at(0).path, pathB);
  QCOMPARE(files.at(1).path, pathC);
  QVERIFY(!files.at(0).isPrimary);
  QVERIFY(!files.at(1).isPrimary);
  QVERIFY(warnings.count >= 1);
  QVERIFY(warnings.messages.join(QLatin1Char('\n')).contains(QStringLiteral("文件不存在")));

  const QVector<WellCurveRef> index =
      WellLogSet::wellCurveIndex(&cat, dir.path(), QStringLiteral("well-1"), &warnings);
  QCOMPARE(index.size(), 2);
  QVERIFY(findMnemonic(index, QStringLiteral("GR")) == nullptr);
  const QString aliasB = QStringLiteral("GR@") + QFileInfo(pathB).completeBaseName();
  const QString aliasC = QStringLiteral("GR@") + QFileInfo(pathC).completeBaseName();
  const WellCurveRef *b = findMnemonic(index, aliasB);
  const WellCurveRef *c = findMnemonic(index, aliasC);
  QVERIFY(b && c);
  QVERIFY(!b->canonical);
  QVERIFY(!c->canonical);
  QCOMPARE(b->sourceVersionId, QStringLiteral("ver-b"));
  QCOMPARE(c->sourceVersionId, QStringLiteral("ver-c"));
}

QTEST_MAIN(TestWellLogSet)
#include "tst_welllogset.moc"
