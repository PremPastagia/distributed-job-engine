#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "test_helpers.hpp"

using namespace je;
using namespace je::test;

TEST(RetryTest, AFailingJobIsRetriedExactlyMaxAttemptsTimes) {
  Harness h(2);
  std::atomic<int> calls{0};
  h.handlers.register_handler("always_fails", [&](const JobContext&) {
    calls.fetch_add(1);
    return JobOutcome::failure("nope");
  });
  h.start();
  SubmitRequest req;
  req.type = "always_fails";
  req.retry.max_attempts = 3;
  req.retry.backoff_base_ms = 5;
  req.retry.backoff_multiplier = 1.0;
  std::string id, err;
  ASSERT_EQ(h.engine->submit(req, id, err), SubmitStatus::Ok);
  ASSERT_TRUE(h.wait_terminal(id, 10000));

  EXPECT_EQ(calls.load(), 3) << "the handler must run exactly max_attempts times";
  Job job;
  ASSERT_TRUE(h.engine->get_job(id, job));
  EXPECT_EQ(job.state, JobState::Failed) << "exhausted retries must end in FAILED (dead letter)";
  EXPECT_EQ(job.attempt, 3);
  EXPECT_NE(job.error.find("nope"), std::string::npos);
  EXPECT_EQ(h.metrics.attempts_retried.load(), 2u) << "3 attempts means 2 retries";
  EXPECT_EQ(h.metrics.jobs_failed.load(), 1u);

  h.store->flush();
  const auto attempts = h.store->list_attempts(id);
  ASSERT_EQ(attempts.size(), 3u) << "every attempt must be recorded";
  for (const AttemptRecord& a : attempts) EXPECT_EQ(a.status, "FAILED");
}

TEST(RetryTest, AJobThatSucceedsOnRetryStopsRetrying) {
  Harness h(2);
  h.start();
  SubmitRequest req;
  req.type = "fail";
  req.payload = Json::parse_or_null(R"({"fail_until_attempt":3})");
  req.retry.max_attempts = 5;
  req.retry.backoff_base_ms = 5;
  req.retry.backoff_multiplier = 1.0;
  std::string id, err;
  ASSERT_EQ(h.engine->submit(req, id, err), SubmitStatus::Ok);
  ASSERT_TRUE(h.wait_terminal(id, 10000));

  Job job;
  ASSERT_TRUE(h.engine->get_job(id, job));
  EXPECT_EQ(job.state, JobState::Succeeded);
  EXPECT_EQ(job.attempt, 3) << "it should stop as soon as an attempt succeeds";
  EXPECT_EQ(job.result.get_int("succeeded_on_attempt", 0), 3);
  h.store->flush();
  const auto attempts = h.store->list_attempts(id);
  ASSERT_EQ(attempts.size(), 3u);
  EXPECT_EQ(attempts[0].status, "FAILED");
  EXPECT_EQ(attempts[1].status, "FAILED");
  EXPECT_EQ(attempts[2].status, "SUCCEEDED");
}

TEST(RetryTest, MaxAttemptsOneMeansNoRetryAtAll) {
  Harness h(2);
  std::atomic<int> calls{0};
  h.handlers.register_handler("once", [&](const JobContext&) {
    calls.fetch_add(1);
    return JobOutcome::failure("x");
  });
  h.start();
  const std::string id = h.submit("once", Json::object(), 0, /*max_attempts=*/1);
  ASSERT_TRUE(h.wait_terminal(id, 5000));
  EXPECT_EQ(calls.load(), 1) << "retry must be opt-in, never the default";
  EXPECT_EQ(h.state_of(id), JobState::Failed);
}

TEST(RetryTest, ObservedBackoffMatchesTheConfiguredExponentialSchedule) {
  Harness h(2);
  std::mutex m;
  std::vector<int64_t> attempt_times;
  h.handlers.register_handler("timed_fail", [&](const JobContext&) {
    std::lock_guard<std::mutex> lock(m);
    attempt_times.push_back(now_ms());
    return JobOutcome::failure("again");
  });
  h.start();

  SubmitRequest req;
  req.type = "timed_fail";
  req.retry.max_attempts = 4;
  req.retry.backoff_base_ms = 60;
  req.retry.backoff_multiplier = 2.0;
  req.retry.backoff_max_ms = 10000;
  req.retry.jitter = false;
  std::string id, err;
  ASSERT_EQ(h.engine->submit(req, id, err), SubmitStatus::Ok);
  ASSERT_TRUE(h.wait_terminal(id, 20000));

  std::lock_guard<std::mutex> lock(m);
  ASSERT_EQ(attempt_times.size(), 4u);
  // Expected gaps: 60, 120, 240 ms. The scheduler can only be late, never early, so the
  // lower bound is asserted exactly and the upper bound allows for scheduling slack.
  const std::vector<int64_t> expected_gaps = {60, 120, 240};
  for (std::size_t i = 0; i < expected_gaps.size(); ++i) {
    const int64_t gap = attempt_times[i + 1] - attempt_times[i];
    EXPECT_GE(gap, expected_gaps[i] - 5)
        << "retry " << i + 1 << " fired early: " << gap << "ms";
    EXPECT_LT(gap, expected_gaps[i] + 400)
        << "retry " << i + 1 << " was much later than scheduled: " << gap << "ms";
  }
}

