/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_PIPELINE_CREATION_QUEUE_H_
#define XENIA_GPU_PIPELINE_CREATION_QUEUE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/pipeline_publish_order.h"

namespace xe {
namespace gpu {

// Asynchronous pipeline creation, shared by the backends: the request queue,
// the creation threads, the publish ordering and the completion handshake. The
// backend supplies the request, the handle creating it produces, the state a
// creation thread needs to do the work, and the two functions that create and
// store one pipeline.
template <typename TRequest, typename THandle, typename TThreadContext,
          size_t kPriorityCount = 1>
class PipelineCreationQueue {
  static_assert(kPriorityCount >= 1, "At least one priority lane is required");

 public:
  using Publication = std::pair<TRequest, THandle>;
  // Creates one pipeline, returning a null handle if it failed. Runs on a
  // creation thread with that thread's context, or with its own on the thread
  // expediting the request or finishing a warm-up.
  using Creator = std::function<THandle(const TRequest&, TThreadContext*)>;
  // Swaps a created pipeline - or a failure, a null handle - into its entry.
  // Called in queue order unless expedited, under the publish order's lock
  // for ordered requests, never the queue's.
  using Store = std::function<void(const TRequest&, THandle)>;
  // The state one creation thread builds and owns for its lifetime.
  using ContextFactory = std::function<std::unique_ptr<TThreadContext>()>;
  // Told true when the queue or a BusyScope turns busy and false once all are
  // idle again.
  // Called under the queue's lock, so the reports arrive in order.
  using BusyChanged = std::function<void(bool)>;

  // Reports pipeline work done outside the queue, such as a pipeline created on
  // the draw thread, through BusyChanged while it lasts. FinishWarmUp ignores
  // it.
  class BusyScope {
   public:
    explicit BusyScope(PipelineCreationQueue& owner) : owner_(owner) {
      owner_.AddOutsideWork(1);
    }
    ~BusyScope() { owner_.AddOutsideWork(-1); }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;

   private:
    PipelineCreationQueue& owner_;
  };

  // Prepares the queue. Call it even with no creation threads, as the storage
  // warm-up goes through it all the same.
  void Initialize(const char* thread_name, Creator creator, Store store,
                  ContextFactory context_factory, BusyChanged busy_changed) {
    thread_name_ = thread_name;
    creator_ = std::move(creator);
    store_ = std::move(store);
    context_factory_ = std::move(context_factory);
    busy_changed_ = std::move(busy_changed);
    busy_reported_ = false;
    threads_busy_ = 0;
    completion_set_event_ = false;
    threads_shutdown_from_ = SIZE_MAX;
    active_thread_limit_ = SIZE_MAX;
    completion_event_ = xe::threading::Event::CreateManualResetEvent(true);
    assert_not_null(completion_event_);
  }

  // Stops the threads and drops everything still queued. Returns what was held
  // back for publishing, for the backend to destroy - nothing will publish it
  // now.
  std::vector<Publication> Shutdown() {
    {
      // Silenced first, so threads finishing don't report into the teardown.
      std::lock_guard<std::mutex> lock(lock_);
      busy_changed_ = nullptr;
    }
    SetThreadCount(0);
    completion_event_.reset();
    std::vector<Publication> held;
    for (Created& created : publish_order_.Reset()) {
      held.emplace_back(std::move(created.taken->second), created.handle);
    }
    {
      std::lock_guard<std::mutex> lock(lock_);
      for (auto& lane : queues_) {
        lane.clear();
      }
      taken_.clear();
      completion_callback_ = nullptr;
      awaiting_completion_.store(false, std::memory_order_release);
      completion_set_event_ = false;
      active_thread_limit_ = SIZE_MAX;
    }
    return held;
  }

  bool has_threads() const {
    return has_threads_.load(std::memory_order_acquire);
  }

  // Grows or shrinks the pool. Shrink with the queue drained - a thread only
  // leaves between requests, so anything still queued would be stranded.
  void SetThreadCount(size_t count) {
    std::lock_guard<std::mutex> threads_lock(threads_lock_);
    SetThreadCountLocked(count);
  }

