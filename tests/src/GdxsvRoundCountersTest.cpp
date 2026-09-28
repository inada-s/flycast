#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "emulator.h"
#include "gdxsv/gdxsv_round_counters.h"

class GdxsvRoundCountersTest : public ::testing::Test {
protected:
	void SetUp() override {
		ASSERT_TRUE(addrspace::reserve());
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		SetSession(0x0c392540);
		for (int outcome : {1, gdxsv_round_counters::kDraw, 2})
			log_.add_round_data()->set_win_team(outcome);
	}
	void SetSession(u32 session) {
		gdxsv_WriteMem32(gdxsv_round_counters::kSessionPointer, session);
		const int teams[] = {1, 2, 1, 0};
		for (int i = 0; i < 4; ++i)
			gdxsv_WriteMem8(session + (i + 1) * 0x2c0 + 0x2ba, teams[i]);
	}
	u32 Address(int slot) {
		return gdxsv_ReadMem32(gdxsv_round_counters::kSessionPointer) + (slot + 1) * 0x2c0 + 0x2b0;
	}
	std::array<u16, 4> Read(int slot) {
		std::array<u16, 4> values;
		for (int i = 0; i < 4; ++i)
			values[i] = gdxsv_ReadMem16(Address(slot) + i * 2);
		return values;
	}
	proto::BattleLogFile log_;
};

TEST_F(GdxsvRoundCountersTest, NormalizesTimeoutWithoutChangingNormalWinners) {
	EXPECT_EQ(1, gdxsv_round_counters::Outcome(1, 0));
	EXPECT_EQ(2, gdxsv_round_counters::Outcome(2, 0));
	EXPECT_EQ(0, gdxsv_round_counters::Outcome(0, 0));
	EXPECT_EQ(0, gdxsv_round_counters::Outcome(0, 1));
	EXPECT_EQ(-1, gdxsv_round_counters::Outcome(1, 1));
	EXPECT_EQ(-1, gdxsv_round_counters::Outcome(2, 1));
	proto::BattleLogRound round, parsed;
	round.set_win_team(-1);
	ASSERT_TRUE(parsed.ParseFromString(round.SerializeAsString()));
	EXPECT_EQ(-1, parsed.win_team()); // Existing int32 protobuf schema.
}

TEST_F(GdxsvRoundCountersTest, RoundJumpRestoresAllSlotsAndSeparatesDraws) {
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_, 2));
	EXPECT_EQ((std::array<u16, 4>{2, 1, 0, 1}), Read(0));
	EXPECT_EQ((std::array<u16, 4>{2, 0, 1, 1}), Read(1));
	EXPECT_EQ(Read(0), Read(2));
	EXPECT_EQ((std::array<u16, 4>{2, 0, 0, 1}), Read(3));
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_, 3));
	EXPECT_EQ((std::array<u16, 4>{3, 1, 1, 1}), Read(0));
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_, 0));
	EXPECT_EQ((std::array<u16, 4>{0, 0, 0, 0}), Read(0));
}

TEST_F(GdxsvRoundCountersTest, RejectsInvalidSessionPointersWithoutChangingRam) {
	constexpr u32 ramEnd = 0x0d000000;
	constexpr u32 recordSpan = (gdxsv_round_counters::kPlayerSlots + 1) * gdxsv_round_counters::kPlayerStride;
	for (u32 session : {0u, 0x0bfffffcu, ramEnd - recordSpan + 4,
			ramEnd - recordSpan + 8, ramEnd, 0xfffffff0u}) {
		SCOPED_TRACE(session);
		// Change only the pointer; invalid targets must never be initialized or used.
		gdxsv_WriteMem32(gdxsv_round_counters::kSessionPointer, session);
		const std::vector<u8> before(&mem_b[0], &mem_b[0] + RAM_SIZE);
		for (int completed : {-1, 2}) {
			SCOPED_TRACE(completed);
			EXPECT_FALSE(gdxsv_round_counters::Restore(log_, completed));
			EXPECT_EQ(0, std::memcmp(before.data(), &mem_b[0], before.size()));
		}
	}
}

