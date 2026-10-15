// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "network/miniupnp.h"

// MiniUPnP plus the queries the P2P status check and its report use.
class GdxsvUPnP : public MiniUPnP {
   public:
	// True if the router maps the port to this host.
	bool CheckPortMapping(int port, bool tcp);
	const char* LocalAddress() const;
	// Not queried from the router yet: always empty.
	const char* ExternalAddress() const { return ""; }
};
