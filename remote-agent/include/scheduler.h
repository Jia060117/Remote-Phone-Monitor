#pragma once
#include <chrono>
#include <atomic>

class Scheduler {
public:
    explicit Scheduler(int interval_sec);

    // 阻塞到下一次采样点
    void wait();

    // 动态更新间隔（热更新）。
    // 主循环是单线程的：本函数只在循环体里调用，因此新间隔从「下一个采样点」
    // 起生效，当前这一轮睡眠仍按旧间隔走完（最长延迟 = 一个旧周期）。
    void set_interval(int interval_sec);

    // 获取当前间隔
    int get_interval() const;

private:
    std::atomic<int> interval_sec_;
    std::chrono::steady_clock::time_point next_tick_;
};
