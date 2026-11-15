#pragma once
#include <cstdint>

// Windows: high priority class and 1 ms timer for the process. macOS: gdxsv_set_thread_priority.
void gdxsv_set_timer_resolution();
// macOS: real-time policy for the calling thread (the priority is per thread there). Elsewhere nothing.
void gdxsv_set_thread_priority();
// macOS: the scheduler demotes a real-time thread that runs for long without blocking to the lowest priority. A thread
// about to run flat out for that long (a replay seek) drops the policy, then takes it back. Nothing on a thread without
// the policy, nor elsewhere.
void gdxsv_suspend_thread_priority();
void gdxsv_restore_thread_priority();
void gdxsv_reset_timer_resolution();
void gdxsv_sleep_us(int64_t us);
int64_t gdxsv_sleep_and_busy_wait(int64_t us);

// Sleep benchmark for measuring timing accuracy
// Enable with CMake option: -DENABLE_SLEEP_BENCHMARK=ON
void gdxsv_sleep_benchmark_periodic_report();
