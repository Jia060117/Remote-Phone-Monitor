#include "scheduler.h"
#include "logger.h"
#include <thread>

Scheduler::Scheduler(int interval_sec)
    : interval_sec_(interval_sec)
    , next_tick_(std::chrono::steady_clock::now()) {
}

void Scheduler::wait() {
    auto interval = std::chrono::seconds(interval_sec_.load());
    next_tick_ += interval;

    auto now = std::chrono::steady_clock::now();
    if (next_tick_ > now) {
        std::this_thread::sleep_until(next_tick_);
    } else {
        // 已经超时，重置到当前时间
        next_tick_ = now;
        LOGW("Scheduler: tick was behind, resetting");
    }
}

void Scheduler::set_interval(int interval_sec) {
    if (interval_sec < 1) interval_sec = 1;
    const int old = interval_sec_.exchange(interval_sec);
    if (old != interval_sec) {
        LOGI("Scheduler interval updated %d -> %d sec", old, interval_sec);
    }
}

int Scheduler::get_interval() const {
    return interval_sec_.load();
}
