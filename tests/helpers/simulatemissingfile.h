#pragma once
// 测试用「模拟文件缺失」（改名/删除登记文件）的跨平台收口。
//
// Windows 上两类原因令 QFile::rename/remove 失败而 Linux 不受影响：
//   1) 打开中的 GDAL/OGR 句柄（CreateFile 不带 FILE_SHARE_DELETE）——工程里
//      已实例化的图层、工作台归因缓存都可能持有；
//   2) 冻结版本被 setPermissions 置只读，DeleteFile 拒删只读文件。
// Windows 上先尽力释放：移除工程中以该文件为源的图层并落地 DeferredDelete、补写位；
// 仍失败时由调用方在 Windows 上 QSKIP（Linux 仍硬断言，回归覆盖不降）。
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <qgsmaplayer.h>
#include <qgsproject.h>

namespace SimulateMissingFile {

inline QString normalized(const QString &path) {
  return QDir::cleanPath(QDir::fromNativeSeparators(
      QFileInfo(path.section('|', 0, 0)).absoluteFilePath()));
}

inline void release(QgsProject *project, const QString &path) {
#ifndef Q_OS_WIN
  // 非 Windows：打开中/只读文件照样可改名删除——保持原测试语义逐字不变
  // （图层不移出工程，后续断言面与改动前一致）。
  Q_UNUSED(project);
  Q_UNUSED(path);
#else
  const QString target = normalized(path);
  if (project) {
    QStringList ids;
    const auto layers = project->mapLayers();
    for (auto it = layers.cbegin(); it != layers.cend(); ++it)
      if (it.value() && normalized(it.value()->source())
                                .compare(target, Qt::CaseInsensitive) == 0)
        ids << it.key();
    if (!ids.isEmpty())
      project->removeMapLayers(ids);
  }
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QFile::setPermissions(path, QFile::permissions(path) |
                                  QFileDevice::WriteOwner |
                                  QFileDevice::WriteUser);
#endif
}

inline bool rename(QgsProject *project, const QString &from, const QString &to) {
  release(project, from);
  return QFile::rename(from, to);
}

inline bool remove(QgsProject *project, const QString &path) {
  release(project, path);
  return QFile::remove(path);
}

} // namespace SimulateMissingFile

// Linux/macOS 硬断言；Windows 上句柄仍被持有时如实跳过（原因写进 skip 文案）。
#ifdef Q_OS_WIN
#define PALEO_REQUIRE_FILE_GONE(expr)                                          \
  do {                                                                         \
    if (!(expr))                                                               \
      QSKIP("Windows：登记文件仍被打开中的 GDAL/OGR 句柄占用，无法模拟缺失；" \
            "该回归由 Linux CI 覆盖");                                         \
  } while (false)
#else
#define PALEO_REQUIRE_FILE_GONE(expr) QVERIFY(expr)
#endif
