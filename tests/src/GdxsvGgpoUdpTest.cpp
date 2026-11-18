// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <thread>

#include "gtest/gtest.h"
#include "ggpo_types.h"
#include "network/udp.h"

namespace {
struct ThrowOnFirst : Udp::Callbacks {
	int calls = 0;
	int last_magic = -1;
	void OnMsg(sockaddr_storage &, UdpMsg *msg, int) override {
		calls++;
		last_magic = msg->hdr.magic;
		if (msg->hdr.magic == 1) throw GGPOException("test", GGPO_ERRORCODE_GENERAL_FAILURE);
	}
};
}  // namespace

TEST(GdxsvGgpoUdpTest, PacketWhoseHandlerThrewIsNotHandledAgain) {
#ifdef _WIN32
	WSADATA wsa;
	ASSERT_EQ(0, WSAStartup(MAKEWORD(2, 2), &wsa));
#endif
	constexpr uint16 port = 47121;
	Poll poll;
	ThrowOnFirst cb;
	Udp udp;
	udp.Init(port, &poll, &cb);

	SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_NE(INVALID_SOCKET, s);
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	for (uint16 magic = 1; magic <= 2; magic++) {
		UdpMsg msg(UdpMsg::KeepAlive);
		msg.hdr.magic = magic;
		sendto(s, (const char *)&msg, (int)sizeof(msg.hdr), 0, (sockaddr *)&to, sizeof(to));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	int threw = 0;
	for (int i = 0; i < 3; i++) {
		try {
			udp.OnLoopPoll(nullptr);
		} catch (const GGPOException &) {
			threw++;
		}
	}
	closesocket(s);
	EXPECT_EQ(1, threw);
	EXPECT_EQ(2, cb.calls);
	EXPECT_EQ(2, cb.last_magic);
}
