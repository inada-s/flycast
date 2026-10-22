#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gdxsv_network.h"
#include "lbs_message.h"

class GdxsvBackendTcp {
   public:
	void Reset();
	bool Connect(const std::string &host, u16 port);
	bool IsConnected() const;
	void Close();
	int Send(const std::vector<u8> &packet);
	const std::string &LocalIP() const;
	const std::string &RemoteHost() const;
	const int RemotePort() const;
	u32 OnSockRead(u32 addr, u32 size);
	u32 OnSockWrite(u32 addr, u32 size);
	u32 OnSockPoll();
	// Passes a message held while the lobby was suspended, as if it arrived now.
	void Inject(const LbsMessage &msg);
	void lbs_packet_filter(std::function<bool(const LbsMessage &)> callback) { lbs_packet_filter_ = std::move(callback); }
	// Called with each message the game sends to the lobby.
	void lbs_sent_observer(std::function<void(const LbsMessage &)> callback) { lbs_sent_observer_ = std::move(callback); }

   private:
	void Stage(LbsMessage &msg);

	TcpClient tcp_client_;
	LbsMessage lbs_msg_;
	LbsMessageReader rx_msg_reader_;
	LbsMessageReader tx_msg_reader_;
	std::deque<u8> recv_buf_;
	std::function<bool(const LbsMessage &)> lbs_packet_filter_;
	std::function<void(const LbsMessage &)> lbs_sent_observer_;
};