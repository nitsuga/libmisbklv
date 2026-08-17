// SPDX-License-Identifier: Apache-2.0
// Direct coverage of PtsMarks::prune (issue #4): a feed whose KLV PID never
// frames must still let the extractor bound the marks queue by stream_off,
// without relying on at() ever being called.
#include <cstdio>

#include "../src/pts_marks.hpp"

using namespace misbklv;
using namespace misbklv::detail;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

static void test_prune_drops_entries_behind_offset() {
  PtsMarks marks;
  marks.mark(0, 100);
  marks.mark(10, 200);
  marks.mark(20, 300);
  marks.prune(15);  // drops off=0 and off=10; off=20 is still ahead
  // The dropped marks' latest value (200) is still in effect for anything
  // queried at/after stream_off but before the next mark.
  check(marks.at(15) == 200, "prune leaves cur_ at the last dropped mark's pts");
  check(marks.at(25) == 300, "mark ahead of the prune point survives");
}

static void test_prune_keeps_offset_equal_entry() {
  PtsMarks marks;
  marks.mark(5, 111);
  marks.mark(10, 222);
  marks.prune(10);  // off==stream_off is still live: not behind it
  check(marks.at(10) == 222, "prune does not drop the entry at stream_off itself");
}

static void test_prune_on_empty_queue() {
  PtsMarks marks;
  marks.prune(1000);  // must not touch cur_ (still kNoPts) or misbehave
  check(marks.at(0) == kNoPts, "prune on an empty queue is a no-op");
}

static void test_prune_bounds_unbounded_growth() {
  // Simulates a non-framing feed: mark() called every "buffer" but stream_off
  // (and thus prune()) only advances via resync of unmatched bytes, never via
  // at(). The queue must not grow past what's still ahead of stream_off.
  PtsMarks marks;
  for (int i = 0; i < 100000; ++i) {
    marks.mark(static_cast<std::size_t>(i), i);
    marks.prune(static_cast<std::size_t>(i));  // resync drops all but the tip
  }
  check(marks.pending() <= 1,
        "queue stays bounded to the still-live window after sustained non-framing input");
}

int main() {
  test_prune_drops_entries_behind_offset();
  test_prune_keeps_offset_equal_entry();
  test_prune_on_empty_queue();
  test_prune_bounds_unbounded_growth();
  std::printf("%s\n", failures == 0 ? "PTS_MARKS: all PASS" : "PTS_MARKS: FAIL");
  return failures == 0 ? 0 : 1;
}
