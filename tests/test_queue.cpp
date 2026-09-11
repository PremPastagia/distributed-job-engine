#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "jobengine/queue.hpp"
#include "jobengine/util.hpp"

using namespace je;

TEST(QueueTest, PopsStrictlyByPriorityThenFifo) {
  PriorityJobQueue q(100);
  // Interleaved priorities, pushed in a deliberately unhelpful order.
  const std::vector<std::pair<std::string, int>> input = {
      {"a", 1}, {"b", 5}, {"c", 1}, {"d", 9}, {"e", 5}, {"f", 9}, {"g", 0},
  };
  for (const auto& kv : input) {
    ASSERT_EQ(q.push(kv.first, kv.second, 0), PriorityJobQueue::PushStatus::Ok);
  }
  // Expected order computed independently: priority descending, insertion order within.
  const std::vector<std::string> expected = {"d", "f", "b", "e", "a", "c", "g"};
  std::vector<std::string> got;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto e = q.pop_for(200);
    ASSERT_TRUE(e.has_value()) << "missing entry at index " << i;
    got.push_back(e->job_id);
  }
  EXPECT_EQ(got, expected);
  EXPECT_EQ(q.size(), 0u);
}

TEST(QueueTest, NegativePrioritiesOrderBelowZero) {
  PriorityJobQueue q(10);
  q.push("low", -5, 0);
  q.push("mid", 0, 0);
  q.push("high", 5, 0);
  EXPECT_EQ(q.pop_for(100)->job_id, "high");
  EXPECT_EQ(q.pop_for(100)->job_id, "mid");
  EXPECT_EQ(q.pop_for(100)->job_id, "low");
}

TEST(QueueTest, DelayedEntriesAreNotReadyBeforeTheirTime) {
  PriorityJobQueue q(10);
  const int64_t now = now_ms();
  q.push("later", 9, now + 150);
  q.push("now", 0, 0);
  // Despite its higher priority, "later" must not be served before it matures.
  const auto first = q.pop_for(50);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->job_id, "now");
  EXPECT_EQ(q.ready_size(), 0u);
  EXPECT_EQ(q.delayed_size(), 1u);
  EXPECT_FALSE(q.pop_for(10).has_value());

  const auto second = q.pop_for(500);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->job_id, "later");
  EXPECT_GE(now_ms() - now, 150);
}

TEST(QueueTest, EligibilityStampDistinguishesDelayFromQueueWait) {
  PriorityJobQueue q(10);
  const int64_t now = now_ms();
  q.push("delayed", 0, now + 100);
  const auto e = q.pop_for(400);
  ASSERT_TRUE(e.has_value());
  // The entry became eligible when the delay expired, not when it was pushed, so the
  // backoff interval is not charged to the job as queue-wait time.
  EXPECT_GE(e->eligible_at_steady, now + 100);
}

TEST(QueueTest, RemoveTombstonesAQueuedEntry) {
  PriorityJobQueue q(10);
  q.push("keep", 0, 0);
  q.push("drop", 5, 0);
  EXPECT_EQ(q.size(), 2u);
  EXPECT_TRUE(q.remove("drop"));
  EXPECT_EQ(q.size(), 1u) << "a removed entry must not be counted in queue depth";
  EXPECT_FALSE(q.remove("drop"));
  EXPECT_FALSE(q.remove("never-pushed"));
  const auto e = q.pop_for(100);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->job_id, "keep");
  EXPECT_FALSE(q.pop_for(10).has_value());
}

TEST(QueueTest, RemoveWorksForDelayedEntriesToo) {
  PriorityJobQueue q(10);
  q.push("delayed", 0, now_ms() + 5000);
  EXPECT_EQ(q.delayed_size(), 1u);
  EXPECT_TRUE(q.remove("delayed"));
  EXPECT_EQ(q.delayed_size(), 0u);
  EXPECT_EQ(q.size(), 0u);
}

TEST(QueueTest, RepushReplacesTheEarlierEntryForTheSameJob) {
  PriorityJobQueue q(10);
  q.push("j", 0, 0);
  q.push("j", 9, 0);  // e.g. a workflow fan-in releasing the same child twice
  EXPECT_EQ(q.size(), 1u) << "a job must not occupy two queue slots";
  const auto e = q.pop_for(100);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->priority, 9) << "the newer entry should win";
  EXPECT_FALSE(q.pop_for(10).has_value());
}

TEST(QueueTest, RemoveDoesNotAffectALaterRePushOfTheSameJob) {
  PriorityJobQueue q(10);
  q.push("j", 0, 0);
  ASSERT_TRUE(q.remove("j"));
  q.push("j", 0, 0);  // a retry of the same job id
  const auto e = q.pop_for(100);
  ASSERT_TRUE(e.has_value()) << "the tombstone must be keyed by entry, not by job id";
  EXPECT_EQ(e->job_id, "j");
}

