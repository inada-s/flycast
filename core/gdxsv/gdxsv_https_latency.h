#pragma once

#include "build.h"

#include <string>
#include <vector>

struct GdxsvHttpsLatencyResult {
	bool ok = false;
	int status = 0;
	int min_ms = -1;
	std::string error;
	std::vector<int> attempts_ms;
};

// Google's regional API endpoints answer a request without credentials with an error such as 404, still a round
// trip to the region. A 5xx counts as no answer: http::get also returns 500 when the request failed.
static inline bool gdxsv_https_latency_answered(int status) { return 200 <= status && status < 500; }

#if defined(__APPLE__) || (defined(_WIN32) && !defined(TARGET_UWP)) || (defined(__linux__) && !defined(__ANDROID__))
GdxsvHttpsLatencyResult measureGdxsvHttpsLatency(const std::string& host, const std::string& path, int attempts);
#else
#include <chrono>
#include "oslib/http_client.h"

// Degraded fallback for platforms without a dedicated persistent HTTPS HEAD implementation.
// This measures generic http::get() calls and may include session/TLS setup overhead.
static inline GdxsvHttpsLatencyResult measureGdxsvHttpsLatency(const std::string& host, const std::string& path, int attempts)
{
	GdxsvHttpsLatencyResult result;
	const std::string url = "https://" + host + path;

	std::vector<u8> body;
	std::string content_type;
	int rc = http::get(url, body, content_type);
	if (!gdxsv_https_latency_answered(rc))
	{
		result.status = rc;
		result.error = "warmup failed";
		return result;
	}

	for (int i = 0; i < attempts; i++)
	{
		body.clear();
		content_type.clear();
		auto t1 = std::chrono::high_resolution_clock::now();
		rc = http::get(url, body, content_type);
		auto t2 = std::chrono::high_resolution_clock::now();
		if (!gdxsv_https_latency_answered(rc))
		{
			result.status = rc;
			result.error = "request failed";
			return result;
		}

		const int rtt = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
		result.attempts_ms.push_back(rtt);
		if (result.min_ms == -1 || rtt < result.min_ms)
			result.min_ms = rtt;
	}

	result.ok = result.min_ms >= 0;
	result.status = rc;
	return result;
}
#endif
