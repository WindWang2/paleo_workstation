// 层：数据
#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QString>

#ifdef Q_OS_WIN
// windows.h defines max as a function-like macro; it silently rewrites std::max
// inside any QGIS header included after this one (C2589 on MSVC).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#endif

// 故障注入点（用于单测模拟中途崩溃、落盘失败或断电中断）
enum class PaleoAtomicFaultPoint
{
  None,
  BeforeWrite,      // 临时文件创建前即刻失败
  DuringWrite,      // 写入部分字节后中断
  SimulateDiskFull, // 模拟磁盘配额耗尽/空间满
  BeforeReplace     // 数据已写完并刷盘，但在执行原子替换前夕中断
};

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

// 原子写入文件：
// 1. 同目录写入临时同胞文件（同目录保证处于同一文件系统卷，使后续原子替换为 O(1) 元数据重命名）。
// 2. 刷盘（flush / fsync）。
// 3. 调用 paleoReplaceFile 原子替换到目标路径。
// 4. 若中途出现任何错误或触发故障注入：清除临时文件，保留原有目标文件 100% 完好无损。
inline bool paleoWriteFileAtomic(const QString &destination,
                                 const QByteArray &data,
                                 QString *errorMessage = nullptr,
                                 PaleoAtomicFaultPoint faultPoint = PaleoAtomicFaultPoint::None)
{
  if (destination.isEmpty())
  {
    if (errorMessage)
      *errorMessage = QStringLiteral("目标路径为空");
    return false;
  }

  QFileInfo destInfo(destination);
  QDir dir = destInfo.dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    if (errorMessage)
      *errorMessage = QStringLiteral("无法创建目标父目录：%1").arg(dir.absolutePath());
    return false;
  }

  if (faultPoint == PaleoAtomicFaultPoint::BeforeWrite)
  {
    if (errorMessage)
      *errorMessage = QStringLiteral("故障注入：写入前模拟中断");
    return false;
  }

  // 同目录临时同胞文件
  const quint64 nonce = QRandomGenerator::global()->generate64();
  const QString tmpPath = dir.filePath(QStringLiteral(".%1.tmp.%2")
                                           .arg(destInfo.fileName())
                                           .arg(nonce, 0, 16));

  QFile tmpFile(tmpPath);
  if (!tmpFile.open(QIODevice::WriteOnly))
  {
    if (errorMessage)
      *errorMessage = QStringLiteral("无法创建临时写入文件：%1 (%2)").arg(tmpPath, tmpFile.errorString());
    return false;
  }

  if (faultPoint == PaleoAtomicFaultPoint::DuringWrite)
  {
    // 写入一半字节后中断模拟
    const qint64 partialSize = data.size() / 2;
    if (partialSize > 0)
      tmpFile.write(data.constData(), partialSize);
    tmpFile.close();
    QFile::remove(tmpPath);
    if (errorMessage)
      *errorMessage = QStringLiteral("故障注入：写入中途模拟中断");
    return false;
  }

  if (faultPoint == PaleoAtomicFaultPoint::SimulateDiskFull)
  {
    tmpFile.close();
    QFile::remove(tmpPath);
    if (errorMessage)
      *errorMessage = QStringLiteral("故障注入：模拟磁盘空间耗尽 (ENOSPC)");
    return false;
  }

  if (tmpFile.write(data) != data.size())
  {
    if (errorMessage)
      *errorMessage = QStringLiteral("写入临时文件失败：%1").arg(tmpFile.errorString());
    tmpFile.close();
    QFile::remove(tmpPath);
    return false;
  }

  if (!tmpFile.flush())
  {
    if (errorMessage)
      *errorMessage = QStringLiteral("临时文件刷盘失败：%1").arg(tmpFile.errorString());
    tmpFile.close();
    QFile::remove(tmpPath);
    return false;
  }

  tmpFile.close();

  if (faultPoint == PaleoAtomicFaultPoint::BeforeReplace)
  {
    QFile::remove(tmpPath);
    if (errorMessage)
      *errorMessage = QStringLiteral("故障注入：原子替换前夕模拟崩溃");
    return false;
  }

  if (!paleoReplaceFile(tmpPath, destination))
  {
    if (errorMessage)
      *errorMessage = QStringLiteral("原子替换失败：%1 -> %2").arg(tmpPath, destination);
    QFile::remove(tmpPath);
    return false;
  }

  return true;
}

