// 层：数据
#pragma once
#include <QFile>
#include <QString>

namespace paleo::python::delivery {

bool safeRelative(const QString &path);
QString checkedAbsolutePath(const QString &path);
bool directory(const QString &absolutePath, bool create);
bool syncDirectory(const QString &absolutePath);
bool regular(const QString &absolutePath);
bool regularHandle(QFile &file);
bool openRegular(QFile &file, const QString &absolutePath);
bool syncFile(QFile &file);

// Same persistent byte/flock lock used by the offline Python preparers.
// Closing releases the OS lock; the file is deliberately never unlinked.
class StoreLock {
public:
  explicit StoreLock(const QString &path);
  ~StoreLock();
  StoreLock(const StoreLock &) = delete;
  StoreLock &operator=(const StoreLock &) = delete;
  bool locked() const;

private:
  QFile m_file;
  bool m_locked = false;
};

} // namespace paleo::python::delivery
