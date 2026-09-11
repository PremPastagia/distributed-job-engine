#include <gtest/gtest.h>

#include <set>

#include "jobengine/job.hpp"

using namespace je;

TEST(JobStateTest, NameRoundTripsForEveryState) {
  for (const JobState s : all_states()) {
    JobState parsed;
    ASSERT_TRUE(state_from_string(to_string(s), parsed)) << to_string(s);
    EXPECT_EQ(parsed, s);
  }
  JobState ignored;
  EXPECT_FALSE(state_from_string("NOT_A_STATE", ignored));
  EXPECT_FALSE(state_from_string("", ignored));
  EXPECT_FALSE(state_from_string("queued", ignored));  // names are case sensitive
}

TEST(JobStateTest, TerminalStatesAreExactlyTheDocumentedFive) {
  const std::set<JobState> expected = {JobState::Succeeded, JobState::Failed,
                                       JobState::TimedOut, JobState::Cancelled,
                                       JobState::Skipped};
  for (const JobState s : all_states()) {
    EXPECT_EQ(is_terminal(s), expected.count(s) > 0) << to_string(s);
  }
  EXPECT_FALSE(is_failure_terminal(JobState::Succeeded));
  EXPECT_TRUE(is_failure_terminal(JobState::Failed));
  EXPECT_TRUE(is_failure_terminal(JobState::TimedOut));
  EXPECT_TRUE(is_failure_terminal(JobState::Cancelled));
  EXPECT_TRUE(is_failure_terminal(JobState::Skipped));
  EXPECT_FALSE(is_failure_terminal(JobState::Running));
}

TEST(JobStateTest, TerminalStatesAreAbsorbing) {
  for (const JobState from : all_states()) {
    if (!is_terminal(from)) continue;
    for (const JobState to : all_states()) {
      EXPECT_FALSE(is_valid_transition(from, to))
          << to_string(from) << " -> " << to_string(to) << " must be rejected";
    }
  }
}

TEST(JobStateTest, SelfTransitionsAreRejected) {
  for (const JobState s : all_states()) {
    EXPECT_FALSE(is_valid_transition(s, s)) << to_string(s);
  }
}

// The transition table is written out independently here. If the implementation and this
// table disagree, one of them is wrong and the test says which pair.
TEST(JobStateTest, TransitionTableMatchesTheDocumentedContract) {
  const std::set<std::pair<JobState, JobState>> allowed = {
      {JobState::Pending, JobState::Queued},
      {JobState::Pending, JobState::Scheduled},
      {JobState::Pending, JobState::Cancelled},
      {JobState::Pending, JobState::Skipped},
      {JobState::Scheduled, JobState::Queued},
      {JobState::Scheduled, JobState::Cancelled},
      {JobState::Scheduled, JobState::Skipped},
      {JobState::Queued, JobState::Running},
      {JobState::Queued, JobState::Cancelled},
      {JobState::Queued, JobState::Skipped},
      {JobState::Running, JobState::Succeeded},
      {JobState::Running, JobState::Retrying},
      {JobState::Running, JobState::Failed},
      {JobState::Running, JobState::TimedOut},
      {JobState::Running, JobState::Cancelled},
      {JobState::Running, JobState::Queued},
      {JobState::Retrying, JobState::Queued},
      {JobState::Retrying, JobState::Cancelled},
      {JobState::Retrying, JobState::Failed},
      {JobState::Retrying, JobState::TimedOut},
      {JobState::Retrying, JobState::Skipped},
  };
  int allowed_seen = 0;
  for (const JobState from : all_states()) {
    for (const JobState to : all_states()) {
      const bool want = allowed.count({from, to}) > 0;
      EXPECT_EQ(is_valid_transition(from, to), want)
          << to_string(from) << " -> " << to_string(to);
      if (want) ++allowed_seen;
    }
  }
  EXPECT_EQ(allowed_seen, static_cast<int>(allowed.size()));
}

TEST(RetryPolicyTest, ExponentialBackoffFollowsTheFormula) {
  RetryPolicy p;
  p.backoff_base_ms = 100;
  p.backoff_multiplier = 2.0;
  p.backoff_max_ms = 30000;
  p.jitter = false;
  // base * multiplier^(failed_attempt - 1)
  EXPECT_EQ(p.delay_after_attempt(1), 100);
  EXPECT_EQ(p.delay_after_attempt(2), 200);
  EXPECT_EQ(p.delay_after_attempt(3), 400);
  EXPECT_EQ(p.delay_after_attempt(4), 800);
  EXPECT_EQ(p.delay_after_attempt(5), 1600);
}

TEST(RetryPolicyTest, BackoffIsCappedAndCannotOverflow) {
  RetryPolicy p;
  p.backoff_base_ms = 100;
  p.backoff_multiplier = 3.0;
  p.backoff_max_ms = 5000;
  EXPECT_EQ(p.delay_after_attempt(1), 100);
  EXPECT_EQ(p.delay_after_attempt(2), 300);
  EXPECT_EQ(p.delay_after_attempt(3), 900);
  EXPECT_EQ(p.delay_after_attempt(4), 2700);
  EXPECT_EQ(p.delay_after_attempt(5), 5000);   // capped
  EXPECT_EQ(p.delay_after_attempt(50), 5000);  // still capped, no overflow
  EXPECT_EQ(p.delay_after_attempt(1000), 5000);
}

TEST(RetryPolicyTest, JitterStaysWithinTheFullJitterWindow) {
  RetryPolicy p;
  p.backoff_base_ms = 100;
  p.backoff_multiplier = 2.0;
  p.backoff_max_ms = 30000;
  p.jitter = true;
  bool saw_below_max = false;
  for (int i = 0; i < 200; ++i) {
    const int64_t d = p.delay_after_attempt(3);  // undelayed value is 400
    EXPECT_GE(d, 0);
    EXPECT_LE(d, 400);
    if (d < 400) saw_below_max = true;
  }
  EXPECT_TRUE(saw_below_max) << "jitter never reduced the delay, so it is not active";
}

TEST(RetryPolicyTest, ZeroAndNegativeAttemptsAreClamped) {
  RetryPolicy p;
  p.backoff_base_ms = 50;
  EXPECT_EQ(p.delay_after_attempt(0), 50);
  EXPECT_EQ(p.delay_after_attempt(-5), 50);
}

TEST(JobTest, JsonViewExposesTheDocumentedFields) {
  Job j;
  j.job_id = "j_TEST";
  j.type = "sleep";
  j.priority = 3;
  j.state = JobState::Succeeded;
  j.attempt = 2;
  j.retry.max_attempts = 5;
  j.timeout_ms = 1000;
  j.created_at_ms = 1757000000000LL;
  j.queue_wait_us = 1500;
  j.exec_us = 2500;
  const Json v = j.to_json();
  EXPECT_EQ(v.get_string("job_id", ""), "j_TEST");
  EXPECT_EQ(v.get_string("state", ""), "SUCCEEDED");
  EXPECT_EQ(v.get_int("attempt", 0), 2);
  EXPECT_EQ(v.get_int("max_attempts", 0), 5);
  EXPECT_DOUBLE_EQ(v.get_double("queue_wait_ms", -1), 1.5);
  EXPECT_DOUBLE_EQ(v.get_double("exec_ms", -1), 2.5);
  EXPECT_EQ(v.get_int("queue_wait_us", -1), 1500);
  EXPECT_TRUE(v.find("error")->is_null());
  EXPECT_TRUE(v.find("workflow_id")->is_null());
}
