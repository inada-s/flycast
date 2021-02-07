#pragma once

#include <string>
#include <deque>
#include <chrono>
#include <atomic>

#include "gdxsv_network.h"
#include "types.h"
#include "cfg/cfg.h"
#include "hw/sh4/sh4_mem.h"
#include "reios/reios.h"

class logFile;

class Gdxsv {
public:
    Gdxsv() = default;

    ~Gdxsv();

    bool Enabled() const;

    bool InGame() const;

    void Reset();

    void Update();

    void SyncNetwork(bool write);

    bool UpdateAvailable();

    void OpenDownloadPage();

    void DismissUpdateDialog();

    std::string LatestVersion();

    std::atomic<int> replay_mode;
    std::atomic<int> replay_state;
    int replay_seq;
private:
    void GcpPingTest(); // run on network thread

    void UpdateNetwork(); // run on network thread

    static std::string GenerateLoginKey();

    std::vector<u8> GeneratePlatformInfoPacket();

    std::string GeneratePlatformInfoString();

    void WritePatch();

    void WritePatchDisk1();

    void WritePatchDisk2();

    void CloseUdpClientWithReason(const char *reason);

    void StartReplay();

    std::atomic<bool> enabled;
    std::atomic<int> disk;
    std::atomic<u8> maxlag;

    std::string server;
    std::string loginkey;
    std::string user_id;
    std::string session_id;
    std::map<std::string, u32> symbols;

    std::atomic<bool> net_terminate;
    std::atomic<bool> start_udp_session;
    std::thread net_thread;
    std::mutex send_buf_mtx;
    std::deque<u8> send_buf;
    std::mutex recv_buf_mtx;
    std::deque<u8> recv_buf;

    TcpClient tcp_client;
    UdpClient udp_client;

    std::atomic<bool> gcp_ping_test_finished;
    std::map<std::string, int> gcp_ping_test_result;

    void handleReleaseJSON(const std::string &json);

    bool update_available = false;
    std::string latest_version;

};

extern Gdxsv gdxsv;
