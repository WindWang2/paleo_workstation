// 层：数据
#include "projectcopy.h"
#include "metadata/managedpath.h"
#include "metadata/paleoprojectfile.h"
#include <QSet>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QObject>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <filesystem>

namespace paleo::projectcopy {
namespace {
using Snapshot = QMap<QString, QPair<qint64, QDateTime>>;
using Stop = std::function<bool()>;

bool memberSelected(const QString &relative, const QSet<QString> &members, const QSet<QString> &trees, bool directory)
{
  for (const auto &tree : trees) if (relative == tree || relative.startsWith(tree + '/')) return true;
  if (members.contains(relative)) return true;
  if (directory) {
    for (const QString &member : members) if (member.startsWith(relative + '/')) return true;
  }
  for (const auto &suffix : {QStringLiteral("-wal"), QStringLiteral("-journal")})
    if (relative.endsWith(suffix) && members.contains(relative.chopped(suffix.size()))) return true;
  return false;
}

bool listProject(const QString &root, const QSet<QString> &members, const QSet<QString> &trees, Snapshot &files, const Stop &stop, QString &error)
{
  const auto fail = [&](const QString &why) { error = why; return false; };
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(QFileInfo(root).filesystemFilePath(), ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (stop && stop()) return fail(QObject::tr("已取消复制工程。"));
    const QFileInfo info(it->path());
    const QString relative = QDir(root).relativeFilePath(info.absoluteFilePath());
    if (!memberSelected(relative, members, trees, info.isDir())) { it.disable_recursion_pending(); continue; }
    if (info.isSymbolicLink() || info.isJunction())
      return fail(QObject::tr("工程中有链接目录或文件，请先处理后再迁移：%1").arg(relative));
    if (relative.endsWith(QStringLiteral("artifacts/metadata/.project.lock")))
      return fail(QObject::tr("工程正在被编辑，关闭写入它的实例后再迁移。"));
    if (!info.isDir() && !info.isFile()) return fail(QObject::tr("无法读取工程成员：%1").arg(relative));
    // SQLite rebuilds the shared-memory index from its copied WAL; SHM is live process state.
    if (relative.endsWith(QStringLiteral("-shm")) && QFileInfo::exists(info.absoluteFilePath().chopped(4))) continue;
    files.insert(relative, {info.isDir() ? -1 : info.size(), info.lastModified()});
  }
  return !ec || fail(QObject::tr("无法完整读取工程目录：%1").arg(root));
}

bool copyMember(const QString &original, const QString &copy, const Stop &stop, QString &error)
{
  QFile input(original), output(copy);
  if (!input.open(QIODevice::ReadOnly)) { error = input.errorString(); return false; }
  if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { error = output.errorString(); return false; }
  QByteArray buffer(1024 * 1024, Qt::Uninitialized);
  for (;;) {
    if (stop && stop()) { error = QObject::tr("已取消复制工程。"); return false; }
    const qint64 n = input.read(buffer.data(), buffer.size());
    if (n < 0) { error = input.errorString(); return false; }
    if (!n) break;
    if (output.write(buffer.constData(), n) != n) { error = output.errorString(); return false; }
  }
  if (!output.flush()) { error = output.errorString(); return false; }
  output.close();
  const QString sourceHash = managedpath::sha256FileHex(original, &error, stop);
  if (sourceHash.isEmpty()) return false;
  const auto copyHash = managedpath::sha256FileHex(copy, &error, stop);
  if (!copyHash.isEmpty() && copyHash == sourceHash) return true;
  if (error.isEmpty()) error = QObject::tr("工程文件校验失败：%1").arg(original);
  return false;
}
}

QString copyVerified(const QString &entry, const QString &parent, const QString &requiredRoot,
                     QString &error, const Stop &cancelled, const std::function<void(qint64,qint64)> &progress, QStringList *sourceReview)
{
  error.clear();
  const auto fail = [&](const QString &message) -> QString { error = message; return {}; };
  const QFileInfo file(entry), folder(file.absolutePath()), targetParent(parent);
  const QString root = folder.canonicalFilePath(), base = targetParent.canonicalFilePath();
  if (!file.isFile() || file.isSymbolicLink() || root.isEmpty() || QDir(root).isRoot())
    return fail(QObject::tr("请选择独立工程目录中的工程文件。"));
  if (!targetParent.isDir() || base.isEmpty() || !managedpath::isWithinRoot(requiredRoot, base) ||
      managedpath::isWithinRoot(root, base))
    return fail(QObject::tr("请选择数据盘中位于原工程目录以外的目标目录。"));
  const QString target = QDir(base).filePath(folder.fileName());
  if (QFileInfo::exists(target) || QFileInfo(target).isSymbolicLink())
    return fail(QObject::tr("目标目录已经存在，请选择其他位置：%1").arg(target));
  const QString manifestPath = paleoProjectFilePath(root);
  if (!QFileInfo::exists(manifestPath))
    return fail(QObject::tr("此 QGIS 文件没有工程清单 project.paleo，无法确认要复制的资料。请先提供带清单的工程束。"));
  bool ok = false;
  const auto manifest = readProjectFile(manifestPath, &ok, &error, 16 * 1024 * 1024);
  if (!ok) return {};
  if ((file.canonicalFilePath() != QFileInfo(manifestPath).canonicalFilePath() &&
       file.canonicalFilePath() != QFileInfo(QDir(root).filePath(manifest.qgz)).canonicalFilePath()) || manifest.qgz.isEmpty())
    return fail(QObject::tr("所选 QGIS 文件不属于该目录的工程清单，请打开对应的 project.paleo。"));
  const QFileInfo qgisMember(manifest.qgz);
  const QDir project(qgisMember.path());
  const auto owned = [&](const QString &name) { return QDir::cleanPath(project.filePath(name)); };
  const QSet<QString> trees{owned("artifacts"), owned(".paleo/capability-ops")};
  QSet<QString> members{QFileInfo(manifestPath).fileName(), manifest.qgz, manifest.manifest, manifest.catalog,
                        manifest.gpkg, manifest.areaRules, manifest.basemapTopo, manifest.basemapHillshade,
                        manifest.qgz + QStringLiteral(".project.sqlite"), owned(qgisMember.completeBaseName() + ".gpkg"),
                        owned("project_area.json"), owned("litho_lexicon.json"), owned(".paleo/import_ledger.json")};
  members.unite(trees);
  Snapshot snapshot;
  if (!listProject(root, members, trees, snapshot, cancelled, error)) return {};
  if (!snapshot.contains(manifest.qgz)) return fail(QObject::tr("工程清单中的 QGIS 文件缺失：%1").arg(manifest.qgz));
  qint64 total = 0, done = 0;
  for (const auto &item : snapshot) if (item.first > 0) total += item.first;
  const QStorageInfo volume(base);
  if (volume.isValid() && volume.bytesAvailable() >= 0 && total > volume.bytesAvailable())
    return fail(QObject::tr("复制工程需要 %1 字节，目标可用 %2 字节。").arg(total).arg(volume.bytesAvailable()));
  QTemporaryDir stage(QDir(base).filePath(QStringLiteral(".paleo-project-copy-XXXXXX")));
  if (!stage.isValid()) return fail(stage.errorString());
  for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
    const auto to = stage.filePath(it.key());
    if (it.value().first < 0) {
      if (!QDir().mkpath(to)) return fail(QObject::tr("无法创建工程副本目录：%1").arg(to));
    } else {
      if (!copyMember(QDir(root).filePath(it.key()), to, cancelled, error)) return {};
      done += it.value().first;
      if (progress) progress(done, total);
    }
  }
  QStringList review;
  for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
    const auto name = QFileInfo(it.key()).fileName();
    if (name != QStringLiteral("catalog.sqlite") && name != QStringLiteral("data.sqlite")) continue;
    if (!inspectStoredBindings(stage.filePath(it.key()), root, stage.path(), review, error)) return {};
  }
  if (!relocateCopiedLayers(manifest, root, stage.path(), target, error)) return {};
  Snapshot current;
  if (!listProject(root, members, trees, current, cancelled, error)) return {};
  if (snapshot != current) return fail(QObject::tr("复制期间工程发生变化，请关闭编辑后重试。"));
  if (!QDir().rename(stage.path(), target)) return fail(QObject::tr("无法发布已校验的工程副本：%1").arg(target));
  stage.setAutoRemove(false);
  if (sourceReview) *sourceReview = review;
  return QDir(target).filePath(file.fileName());
}
}