  // Limits how many threads may take new work, parking threads at indices >=
  // |limit| on a separate condition variable and lazily spawning extra threads
  // if |limit| exceeds the current pool size (when the pool has threads and
  // |limit| != SIZE_MAX). Thread-safe.
  void SetActiveThreadLimit(size_t limit) {
    if (limit != SIZE_MAX) {
      std::lock_guard<std::mutex> threads_lock(threads_lock_);
      if (!threads_.empty() && limit > threads_.size()) {
        SetThreadCountLocked(limit);
      }
    }
    {
      std::lock_guard<std::mutex> lock(lock_);
      active_thread_limit_ = limit;
    }
    park_cond_.notify_all();
    cond_.notify_all();
  }

  // Queues a pipeline the game is drawing now - it is stored once every request
  // queued before it has been.
  void Push(TRequest request) { PushRequest(std::move(request), true, 0); }

  // Queues one nothing is drawing yet (the storage warm-up), stored as soon as
  // it is built. Higher |priority| lanes are dequeued first
  // (0..kPriorityCount-1).
  void PushUnordered(TRequest request, size_t priority = 0) {
    PushRequest(std::move(request), false, priority);
  }

  // Moves the first queued request matching |match| from any lane below
  // |priority| to |priority|. No-op if not queued or already at >= |priority|.
  template <typename Match>
  bool Promote(Match match, size_t priority) {
    const size_t target = std::min(priority, kPriorityCount - 1);
    bool promoted = false;
    {
      std::lock_guard<std::mutex> lock(lock_);
      for (size_t lane = 0; lane < target; ++lane) {
        auto it = std::find_if(queues_[lane].begin(), queues_[lane].end(),
                               [&](const std::pair<uint32_t, TRequest>& entry) {
                                 return match(entry.second);
                               });
        if (it != queues_[lane].end()) {
          queues_[target].push_back(std::move(*it));
          queues_[lane].erase(it);
          promoted = true;
          break;
        }
      }
    }
    if (promoted) {
      cond_.notify_one();
    }
    return promoted;
  }

  // Removes the first queued request matching |match| so the caller can build
  // it synchronously instead. Returns false if it was not queued (already
  // taken by a worker or not present).
  template <typename Match>
  bool TakeQueued(Match match) {
    std::lock_guard<std::mutex> lock(lock_);
    for (size_t lane = kPriorityCount; lane > 0; --lane) {
      auto& q = queues_[lane - 1];
      auto it = std::find_if(q.begin(), q.end(),
                             [&](const std::pair<uint32_t, TRequest>& entry) {
                               return match(entry.second);
                             });
      if (it != q.end()) {
        q.erase(it);
        ReportBusyLocked();
        return true;
      }
    }
    return false;
  }

  // Wakes a creation thread without waiting for it.
  void Notify() { cond_.notify_one(); }

  // Finishes a storage warm-up queued with PushUnordered. When blocking, or
  // with no creation threads to leave it to, it's created here with |context|.
  // Otherwise |completion_callback| is taken to be invoked once everything
  // queued is created, and left with the caller if that already is.
  void FinishWarmUp(bool blocking, TThreadContext* context,
                    std::function<void()>& completion_callback) {
    if (blocking || !has_threads()) {
      DrainOnCallingThread(context);
      AwaitCompletion();
      return;
    }
    TakeCompletionCallback(completion_callback);
  }

  // Whether FinishWarmUp is still waiting for everything queued to be created.
  bool IsAwaitingCompletion() const {
    return awaiting_completion_.load(std::memory_order_acquire);
  }

  // Stores the request |match| picks without waiting for anything queued
  // before it, for a draw that can't use a stand-in. It's created here with
  // |context| if no creation thread has taken it yet. Returns once it's
  // stored.
  template <typename Match>
  void Expedite(Match match, TThreadContext* context) {
    auto matches = [&](const std::pair<uint32_t, TRequest>& entry) {
      return match(entry.second);
    };
    std::unique_lock<std::mutex> lock(lock_);
    for (size_t lane = kPriorityCount; lane > 0; --lane) {
      auto& q = queues_[lane - 1];
      auto queued = std::find_if(q.begin(), q.end(), matches);
      if (queued != q.end()) {
        auto taken = taken_.insert(taken_.end(), std::move(*queued));
        q.erase(queued);
        ++threads_busy_;
        lock.unlock();
        publish_order_.Expedite(taken->first, StoreCreated());
        Create(taken, context);
        return;
      }
    }
    auto taken = std::find_if(taken_.begin(), taken_.end(), matches);
    if (taken == taken_.end()) {
      return;
    }
    uint32_t sequence = taken->first;
    lock.unlock();
    publish_order_.Expedite(sequence, StoreCreated());
    lock.lock();
    stored_cond_.wait(lock, [&]() {
      return std::none_of(taken_.begin(), taken_.end(), matches);
    });
  }