TEST(RetryTest, ARetryingJobIsCountedAsOutstandingUntilItIsTerminal) {
  Harness h(1);
  h.start();
  SubmitRequest req;
  req.type = "fail";
  req.retry.max_attempts = 3;
  req.retry.backoff_base_ms = 200;
  req.retry.backoff_multiplier = 1.0;
  std::string id, err;
  ASSERT_EQ(h.engine->submit(req, id, err), SubmitStatus::Ok);
  ASSERT_TRUE(wait_until([&] { return h.state_of(id) == JobState::Retrying; }, 3000));
  EXPECT_EQ(h.engine->outstanding(), 1u);
  EXPECT_GE(h.engine->queue_depth(), 1u) << "a backing-off job still occupies the queue";
  ASSERT_TRUE(h.wait_terminal(id, 10000));
  EXPECT_EQ(h.engine->outstanding(), 0u);
}

// ---- timeouts --------------------------------------------------------------------------

TEST(TimeoutTest, ACooperativeHandlerIsStoppedAtItsDeadline) {
  Harness h(2);
  h.start();
  const int64_t t0 = now_ms();
  // Asks to sleep for 5s but is given a 150ms deadline.
  const std::string id =
      h.submit("sleep", Json::parse_or_null(R"({"ms":5000})"), 0, 1, /*timeout_ms=*/150);
  ASSERT_FALSE(id.empty());
  ASSERT_TRUE(h.wait_terminal(id, 5000));
  const int64_t elapsed = now_ms() - t0;

  Job job;
  ASSERT_TRUE(h.engine->get_job(id, job));
  EXPECT_EQ(job.state, JobState::TimedOut)
      << "an exhausted deadline must be distinguishable from an ordinary failure";
  EXPECT_NE(job.error.find("deadline exceeded"), std::string::npos);
  EXPECT_LT(elapsed, 2000) << "the job was not stopped anywhere near its deadline";
  EXPECT_GE(elapsed, 140);
  EXPECT_EQ(h.metrics.jobs_timed_out.load(), 1u);
  h.store->flush();
  const auto attempts = h.store->list_attempts(id);
  ASSERT_EQ(attempts.size(), 1u);
  EXPECT_EQ(attempts[0].status, "TIMED_OUT");
}

TEST(TimeoutTest, ATimedOutJobIsRetriedWhenItHasAttemptsLeft) {
  Harness h(2);
  h.start();
  SubmitRequest req;
  req.type = "sleep";
  req.payload = Json::parse_or_null(R"({"ms":5000})");
  req.timeout_ms = 100;
  req.retry.max_attempts = 2;
  req.retry.backoff_base_ms = 10;
  req.retry.backoff_multiplier = 1.0;
  std::string id, err;
  ASSERT_EQ(h.engine->submit(req, id, err), SubmitStatus::Ok);
  ASSERT_TRUE(h.wait_terminal(id, 10000));
  Job job;
  ASSERT_TRUE(h.engine->get_job(id, job));
  EXPECT_EQ(job.state, JobState::TimedOut);
  EXPECT_EQ(job.attempt, 2) << "a timeout consumes an attempt like any other failure";
  h.store->flush();
  EXPECT_EQ(h.store->list_attempts(id).size(), 2u);
}

// This test exists to demonstrate the documented limit of cooperative cancellation
// (docs/DESIGN_DECISIONS.md D-07) rather than assert it only in prose: a handler that
// never checks its cancellation flag runs to completion and holds its worker thread.
TEST(TimeoutTest, UncooperativeHandlerHoldsItsThreadPastTheDeadline) {
  Harness h(1);
  h.start();
  const int64_t t0 = now_ms();
  const std::string id = h.submit("sleep_uncooperative",
                                  Json::parse_or_null(R"({"ms":400})"), 0, 1,
                                  /*timeout_ms=*/50);
  ASSERT_FALSE(id.empty());
  ASSERT_TRUE(h.wait_terminal(id, 5000));
  const int64_t elapsed = now_ms() - t0;

  // The engine still records the deadline breach...
  EXPECT_EQ(h.state_of(id), JobState::TimedOut);
  // ...but it could not stop the handler, so the worker was occupied for the full 400ms.
  EXPECT_GE(elapsed, 380)
      << "if this fails, cancellation became preemptive and D-07 should be rewritten";
}

TEST(TimeoutTest, NoDeadlineMeansTheJobRunsToCompletion) {
  Harness h(2);
  h.start();
  const std::string id = h.submit("sleep", Json::parse_or_null(R"({"ms":120})"), 0, 1, 0);
  ASSERT_TRUE(h.wait_terminal(id, 5000));
  EXPECT_EQ(h.state_of(id), JobState::Succeeded);
}

