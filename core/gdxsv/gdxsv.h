#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include "gdxsv.pb.h"
#include "gdxsv_backend_replay.h"
#include "gdxsv_backend_rollback.h"
#include "gdxsv_backend_tcp.h"
#include "gdxsv_backend_udp.h"
#include "gdxsv_key_display.h"
#include "gdxsv_lobby_status.h"
#include "gdxsv_projectile_view.h"
#include "gdxsv_slowdown.h"
#include "network/miniupnp.h"
#include "types.h"

class Gdxsv {
   public:
	friend GdxsvBackendReplay;
	friend GdxsvBackendRollback;
	friend GdxsvBackendTcp;
	friend GdxsvBackendUdp;

	enum class NetMode {
		Offline,
		Lbs,
		McsUdp,
		McsRollback,
		Replay,
	};

	bool Enabled() const;
	bool InGame() const;
	bool InBattlePlay() const;
	bool IsOnline() const;
	bool IsSaveStateAllowed() const;
	bool IsReplaying() const;
	bool IsReplaySeeking() const;
	void DisplayOSD();
	const char* NetModeString() const;
	// Loading a savestate keeps a suspended lobby connection; any other reset closes it.
	void Reset(bool loading_state = false);
	bool HookOpenMenu();
	void HookVBlank();
	void HookMainUiLoop();
	void HookEndOfFrame();
	void HookNextFrame();
	void HandleRPC();
	void RestoreOnlinePatch();
	void StartPingTest();
	std::string PingResult() const { return pingResult; }
	void SetPingResult(const std::string& result) { pingResult = result; }
	std::string P2PStatus() const { return p2pStatus; }
	void SetP2PStatus(const std::string& status) { p2pStatus = status; }
	void StartP2PFeasibilityTest();
	std::shared_future<P2PFeasibility> P2PFeasibilityResult() const { return p2p_feasibility_result_; }
	void FetchPublicIP();
	// Ends the ping, public IP and P2P tests and waits for them. Called on exit.
	void StopNetworkTests();
	bool NetworkTestsStopped() const { return network_tests_stopped_; }
	std::shared_future<std::pair<bool, std::string>> PublicIPv4() const { return public_ipv4_; }
	std::shared_future<std::pair<bool, std::string>> PublicIPv6() const { return public_ipv6_; }
	void NotifyWanPort() const;
	bool StartReplayFile(const char* path, int pov);
	bool StartReplayBuffer(const std::vector<u8>& buf, int pov);
	bool StartLiveSpectate(const char* battle_code, int pov);
	bool StartLiveFromHost(int pov);
	void StopReplay();
	// A replay can be watched from the lobby, also while waiting for a match:
	// the connection stays open and what the server sends is held for the game.
	bool CanSuspendLobby() const;
	void SuspendLobby();
	void ResumeLobby();
	bool LobbySuspended() const { return lobby_suspended_; }
	// A match was found, or the server shut down, while the lobby was suspended.
	bool LobbyCalling() const { return lobby_calling_; }
	// The lobby status overlay, for screens drawn while the GUI is open.
	void DisplayLobbyStatus() { lobby_status_.DisplayOSD(); }
	bool StartRollbackTest(const char* param);
	void WritePatch();
	int Disk() const { return disk_; }
	void InvalidateWidescreenPatch() { widescreen_viewport_width_ = 0; }
	std::string UserId() const { return user_id_; }
	MiniUPnP& UPnP() { return upnp_; }

   private:
	static std::string GenerateLoginKey();
	std::vector<u8> GeneratePlatformInfoPacket();
	std::vector<u8> GenerateP2PMatchReportPacket();
	void ApplyOnlinePatch(bool first_time);
	void WritePatchDisk1();
	void WritePatchDisk2();
	void WriteWidescreenPatch();
	void WriteWarningSoundVolume();
	bool WidescreenSitesKnown();
	void PrepareP2PMatching(const LbsMessage& lbs_msg);
	void HoldLobbyMessage(const LbsMessage& lbs_msg);

	NetMode netmode_ = NetMode::Offline;
	std::atomic<bool> enabled_;
	std::atomic<int> disk_;
	std::atomic<int> maxlag_;
	std::atomic<int> maxrebattle_;
	std::atomic<bool> network_tests_stopped_{false};
	std::string user_id_;
	std::string pingResult = "Latency check...";
	std::string p2pStatus;
	std::map<std::string, u32> symbols_;
	proto::GamePatchList patch_list_;
	bool going_to_battle_ = false;
	std::atomic<bool> lobby_suspended_{false};
	std::atomic<bool> lobby_calling_{false};
	std::mutex lobby_held_mutex_;
	std::deque<LbsMessage> lobby_held_;
	// The savestate restores the original code; these are applied again on resume.
	proto::GamePatchList lobby_patch_list_;
	// A zero width invalidates the last applied enable state, viewport and settings.
	bool widescreen_enabled_ = false;
	int widescreen_viewport_width_ = 0;
	int widescreen_viewport_height_ = 0;
	bool widescreen_super_ = false;
	int widescreen_hud_layout_ = 0;

	std::shared_future<std::map<std::string, int>> gcp_ping_test_result_;
	std::shared_future<std::pair<bool, std::string>> public_ipv4_, public_ipv6_;
	std::shared_future<P2PFeasibility> p2p_feasibility_result_;

	MiniUPnP upnp_;
	GdxsvBackendTcp lbs_net_;
	GdxsvBackendUdp udp_net_;
	GdxsvBackendReplay replay_net_;
	GdxsvBackendRollback rollback_net_;
	GdxsvKeyDisplay key_display_;
	GdxsvLobbyStatus lobby_status_;
	GdxsvProjectileView projectile_view_;
	GdxsvSlowdown slowdown_;
};

extern Gdxsv gdxsv;
