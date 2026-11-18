#include "gdxsv_sleep.h"

#include <cstdio>
#include <chrono>
#include <thread>
#include <algorithm>
#if _WIN32
#include <windows.h>
#elif __APPLE__
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#else
#include <sched.h>
#include <time.h>
#endif

#include "log/Log.h"

#if __APPLE__
namespace {
// The calling thread's: the policy is per thread on macOS.
struct GdxsvThreadPolicy {
	bool realtime = false;
	bool suspended = false;
};
thread_local GdxsvThreadPolicy current_policy;
}  // namespace
#endif

#if _WIN32
static NTSTATUS(__stdcall* NtDelayExecution)(BOOL Alertable, PLARGE_INTEGER DelayInterval) = (NTSTATUS(__stdcall*)(BOOL, PLARGE_INTEGER)) GetProcAddress(GetModuleHandle("ntdll.dll"), "NtDelayExecution");
static NTSTATUS(__stdcall* ZwSetTimerResolution)(IN ULONG RequestedResolution, IN BOOLEAN Set, OUT PULONG ActualResolution) = (NTSTATUS(__stdcall*)(ULONG, BOOLEAN, PULONG)) GetProcAddress(GetModuleHandle("ntdll.dll"), "ZwSetTimerResolution");
#endif

#ifdef SLEEP_BENCHMARK
// Benchmark statistics (internal)
static constexpr int64_t kSleepBenchmarkReportInterval = 600;  // Report every ~10 seconds at 60fps
static struct {
	int64_t frameCount = 0;
	int64_t totalRequested = 0;
	int64_t totalSlept = 0;
	int64_t totalBusyWait = 0;
	int64_t totalOvershoot = 0;
	int64_t maxOvershoot = 0;
	int64_t oversleepCount = 0;
	int64_t frameDropCount = 0;
	int64_t reportCounter = 0;
} g_sleepBenchmark;
#endif

void gdxsv_sleep_benchmark_periodic_report()
{
#ifdef SLEEP_BENCHMARK
	if (++g_sleepBenchmark.reportCounter < kSleepBenchmarkReportInterval)
		return;

	g_sleepBenchmark.reportCounter = 0;
	const auto& s = g_sleepBenchmark;

	if (s.frameCount == 0) {
		NOTICE_LOG(COMMON, "SleepBenchmark: No data collected");
		return;
	}

	const double avgRequested = (double)s.totalRequested / s.frameCount;
	const double avgSlept = (double)s.totalSlept / s.frameCount;
	const double avgBusyWait = (double)s.totalBusyWait / s.frameCount;
	const double avgOvershoot = (double)s.totalOvershoot / s.frameCount;
	const double sleepRatio = s.totalRequested > 0 ? (double)s.totalSlept / s.totalRequested * 100.0 : 0;
	const double busyRatio = s.totalRequested > 0 ? (double)s.totalBusyWait / s.totalRequested * 100.0 : 0;

	NOTICE_LOG(COMMON, "=== Sleep Benchmark Report ===");
	NOTICE_LOG(COMMON, "Frames: %lld", s.frameCount);
	NOTICE_LOG(COMMON, "Avg requested: %.1f us", avgRequested);
	NOTICE_LOG(COMMON, "Avg OS sleep:  %.1f us (%.1f%%)", avgSlept, sleepRatio);
	NOTICE_LOG(COMMON, "Avg busy wait: %.1f us (%.1f%%)", avgBusyWait, busyRatio);
	NOTICE_LOG(COMMON, "Avg overshoot: %.1f us, max: %lld us", avgOvershoot, s.maxOvershoot);
	NOTICE_LOG(COMMON, "Oversleep (>1ms): %lld times (%.2f%%)", s.oversleepCount, (double)s.oversleepCount / s.frameCount * 100.0);
	NOTICE_LOG(COMMON, "Frame drops: %lld (%.2f%%)", s.frameDropCount, (double)s.frameDropCount / s.frameCount * 100.0);
	NOTICE_LOG(COMMON, "==============================");

	// Reset stats after report
	g_sleepBenchmark.frameCount = 0;
	g_sleepBenchmark.totalRequested = 0;
	g_sleepBenchmark.totalSlept = 0;
	g_sleepBenchmark.totalBusyWait = 0;
	g_sleepBenchmark.totalOvershoot = 0;
	g_sleepBenchmark.maxOvershoot = 0;
	g_sleepBenchmark.oversleepCount = 0;
	g_sleepBenchmark.frameDropCount = 0;
#endif
}

void gdxsv_set_timer_resolution()
{
#if _WIN32
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	ULONG actual_resolution;
	ZwSetTimerResolution(1, true, &actual_resolution);
#elif __APPLE__
	gdxsv_set_thread_priority();
#endif
}

