#include "gdxsv_https_latency.h"

#if defined(_WIN32) && !defined(TARGET_UWP)

#include <chrono>

#include <windows.h>
#include <wininet.h>

#include "oslib/ax_offline.h"
#include "oslib/http_client.h"

namespace {

static std::string wininetError(const char *prefix, DWORD error)
{
	return std::string(prefix) + ": " + std::to_string(error);
}

static bool queryStatus(HINTERNET request, int& status)
{
	DWORD value = 0;
	DWORD size = sizeof(value);
	DWORD index = 0;
	if (!HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &value, &size, &index))
		return false;
	status = (int)value;
	return true;
}

static bool performHead(HINTERNET connection, const std::string& path, int& status, std::string& error)
{
	const char *accept_types[] = {"*/*", nullptr};
	HINTERNET request = HttpOpenRequestA(connection, "HEAD", path.c_str(), nullptr, nullptr, accept_types,
		INTERNET_FLAG_SECURE | INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_CACHE_WRITE
			| INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD,
		0);
	if (request == nullptr)
	{
		error = wininetError("HttpOpenRequest failed", GetLastError());
		return false;
	}

	DWORD timeout = 10000;
	InternetSetOptionA(request, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
	InternetSetOptionA(request, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

	bool ok = true;
	if (!HttpSendRequestA(request, nullptr, 0, nullptr, 0))
	{
		error = wininetError("HttpSendRequest failed", GetLastError());
		ok = false;
	}
	else if (!queryStatus(request, status))
	{
		error = wininetError("HttpQueryInfo failed", GetLastError());
		ok = false;
	}

	InternetCloseHandle(request);
	return ok;
}

} // namespace

GdxsvHttpsLatencyResult measureGdxsvHttpsLatency(const std::string& host, const std::string& path, int attempts)
{
	GdxsvHttpsLatencyResult result;
	if (axoffline::blockHost(host, 443))
		return result;

	HINTERNET internet = InternetOpenA(http::getUserAgent().c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (internet == nullptr)
	{
		result.error = wininetError("InternetOpen failed", GetLastError());
		return result;
	}

	DWORD timeout = 5000;
	InternetSetOptionA(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));

	HINTERNET connection = InternetConnectA(internet, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr,
		INTERNET_SERVICE_HTTP, 0, 0);
	if (connection == nullptr)
	{
		result.error = wininetError("InternetConnect failed", GetLastError());
		InternetCloseHandle(internet);
		return result;
	}

	int status = 0;
	std::string error;
	if (!performHead(connection, path, status, error) || !http::success(status))
	{
		result.status = status;
		result.error = error.empty() ? "warmup failed" : error;
		InternetCloseHandle(connection);
		InternetCloseHandle(internet);
		return result;
	}

	for (int i = 0; i < attempts; i++)
	{
		auto t1 = std::chrono::high_resolution_clock::now();
		status = 0;
		error.clear();
		const bool ok = performHead(connection, path, status, error);
		auto t2 = std::chrono::high_resolution_clock::now();

		if (!ok || !http::success(status))
		{
			result.status = status;
			result.error = error.empty() ? "request failed" : error;
			InternetCloseHandle(connection);
			InternetCloseHandle(internet);
			return result;
		}

		const int rtt = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
		result.attempts_ms.push_back(rtt);
		if (result.min_ms == -1 || rtt < result.min_ms)
			result.min_ms = rtt;
		result.status = status;
	}

	InternetCloseHandle(connection);
	InternetCloseHandle(internet);

	result.ok = result.min_ms >= 0;
	return result;
}

#endif
