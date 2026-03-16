// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "stop_watch.h"
#include <assert.h>
#include <chrono>

void StopWatch::start() {
  start_time_ = std::chrono::steady_clock::now();
  running_ = true;
}

void StopWatch::stop() {
  assert(running_);
  sum_duration_ += std::chrono::steady_clock::now() - start_time_;
  laps_++;
  running_ = false;
}

std::chrono::microseconds StopWatch::avg_lap_time() {
  assert(!running_);
  assert(laps_ > 0);
  return std::chrono::duration_cast<std::chrono::microseconds>(
      sum_duration_ / laps_);
}
