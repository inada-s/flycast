#pragma once

#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "gdxsv.pb.h"
#include "network/net_platform.h"
#include "types.h"

enum class P2PStatus {
	Testing,
	Optimal,    // Direct / UPnP / Full Cone
	Fair,       // Restricted Cone (Hole Punching)
	Poor,       // Symmetric NAT (Relay likely)
	Blocked     // UDP blocked / Error
};

struct P2PFeasibility {
	P2PStatus status_code = P2PStatus::Testing;
	std::string status;
	std::string description;
	std::string port_test_v4;
	std::string upnp_result;
	uint32_t color; // ABGR
};

std::future<P2PFeasibility> test_p2p_feasibility(int port);
std::future<std::pair<bool, std::string>> get_public_ip_address(bool ipv6);
std::future<std::map<std::string, int>> gcp_ping_test();
int get_random_port_number();
std::string sockaddr_to_string(const sockaddr *addr);
bool is_loopback_addr(const sockaddr *addr);
bool is_private_addr(const sockaddr *addr);
bool is_same_addr(const sockaddr *addr1, const sockaddr *addr2);
std::string mask_ip_address(std::string addr);

class TcpClient {
   public:
	~TcpClient() { Close(); }
	bool Connect(const char *host, int port);
	void SetNonBlocking();
	int IsConnected() const;
	int Recv(char *buf, int len);
	int Send(const char *buf, int len);
	void Close();
	u32 ReadableSize() const;
	const std::string &host() const { return host_; }
	const std::string &local_ip() const { return local_ip_; }
	int port() const { return port_; }

   private:
	sock_t sock_ = INVALID_SOCKET;
	std::string host_;
	std::string local_ip_;
	int port_ = 0;
};

class MessageBuffer {
   public:
	static const int kBufSize = 50;
	MessageBuffer();
	void SessionId(const std::string &session_id);
	bool CanPush() const;
	bool PushBattleMessage(const std::string &user_id, u8 *body, u32 body_length);
	const proto::Packet &Packet();
	void ApplySeqAck(u32 seq, u32 ack);
	void Clear();

   private:
	u32 msg_seq_;
	u32 snd_seq_;
	proto::Packet packet_;
};

class MessageFilter {
   public:
	bool IsNextMessage(const proto::BattleMessage &msg);
	void Clear();

   private:
	std::map<std::string, u32> recv_seq;
};

class UdpRemote {
   public:
	enum class IpPref { Any, V4Only, V6Only };
	bool Open(const char *host, int port, IpPref pref = IpPref::Any);
	bool Open(const std::string &ip_port);
	bool Open(const sockaddr *addr, socklen_t addrlen);
	void Close();
	bool is_open() const { return 0 < net_addr_len_; }
	bool is_v6() const { return 0 < net_addr_len_ && net_addr_.ss_family == AF_INET6; }
	std::string str_addr() const { return sockaddr_to_string(reinterpret_cast<const sockaddr *>(&net_addr_)); }
	std::string masked_addr() const { return mask_ip_address(str_addr()); }
	const sockaddr *net_addr() const { return (sockaddr *)&net_addr_; }
	const size_t net_addr_len() const { return net_addr_len_; }

   private:
	sockaddr_storage net_addr_{};
	size_t net_addr_len_ = 0;
};

class UdpClient {
   public:
	~UdpClient() { Close(); }
	bool Bind(int port);
	bool Initialized() const;
	int RecvFrom(char *buf, int len, sockaddr_storage *from_addr, socklen_t *addrlen);
	int SendTo(const char *buf, int len, const UdpRemote &remote);
	void Close();
	int bound_port() const { return bound_port_; }

   private:
	sock_t sock_v4_ = INVALID_SOCKET;
	sock_t sock_v6_ = INVALID_SOCKET;
	int bound_port_ = 0;
};

