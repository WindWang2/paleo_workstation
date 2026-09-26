#include "crashreport.h"

// 崩溃报告实现。两层结构：
//   1) Qt 侧（非信号上下文）：installCrashHandler/setProjectContext/旗标读写/
//      头部预渲染——QString/QFile 随便用，因为它们只在启动早期与工程切换时跑；
//   2) fd 侧（崩溃处理器路径）：只用 open/write/close/backtrace 系（POSIX
//      async-signal-safe 面）。崩溃时刻禁止分配/锁/locale——时间戳与信号号
//      都用手工数字排版，文件名/路径写进栈上 CharBuf，报告头是启动时预渲染
//      好的字节块。backtrace() 本身不是 POSIX 保证安全（glibc 事实上安全），
//      这是行业通行妥协，见 docs/CRASH_REPORTING.md 隐私边界一节。
//
// Windows：execinfo 不存在（__has_include 探测）；sigaction 换 CRT signal()
// 最佳努力（OS 级访问违抗不一定走 CRT 处理器），落盘核心同一套 fd 代码。

#include <qgsconfig.h> // _QGIS_VERSION

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <ctime>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__has_include)
#if __has_include(<execinfo.h>)
#define PALEO_HAVE_EXECINFO 1
#include <execinfo.h>
#endif
#endif

