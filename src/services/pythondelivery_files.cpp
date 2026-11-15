// 层：数据
#include "pythondelivery_files.h"
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QVector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace paleo::python::delivery {
namespace {
QString checkedAbsolute(const QString &path) {
  if (path.isEmpty() || path.contains(QChar(0)) || !QDir::isAbsolutePath(path)) return {};
  auto native = QDir::fromNativeSeparators(path);
  while (native.size() > 1 && native.endsWith('/') && !QDir(native).isRoot()) native.chop(1);
  if (QDir::cleanPath(native) != native) return {};
#ifdef Q_OS_WIN
  // Device and UNC aliases cannot provide the local immutable-store contract.
  if (native.size() < 3 || !native.at(0).isLetter() || native.at(1) != ':' || native.at(2) != '/') return {};
  if (native.mid(2).contains(':')) return {};
#endif
  return native;
}

#ifdef Q_OS_WIN
HANDLE nativeHandle(QFile &file) {
  if (!file.isOpen() || file.handle() < 0) return INVALID_HANDLE_VALUE;
  return reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
}

bool plainDirectory(const QString &path) {
  const auto native = QDir::toNativeSeparators(path);
  const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool pinParents(const QString &path, QVector<HANDLE> &retained) {
  QString current = path.left(3);
  auto parts = QFileInfo(path).absolutePath().mid(3).split('/', Qt::SkipEmptyParts);
  parts.prepend(QString());
  for (const auto &part : parts) {
    if (!part.isEmpty()) current = QDir(current).filePath(part);
    const auto native = QDir::toNativeSeparators(current);
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    retained.append(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
  }
  return true;
}

bool adopt(QFile &file, const QString &path, bool write) {
  QVector<HANDLE> retained;
  const auto release = qScopeGuard([&] { for (const auto handle : retained) CloseHandle(handle); });
  if (!pinParents(path, retained)) return false;
  const auto native = QDir::toNativeSeparators(path);
  const DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
  const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), access,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 write ? OPEN_ALWAYS : OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), (write ? _O_RDWR : _O_RDONLY) | _O_BINARY);
  if (fd < 0) { CloseHandle(handle); return false; }
  if (file.open(fd, (write ? QIODevice::ReadWrite : QIODevice::ReadOnly) | QIODevice::Unbuffered,
                QFileDevice::AutoCloseHandle)) return true;
  _close(fd);
  return false;
}
#else
int openDirectory(const QString &path, bool create) {
  const auto checked = checkedAbsolute(path);
  if (checked.isEmpty()) return -1;
  int parent = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  for (const auto &part : checked.split('/', Qt::SkipEmptyParts)) {
    if (parent < 0) return -1;
    const auto name = QFile::encodeName(part);
    int next = ::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0 && create && errno == ENOENT) {
      if (::mkdirat(parent, name.constData(), 0700) == 0 || errno == EEXIST)
        next = ::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    ::close(parent);
    parent = next;
  }
  return parent;
}

bool adopt(QFile &file, const QString &path, bool write) {
  const auto checked = checkedAbsolute(path);
  if (checked.isEmpty()) return false;
  const QFileInfo info(checked);
  const auto parent = openDirectory(info.absolutePath(), false);
  if (parent < 0) return false;
  const auto name = QFile::encodeName(info.fileName());
  const int flags = (write ? O_RDWR | O_CREAT : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
  const int fd = ::openat(parent, name.constData(), flags, 0600);
  ::close(parent);
  if (fd < 0) return false;
  if (file.open(fd, (write ? QIODevice::ReadWrite : QIODevice::ReadOnly) | QIODevice::Unbuffered,
                QFileDevice::AutoCloseHandle)) return true;
  ::close(fd);
  return false;
}
#endif
} // namespace

bool safeRelative(const QString &path) {
  if (path.isEmpty() || path.size() > 1024 || path.toUcs4().size() > 512 ||
      path.contains('\\') || path.contains(':') || QDir::isAbsolutePath(path)) return false;
  for (const auto c : path)
    if (c.unicode() < 32 || QStringLiteral("<>\"|?*").contains(c)) return false;
  for (const auto &part : path.split('/')) {
    if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) return false;
    const auto base = part.section('.', 0, 0).toUpper();
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return false;
    if (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT")) &&
        base.at(3) >= '1' && base.at(3) <= '9') return false;
  }
  return true;
}

