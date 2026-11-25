// 层：数据
#pragma once

#include <QFileInfo>
#ifdef Q_OS_WIN
#include "fspathutils.h"
#include <QDir>
#endif

namespace paleo {
inline QString canonicalLocalFilePath(const QFileInfo &file)
{
#ifdef Q_OS_WIN
  // QFileInfo can leave a parent directory junction in the result. Resolve the
  // existing file by handle so its working-copy and catalog paths agree.
  std::error_code error;
  const auto path = std::filesystem::canonical(toFsPath(file.filePath()), error);
  return error ? QString() : QDir::fromNativeSeparators(fromFsPath(path));
#else
  return file.canonicalFilePath();
#endif
}
} // namespace paleo