 private:
  using TakenList = std::list<std::pair<uint32_t, TRequest>>;
  // What the publish order carries for one created request.
  struct Created {
    typename TakenList::iterator taken;
    THandle handle;
  };

  bool QueuesEmptyLocked() const {
    for (const auto& lane : queues_) {
      if (!lane.empty()) {
        return false;
      }
    }
    return true;
  }

  bool IsBusyLocked() const {
    return !QueuesEmptyLocked() || threads_busy_ != 0;
  }

  void SetThreadCountLocked(size_t count) {
    if (count < threads_.size()) {
      {
        std::lock_guard<std::mutex> lock(lock_);
        threads_shutdown_from_ = count;
      }
      park_cond_.notify_all();
      cond_.notify_all();
      while (threads_.size() > count) {
        xe::threading::Wait(threads_.back().get(), false);
        threads_.pop_back();
      }
      has_threads_.store(!threads_.empty(), std::memory_order_release);
      // So threads can be added again later.
      std::lock_guard<std::mutex> lock(lock_);
      threads_shutdown_from_ = SIZE_MAX;
      return;
    }
    while (threads_.size() < count) {
      size_t thread_index = threads_.size();
      std::unique_ptr<xe::threading::Thread> thread =
          xe::threading::Thread::Create(
              {}, [this, thread_index]() { ThreadMain(thread_index); });
      assert_not_null(thread);
      thread->set_name(thread_name_);
      threads_.push_back(std::move(thread));
    }
    has_threads_.store(!threads_.empty(), std::memory_order_release);
  }

  // Blocks until everything queued has been created.
  void AwaitCompletion() {
    if (!has_threads()) {
      return;
    }
    bool await;
    {
      std::lock_guard<std::mutex> lock(lock_);
      await = IsBusyLocked();
      if (await) {
        completion_event_->Reset();
        completion_set_event_ = true;
      }
    }
    if (await) {
      cond_.notify_one();
      xe::threading::Wait(completion_event_.get(), false);
    }
  }

  // Takes the callback to invoke when everything queued has been created, and
  // leaves it with the caller if there is nothing to wait for.
  void TakeCompletionCallback(std::function<void()>& callback) {
    std::lock_guard<std::mutex> lock(lock_);
    if (!IsBusyLocked()) {
      return;
    }
    completion_callback_ = std::move(callback);
    callback = nullptr;
    awaiting_completion_.store(true, std::memory_order_release);
  }

  // Creates whatever is queued on the calling thread, with a context of its
  // own.
  void DrainOnCallingThread(TThreadContext* context) {
    while (CreateOneQueued(context)) {
    }
  }

  void AddOutsideWork(int delta) {
    std::lock_guard<std::mutex> lock(lock_);
    outside_work_ += delta;
    ReportBusyLocked();
  }

  void ReportBusyLocked() {
    bool busy = IsBusyLocked() || outside_work_ != 0;
    if (busy_reported_ == busy) {
      return;
    }
    busy_reported_ = busy;
    if (busy_changed_) {
      busy_changed_(busy);
    }
  }

  void PushRequest(TRequest request, bool ordered, size_t priority) {
    const size_t lane = std::min(priority, kPriorityCount - 1);
    {
      std::lock_guard<std::mutex> lock(lock_);
      // Numbered under the lock that orders the push, so the publish order
      // matches the queue order.
      queues_[lane].emplace_back(ordered ? publish_order_.NextSequence() : 0,
                                 std::move(request));
      ReportBusyLocked();
    }
    cond_.notify_one();
  }

  bool PopFrontLocked(typename TakenList::iterator& taken_out) {
    for (size_t lane = kPriorityCount; lane > 0; --lane) {
      auto& q = queues_[lane - 1];
      if (!q.empty()) {
        // Busy until the pipeline is created rather than just dequeued: other
        // threads must be able to take requests, but must not report completion
        // while this one is still building.
        taken_out = taken_.insert(taken_.end(), std::move(q.front()));
        q.pop_front();
        ++threads_busy_;
        return true;
      }
    }
    return false;
  }

  // Creates the next queued pipeline, or returns false with nothing queued.
  bool CreateOneQueued(TThreadContext* context) {
    typename TakenList::iterator taken;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (!PopFrontLocked(taken)) {
        return false;
      }
    }
    Create(taken, context);
    return true;
  }

