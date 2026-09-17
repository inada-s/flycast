// ai-analysis: outbound network kill switch. Off unless switched on.
//
// Switch: env AX_OFFLINE=1. When on, every outbound connection whose host is not a loopback
// address is refused before any DNS lookup or socket call, so a run can never reach a live
// service (gdxsv production, the replay uploader, the GCP region pings, STUN, ipify, ...).
// A local server on 127.0.0.1 / localhost still works, which is what scripted lobby and battle
// experiments use. A normal run of flycast (env unset) executes none of this.
//
// Gated call sites: http::get / http::post (core/oslib/http_client.cpp, both the WinINet and the
// curl implementation), measureGdxsvHttpsLatency (all three platform implementations),
// gcp_ping_test / test_p2p_feasibility / get_public_ip_address, TcpClient::Connect and
// UdpRemote::Open (core/gdxsv/gdxsv_network.cpp).
#pragma once
#include <string>

namespace axoffline
{
// true when switched on by env (fixed at first call)
bool enabled();
// true when a connection to this host may proceed (always true when the switch is off)
bool hostAllowed(const std::string &host);
// true when the request must be refused; logs the first refusal per host
bool blockHost(const std::string &host, int port);
// same, for a full URL ("https://host[:port]/path")
bool blockUrl(const std::string &url);
}  // namespace axoffline
