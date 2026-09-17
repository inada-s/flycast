#include "gdxsv_https_latency.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <chrono>

#include <curl/curl.h>

#include "oslib/ax_offline.h"
#include "oslib/http_client.h"

namespace {

static GdxsvHttpsLatencyResult failResult(CURLcode code, long status, const char *prefix)
{
	GdxsvHttpsLatencyResult result;
	result.status = (int)status;
	if (code != CURLE_OK)
		result.error = std::string(prefix) + ": " + curl_easy_strerror(code);
	else
		result.error = prefix;
	return result;
}

static CURLcode performHead(CURL *curl, long& status)
{
	CURLcode code = curl_easy_perform(curl);
	status = 0;
	if (code == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	return code;
}

} // namespace

GdxsvHttpsLatencyResult measureGdxsvHttpsLatency(const std::string& host, const std::string& path, int attempts)
{
	GdxsvHttpsLatencyResult result;
	if (axoffline::blockHost(host, 443))
		return result;
	const std::string url = "https://" + host + path;

	CURL *curl = curl_easy_init();
	if (curl == nullptr)
	{
		result.error = "curl_easy_init failed";
		return result;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, http::getUserAgent().c_str());
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

	long status = 0;
	CURLcode code = performHead(curl, status);
	if (code != CURLE_OK || !http::success((int)status))
	{
		curl_easy_cleanup(curl);
		return failResult(code, status, "warmup failed");
	}

	for (int i = 0; i < attempts; i++)
	{
		auto t1 = std::chrono::high_resolution_clock::now();
		code = performHead(curl, status);
		auto t2 = std::chrono::high_resolution_clock::now();

		if (code != CURLE_OK || !http::success((int)status))
		{
			curl_easy_cleanup(curl);
			return failResult(code, status, "request failed");
		}

		const int rtt = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
		result.attempts_ms.push_back(rtt);
		if (result.min_ms == -1 || rtt < result.min_ms)
			result.min_ms = rtt;
		result.status = (int)status;
	}

	curl_easy_cleanup(curl);

	result.ok = result.min_ms >= 0;
	return result;
}

#endif
