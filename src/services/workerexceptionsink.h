// 层：服务
#pragma once

#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

// PW-014：std::thread 入口函数抛出的异常会直接 std::terminate 整个进程。
// WorkerExceptionSink 包装工作线程分片：捕获首个异常（first wins），join 之后
// 由调用线程 rethrowIfAny() 重新抛出，按任务失败处理而不是进程崩溃。
class WorkerExceptionSink
{
  public:
    template <typename F> auto wrap( F f )
    {
      return [this, f]( auto &&...args ) mutable {
        if ( m_failed.load( std::memory_order_relaxed ) )
          return; // 已有分片失败：其余分片不再起算
        try
        {
          f( std::forward<decltype( args )>( args )... );
        }
        catch ( ... )
        {
          capture( std::current_exception() );
        }
      };
    }

    void capture( std::exception_ptr e )
    {
      std::lock_guard<std::mutex> lock( m_mutex );
      if ( !m_first )
        m_first = std::move( e );
      m_failed.store( true, std::memory_order_relaxed );
    }

    bool failed() const { return m_failed.load( std::memory_order_relaxed ); }

    // 必须在全部工作线程 join 之后调用。
    void rethrowIfAny()
    {
      std::exception_ptr e;
      {
        std::lock_guard<std::mutex> lock( m_mutex );
        e = m_first;
      }
      if ( e )
        std::rethrow_exception( e );
    }

  private:
    std::mutex m_mutex;
    std::exception_ptr m_first;
    std::atomic<bool> m_failed { false };
};