TEST_F(GdxsvRoundCountersTest, RestoresAllSlotsAtMainRamBoundaries) {
	constexpr u32 recordSpan = (gdxsv_round_counters::kPlayerSlots + 1) * gdxsv_round_counters::kPlayerStride;
	for (u32 session : {0x0c000000u, 0x0d000000u - recordSpan}) {
		SCOPED_TRACE(session);
		SetSession(session);
		ASSERT_TRUE(gdxsv_round_counters::Restore(log_, 2));
		EXPECT_EQ((std::array<u16, 4>{2, 1, 0, 1}), Read(0));
		EXPECT_EQ((std::array<u16, 4>{2, 0, 1, 1}), Read(1));
		EXPECT_EQ(Read(0), Read(2));
		EXPECT_EQ((std::array<u16, 4>{2, 0, 0, 1}), Read(3));
		ASSERT_TRUE(gdxsv_round_counters::Restore(log_));
	}
}

TEST_F(GdxsvRoundCountersTest, MissingResultsWaitThenRestoreAndLateDrawCorrects) {
	log_.mutable_round_data(0)->set_win_team(0);
	gdxsv_WriteMem16(Address(0) + 2, 7);
	EXPECT_FALSE(gdxsv_round_counters::Restore(log_, 1));
	EXPECT_EQ((std::array<u16, 4>{1, 7, 0, 0}), Read(0));
	log_.mutable_round_data(0)->set_win_team(2);
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_));
	EXPECT_EQ((std::array<u16, 4>{1, 0, 1, 0}), Read(0));
	log_.mutable_round_data(0)->set_win_team(-1); // Opposite legacy report arrives late.
	for (int i = 0; i < 3; ++i) {
		ASSERT_TRUE(gdxsv_round_counters::Restore(log_));
		EXPECT_EQ((std::array<u16, 4>{1, 0, 0, 1}), Read(0));
		EXPECT_EQ((std::array<u16, 4>{1, 0, 0, 1}), Read(1));
	}
	log_.mutable_round_data(0)->set_win_team(99);
	EXPECT_FALSE(gdxsv_round_counters::Restore(log_));
	EXPECT_EQ((std::array<u16, 4>{1, 0, 0, 1}), Read(0));
}

TEST_F(GdxsvRoundCountersTest, UsesGuestCompletedCountAfterAccountingAndBackwardLoad) {
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_, 2));
	// Simulate the next round's accounting, including a skipped result card.
	for (int i = 0; i < 4; ++i)
		gdxsv_WriteMem16(Address(i), 3);
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_));
	EXPECT_EQ((std::array<u16, 4>{3, 1, 1, 1}), Read(0));
	// A backward state can restore an obsolete outcome and a different pointer.
	SetSession(0x0c380000);
	for (int i = 0; i < 4; ++i) {
		gdxsv_WriteMem16(Address(i), 1);
		gdxsv_WriteMem16(Address(i) + 2, 99);
		gdxsv_WriteMem16(Address(i) - 0x14, 999); // Lifetime field: not ours.
	}
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_));
	EXPECT_EQ((std::array<u16, 4>{1, 1, 0, 0}), Read(0));
	EXPECT_EQ(999, gdxsv_ReadMem16(Address(0) - 0x14));
	EXPECT_EQ(0, gdxsv_ReadMem16(0x0c380000 + 0x2b0)); // Record zero untouched.
	const auto before = Read(0);
	EXPECT_FALSE(gdxsv_round_counters::Restore(log_, 11));
	EXPECT_EQ(before, Read(0));
}

TEST_F(GdxsvRoundCountersTest, Disc1UsesItsOwnSessionPointer) {
	const u32 session = 0x0c2f6e00;
	gdxsv_WriteMem32(gdxsv_round_counters::kSessionPointerDisk1, session);
	gdxsv_WriteMem8(session + 0x2c0 + 0x2ba, 1);
	ASSERT_TRUE(gdxsv_round_counters::Restore(log_, 2, 1));
	const u32 addr = session + 0x2c0 + 0x2b0;
	EXPECT_EQ((std::array<u16, 4>{2, 1, 0, 1}),
		(std::array<u16, 4>{gdxsv_ReadMem16(addr), gdxsv_ReadMem16(addr + 2), gdxsv_ReadMem16(addr + 4), gdxsv_ReadMem16(addr + 6)}));
	EXPECT_EQ((std::array<u16, 4>{0, 0, 0, 0}), Read(0)); // Disc 2 records untouched.
}
