#pragma once
#include <array>
#include <future>
#include <mutex>
#include <set>

#include "gdxsv_network.h"
#include "network/ggpo.h"
#include "gdxsv_spectator_uplink.h"
#include "lbs_message.h"

class GdxsvBackendRollback {
   public:
	enum class State {
		None,
		StartLocalTest,
		LbsStartBattleFlow,
		McsWaitJoin,

		McsSessionExchange,
		StopEmulator,
		WaitPingPong,
		StartGGPOSession,
		WaitGGPOSession,
		McsInBattle,
		CloseWait,
		End,
		Closed,
	};

	void DisplayOSD();
	void Reset();
	void OnMainUiLoop();
	void OnNextFrame();
	bool StartLocalTest(const char *param);
	void Prepare(const proto::P2PMatching &matching, int port);
	void Open();
	void Close();
	u32 OnSockWrite(u32 addr, u32 size);
	u32 OnSockRead(u32 addr, u32 size);
	u32 OnSockPoll();
	bool SetCloseReason(const char *reason);
	void SaveReplay() const;
	const proto::P2PMatching &GetMatching() { return matching_; }
	const proto::P2PMatchingReport &GetReport() { return report_; }
	const std::vector<proto::BattleLogRound> &GetRoundData() { return round_data_; }
	void ClearReport() { report_.Clear(); }
	void ToggleNetworkStat() { osd_network_stat_ = !osd_network_stat_; }

   private:
	void ApplyPatch(bool first_time);
	void RestorePatch();
	void ProcessLbsMessage();
	void ResetGgpoGameRendererState();
	// Pushes locally-settled (input_logs_/start_msg_indexes_/start_msg_randoms_/
	// round_data_) entries to spectator_uplink_ once GGPO's confirmed frame has
	// passed them - see the call site in OnSockWrite for why this can't just
	// happen inline where those vectors are written (they're written on every
	// speculative/rollback simulation pass, not just the final settled one).
	void FlushConfirmedToSpectatorUplink();
	// Highest frame GGPO can no longer roll back, or -1 without a session.
	int ConfirmedFrame();
	void UpdateNetworkStatSnapshot();

	State state_ = State::None;
	bool is_local_test_ = false;
	bool error_fast_return_ = false;
	bool ggpo_game_renderer_reset_ = false;
	bool osd_network_stat_ = false;
	int osd_network_stat_countdown_ = 0;
	int start_button_counter_ = 0;
	int recv_delay_ = 0;
	int port_ = 0;
	std::deque<u8> recv_buf_;
	// GGPO frames whose battle input the slowdown dropped (the next frame delivers instead).
	std::set<int> slowdown_stall_frames_;
	int slowdown_last_frame_ = -1;
	LbsMessageReader lbs_tx_reader_;
	proto::P2PMatching matching_;
	proto::P2PMatchingReport report_;
	UdpPingPong ping_pong_;
	GdxsvSpectatorUplink spectator_uplink_;
	std::future<bool> start_network_;

	int64_t start_at_ = 0;
	std::vector<std::pair<int, u64>> input_logs_;
	std::vector<std::pair<int, int>> start_msg_indexes_;
	std::vector<std::pair<int, u32>> start_msg_randoms_;
	std::vector<proto::BattleLogRound> round_data_;

	// Watermarks/queue for FlushConfirmedToSpectatorUplink - see its doc above.
	int32_t spectator_flushed_inputs_ = 0;
	int32_t spectator_flushed_round_events_ = 0;
	std::vector<std::pair<int, int32_t>> pending_spectator_round_results_;

	// First frame the re-battle-cancel scene was seen on, or -1. The match is closed once this frame is confirmed.
	int battle_end_frame_ = -1;
	int disconnect_frame_ = 0;

	// GGPO_TEST_LOG: log scene changes for tools/rbk_test.
	bool test_log_ = false;
	int last_scene_ = -1;

	// Network stats copied on the emulation thread for the OSD. The UI thread must not call into GGPO: the emu
	// thread can hold the GGPO lock for seconds (prediction barrier while a peer drops), freezing the window.
	std::mutex net_stat_mutex_;
	std::array<ggpo::NetworkStats, 4> net_stats_{};
	std::array<bool, 4> net_connected_{};
};
