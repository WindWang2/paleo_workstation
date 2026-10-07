// 层：功能
#include "projectopen.h"

#include "../qgis/qgisprojectservice.h"
#include "../metadata/paleoprojectfile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

ProjectOpenWorkflow::ProjectOpenWorkflow(QgisProjectService *projSvc,
                                         QObject *parent)
    : QObject(parent), m_projSvc(projSvc)
{
  if (m_projSvc)
    connect(m_projSvc, &QgisProjectService::openFinished, this, [this](bool success) {
      if (!success && !m_projSvc->lastOpenCancelled())
        emit openFailed(tr("打开工程失败"), m_projSvc->lastErrors().join(QLatin1Char('\n')), true);
    });
}

bool ProjectOpenWorkflow::openPath(const QString &path)
{
  if (path.isEmpty() || !m_projSvc)
    return false;
  const QFileInfo fi(path);
  if (!fi.exists())
  {
    emit openFailed(tr("打开工程"), tr("路径不存在: %1").arg(path), false);
    return false;
  }
  if (fi.isFile())
  {
    if (!m_projSvc->openProjectAsync(fi.absoluteFilePath()))
    {
      if (!m_projSvc->lastOpenCancelled()) // #152：锁冲突时用户取消 → 不报错
        emit openFailed(tr("打开工程失败"),
                      m_projSvc->lastErrors().join(QLatin1Char('\n')), true);
      return false;
    }
    return true;
  }
  if (fi.isDir())
  {
    const QString dir = fi.absoluteFilePath();
    const QString paleo = paleoProjectFilePath(dir);
    if (QFile::exists(paleo))
    {
      if (!m_projSvc->openProjectAsync(paleo))
      {
        if (!m_projSvc->lastOpenCancelled()) // #152：锁冲突时用户取消 → 不报错
          emit openFailed(tr("打开工程失败"),
                        m_projSvc->lastErrors().join(QLatin1Char('\n')), true);
        return false;
      }
      return true;
    }
    const QString qgz = QDir(dir).filePath(
        QFileInfo(dir).fileName() + QStringLiteral(".qgz"));
    const QStringList qgzFiles =
        QDir(dir).entryList(QStringList{QStringLiteral("*.qgz")});
    if (QFile::exists(qgz) || !qgzFiles.isEmpty())
    {
      const QString adopt =
          QFile::exists(qgz) ? qgz : QDir(dir).filePath(qgzFiles.first());
      if (!m_projSvc->openProjectAsync(adopt))
      {
        if (!m_projSvc->lastOpenCancelled()) // #152：锁冲突时用户取消 → 不报错
          emit openFailed(tr("打开工程失败"),
                        m_projSvc->lastErrors().join(QLatin1Char('\n')), true);
        return false;
      }
      return true;
    }
    if (!m_projSvc->createProject(qgz))
    {
      if (!m_projSvc->lastOpenCancelled()) // #152：锁冲突时用户取消 → 不报错
        emit openFailed(tr("新建工程失败"),
                      m_projSvc->lastErrors().join(QLatin1Char('\n')), true);
      return false;
    }
    if (QFile::exists(paleoProjectFilePath(dir)))
    {
      bool ok = false;
      QString perr;
      PaleoProjectFile pf = readProjectFile(paleoProjectFilePath(dir), &ok, &perr);
      if (ok)
      {
        pf.sourceAreaRoot = dir;
        QString werr;
        if (!writeProjectFile(dir, pf, &werr))
          qWarning() << "projectopen: stamp sourceArea failed:" << werr;
      }
    }
    emit folderImportRequested(dir);
    return true;
  }
  return false;
}

void ProjectOpenWorkflow::stampSourceArea(const QString &dir,
                                          const QVariantMap &stats)
{
  if (!m_projSvc || m_projSvc->projectPath().isEmpty())
    return;
  // 只回填「从工区文件夹新建」的工程（工程目录 == 导入目录）；否则静默跳过——
  // 普通「导入工区文件夹」不应改写一个不相关工程的来源字段。
  const QString projDir = QFileInfo(m_projSvc->projectPath()).absolutePath();
  if (QDir(projDir) != QDir(dir))
    return;
  const QString paleo = paleoProjectFilePath(projDir);
  if (!QFile::exists(paleo))
    return;
  bool ok = false;
  QString rerr;
  PaleoProjectFile pf = readProjectFile(paleo, &ok, &rerr);
  if (!ok)
  {
    qWarning() << "projectopen: read project file failed:" << rerr;
    return;
  }
  pf.sourceAreaRoot = dir;
  pf.sourceAreaImportedUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  pf.sourceStats = stats;
  QString werr;
  if (!writeProjectFile(projDir, pf, &werr))
    qWarning() << "projectopen: write project file failed:" << werr;
}
