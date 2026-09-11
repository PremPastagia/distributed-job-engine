#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace je {

// A scheduling hint, not the authoritative job record. The record lives in the store;
// the queue only decides *order*. That separation is what lets the queue stay a pure,
// independently testable data structure.
struct QueueEntry {
  std::string job_id;
  int priority = 0;
  int64_t seq = 0;          // assigned by the queue on push; FIFO tie-break
  int64_t ready_at_ms = 0;  // steady-clock stamp; 0 or past == ready now
  // Steady stamp of the moment this entry became *eligible* (push time for an immediate
  // entry, ready_at_ms for a delayed one). Scheduling latency is measured from here, so a
  // retry's backoff is not mistaken for queue wait.
  int64_t eligible_at_steady = 0;
};

// Thread-safe bounded priority queue with delayed entries.
//
// Design notes (docs/ARCHITECTURE.md §4):
//  * One mutex + one condition_variable guard BOTH the ready heap and the delayed heap,
//    so "is anything ready?" and "when does the earliest delayed entry mature?" are
//    decided atomically. Consumers block in wait_until(earliest_ready_at), so there is
//    no polling loop and no separate timer thread.
//  * Ready order is (priority DESC, seq ASC): strict priority, FIFO within a level.
//  * Cancellation uses seq-keyed tombstones, so removal is O(1) instead of an O(n) heap
//    rebuild, and a later re-push of the same job_id (a retry) is unaffected because it
//    carries a fresh seq.
class PriorityJobQueue {
 public:
  enum class PushStatus { Ok, Full, Closed };

  explicit PriorityJobQueue(std::size_t capacity = 100000);

  // Assigns a fresh seq and inserts. out_seq receives it when the push succeeds.
  PushStatus push(const std::string& job_id, int priority, int64_t ready_at_ms,
                  int64_t* out_seq = nullptr);

  // Blocks until an entry is ready, or the queue is closed and has no ready entries.
  std::optional<QueueEntry> pop();
  // Same, but gives up after timeout_ms (returns nullopt). timeout_ms <= 0 polls once.
  std::optional<QueueEntry> pop_for(int64_t timeout_ms);
  // Up to max_items ready entries, waiting at most wait_ms for the first one.
  std::vector<QueueEntry> pop_batch(std::size_t max_items, int64_t wait_ms);

  // Tombstones the queued entry for job_id if present. Returns true if one was removed.
  bool remove(const std::string& job_id);

  void close();
  bool is_closed() const;

  std::size_t size() const;          // ready + delayed, tombstones excluded
  std::size_t ready_size() const;
  std::size_t delayed_size() const;
  std::size_t capacity() const { return capacity_; }
  int64_t total_pushed() const;
  int64_t total_popped() const;

 private:
  struct ReadyOrder {  // std::priority_queue is a max-heap: "less" means popped later
    bool operator()(const QueueEntry& a, const QueueEntry& b) const {
      if (a.priority != b.priority) return a.priority < b.priority;
      return a.seq > b.seq;  // smaller seq wins the tie, so it must compare "greater"
    }
  };
  struct DelayedOrder {  // min-heap on ready_at_ms
    bool operator()(const QueueEntry& a, const QueueEntry& b) const {
      if (a.ready_at_ms != b.ready_at_ms) return a.ready_at_ms > b.ready_at_ms;
      if (a.priority != b.priority) return a.priority < b.priority;
      return a.seq > b.seq;
    }
  };

  // Caller must hold mutex_. Moves every matured delayed entry into the ready heap.
  void promote_matured_locked(int64_t now);
  // Caller must hold mutex_. Pops the next live ready entry, discarding tombstones.
  std::optional<QueueEntry> take_ready_locked();
  std::size_t live_count_locked() const { return live_ready_ + live_delayed_; }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, ReadyOrder> ready_;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, DelayedOrder> delayed_;
  struct Loc {
    int64_t seq = 0;
    bool delayed = false;  // which heap currently holds this entry
  };
  std::unordered_map<std::string, Loc> index_;  // job_id -> location of its live entry
  std::unordered_set<int64_t> tombstones_;
  std::size_t capacity_;
  std::size_t live_ready_ = 0;
  std::size_t live_delayed_ = 0;
  int64_t next_seq_ = 1;
  int64_t pushed_ = 0;
  int64_t popped_ = 0;
  bool closed_ = false;
};

}  // namespace je