class UdpPingPong {
   public:
	static const int N = 4;
	static const int MAX_RELAYS = 4;
	void Start(uint32_t session_id, uint8_t peer_id, int port, int duration_ms);
	void Stop();
	void Reset();
	bool Running() const;
	int ElapsedMs() const;
	void AddCandidate(const std::string &user_id, uint8_t peer_id, const std::string &ip, int port);
	// A relay is pinged over IPv4 and IPv6 (ip6 may be empty); the faster family that answers is used.
	void AddRelay(const std::string &ip, const std::string &ip6, int port, uint64_t token);
	bool GetAvailableAddress(uint8_t peer_id, sockaddr_storage *dst, float *rtt);
	// use: the relay's address in the faster family that answered, false if neither did.
	// alt: its address in the other family, if any (ss_family 0 otherwise).
	bool GetRelayAddress(int relay_idx, sockaddr_storage *use, sockaddr_storage *alt);
	void GetRttMatrix(uint8_t matrix[N][N]);
	// matrix[peer][relay] is the RTT from the peer to the relay, 0 when unknown.
	void GetRelayRttMatrix(uint8_t matrix[N][MAX_RELAYS]);
	int RelayCount();
	void PrintRttMatrix();
	void DebugUnreachable(uint8_t peer_id, uint8_t remote_peer_id);
	void DebugSetRtt(uint8_t peer_id, uint8_t remote_peer_id, uint8_t rtt);
	void DebugSetRelayRtt(uint8_t peer_id, int relay_idx, uint8_t rtt);

   private:
	static const uint32_t MAGIC = 2205246188;
	static const uint8_t PING = 1;
	static const uint8_t PONG = 2;
	// Relay pings share gdxsv relay.go's format.
	static const uint32_t RELAY_MAGIC = 0x594c4552;
	static const uint8_t RELAY_PING = 1;
	static const uint8_t RELAY_PONG = 2;

	struct Candidate {
		uint8_t peer_id;
		UdpRemote remote;
		int ping_count;
		int pong_count;
		float rtt;
		std::vector<int> rtt_samples;
	};

	struct RelayPath {
		UdpRemote remote;
		int ping_count;
		int pong_count;
		int rtt;  // median, 0 until a pong
		std::vector<int> rtt_samples;
	};

	struct Relay {
		uint64_t token;
		RelayPath paths[2];  // IPv4, IPv6
		int BestPath() const;
	};

#pragma pack(1)
	struct Packet {
		uint32_t magic;
		uint32_t session_id;
		uint8_t type;
		uint8_t from_peer_id;
		uint8_t to_peer_id;
		uint8_t candidate_idx;
		uint64_t send_timestamp;
		uint64_t ping_timestamp;
		uint8_t rtt_matrix[N][N];
	};
	// Sent instead of Packet only when the match has relays, which every peer then supports.
	struct PacketWithRelays : Packet {
		uint8_t relay_rtt_matrix[N][MAX_RELAYS];
	};
	struct RelayPacket {
		uint32_t magic;
		uint8_t type;
		uint8_t peer_id;
		uint8_t relay_idx;
		uint8_t reserved;
		uint32_t session_id;
		uint64_t token;
		uint64_t timestamp;
	};
#pragma pack()
	static_assert(sizeof(RelayPacket) == 28, "RelayPacket must match gdxsv relayPingSize");

	void SendPeerPacket(PacketWithRelays &p, const UdpRemote &remote);
	void OnRelayPong(const RelayPacket &recv, uint32_t session_id, bool from_v6);

	std::atomic<bool> running_;
	std::chrono::high_resolution_clock::time_point start_time_;
	UdpClient client_ = UdpClient{};

	std::recursive_mutex mutex_;
	uint8_t peer_id_;
	uint8_t rtt_matrix_[N][N] = {};
	uint8_t relay_rtt_matrix_[N][MAX_RELAYS] = {};
	std::vector<Candidate> candidates_;
	std::vector<Relay> relays_;
	std::map<std::string, int> user_to_peer_;
	std::map<int, std::string> peer_to_user_;
};
