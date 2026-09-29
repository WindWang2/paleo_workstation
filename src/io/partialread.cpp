// 层：数据
#include "partialread.h"

#include "encodingdetect.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

#include <cerrno>
#include <csignal>

namespace
{
  // 过渡文件名特征：受管写入路径的既有约定（dataimportservice 的 .partial、
  // 各缓存的 .tmp<pid>_xxxx、崩溃报告的 .running）。
  bool isTempName(const QString &fileName)
  {
    return fileName.endsWith(QStringLiteral(".partial")) ||
           fileName.contains(QStringLiteral(".tmp")) ||
           fileName.endsWith(QStringLiteral(".running"));
  }

  // ".tmp12345_678" / ".tmp12345" 形态里抠 pid；抠不到 → 0。
  qint64 pidFromTempName(const QString &fileName)
  {
    const int at = fileName.lastIndexOf(QStringLiteral(".tmp"));
    if (at < 0)
      return 0;
    qint64 pid = 0;
    int i = at + 4;
    for (; i < fileName.size() && fileName[i].isDigit(); ++i)
      pid = pid * 10 + fileName[i].digitValue();
    if (i == at + 4) // .tmp 后没跟数字
      return 0;
    return pid;
  }
} // namespace

namespace PartialRead
{

PartialText readTextPartial(const QString &path, qint64 maxBytes, QString *error)
{
  PartialText out;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
    return out;
  }
  out.bytesTotal = f.size();
  const qint64 want = maxBytes > 0 ? qMin(maxBytes, out.bytesTotal) : out.bytesTotal;
  QByteArray raw = f.read(want);
  out.bytesRead = raw.size();
  out.truncated = out.bytesRead < out.bytesTotal;
  if (out.truncated)
  {
    // 行中间截断：回退到最后一个换行，半行丢弃。
    const int lastNl = raw.lastIndexOf('\n');
    if (lastNl >= 0)
      raw.truncate(lastNl + 1);
    else
      raw.clear(); // 前 maxBytes 里连一个完整行都没有
  }
  out.text = EncodingDetect::decodeText(raw);
  return out;
}

bool processAlive(qint64 pid)
{
  if (pid <= 0)
    return false;
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

QStringList sweepTempFiles(const QString &dir, qint64 keepNewerThanMs)
{
  QStringList removed;
  QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  while (it.hasNext())
  {
    const QString path = it.next();
    const QFileInfo info(path);
    if (!isTempName(info.fileName()))
      continue;
    const qint64 pid = pidFromTempName(info.fileName());
    if (pid > 0)
    {
      if (processAlive(pid))
        continue; // 活进程的过渡文件——可能正在写
      if (QFile::remove(path))
        removed.append(path);
      continue;
    }
    // 无 pid 后缀：按超龄清。
    const qint64 ageMs = nowMs - info.lastModified().toMSecsSinceEpoch();
    if (ageMs >= keepNewerThanMs && QFile::remove(path))
      removed.append(path);
  }
  return removed;
}

// ---------------------------------------------------------------------------
// WriteGuard
// ---------------------------------------------------------------------------
static QString pidLineOf(qint64 pid, const QString &note)
{
  return QStringLiteral("%1 %2").arg(pid).arg(note);
}

WriteGuard::WriteGuard(const QString &targetPath, int maxWaitMs)
{
  m_lockPath = targetPath + QStringLiteral(".wlock");
  m_locked = acquire(maxWaitMs);
}

bool WriteGuard::acquire(int maxWaitMs)
{
  const int stepMs = 20;
  int waited = 0;
  const QByteArray line =
      pidLineOf(QCoreApplication::applicationPid(), QStringLiteral("paleo-write")).toUtf8();
  for (;;)
  {
    // 创建语义必须「不存在才成功」——NewOnly，绝不覆盖在途锁。
    QFile f(m_lockPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
      if (f.write(line) == line.size() && f.flush())
      {
        f.close();
        return true;
      }
      f.close();
      QFile::remove(m_lockPath); // 写失败不占位
    }
    else if (QFile::exists(m_lockPath))
    {
      // 已有锁：持有者活着（且不是自己）→ 拒；死锁/自残留 → 回收重试。
      QFile existing(m_lockPath);
      if (existing.open(QIODevice::ReadOnly))
      {
        const qint64 holder =
            QString::fromUtf8(existing.readLine()).section(' ', 0, 0).toLongLong();
        if (holder > 0 && processAlive(holder) &&
            holder != QCoreApplication::applicationPid())
        {
          m_reason = QStringLiteral("被进程 %1 锁定（%2）").arg(holder).arg(m_lockPath);
          return false;
        }
      }
      QFile::remove(m_lockPath); // 陈锁回收——立即重试（不等轮询节拍）
      continue;
    }
    if (maxWaitMs <= 0 || waited >= maxWaitMs)
    {
      m_reason = QStringLiteral("无法获取写锁 %1").arg(m_lockPath);
      return false;
    }
    QThread::msleep(stepMs);
    waited += stepMs;
  }
}

void WriteGuard::release()
{
  if (m_locked)
  {
    QFile::remove(m_lockPath);
    m_locked = false;
  }
}

WriteGuard::~WriteGuard()
{
  release();
}

} // namespace PartialRead