namespace
{

constexpr int kMaxFrames = 64;

// ---- 处理器可读的静态状态（只在非信号上下文写入）----
char s_dir[4096] = {0};       // 报告落盘目录（UTF-8，install 时解析）
char s_header[2048] = {0};    // 预渲染报告头（app/qt/qgis/project/pid 行）
int s_headerLen = 0;
QString s_project;            // 当前工程路径（重渲染头用；handler 不读它）
volatile sig_atomic_t s_inHandler = 0;

long currentPid()
{
#if defined(Q_OS_WIN)
  return _getpid();
#else
  return static_cast<long>(::getpid());
#endif
}

// ---- fd 原语（两个平台同一语义）----
int openReport(const char *path)
{
#if defined(Q_OS_WIN)
  return _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  return ::open(path, O_WRONLY | O_CREAT | O_EXCL,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#endif
}

void writeAllBytes(int fd, const char *data, size_t len)
{
  while (len > 0)
  {
#if defined(Q_OS_WIN)
    const int n = _write(fd, data, static_cast<unsigned>(len));
#else
    const ssize_t n = ::write(fd, data, len);
#endif
    if (n <= 0)
      return; // 尽力而为：崩溃处理器里没有重试预算
    data += n;
    len -= static_cast<size_t>(n);
  }
}

void writeStr(int fd, const char *s) { writeAllBytes(fd, s, std::strlen(s)); }

// ---- 手工排版（strftime/snprintf 不是 async-signal-safe）----
struct CharBuf
{
  char data[4224];
  size_t len = 0;

  void add(const char *s)
  {
    const size_t n = std::strlen(s);
    if (len + n < sizeof data)
    {
      std::memcpy(data + len, s, n);
      len += n;
    }
  }
  void add1(char c)
  {
    if (len + 1 < sizeof data)
      data[len++] = c;
  }
  void addUint(unsigned long v, int width)
  {
    char tmp[24];
    int n = 0;
    do
    {
      tmp[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v > 0);
    while (n < width)
      tmp[n++] = '0';
    for (int i = n - 1; i >= 0; --i)
      add1(tmp[i]);
  }
  bool terminated()
  {
    if (len < sizeof data)
    {
      data[len] = '\0';
      return true;
    }
    data[sizeof data - 1] = '\0';
    return false; // 超长 → 放弃这一份报告，不写坏路径
  }
};

// epoch 秒 → UTC "YYYYmmdd-HHMMSS"（Hinnant civil_from_days；纯整数运算）。
void formatTimestamp(char out[16])
{
  const time_t t = std::time(nullptr);
  long days = static_cast<long>(t / 86400);
  long secs = static_cast<long>(t % 86400);
  if (secs < 0)
  {
    secs += 86400;
    --days; // 负 epoch（1970 前）不会出现在本应用，稳妥起见仍处理
  }
  long z = days + 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned long doe = static_cast<unsigned long>(z - era * 146097);
  const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long y = static_cast<long>(yoe) + era * 400;
  const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned long mp = (5 * doy + 2) / 153;
  const unsigned long d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned long m = mp < 10 ? mp + 3 : mp - 9;
  const unsigned long year = m <= 2 ? static_cast<unsigned long>(y + 1)
                                    : static_cast<unsigned long>(y);
  const unsigned long hh = static_cast<unsigned long>(secs) / 3600;
  const unsigned long mm = (static_cast<unsigned long>(secs) / 60) % 60;
  const unsigned long ss = static_cast<unsigned long>(secs) % 60;

  CharBuf b;
  b.addUint(year, 4);
  b.addUint(m, 2);
  b.addUint(d, 2);
  b.add1('-');
  b.addUint(hh, 2);
  b.addUint(mm, 2);
  b.addUint(ss, 2);
  if (!b.terminated() || b.len != 15)
  {
    std::memcpy(out, "00000000-000000", 16); // 理论不可达；给可识别占位
    return;
  }
  std::memcpy(out, b.data, 16);
}

const char *signalName(int sig)
{
#ifdef SIGSEGV
  if (sig == SIGSEGV) return "SIGSEGV";
#endif
#ifdef SIGABRT
  if (sig == SIGABRT) return "SIGABRT";
#endif
#ifdef SIGFPE
  if (sig == SIGFPE) return "SIGFPE";
#endif
#ifdef SIGILL
  if (sig == SIGILL) return "SIGILL";
#endif
#ifdef SIGBUS
  if (sig == SIGBUS) return "SIGBUS";
#endif
  return "SIG";
}

void closeReport(int fd)
{
#if defined(Q_OS_WIN)
  _close(fd);
#else
  ::close(fd);
#endif
}

// 崩溃落盘核心：处理器与 writeReportForSignal（测试/诊断）共用，行为一致。
bool writeReportFd(int sig)
{
  if (!s_dir[0])
    return false;
  char ts[16];
  formatTimestamp(ts);

  for (int attempt = 0; attempt < 100; ++attempt)
  {
    CharBuf path;
    path.add(s_dir);
    path.add1('/');
    path.add(ts);
    if (attempt > 0)
    {
      path.add1('-');
      path.addUint(static_cast<unsigned long>(attempt + 1), 0);
    }
    path.add(".txt");
    if (!path.terminated())
      return false;
    const int fd = openReport(path.data);
    if (fd < 0)
    {
#if !defined(Q_OS_WIN)
      if (errno == EEXIST) // 同秒已有报告 → 加后缀再试
        continue;
#endif
      return false; // 目录没了/不可写：尽力而为到此为止
    }

    writeStr(fd, "paleo crash report\n");
    writeStr(fd, "time: ");
    writeStr(fd, ts);
    writeStr(fd, "\nsignal: ");
    CharBuf sigLine;
    sigLine.addUint(static_cast<unsigned long>(sig), 0);
    sigLine.add(" (");
    sigLine.add(signalName(sig));
    sigLine.add(")\n");
    writeAllBytes(fd, sigLine.data, sigLine.len);
    writeAllBytes(fd, s_header, static_cast<size_t>(s_headerLen));
    writeStr(fd, "--- backtrace ---\n");
#ifdef PALEO_HAVE_EXECINFO
    void *frames[kMaxFrames];
    const int n = ::backtrace(frames, kMaxFrames);
    if (n > 0)
      ::backtrace_symbols_fd(frames, n, fd);
    else
      writeStr(fd, "(no frames captured)\n");
#else
    writeStr(fd, "(此平台无 execinfo 回溯——仅旗标+时间戳报告)\n");
#endif
    closeReport(fd);
    return true;
  }
  return false;
}

#if !defined(Q_OS_WIN)
// SA_RESETHAND：进入即恢复默认处置——写完报告 raise 会让进程按原信号默认
// 行为终止（保留 core dump 语义），不会回到处理器自身。
void fatalSignalHandler(int sig, siginfo_t *, void *)
{
  if (!s_inHandler)
  {
    s_inHandler = 1;
    writeReportFd(sig);
  }
  ::raise(sig);
}

void installSignalHandlers()
{
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = &fatalSignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL
#ifdef SIGBUS
                  ,
                  SIGBUS
#endif
  })
    ::sigaction(sig, &sa, nullptr);
}
#else
// Windows：CRT signal() 对 OS 级访问违抗不可靠（SIGABRT/SIGFPE 最可靠），
// 处理器返回后 CRT 行为未定——写完报告直接 _exit。
void fatalSignalHandlerCrt(int sig)
{
  if (!s_inHandler)
  {
    s_inHandler = 1;
    writeReportFd(sig);
  }
  _exit(3);
}

void installSignalHandlers()
{
  std::signal(SIGABRT, &fatalSignalHandlerCrt);
  std::signal(SIGFPE, &fatalSignalHandlerCrt);
  std::signal(SIGILL, &fatalSignalHandlerCrt);
  std::signal(SIGSEGV, &fatalSignalHandlerCrt);
}
#endif

QString flagPath(const QString &crashDir)
{
  return crashDir + QStringLiteral("/.running");
}

void renderHeader()
{
  const QString head = QStringLiteral(
                           "app: paleo_workstation (built %1)\n"
                           "qt: %2\n"
                           "qgis: %3\n"
                           "project: %4\n"
                           "pid: %5\n")
                           .arg(QLatin1String(__DATE__), QLatin1String(qVersion()),
                                QLatin1String(_QGIS_VERSION),
                                s_project.isEmpty() ? QStringLiteral("(none)") : s_project)
                           .arg(currentPid());
  const QByteArray utf8 = head.toUtf8();
  s_headerLen = qMin<int>(utf8.size(), static_cast<int>(sizeof s_header) - 1);
  std::memcpy(s_header, utf8.constData(), static_cast<size_t>(s_headerLen));
}

} // namespace