  // Creates and publishes a request taken off the queue, counted in
  // threads_busy_.
  void Create(typename TakenList::iterator taken, TThreadContext* context) {
    THandle handle = creator_(taken->second, context);
    // Published even when creation failed: the order has to advance past a
    // failure, or everything queued after it waits on it forever.
    publish_order_.Publish(taken->first, {taken, handle}, StoreCreated());
    std::unique_lock<std::mutex> lock(lock_);
    --threads_busy_;
    ReportBusyLocked();
    SignalCompletionLocked(lock);
  }

  // Stores a created request and stops tracking it, under the publish order's
  // lock for ordered requests.
  auto StoreCreated() {
    return [this](Created& created) {
      store_(created.taken->second, created.handle);
      std::lock_guard<std::mutex> lock(lock_);
      taken_.erase(created.taken);
      stored_cond_.notify_all();
    };
  }

  // Called by an idle thread with the lock held, which it drops around the
  // callback.
  void SignalCompletionLocked(std::unique_lock<std::mutex>& lock) {
    if (IsBusyLocked()) {
      return;
    }
    if (completion_set_event_) {
      // Blocking mode.
      completion_set_event_ = false;
      completion_event_->Set();
    }
    awaiting_completion_.store(false, std::memory_order_release);
    if (completion_callback_) {
      // Non-blocking mode.
      auto callback = std::move(completion_callback_);
      completion_callback_ = nullptr;
      lock.unlock();
      callback();
      lock.lock();
    }
  }

  void ThreadMain(size_t thread_index) {
    std::unique_ptr<TThreadContext> context =
        context_factory_ ? context_factory_() : nullptr;
    for (;;) {
      typename TakenList::iterator taken;
      {
        std::unique_lock<std::mutex> lock(lock_);
        while (thread_index < threads_shutdown_from_ &&
               thread_index >= active_thread_limit_) {
          park_cond_.wait(lock);
        }
        // A thread being shut down stops taking work, leaving the rest of the
        // queue to whoever is left.
        if (thread_index >= threads_shutdown_from_ || !PopFrontLocked(taken)) {
          // Also the path a thread woken only to report completion takes.
          SignalCompletionLocked(lock);
          if (thread_index >= threads_shutdown_from_) {
            return;
          }
          cond_.wait(lock);
          continue;
        }
      }
      Create(taken, context.get());
    }
  }

  std::string thread_name_;
  Creator creator_;
  Store store_;
  ContextFactory context_factory_;
  // All guarded by lock_.
  BusyChanged busy_changed_;
  bool busy_reported_ = false;
  // Open BusyScopes.
  int outside_work_ = 0;

  std::mutex lock_;
  std::condition_variable cond_;
  std::condition_variable park_cond_;
  // Pipelines are never evicted - games have a finite set that should all stay
  // cached - so this holds nothing but requests waiting for a thread. FIFO per
  // priority lane, higher lanes dequeued first.
  std::array<std::deque<std::pair<uint32_t, TRequest>>, kPriorityCount> queues_;
  // Requests taken off the queue and not stored yet. Guarded by lock_.
  TakenList taken_;
  // Notified when one of taken_ is stored.
  std::condition_variable stored_cond_;
  PipelinePublishOrder<Created> publish_order_;
  // Threads that have taken a request but not finished creating it. Guarded by
  // lock_.
  size_t threads_busy_ = 0;
  // Manual-reset event set when the queue drains, for a blocking wait. Whether
  // it is wanted is guarded by lock_.
  std::unique_ptr<xe::threading::Event> completion_event_;
  bool completion_set_event_ = false;
  // Invoked instead when the wait is non-blocking. Guarded by lock_.
  std::function<void()> completion_callback_;
  // Set while a non-blocking wait is pending, written under lock_.
  std::atomic<bool> awaiting_completion_{false};
  // Threads with this index or above leave as soon as they are idle. Guarded by
  // lock_, notify_all cond_ and park_cond_ when set.
  size_t threads_shutdown_from_ = SIZE_MAX;
  // Threads with this index or above park on park_cond_ until the limit rises
  // or shutdown begins. Guarded by lock_.
  size_t active_thread_limit_ = SIZE_MAX;
  std::mutex threads_lock_;
  std::atomic<bool> has_threads_{false};
  std::vector<std::unique_ptr<xe::threading::Thread>> threads_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_PIPELINE_CREATION_QUEUE_H_
