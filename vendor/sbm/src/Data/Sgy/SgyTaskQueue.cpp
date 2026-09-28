#include "Data/Sgy/SgyTaskQueue.h"

#include <algorithm>

namespace seismic {

SgyTaskQueue::SgyTaskQueue(int workerCount, std::size_t maxPending)
    : maxPending_(maxPending == 0 ? 1 : maxPending) {
    const int count = std::max(1, workerCount);
    workers_.reserve(static_cast<std::size_t>(count));
    for(int i = 0; i < count; ++i) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
}

SgyTaskQueue::~SgyTaskQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        // Cancel BOTH pending and running work: a running long task (for
        // example a full-volume time slice) must observe the flag and stop, so
        // the join below cannot block for minutes.
        for(Task& task : pending_) {
            if(task.cancelFlag) {
                task.cancelFlag->store(true);
            }
        }
        pending_.clear();
        for(Task& task : running_) {
            if(task.cancelFlag) {
                task.cancelFlag->store(true);
            }
        }
    }
    condition_.notify_all();
    for(std::thread& worker : workers_) {
        if(worker.joinable()) {
            worker.join();
        }
    }
}

bool SgyTaskQueue::IsRunning(const std::string& key) const {
    return std::any_of(running_.begin(), running_.end(), [&key](const Task& task) {
        return task.key == key;
    });
}

bool SgyTaskQueue::HasTask(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsRunning(key) || std::any_of(pending_.begin(), pending_.end(),
        [&key](const Task& task) { return task.key == key; });
}

bool SgyTaskQueue::Submit(const std::string& key, SgyTaskPriority priority, TaskFn task) {
    if(!task) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_) {
        return false;
    }

    // Coalesce: a newer request for the same key supersedes both a pending and
    // a running task (the running task observes its cancel flag and stops).
    for(Task& running : running_) {
        if(running.key == key && running.cancelFlag) {
            running.cancelFlag->store(true);
        }
    }
    for(auto it = pending_.begin(); it != pending_.end(); ++it) {
        if(it->key == key) {
            if(it->cancelFlag) {
                it->cancelFlag->store(true);
            }
            pending_.erase(it);
            ++coalesced_;
            break;
        }
    }

    if(pending_.size() >= maxPending_) {
        ++rejected_;
        return false;
    }

    Task queued;
    queued.key = key;
    queued.priority = priority;
    queued.sequence = nextSequence_++;
    queued.fn = std::move(task);
    queued.cancelFlag = std::make_shared<std::atomic<bool>>(false);
    pending_.push_back(std::move(queued));

    // Highest priority first; FIFO inside the same priority.
    std::stable_sort(pending_.begin(), pending_.end(), [](const Task& a, const Task& b) {
        if(a.priority != b.priority) {
            return static_cast<int>(a.priority) < static_cast<int>(b.priority);
        }
        return a.sequence < b.sequence;
    });

    condition_.notify_one();
    return true;
}

void SgyTaskQueue::CancelKey(const std::string& key) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto it = pending_.begin(); it != pending_.end();) {
            if(it->key == key) {
                if(it->cancelFlag) {
                    it->cancelFlag->store(true);
                }
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        // A running task must also observe the cancellation.
        for(Task& task : running_) {
            if(task.key == key && task.cancelFlag) {
                task.cancelFlag->store(true);
            }
        }
    }
    condition_.notify_all();
}

void SgyTaskQueue::CancelAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(Task& task : pending_) {
            if(task.cancelFlag) {
                task.cancelFlag->store(true);
            }
        }
        pending_.clear();
        for(Task& task : running_) {
            if(task.cancelFlag) {
                task.cancelFlag->store(true);
            }
        }
    }
    condition_.notify_all();
}

void SgyTaskQueue::WaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() {
        return pending_.empty() && running_.empty();
    });
}

std::size_t SgyTaskQueue::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

std::size_t SgyTaskQueue::RunningCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_.size();
}

std::size_t SgyTaskQueue::CompletedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_;
}

std::size_t SgyTaskQueue::CoalescedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return coalesced_;
}

std::size_t SgyTaskQueue::RejectedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rejected_;
}

void SgyTaskQueue::WorkerLoop() {
    while(true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() { return stopping_ || !pending_.empty(); });
            if(stopping_) {
                return;
            }
            task = std::move(pending_.front());
            pending_.erase(pending_.begin());
            running_.push_back(task);
        }

        if(task.cancelFlag && task.cancelFlag->load()) {
            // Cancelled before it started.
        } else {
            SgyTaskContext context(task.cancelFlag);
            task.fn(context);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.erase(
                std::remove_if(running_.begin(), running_.end(), [&task](const Task& running) {
                    return running.key == task.key;
                }),
                running_.end());
            ++completed_;
        }
        condition_.notify_all();
    }
}

} // namespace seismic
