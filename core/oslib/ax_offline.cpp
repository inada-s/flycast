// ai-analysis: outbound network kill switch. See ax_offline.h for the switch and the gated sites.
#include "ax_offline.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include "types.h"

namespace axoffline
{
static bool on;
static bool onInit;
static std::mutex reportedMutex;
static std::set<std::string> reported;

bool enabled()
{
	if (!onInit)
	{
		const char *e = getenv("AX_OFFLINE");
		on = e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
		onInit = true;
		if (on)
			NOTICE_LOG(COMMON, "AX_OFFLINE: outbound network disabled (loopback only)");
	}
	return on;
}

// host of "scheme://[user@]host[:port]/path" (or of a bare "host[:port]")
static std::string urlHost(const std::string &url)
{
	size_t start = url.find("://");
	start = start == std::string::npos ? 0 : start + 3;
	size_t end = url.find_first_of("/?#", start);
	std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
	size_t at = authority.rfind('@');
	if (at != std::string::npos)
		authority = authority.substr(at + 1);
	if (!authority.empty() && authority[0] == '[')
	{	// [v6addr]:port
		size_t close = authority.find(']');
		return close == std::string::npos ? authority.substr(1) : authority.substr(1, close - 1);
	}
	size_t colon = authority.rfind(':');
	return colon == std::string::npos ? authority : authority.substr(0, colon);
}

bool hostAllowed(const std::string &host)
{
	if (!enabled())
		return true;
	if (host.empty())
		return false;
	if (host == "localhost" || host == "::1" || host == "[::1]" || host == "0.0.0.0")
		return true;
	// 127.0.0.0/8 in dotted form
	if (host.compare(0, 4, "127.") == 0)
		return true;
	return false;
}

bool blockHost(const std::string &host, int port)
{
	if (hostAllowed(host))
		return false;
	bool first;
	{
		std::lock_guard<std::mutex> lock(reportedMutex);
		first = reported.insert(host).second;
	}
	if (first)
		WARN_LOG(COMMON, "AX_OFFLINE: refused connection to %s:%d", host.c_str(), port);
	return true;
}

bool blockUrl(const std::string &url)
{
	if (!enabled())
		return false;
	return blockHost(urlHost(url), 0);
}
}  // namespace axoffline