namespace CrashReport
{

QString crashDirFor(const QString &baseDir)
{
  return QDir(baseDir).filePath(QStringLiteral("crash"));
}

SessionStart installCrashHandler(const QString &baseDir)
{
  SessionStart out;
  if (baseDir.isEmpty())
    return out;
  const QString dir = crashDirFor(baseDir);
  if (!QDir().mkpath(dir))
  {
    qWarning("CrashReport: cannot create crash dir: %s", qPrintable(dir));
    return out; // 诊断件绝不阻塞启动：干净态返回，不装处理器
  }

  const QString flag = flagPath(dir);
  out.previousDirtyExit = QFileInfo::exists(flag);
  if (out.previousDirtyExit)
    out.lastReportPath = latestReportPath(baseDir);

  // 新会话旗标（pid+启动时刻——崩溃后现场诊断用）
  QFile f(flag);
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    f.write(QStringLiteral("pid=%1\nstarted=%2\n")
                .arg(currentPid())
                .arg(QDateTime::currentDateTimeUtc().toString(
                    QStringLiteral("yyyyMMdd-HHmmss")))
                .toUtf8());
  else
    qWarning("CrashReport: cannot write running flag: %s", qPrintable(flag));

  const QByteArray dir8 = QFile::encodeName(dir);
  if (dir8.size() >= static_cast<int>(sizeof s_dir))
  {
    qWarning("CrashReport: crash dir path too long, handler disabled");
    return out; // 旗标逻辑已生效（脏退出检测可用），仅处理器落盘不可用
  }
  std::memcpy(s_dir, dir8.constData(), static_cast<size_t>(dir8.size()) + 1);
  s_project.clear();
  renderHeader(); // 新会话：工程上下文从 (none) 重新开始
  installSignalHandlers();
  return out;
}

void setProjectContext(const QString &projectDir)
{
  s_project = projectDir;
  renderHeader();
}

void clearRunningFlag()
{
  if (!s_dir[0])
    return;
  QFile::remove(flagPath(QFile::decodeName(s_dir)));
}

QString latestReportPath(const QString &baseDir)
{
  const QDir dir(crashDirFor(baseDir));
  // 文件名即 UTC 秒级时间戳（YYYYmmdd-HHMMSS[-N].txt）——按名字取最大即最新
  //（同秒后缀不影响「最新一秒」的判定）。
  QString best;
  const QStringList files = dir.entryList({QStringLiteral("*.txt")}, QDir::Files);
  for (const QString &name : files)
    if (name > best)
      best = name;
  return best.isEmpty() ? QString() : dir.filePath(best);
}

bool writeReportForSignal(int signalNumber)
{
  return writeReportFd(signalNumber);
}

QString recoveryNoticeText(const SessionStart &start)
{
  if (!start.previousDirtyExit)
    return QString();
  if (start.lastReportPath.isEmpty())
    return QStringLiteral("上次未能正常退出 Paleo（没有生成崩溃报告）。");
  return QStringLiteral("上次未能正常退出 Paleo。崩溃报告已保存在：%1")
      .arg(start.lastReportPath);
}

} // namespace CrashReport
