// SPDX-License-Identifier: Apache-2.0
// Result<T> dereference contract: const access works on an ok Result, and (debug
// builds, POSIX) dereferencing an error Result aborts instead of handing out the
// value-initialized payload.
#include <cstdio>
#include <string>

#include "misbklv/types.hpp"

#if !defined(NDEBUG) && defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#define MISBKLV_TEST_DEATH 1
#endif

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  %-42s FAIL\n", what);
    ++failures;
  }
}

#ifdef MISBKLV_TEST_DEATH
// Fork a child that runs `fn`; true iff it died of SIGABRT.
template <class F> static bool aborts(F fn) {
  std::fflush(nullptr);
  const pid_t pid = fork();
  if (pid == 0) {
    std::freopen("/dev/null", "w", stderr);  // keep the assert message out of the log
    fn();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

int main() {
  const auto ok = Result<std::string>::ok("abc");
  check(bool(ok) && *ok == "abc" && ok->size() == 3, "const ok Result: operator* and operator->");

#ifdef MISBKLV_TEST_DEATH
  check(aborts([] {
          auto e = Result<std::string>::err(Error::Backend);
          volatile auto n = e->size();
          (void)n;
        }),
        "error Result: operator-> aborts");
  check(aborts([] {
          const auto e = Result<std::string>::err(Error::Backend);
          volatile auto n = (*e).size();
          (void)n;
        }),
        "error Result: const operator* aborts");
#endif
  return failures ? 1 : 0;
}