// 原子事务与脏退出标记（仿照 CrashReport .running 机制）：
// 用于复杂或多步骤原子落盘，具有明确的脏标记（.running）、提交（commit）与回滚（rollback）能力。
// 若程序在 commit 前崩溃，磁盘残留目标文件对应的 .running 脏标记，且原有 destination 文件完好。
class PaleoAtomicTransaction
{
public:
  explicit PaleoAtomicTransaction(const QString &destination)
    : m_destination(destination)
  {
    if (!destination.isEmpty())
    {
      QFileInfo info(destination);
      m_runningMarkerPath = info.dir().filePath(QStringLiteral(".%1.running").arg(info.fileName()));
      const quint64 nonce = QRandomGenerator::global()->generate64();
      m_stagePath = info.dir().filePath(QStringLiteral(".%1.staged.%2").arg(info.fileName()).arg(nonce, 0, 16));
    }
  }

  ~PaleoAtomicTransaction()
  {
    if (m_begun && !m_committed)
    {
      rollback();
    }
  }

  // 检查目标文件是否残留未完成的脏标记
  static bool hasDirtyMarker(const QString &destination)
  {
    if (destination.isEmpty())
      return false;
    QFileInfo info(destination);
    const QString marker = info.dir().filePath(QStringLiteral(".%1.running").arg(info.fileName()));
    return QFile::exists(marker);
  }

  // 清除脏退出标记与残留的孤立临时文件
  static bool recover(const QString &destination)
  {
    if (destination.isEmpty())
      return false;
    QFileInfo info(destination);
    const QString marker = info.dir().filePath(QStringLiteral(".%1.running").arg(info.fileName()));
    if (QFile::exists(marker))
      QFile::remove(marker);

    // 清理同目录残留的同名 .tmp / .staged 文件
    QDir dir = info.dir();
    const QString name = info.fileName();
    const QStringList orphanPatterns = {
      QStringLiteral(".%1.tmp.*").arg(name),
      QStringLiteral(".%1.staged.*").arg(name)
    };
    const QStringList orphans = dir.entryList(orphanPatterns, QDir::Files);
    for (const QString &orphan : orphans)
    {
      dir.remove(orphan);
    }
    return true;
  }

  QString runningMarkerPath() const { return m_runningMarkerPath; }
  QString stagePath() const { return m_stagePath; }

  bool begin(QString *errorMessage = nullptr)
  {
    if (m_destination.isEmpty())
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("目标路径为空");
      return false;
    }
    QFileInfo info(m_destination);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("无法创建目标目录：%1").arg(dir.absolutePath());
      return false;
    }

    // 建立 .running 脏标记
    QFile flag(m_runningMarkerPath);
    if (!flag.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("无法创建 .running 脏标记：%1").arg(flag.errorString());
      return false;
    }
    flag.write(QByteArrayLiteral("active\n"));
    flag.close();

    m_begun = true;
    m_committed = false;
    return true;
  }

  bool writeStaged(const QByteArray &data, QString *errorMessage = nullptr)
  {
    if (!m_begun)
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("事务尚未开始");
      return false;
    }

    QFile f(m_stagePath);
    if (!f.open(QIODevice::WriteOnly))
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("无法打开暂存文件：%1 (%2)").arg(m_stagePath, f.errorString());
      return false;
    }
    if (f.write(data) != data.size())
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("暂存数据写入失败：%1").arg(f.errorString());
      f.close();
      return false;
    }
    f.flush();
    f.close();
    return true;
  }

  bool commit(QString *errorMessage = nullptr)
  {
    if (!m_begun)
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("事务尚未开始");
      return false;
    }
    if (!QFile::exists(m_stagePath))
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("暂存文件不存在：%1").arg(m_stagePath);
      return false;
    }

    if (!paleoReplaceFile(m_stagePath, m_destination))
    {
      if (errorMessage)
        *errorMessage = QStringLiteral("提交原子替换失败：%1 -> %2").arg(m_stagePath, m_destination);
      return false;
    }

    // 替换成功后清除 .running 脏标记
    QFile::remove(m_runningMarkerPath);
    m_committed = true;
    return true;
  }

  void rollback()
  {
    if (QFile::exists(m_stagePath))
      QFile::remove(m_stagePath);
    if (QFile::exists(m_runningMarkerPath))
      QFile::remove(m_runningMarkerPath);
    m_committed = false;
  }

  // 模拟程序崩溃（保留 .running 与 stagePath，不调 rollback/commit）
  void simulateCrash()
  {
    m_committed = true; // 阻止析构函数自动回滚，模拟硬杀残留
  }

private:
  QString m_destination;
  QString m_runningMarkerPath;
  QString m_stagePath;
  bool m_begun = false;
  bool m_committed = false;
};
