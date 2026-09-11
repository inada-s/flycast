#pragma once

#include <atomic>
#include <string>

#include "gdxsv.pb.h"
#include "gdxsv_backend_replay.h"
#include "gdxsv_backend_rollback.h"
#include "gdxsv_backend_tcp.h"
#include "gdxsv_backend_udp.h"
#include "gdxsv_key_display.h"
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
	bool IsOnline() const;
	bool IsSaveStateAllowed() const;
	bool IsReplaying() const;
	void DisplayOSD();
	const char* NetModeString() const;
	void Reset();
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
	std::shared_future<std::pair<bool, std::string>> PublicIPv4() const { return public_ipv4_; }
	std::shared_future<std::pair<bool, std::string>> PublicIPv6() const { return public_ipv6_; }
	void NotifyWanPort() const;
	bool StartReplayFile(const char* path, int pov);
	bool StartLiveSpectate(const char* battle_code, int pov);
	void StopReplay();
	bool StartRollbackTest(const char* param);
	void WritePatch();
	int Disk() const { return disk_; }
	bool WidescreenPatchEnabled() const { return enabled_ && widescreen_patch_enabled_; }
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
	void WriteWidescreenPatchDisk2();

	NetMode netmode_ = NetMode::Offline;
	std::atomic<bool> enabled_;
	std::atomic<int> disk_;
	std::atomic<int> maxlag_;
	std::atomic<int> maxrebattle_;
	std::string user_id_;
	std::string pingResult = "Latency check...";
	std::string p2pStatus;
	std::map<std::string, u32> symbols_;
	proto::GamePatchList patch_list_;
	bool going_to_battle_ = false;
	// Enabling the hack requires a restart; aspect/HUD placement update per frame.
	bool widescreen_patch_enabled_ = false;
	// A zero width invalidates the last successfully applied viewport/settings.
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
};

extern Gdxsv gdxsv;