// ---- cancellation ------------------------------------------------------------------------

TEST(CancelTest, CancellingAQueuedJobIsImmediateAndItNeverRuns) {
  Harness h(0);  // no workers, so the job stays queued
  h.start();
  const std::string id = h.submit("noop");
  ASSERT_FALSE(id.empty());
  EXPECT_EQ(h.state_of(id), JobState::Queued);
  EXPECT_EQ(h.engine->queue_depth(), 1u);

  std::string err;
  EXPECT_EQ(h.engine->cancel(id, err), CancelStatus::Ok);
  EXPECT_EQ(h.state_of(id), JobState::Cancelled);
  EXPECT_EQ(h.engine->queue_depth(), 0u) << "a cancelled job must leave the queue";
  EXPECT_EQ(h.engine->outstanding(), 0u);
  EXPECT_EQ(h.metrics.jobs_cancelled.load(), 1u);
}

TEST(CancelTest, CancellingARunningJobRequestsCooperativeStop) {
  Harness h(2);
  std::atomic<bool> saw_cancel{false};
  std::atomic<bool> entered{false};
  h.handlers.register_handler("watches_cancel", [&](const JobContext& ctx) {
    entered.store(true);
    for (int i = 0; i < 500; ++i) {
      if (ctx.cancel_requested()) {
        saw_cancel.store(true);
        return JobOutcome::failure("stopped on request");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return JobOutcome::success();
  });
  h.start();
  const std::string id = h.submit("watches_cancel");
  ASSERT_TRUE(wait_until([&] { return entered.load(); }, 3000));

  std::string err;
  EXPECT_EQ(h.engine->cancel(id, err), CancelStatus::Ok);
  ASSERT_TRUE(h.wait_terminal(id, 5000));
  EXPECT_TRUE(saw_cancel.load()) << "the handler never observed the cancellation flag";
  EXPECT_EQ(h.state_of(id), JobState::Cancelled);
  EXPECT_EQ(h.engine->outstanding(), 0u);
}

TEST(CancelTest, CancellingATerminalJobIsRejected) {
  Harness h(1);
  h.start();
  const std::string id = h.submit("noop");
  ASSERT_TRUE(h.wait_terminal(id, 5000));
  std::string err;
  EXPECT_EQ(h.engine->cancel(id, err), CancelStatus::AlreadyTerminal);
  EXPECT_EQ(h.state_of(id), JobState::Succeeded) << "a terminal state must not be overwritten";
}

TEST(CancelTest, CancellingAnUnknownJobIsReportedNotSilentlyAccepted) {
  Harness h(1);
  h.start();
  std::string err;
  EXPECT_EQ(h.engine->cancel("j_nope", err), CancelStatus::NotFound);
}

TEST(CancelTest, CancelRacingWithStartResolvesToExactlyOneOutcome) {
  // Repeats a tight race: cancel is issued at the same moment a worker picks the job up.
  // Whichever wins, the job must end in exactly one terminal state and the handler must
  // not run after a successful queued-cancel.
  for (int round = 0; round < 40; ++round) {
    Harness h(2);
    std::atomic<int> executions{0};
    h.handlers.register_handler("racy", [&](const JobContext& ctx) {
      executions.fetch_add(1);
      ctx.sleep_or_cancel(30);
      return JobOutcome::success();
    });
    h.start();
    const std::string id = h.submit("racy");
    ASSERT_FALSE(id.empty());

    std::string err;
    std::thread canceller([&] { h.engine->cancel(id, err); });
    canceller.join();
    ASSERT_TRUE(h.wait_terminal(id, 5000)) << "round " << round;

    Job job;
    ASSERT_TRUE(h.engine->get_job(id, job));
    EXPECT_TRUE(job.state == JobState::Cancelled || job.state == JobState::Succeeded)
        << "round " << round << " ended in " << to_string(job.state);
    EXPECT_LE(executions.load(), 1) << "the handler ran more than once in round " << round;
    if (job.state == JobState::Cancelled && job.attempt == 0) {
      EXPECT_EQ(executions.load(), 0)
          << "a job cancelled while queued must never have executed";
    }
    EXPECT_EQ(h.engine->outstanding(), 0u) << "round " << round;
  }
}

TEST(CancelTest, CancellingADelayedJobRemovesItBeforeItMatures) {
  Harness h(2);
  h.start();
  const std::string id = h.submit("noop", Json::object(), 0, 1, 0, /*delay_ms=*/1000);
  EXPECT_EQ(h.state_of(id), JobState::Scheduled);
  std::string err;
  EXPECT_EQ(h.engine->cancel(id, err), CancelStatus::Ok);
  EXPECT_EQ(h.state_of(id), JobState::Cancelled);
  EXPECT_EQ(h.engine->delayed_depth(), 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_EQ(h.state_of(id), JobState::Cancelled) << "it must not resurrect when it matures";
}
