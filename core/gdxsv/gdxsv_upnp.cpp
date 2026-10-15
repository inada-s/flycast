// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_upnp.h"

#ifndef FEAT_NO_MINIUPNPC
#include <upnpcommands.h>

#include <cstring>
#include <string>

bool GdxsvUPnP::CheckPortMapping(int port, bool tcp) {
	if (!initialized && !Init()) return false;

	char internalClient[40];
	char internalPort[10];
	char duration[10];
	char description[80];
	char enabled[10];

	int error = UPNP_GetSpecificPortMappingEntry(urls.controlURL, data.first.servicetype, std::to_string(port).c_str(), tcp ? "TCP" : "UDP",
												 nullptr, internalClient, internalPort, description, enabled, duration);
	return error == 0 && strcmp(internalClient, lanAddress) == 0;
}

const char* GdxsvUPnP::LocalAddress() const { return lanAddress; }

#else

bool GdxsvUPnP::CheckPortMapping(int port, bool tcp) { return false; }

const char* GdxsvUPnP::LocalAddress() const { return ""; }

#endif
