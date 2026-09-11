// Evidence fixture for scripts/gates/g20_static.mjs.
//
// The clang static analyzer's unix.BlockInCriticalSection checker reports a blocking call
// as being "inside a critical section" whenever the path went through
// std::condition_variable::wait, even when the lock is demonstrably released first. This
// file pins that behaviour down so the gate's exclusion is based on a reproducible
// demonstration rather than on an assumption.
//
// Expected analyzer verdict:
//   case_a, case_b, case_c, case_g  -> NO warning (the checker models the release correctly)
//   case_e, case_f                  -> warning, although the lock IS released (false positive)
//   case_d                          -> warning, and correctly so (positive control)
#include <condition_variable>
#include <mutex>
#include <vector>

#include <unistd.h>

namespace {
std::mutex m;
std::condition_variable cv;
std::vector<int> q;
int guarded;
}  // namespace

// lock_guard in a nested scope, blocking call afterwards.
void case_a(int fd, char* buf) {
  { std::lock_guard<std::mutex> lk(m); guarded = 1; }
  ::read(fd, buf, 16);
}

// unique_lock in a nested scope, blocking call afterwards.
void case_b(int fd, char* buf) {
  { std::unique_lock<std::mutex> lk(m); guarded = 2; }
  ::read(fd, buf, 16);
}

// unique_lock with an explicit unlock() before the blocking call.
void case_c(int fd, char* buf) {
  std::unique_lock<std::mutex> lk(m);
  guarded = 3;
  lk.unlock();
  ::read(fd, buf, 16);
}

// POSITIVE CONTROL: the lock really is held across the blocking call.
void case_d(int fd, char* buf) {
  std::lock_guard<std::mutex> lk(m);
  ::read(fd, buf, 16);
}

// FALSE POSITIVE: cv.wait, then the scope ends and the lock is released.
void case_e(int fd, char* buf) {
  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [] { return !q.empty(); });
    q.pop_back();
  }
  ::read(fd, buf, 16);
}

// FALSE POSITIVE: cv.wait, then an explicit unlock() before the blocking call.
void case_f(int fd, char* buf) {
  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [] { return !q.empty(); });
    q.pop_back();
    lk.unlock();
  }
  ::read(fd, buf, 16);
}

// Control for the exclusion rule: no condition_variable anywhere, so a blocking call
// after a released lock must stay clean.
void case_g(int fd, char* buf) {
  {
    std::unique_lock<std::mutex> lk(m);
    guarded = 7;
    lk.unlock();
  }
  ::read(fd, buf, 16);
}
