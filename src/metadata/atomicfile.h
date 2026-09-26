#pragma once

#include <QFile>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cstdio>
#endif

// Replace a file with a fully written sibling without first removing the
// existing destination. QFile::rename and the Windows CRT rename reject an
// existing destination; MoveFileExW provides the replace operation on Windows.
inline bool paleoReplaceFile(const QString &source, const QString &destination)
{
#ifdef Q_OS_WIN
  return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return ::rename(QFile::encodeName(source).constData(),
                  QFile::encodeName(destination).constData()) == 0;
#endif
}
