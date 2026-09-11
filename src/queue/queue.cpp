#include "jobengine/queue.hpp"

#include <algorithm>
#include <chrono>

#include "jobengine/util.hpp"

namespace je {

PriorityJobQueue::PriorityJobQueue(std::size_t capacity) : capacity_(capacity) {}

PriorityJobQueue::PushStatus PriorityJobQueue::push(const std::string& job_id, int priority,
                                                    int64_t ready_at_ms, int64_t* out_seq) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) return PushStatus::Closed;
  if (live_count_locked() >= capacity_) return PushStatus::Full;

  // A re-push of an already-queued job replaces the earlier entry rather than creating a
  // second one; otherwise a duplicate release (workflow fan-in) could enqueue twice.
  const auto existing = index_.find(job_id);
  if (existing != index_.end()) {
    tombstones_.insert(existing->second.seq);
    if (existing->second.delayed) { if (live_delayed_ > 0) --live_delayed_; }
    else { if (live_ready_ > 0) --live_ready_; }
    index_.erase(existing);
  }

  QueueEntry e;
  e.job_id = job_id;
  e.priority = priority;
  e.seq = next_seq_++;
  e.ready_at_ms = ready_at_ms;
  if (out_seq) *out_seq = e.seq;
  ++pushed_;

  const int64_t now = now_ms();
  const bool is_delayed = ready_at_ms > now;
  e.eligible_at_steady = is_delayed ? ready_at_ms : now;
  index_[job_id] = Loc{e.seq, is_delayed};
  if (!is_delayed) {
    ready_.push(std::move(e));
    ++live_ready_;
    lock.unlock();
    cv_.notify_one();
  } else {
    const bool becomes_earliest = delayed_.empty() || ready_at_ms < delayed_.top().ready_at_ms;
    delayed_.push(std::move(e));
    ++live_delayed_;
    lock.unlock();
    // Only wake a waiter if this entry changes when the next wake-up must happen.
    if (becomes_earliest) cv_.notify_all();
  }
  return PushStatus::Ok;
}

void PriorityJobQueue::promote_matured_locked(int64_t now) {
  while (!delayed_.empty() && delayed_.top().ready_at_ms <= now) {
    QueueEntry e = delayed_.top();
    delayed_.pop();
    if (tombstones_.count(e.seq) > 0) {
      tombstones_.erase(e.seq);
      continue;  // liveness was already decremented when the entry was tombstoned
    }
    if (live_delayed_ > 0) --live_delayed_;
    ++live_ready_;
    const auto it = index_.find(e.job_id);
    if (it != index_.end() && it->second.seq == e.seq) it->second.delayed = false;
    ready_.push(std::move(e));
  }
}

std::optional<QueueEntry> PriorityJobQueue::take_ready_locked() {
  while (!ready_.empty()) {
    QueueEntry e = ready_.top();
    ready_.pop();
    if (tombstones_.count(e.seq) > 0) {
      tombstones_.erase(e.seq);
      continue;  // cancelled while queued
    }
    if (live_ready_ > 0) --live_ready_;
    const auto it = index_.find(e.job_id);
    if (it != index_.end() && it->second.seq == e.seq) index_.erase(it);
    ++popped_;
    return e;
  }
  return std::nullopt;
}

std::optional<QueueEntry> PriorityJobQueue::pop() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    const int64_t now = now_ms();
    promote_matured_locked(now);
    if (auto e = take_ready_locked()) return e;
    if (closed_) return std::nullopt;
    if (!delayed_.empty()) {
      const int64_t wait_ms = delayed_.top().ready_at_ms - now;
      cv_.wait_for(lock, std::chrono::milliseconds(wait_ms > 0 ? wait_ms : 0));
    } else {
      cv_.wait(lock);
    }
  }
}

std::optional<QueueEntry> PriorityJobQueue::pop_for(int64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  const int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    const int64_t now = now_ms();
    promote_matured_locked(now);
    if (auto e = take_ready_locked()) return e;
    if (closed_) return std::nullopt;
    int64_t wait_ms = deadline - now;
    if (wait_ms <= 0) return std::nullopt;
    if (!delayed_.empty()) {
      const int64_t until_ready = delayed_.top().ready_at_ms - now;
      if (until_ready > 0 && until_ready < wait_ms) wait_ms = until_ready;
    }
    cv_.wait_for(lock, std::chrono::milliseconds(wait_ms));
  }
}

std::vector<QueueEntry> PriorityJobQueue::pop_batch(std::size_t max_items, int64_t wait_ms) {
  std::vector<QueueEntry> out;
  if (max_items == 0) return out;
  if (auto first = pop_for(wait_ms)) {
    out.push_back(std::move(*first));
  } else {
    return out;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  const int64_t now = now_ms();
  promote_matured_locked(now);
  while (out.size() < max_items) {
    auto e = take_ready_locked();
    if (!e) break;
    out.push_back(std::move(*e));
  }
  return out;
}

bool PriorityJobQueue::remove(const std::string& job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = index_.find(job_id);
  if (it == index_.end()) return false;
  tombstones_.insert(it->second.seq);
  // The entry stays in its heap until it surfaces, but liveness drops immediately so
  // size()/queue_depth never counts a cancelled job.
  if (it->second.delayed) { if (live_delayed_ > 0) --live_delayed_; }
  else { if (live_ready_ > 0) --live_ready_; }
  index_.erase(it);
  return true;
}

void PriorityJobQueue::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  cv_.notify_all();
}

bool PriorityJobQueue::is_closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

std::size_t PriorityJobQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_ready_ + live_delayed_;
}
std::size_t PriorityJobQueue::ready_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_ready_;
}
std::size_t PriorityJobQueue::delayed_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_delayed_;
}
int64_t PriorityJobQueue::total_pushed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pushed_;
}
int64_t PriorityJobQueue::total_popped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return popped_;
}

}  // namespace je
