/**
 * Simple stopwatch to keep track of time spent.
 */
#pragma once

#include <chrono>
#include <assert.h>

using namespace std::chrono;

class StopWatch
{
public:
    StopWatch()
    {
        running_ = false;
        laps_ = 0;
        sum_duration_ = {};
    }

    void start()
    {
        start_time_ = steady_clock::now();
        running_ = true;
    }

    void stop()
    {
        assert(running_);
        sum_duration_ += steady_clock::now() - start_time_;
        laps_++;
        running_ = false;
    }

    microseconds avg_lap_time()
    {
        assert(!running_);
        assert(laps_ > 0);
        return duration_cast<microseconds>(sum_duration_ / laps_);
    }

    double avg_lap_time_ns()
    {
        assert(laps_ > 0);
        auto res = duration_cast<nanoseconds>(sum_duration_);
        return (double)res.count() / laps_;
    }

private:
    time_point<steady_clock> start_time_;
    duration<double> sum_duration_;
    int laps_;
    bool running_;
};


