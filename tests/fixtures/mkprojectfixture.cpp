// 层：测试壳
#include "mkprojectfixture.h"

#include "../../src/catalog/datacatalog.h"
#include "../../src/metadata/paleoprojectfile.h"

#include <QDir>
#include <QProcess>

#ifndef MKPROJECT_BIN
#error "mkprojectfixture.cpp needs MKPROJECT_BIN (path to paleo_mkproject executable)"
#endif
#ifndef MKPROJECT_MINI_DIR
#error "mkprojectfixture.cpp needs MKPROJECT_MINI_DIR (tools/reference/mkproject/mini)"
#endif

namespace MkProjectFixture
{
  QString miniDir() { return QString::fromLatin1(MKPROJECT_MINI_DIR); }
  QString miniManifestPath()
  {
    return QDir(miniDir()).filePath(QStringLiteral("manifest.json"));
  }
  QString mkprojectBinary() { return QString::fromLatin1(MKPROJECT_BIN); }

  RunResult buildMiniProject(const QString &outDir, int timeoutMs)
  {
    RunResult r;
    r.projectDir = outDir;
    r.qgzPath = QDir(outDir).filePath(QStringLiteral("mini.qgz"));
    r.paleoPath = QDir(outDir).filePath(
        QString::fromLatin1(PaleoProjectFile::kFileName));
    QDir().mkpath(outDir);
    QProcess proc;
    proc.setProgram(mkprojectBinary());
    proc.setArguments({QStringLiteral("--manifest"), miniManifestPath(),
                       QStringLiteral("--out"), outDir,
                       QStringLiteral("--name"), QStringLiteral("mini")});
    proc.start();
    if (!proc.waitForStarted(10000))
    {
      r.errorText = QStringLiteral("paleo_mkproject 起不来: %1 (%2)")
                        .arg(mkprojectBinary(), proc.errorString());
      return r;
    }
    if (!proc.waitForFinished(timeoutMs))
    {
      // 就绪等待：超时杀进程，把已产出的 stdout 留给诊断。
      r.standardOutput = QString::fromLocal8Bit(proc.readAllStandardOutput());
      r.errorText = QStringLiteral("paleo_mkproject 超时（%1ms）").arg(timeoutMs);
      proc.kill();
      proc.waitForFinished(5000);
      return r;
    }
    r.ran = true;
    r.exitCode = proc.exitCode();
    // mkproject 的输出版面走 qPrintable（本地 8 位：Windows=GBK、Linux=
    // UTF-8）——按同口径解码，中文断言跨平台成立。
    r.standardOutput = QString::fromLocal8Bit(proc.readAllStandardOutput());
    if (proc.exitStatus() != QProcess::NormalExit)
      r.errorText = QStringLiteral("paleo_mkproject 异常退出（crash）");
    return r;
  }

  CatalogSummary openCatalogSummary(const QString &projectDir)
  {
    CatalogSummary s;
    DataCatalog cat;
    QString err;
    s.opened = cat.open(projectDir, &err);
    s.openError = err;
    if (!s.opened)
      return s;
    const QVector<CatalogEntity> entities = cat.entities();
    for (const CatalogEntity &e : entities)
      s.entitiesByType[e.entityType] += 1;
    for (const CatalogEntity &e : cat.entities(QStringLiteral("well")))
    {
      s.wellNames.append(e.name);
      s.wellCoordinateStatus.insert(e.name, e.coordinateStatus);
    }
    s.wells = s.wellNames.size();
    s.wellNames.sort();
    s.assets = cat.assets().size();
    QHash<QString, QString> idToName;
    for (const CatalogEntity &e : entities)
      idToName.insert(e.id, e.name);
    for (const EntityAssetLink &l : cat.links())
    {
      s.linkRoles[l.role] += 1;
      if (l.unresolved)
      {
        ++s.unresolvedLinks;
        continue;
      }
      const QString well = idToName.value(l.entityId);
      if (!well.isEmpty())
        s.wellRoles[well][l.role] += 1;
    }
    return s;
  }
} // namespace MkProjectFixture
