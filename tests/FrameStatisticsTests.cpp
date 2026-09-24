#include "graphics/presentation/frameStatistics.h"

#include <cmath>
#include <cstdio>

int main() {
  using Libs::Graphics::FrameStatistics;
  int failures = 0;
  const auto check = [&failures](bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "FAIL: %s\n", message);
      ++failures;
    }
  };
  const auto near = [](double actual, double expected) {
    return std::abs(actual - expected) < 0.00001;
  };
  FrameStatistics stats;
  check(stats.Record(0, 6000, true), "first presentation publishes a title");
  for (uint64_t frame = 1; frame <= 60; ++frame) {
    check(stats.Record(frame * 100, 6000, frame % 2 == 0) == (frame == 60),
          "title publication is limited to the sampling interval");
  }
  check(near(stats.FrameRate(), 30.0),
        "reused frames are excluded from game FPS");
  check(near(stats.PresentRate(), 60.0),
        "all successful presents count toward present rate");
  check(stats.TotalFrames() == 31 && stats.TotalPresents() == 61,
        "lifetime counters");
  for (uint64_t frame = 61; frame <= 120; ++frame) {
    stats.Record(frame * 100, 6000, false);
  }
  check(near(stats.FrameRate(), 0.0),
        "paused/reused-only rendering reports zero game FPS");
  check(near(stats.PresentRate(), 60.0),
        "reused-only rendering retains present rate");
  check(stats.Record(24000, 6000, true), "long gap produces a sample");
  check(near(stats.FrameRate(), 0.5),
        "elapsed time is measured rather than assumed");
  check(stats.Record(1, 6000, true),
        "clock rollback resets the sampling window");
  check(near(stats.FrameRate(), 0.0),
        "clock reset avoids an unsigned elapsed overflow");
  check(!stats.Record(2, 0, true), "zero frequency avoids division by zero");
  FrameStatistics second;
  second.Record(30000, 6000, false);
  check(second.TotalFrames() == 0 && second.TotalPresents() == 1,
        "windows do not share frame counters");
  if (failures == 0) {
    std::puts("FrameStatisticsTests: all passed");
  }
  return failures == 0 ? 0 : 1;
}