TEST(QueueTest, EnforcesCapacity) {
  PriorityJobQueue q(3);
  EXPECT_EQ(q.push("a", 0, 0), PriorityJobQueue::PushStatus::Ok);
  EXPECT_EQ(q.push("b", 0, 0), PriorityJobQueue::PushStatus::Ok);
  EXPECT_EQ(q.push("c", 0, 0), PriorityJobQueue::PushStatus::Ok);
  EXPECT_EQ(q.push("d", 0, 0), PriorityJobQueue::PushStatus::Full);
  ASSERT_TRUE(q.pop_for(100).has_value());
  EXPECT_EQ(q.push("d", 0, 0), PriorityJobQueue::PushStatus::Ok);
}

TEST(QueueTest, CloseWakesBlockedConsumersAndRejectsPushes) {
  PriorityJobQueue q(10);
  std::atomic<bool> returned{false};
  std::thread consumer([&] {
    const auto e = q.pop();  // blocks indefinitely until the queue closes
    EXPECT_FALSE(e.has_value());
    returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(returned.load());
  q.close();
  consumer.join();
  EXPECT_TRUE(returned.load());
  EXPECT_TRUE(q.is_closed());
  EXPECT_EQ(q.push("late", 0, 0), PriorityJobQueue::PushStatus::Closed);
}

TEST(QueueTest, CloseStillDrainsAlreadyReadyEntries) {
  PriorityJobQueue q(10);
  q.push("a", 0, 0);
  q.push("b", 0, 0);
  q.close();
  EXPECT_TRUE(q.pop().has_value());
  EXPECT_TRUE(q.pop().has_value());
  EXPECT_FALSE(q.pop().has_value());
}

TEST(QueueTest, PopBatchTakesUpToTheRequestedCount) {
  PriorityJobQueue q(10);
  for (int i = 0; i < 5; ++i) q.push("j" + std::to_string(i), 0, 0);
  const auto batch = q.pop_batch(3, 100);
  EXPECT_EQ(batch.size(), 3u);
  EXPECT_EQ(q.size(), 2u);
  const auto rest = q.pop_batch(10, 100);
  EXPECT_EQ(rest.size(), 2u);
  EXPECT_TRUE(q.pop_batch(10, 10).empty());
}

// The core concurrency property: with many producers and many consumers, every pushed
// entry is delivered exactly once - none lost, none duplicated.
TEST(QueueTest, MultiProducerMultiConsumerLosesNothingAndDuplicatesNothing) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 2000;
  constexpr int kTotal = kProducers * kPerProducer;

  PriorityJobQueue q(kTotal + 10);
  std::atomic<int> produced{0};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        const std::string id = "p" + std::to_string(p) + "_" + std::to_string(i);
        ASSERT_EQ(q.push(id, i % 5, 0), PriorityJobQueue::PushStatus::Ok);
        produced.fetch_add(1);
      }
    });
  }

  std::vector<std::vector<std::string>> consumed(kConsumers);
  std::atomic<int> total_consumed{0};
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&, c] {
      for (;;) {
        auto e = q.pop();
        if (!e) return;
        consumed[static_cast<std::size_t>(c)].push_back(e->job_id);
        if (total_consumed.fetch_add(1) + 1 == kTotal) {
          q.close();  // everything has been taken; release the other consumers
          return;
        }
      }
    });
  }

  for (auto& t : producers) t.join();
  for (auto& t : consumers) t.join();

  EXPECT_EQ(produced.load(), kTotal);
  std::set<std::string> seen;
  std::size_t count = 0;
  for (const auto& v : consumed) {
    for (const std::string& id : v) {
      EXPECT_TRUE(seen.insert(id).second) << "duplicate delivery of " << id;
      ++count;
    }
  }
  EXPECT_EQ(count, static_cast<std::size_t>(kTotal)) << "entries were lost";
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(kTotal));
  EXPECT_EQ(q.total_pushed(), kTotal);
  EXPECT_EQ(q.total_popped(), kTotal);
}

TEST(QueueTest, ConcurrentRemovesAndPopsStayConsistent) {
  PriorityJobQueue q(20000);
  constexpr int kCount = 4000;
  for (int i = 0; i < kCount; ++i) q.push("j" + std::to_string(i), i % 3, 0);

  std::atomic<int> removed{0};
  std::atomic<int> popped{0};
  std::thread remover([&] {
    for (int i = 0; i < kCount; i += 2) {
      if (q.remove("j" + std::to_string(i))) removed.fetch_add(1);
    }
  });
  std::vector<std::thread> poppers;
  for (int t = 0; t < 3; ++t) {
    poppers.emplace_back([&] {
      while (auto e = q.pop_for(50)) {
        popped.fetch_add(1);
      }
    });
  }
  remover.join();
  for (auto& t : poppers) t.join();

  // Every entry is either removed or popped, never both and never neither.
  EXPECT_EQ(removed.load() + popped.load(), kCount);
  EXPECT_EQ(q.size(), 0u);
}
