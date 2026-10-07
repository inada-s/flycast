#include "gtest/gtest.h"
#include "gdxsv/gdxsv_network.h"

namespace {
constexpr int N = UdpPingPong::N;
constexpr int R = UdpPingPong::MAX_RELAYS;
}  // namespace

// Peer 1 is behind a symmetric NAT: peer 0 never hears from it, only from peer 3, which carries peer 1's row.
TEST(GdxsvPingPongTest, MergeRelayRttTakesRowsOfPeersNotReachedDirectly) {
	uint8_t mine[N][R] = {{18, 67}};
	const uint8_t from3[N][R] = {{17, 66}, {4, 55}, {60, 5}, {53, 4}};
	UdpPingPong::MergeRelayRtt(mine, from3, 0, 3);
	EXPECT_EQ(18, mine[0][0]);  // our own row stays as we measured it
	EXPECT_EQ(67, mine[0][1]);
	EXPECT_EQ(4, mine[1][0]);
	EXPECT_EQ(55, mine[1][1]);
	EXPECT_EQ(60, mine[2][0]);
	EXPECT_EQ(53, mine[3][0]);
}

TEST(GdxsvPingPongTest, MergeRelayRttKeepsKnownRowsAgainstUnknown) {
	uint8_t mine[N][R] = {{18}, {4}, {60}, {53}};
	const uint8_t from2[N][R] = {{0}, {0}, {61}, {0}};
	UdpPingPong::MergeRelayRtt(mine, from2, 0, 2);
	EXPECT_EQ(4, mine[1][0]);  // peer 2 does not know peer 1: keep what peer 3 told us
	EXPECT_EQ(61, mine[2][0]);
	EXPECT_EQ(53, mine[3][0]);
}

TEST(GdxsvPingPongTest, MergeRelayRttTakesSendersOwnRowAsIs) {
	uint8_t mine[N][R] = {{18}, {0}, {0}, {53}};
	const uint8_t from3[N][R] = {{0}, {0}, {0}, {0}};
	UdpPingPong::MergeRelayRtt(mine, from3, 0, 3);
	EXPECT_EQ(0, mine[3][0]);  // the sender lost its relay
	EXPECT_EQ(18, mine[0][0]);
}