void gdxsv_set_thread_priority()
{
#if __APPLE__
	thread_port_t mach_thread_id = pthread_mach_thread_np(pthread_self());

	// Make thread fixed priority
	thread_extended_policy_data_t extended_policy;
	extended_policy.timeshare = 0;
	kern_return_t kr = thread_policy_set(
		mach_thread_id,
		THREAD_EXTENDED_POLICY,
		(thread_policy_t)&extended_policy,
		THREAD_EXTENDED_POLICY_COUNT);
	if (kr != KERN_SUCCESS) {
		ERROR_LOG(COMMON, "Cannot make thread fixed priority: %d", kr);
		return;
	}

	// Set to relatively high priority.
	thread_precedence_policy_data_t precedence_policy;
	precedence_policy.importance = 63;
	kr = thread_policy_set(
		mach_thread_id, THREAD_PRECEDENCE_POLICY,
		(thread_policy_t)&precedence_policy,
		THREAD_PRECEDENCE_POLICY_COUNT);
	if (kr != KERN_SUCCESS) {
		ERROR_LOG(COMMON, "Cannot set high priority: %d", kr);
		return;
	}

	mach_timebase_info_data_t timebase;
	kr = mach_timebase_info(&timebase);
	if (kr != KERN_SUCCESS) {
		ERROR_LOG(COMMON, "Couldn't get timebase: %d", kr);
		return;
	}
	double clock2abs = ((double)timebase.denom / (double)timebase.numer) * USEC_PER_SEC;

	// Set the thread priority.
	thread_time_constraint_policy tc_policy;
	tc_policy.period = 0;
	tc_policy.computation = 50 * clock2abs;
	tc_policy.constraint = 100 * clock2abs;
	tc_policy.preemptible = FALSE;

	kr = thread_policy_set(
		mach_thread_id,
		THREAD_TIME_CONSTRAINT_POLICY,
		(thread_policy_t)&tc_policy,
		THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (kr != KERN_SUCCESS) {
		ERROR_LOG(COMMON, "Could not set thread policy: %d", kr);
	}
	current_policy.realtime = kr == KERN_SUCCESS;
#endif
}

void gdxsv_suspend_thread_priority()
{
#if __APPLE__
	if (!current_policy.realtime) return;
	thread_standard_policy_data_t standard{};
	if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY, (thread_policy_t)&standard,
						  THREAD_STANDARD_POLICY_COUNT) != KERN_SUCCESS)
		return;
	current_policy.realtime = false;
	current_policy.suspended = true;
#endif
}

void gdxsv_restore_thread_priority()
{
#if __APPLE__
	if (!current_policy.suspended) return;
	current_policy.suspended = false;
	gdxsv_set_thread_priority();
#endif
}

void gdxsv_reset_timer_resolution()
{
#if _WIN32
	ULONG actual_resolution;
	ZwSetTimerResolution(1, false, &actual_resolution);
#endif
}

void gdxsv_sleep_us(int64_t us)
{
#if _WIN32
	LARGE_INTEGER interval;
	interval.QuadPart = -us * 10;
	NtDelayExecution(false, &interval);
#else
	timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = us * 1000;
	while (nanosleep(&ts, &ts));
#endif
}

int64_t gdxsv_sleep_and_busy_wait(int64_t us)
{
	const auto t1 = std::chrono::steady_clock::now();

#ifdef SLEEP_BENCHMARK
	g_sleepBenchmark.frameCount++;
	g_sleepBenchmark.totalRequested += std::max<int64_t>(0, us);

	// Check for frame drop (no time to wait)
	if (us <= 0) {
		g_sleepBenchmark.frameDropCount++;
		return -us;  // Return how much we're behind
	}
#endif

	const auto us2 = (us / 1000) * 1000;
	int64_t sleepRequested = 0;

#if defined(_WIN32) || defined(__APPLE__)
	if (2000 <= us2) {
		sleepRequested = us2 - 1000;
		gdxsv_sleep_us(sleepRequested);
	}
#else
	// FIXME: Optimize for other platforms
	if (4000 <= us2) {
		sleepRequested = us2 - 2000;
		gdxsv_sleep_us(sleepRequested);
	}
#endif

#ifdef SLEEP_BENCHMARK
	const auto afterSleep = std::chrono::steady_clock::now();
	const auto sleptTime = std::chrono::duration_cast<std::chrono::microseconds>(afterSleep - t1).count();
	g_sleepBenchmark.totalSlept += sleptTime;

	// Check for oversleep (slept more than requested + 1ms tolerance)
	if (sleepRequested > 0 && sleptTime > sleepRequested + 1000) {
		g_sleepBenchmark.oversleepCount++;
	}
#endif

	int64_t overshoot = 0;
	if (0 < us) {
		for (;;) {
			const auto t2 = std::chrono::steady_clock::now();
			const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
			if (us <= dt) {
				overshoot = dt - us;
				break;
			}
		}
	}

#ifdef SLEEP_BENCHMARK
	const auto t3 = std::chrono::steady_clock::now();
	const auto totalTime = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t1).count();
	const auto busyWaitTime = totalTime - sleptTime;
	g_sleepBenchmark.totalBusyWait += busyWaitTime;
	g_sleepBenchmark.totalOvershoot += overshoot;
	g_sleepBenchmark.maxOvershoot = std::max(g_sleepBenchmark.maxOvershoot, overshoot);
#endif

	return overshoot;
}
