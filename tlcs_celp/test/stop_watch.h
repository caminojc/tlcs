// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <chrono>

/**
 * Simple stopwatch to keep track of time spent.
 */
class StopWatch {
 public:
  StopWatch() {
    running_ = false;
    laps_ = 0;
    sum_duration_ = {};
  }

  void start();
  void stop();
  std::chrono::microseconds avg_lap_time();

 private:
  std::chrono::time_point<std::chrono::steady_clock> start_time_;
  std::chrono::duration<double> sum_duration_;
  int laps_;
  bool running_;
};
