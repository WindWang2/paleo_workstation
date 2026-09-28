#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

namespace seismic {

enum class SgyTaskPriority {
    UserPreview = 0,   // what the user is looking at right now
    UserSection = 1,   // the section the user just requested
    BackgroundIndex = 2,
    Prefetch = 3,      // idle-time extras
};

// Context passed to a task. Long tasks must poll cancelled() regularly so a
// superseded request can stop quickly without blocking the worker.
class SgyTaskContext {
public:
    bool cancelled() const { return cancelFlag_ && cancelFlag_->load(); }

private:
    friend class SgyTaskQueue;
    explicit SgyTaskContext(std::shared_ptr<std::atomic<bool>> flag) : cancelFlag_(std::move(flag)) {}
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
};

// Small bounded, priority-ordered task queue with request coalescing.
//
// Design notes:
//   * one worker thread by default: concurrent tasks would compete for the same
//     disk and make everything slower; raise it only after measuring,
//   * a task is identified by a key; submitting the same key again replaces the
//     pending request (coalescing), which is what fast slider dragging needs,
//   * the queue is bounded; when full the new request is rejected instead of
//     growing without limit,
//   * cancellation is cooperative and never blocks the caller,
//   * the destructor cancels every task and joins the workers.
class SgyTaskQueue {
public:
    using TaskFn = std::function<void(const SgyTaskContext&)>;

    explicit SgyTaskQueue(int workerCount = 1, std::size_t maxPending = 64);
    ~SgyTaskQueue();

    SgyTaskQueue(const SgyTaskQueue&) = delete;
    SgyTaskQueue& operator=(const SgyTaskQueue&) = delete;

    // Returns false when the queue is full or the key was already running and
    // could not be replaced (the caller may retry after the current task ends).
    bool Submit(const std::string& key, SgyTaskPriority priority, TaskFn task);

    void CancelKey(const std::string& key);
    bool HasTask(const std::string& key) const;
    void CancelAll();

    // Blocks until every queued task finished or was cancelled. For tests and
    // shutdown only - never call this from the UI thread while tasks depend on
    // UI progress.
    void WaitIdle();

    std::size_t PendingCount() const;
    std::size_t RunningCount() const;
    std::size_t CompletedCount() const;
    std::size_t CoalescedCount() const;
    std::size_t RejectedCount() const;
    std::size_t WorkerCount() const { return workers_.size(); }

private:
    struct Task {
        std::string key;
        SgyTaskPriority priority = SgyTaskPriority::Prefetch;
        std::uint64_t sequence = 0;
        TaskFn fn;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
    };

    void WorkerLoop();
    bool IsRunning(const std::string& key) const;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Task> pending_;
    std::vector<Task> running_;
    bool stopping_ = false;
    std::uint64_t nextSequence_ = 0;
    std::size_t maxPending_ = 64;
    std::size_t completed_ = 0;
    std::size_t coalesced_ = 0;
    std::size_t rejected_ = 0;
    std::vector<std::thread> workers_;
};

} // namespace seismic
