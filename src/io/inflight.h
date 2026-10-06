// 层：数据
#pragma once
#include <QHash>
#include <QMutex>

#include <functional>
#include <future>
#include <utility>

// io/ — 同键并发请求合并器（wave/io-perf-cache D4.7）。
// 「同资产并发请求只跑一份」：同键（如同一 LAS 文件的指纹）的第一个提交
// 者成为执行者并当场跑 job；并发提交同键的其他线程拿到同一 future，
// blocking 等执行者 set_value 后各取各的结果拷贝。
//
// 约定：
// · job 在提交线程同步执行（本类型不调度线程——调度是任务服务的事）；
// · Result 需可默认构造 + 可拷贝（LasDoc / shared_ptr 均满足）；
// · job 抛异常 → 异常存入 future，等待方 get() 时重抛——绝不留「永远不
//   set」的挂死 future（执行者无论成败都会离开登记表）。
// · 等待方是 GUI 线程时会阻塞事件循环——调用方自行保证重活都在 worker
//   线程（PreviewDocService 的同步降级路径没有并发提交者，天然不等待）。
template <typename Key, typename Result>
class InflightCoalescer
{
  public:
    struct Ticket
    {
        std::shared_future<Result> future;
        bool executor = false; // true = 本线程刚执行了 job
    };

    Ticket submit(const Key &key, std::function<Result()> job)
    {
      std::promise<Result> promise;
      std::shared_future<Result> future = promise.get_future().share();
      {
        QMutexLocker lock(&m_mutex);
        const auto it = m_map.find(key);
        if (it != m_map.end())
        {
          ++m_ridersJoined;
          return Ticket{it.value(), false};
        }
        m_map.insert(key, future);
      }

      bool ok = false;
      Result value{};
      try
      {
        value = job();
        ok = true;
      }
      catch (...)
      {
        // 先离表再 set —— 等待方拿 future.get() 重抛异常。
        QMutexLocker lock(&m_mutex);
        m_map.remove(key);
        lock.unlock();
        promise.set_exception(std::current_exception());
        return Ticket{future, true};
      }
      {
        QMutexLocker lock(&m_mutex);
        // 执行者离表：登记项必然是自己的（等待方从不删除）。
        m_map.remove(key);
      }
      promise.set_value(std::move(value));
      return Ticket{future, true};
    }

    // 当前在途键数（诊断/测试）。
    int inFlightCount() const
    {
      QMutexLocker lock(&m_mutex);
      return m_map.size();
    }

    // 累计搭车（非执行者）提交次数，单调递增（诊断/测试：确定性构造
    // 「执行者在跑、搭车者已入队」的并发时序）。
    int ridersJoined() const
    {
      QMutexLocker lock(&m_mutex);
      return m_ridersJoined;
    }

  private:
    mutable QMutex m_mutex;
    int m_ridersJoined = 0;
    QHash<Key, std::shared_future<Result>> m_map;
};
