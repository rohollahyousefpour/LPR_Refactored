#pragma once
// BlockingQueue<T> - one generic, thread-safe producer/consumer queue that
// replaces both hand-rolled queues:
//   * concurrent_queue (frames) -> bounded, drop-oldest  -> BlockingQueue<T>(maxSize)
//   * Send_Que_Data    (plates) -> unbounded             -> BlockingQueue<T>()
// STL-only (no OpenCV / SDKs). Adds close() for clean shutdown, which the
// originals lacked (their blocking get()/Pop() could hang forever on teardown).
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <chrono>
#include <optional>
#include <utility>

namespace lpr {

template <typename T>
class BlockingQueue {
public:
    // maxSize == 0  => unbounded.
    // maxSize  > 0  => bounded; push() drops the OLDEST item when full
    //                  (matches the original frame buffer's overwrite-oldest policy).
    explicit BlockingQueue(std::size_t maxSize = 0) : maxSize_(maxSize) {}

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // Enqueue an item. Returns false if an older item had to be dropped to make
    // room (bounded queue was full), true otherwise.
    bool push(T value) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (maxSize_ > 0 && queue_.size() >= maxSize_) {
                queue_.pop_front();
                dropped = true;
            }
            queue_.push_back(std::move(value));
        }
        cv_.notify_one();
        return !dropped;
    }

    // Backpressure enqueue: BLOCK until there is room (or the queue closes), then
    // enqueue WITHOUT dropping. For OFFLINE video sources where every frame must be
    // processed so the direction estimator gets the maximum sightings — the producer
    // is paced by the (slower) consumer instead of the queue silently dropping most
    // frames. NEVER use for a LIVE camera: there you must not stall the sensor, so
    // drop-oldest via push() is correct.
    void pushBlocking(T value) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (maxSize_ > 0)
                notFull_.wait(lock, [this] { return closed_ || queue_.size() < maxSize_; });
            if (closed_) return;
            queue_.push_back(std::move(value));
        }
        cv_.notify_one();
    }

    // Block until an item is available or the queue is closed.
    // Returns std::nullopt only when the queue is closed AND drained.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty())
            return std::nullopt;            // closed and empty
        T value = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();              // woke a backpressure producer, if any
        return value;
    }

    // Block until an item is available, the queue is closed, or the timeout
    // elapses. Returns std::nullopt on timeout or when closed AND drained. Lets a
    // consumer poll a running_ flag without busy-waiting or closing a shared queue.
    std::optional<T> popFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; }))
            return std::nullopt;            // timed out
        if (queue_.empty())
            return std::nullopt;            // closed and empty
        T value = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();              // woke a backpressure producer, if any
        return value;
    }

    // Non-blocking dequeue. Returns false if the queue is empty.
    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty())
            return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();              // woke a backpressure producer, if any
        return true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // Drop all queued items (wakes any waiters so they can re-check).
    void clear() {
        { std::lock_guard<std::mutex> lock(mutex_); queue_.clear(); }
        cv_.notify_all();
        notFull_.notify_all();              // room freed -> release backpressure producers
    }

    // Signal shutdown: wakes every blocked pop(); each returns nullopt once the
    // queue is drained. Idempotent. Also releases any producer blocked in pushBlocking.
    void close() {
        { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; }
        cv_.notify_all();
        notFull_.notify_all();
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::condition_variable notFull_;       // wakes a pushBlocking() producer when room frees
    std::deque<T>           queue_;
    std::size_t             maxSize_;
    bool                    closed_ = false;
};

} // namespace lpr
