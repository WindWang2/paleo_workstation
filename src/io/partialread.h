// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// io/ — 部分读取/临时文件/文件锁（wave/io-perf-cache D7.1 / D7.4 / D7.5）。
//
// D7.1 截断文件不再「全有或全无」：按字节上限读取，截断发生在行中间时丢弃
// 半行并给 warning——调用方拿到「已读部分 + 警告」继续干活（预览/统计类
// 场景），而不是整份失败。
//
// D7.4 崩溃残留卫生：受管写入用 *.tmp<pid> / *.partial / *.running 过渡名，
// 崩溃后可能残留。sweepTempFiles 只清「进程已死」的 pid 残留 + 无 pid 后缀
// 且超龄的过渡文件——正常运行中的过渡文件绝不动。
//
// D7.5 资产写锁：同资产并发写检测。锁文件 <path>.wlock 一行 PID；持有者
// 进程存活则第二个写者被拒；持有者已死视为陈锁回收。不用 QLockFile——它的
// 三行格式与「删锁竞态」在本仓有过假绿测试教训（见 memory/data-foundation），
// PID 存活探测语义更直白。
namespace PartialRead
{
  struct PartialText
  {
      QString text;
      bool truncated = false;   // 文件比 maxBytes 大
      qint64 bytesRead = 0;     // 实际读到的字节数
      qint64 bytesTotal = 0;
  };

  // 读文本前 maxBytes 字节（编码经 EncodingDetect 统一解码）。行中间截断
  // → 丢半行（truncated 仍为 true，text 是完整行集合）。
  PartialText readTextPartial(const QString &path, qint64 maxBytes,
                              QString *error = nullptr);

  // D7.4：扫描目录（递归）里的过渡文件。返回被清理的绝对路径。
  // keepNewerThanMs：无 pid 后缀的过渡文件必须比该龄老才清（默认 24h，
  // 防止误清刚创建的）；带 pid 的过渡文件只清「进程已死」的。
  QStringList sweepTempFiles(const QString &dir, qint64 keepNewerThanMs = 24 * 3600 * 1000);

  // 进程存活探测（/proc/<pid> 存在 或 kill(pid,0)==0/ESRCH 语义）。
  bool processAlive(qint64 pid);

  // D7.5 写守卫：RAII。构造时抢 <path>.wlock；抢不到（别的活进程持有）
  // → locked()==false，调用方必须放弃写并如实报错。析构释放。
  class WriteGuard
  {
    public:
      // maxWaitMs>0 时自旋等锁（重试间隔 20ms）——默认不等的拒绝语义。
      WriteGuard(const QString &targetPath, int maxWaitMs = 0);
      ~WriteGuard();
      WriteGuard(const WriteGuard &) = delete;
      WriteGuard &operator=(const WriteGuard &) = delete;

      bool locked() const { return m_locked; }
      QString refusalReason() const { return m_reason; } // 未抢到的原因（含持锁 pid）
      QString lockFilePath() const { return m_lockPath; }

    private:
      bool acquire(int maxWaitMs);
      void release();

      QString m_lockPath;
      bool m_locked = false;
      QString m_reason;
  };
} // namespace PartialRead