bool directory(const QString &absolutePath, bool create) {
  const auto path = checkedAbsolute(absolutePath);
  if (path.isEmpty()) return false;
#ifdef Q_OS_WIN
  QString current = path.left(3);
  if (!plainDirectory(current)) return false;
  for (const auto &part : path.mid(3).split('/', Qt::SkipEmptyParts)) {
    current = QDir(current).filePath(part);
    if (!plainDirectory(current)) {
      const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
      if (!create || !QDir().mkdir(current, permissions) || !plainDirectory(current)) return false;
    }
  }
  return true;
#else
  const auto fd = openDirectory(path, create);
  if (fd < 0) return false;
  ::close(fd);
  return true;
#endif
}

bool syncDirectory(const QString &absolutePath) {
#ifdef Q_OS_WIN
  // Match the offline preparer: Windows does not claim POSIX directory fsync.
  Q_UNUSED(absolutePath)
  return true;
#else
  const auto fd = openDirectory(absolutePath, false);
  if (fd < 0) return false;
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  return synced;
#endif
}

bool regularHandle(QFile &file) {
  if (!file.isOpen() || file.handle() < 0) return false;
#ifdef Q_OS_WIN
  const auto handle = nativeHandle(file);
  BY_HANDLE_FILE_INFORMATION info{};
  return handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK &&
         GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks == 1 &&
         !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#else
  struct stat info{};
  return ::fstat(file.handle(), &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1;
#endif
}

bool regular(const QString &absolutePath) {
  QFile file;
  return openRegular(file, absolutePath);
}

bool syncFile(QFile &file) {
  if (!regularHandle(file) || !file.flush()) return false;
#ifdef Q_OS_WIN
  return FlushFileBuffers(nativeHandle(file)) != FALSE;
#else
  return ::fsync(file.handle()) == 0;
#endif
}

bool openRegular(QFile &file, const QString &absolutePath) {
  const auto path = checkedAbsolute(absolutePath);
  if (file.isOpen() || path.isEmpty() || !directory(QFileInfo(path).absolutePath(), false)) return false;
  file.setFileName(path);
  if (adopt(file, path, false) && regularHandle(file)) return true;
  file.close();
  return false;
}

StoreLock::StoreLock(const QString &path) : m_file(path) {
  const auto absolute = checkedAbsolute(path);
  if (absolute.isEmpty() || !directory(QFileInfo(absolute).absolutePath(), false) ||
      !adopt(m_file, absolute, true) || !regularHandle(m_file)) { m_file.close(); return; }
#ifdef Q_OS_WIN
  OVERLAPPED overlap{};
  m_locked = LockFileEx(nativeHandle(m_file), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0, 1, 0, &overlap) != FALSE;
#else
  m_locked = ::flock(m_file.handle(), LOCK_EX | LOCK_NB) == 0;
#endif
  if (!m_locked || !regularHandle(m_file) ||
      (m_file.size() == 0 && (m_file.write("0", 1) != 1 || !m_file.flush()))) {
    m_locked = false;
    m_file.close();
  }
}

StoreLock::~StoreLock() {
  if (!m_locked) return;
#ifdef Q_OS_WIN
  OVERLAPPED overlap{};
  UnlockFileEx(nativeHandle(m_file), 0, 1, 0, &overlap);
#else
  ::flock(m_file.handle(), LOCK_UN);
#endif
  m_file.close();
}

bool StoreLock::locked() const { return m_locked; }

} // namespace paleo::python::delivery
