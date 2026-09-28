#include "gdxsv_backend_tcp.h"

#include "gdx_rpc.h"
#include "gdxsv.h"
#include "libs.h"

void GdxsvBackendTcp::Reset() {
	tcp_client_.Close();
	rx_msg_reader_.Clear();
	tx_msg_reader_.Clear();
	recv_buf_.clear();
	lbs_packet_filter_ = nullptr;
}

bool GdxsvBackendTcp::Connect(const std::string &host, u16 port) {
	bool ok = tcp_client_.Connect(host.c_str(), port);
	if (!ok) {
		WARN_LOG(COMMON, "Failed to connect with TCP %s:%d\n", host.c_str(), port);
		return false;
	}

	tcp_client_.SetNonBlocking();
	rx_msg_reader_.Clear();
	tx_msg_reader_.Clear();
	recv_buf_.clear();
	return true;
}

bool GdxsvBackendTcp::IsConnected() const { return tcp_client_.IsConnected(); }

void GdxsvBackendTcp::Close() { tcp_client_.Close(); }

int GdxsvBackendTcp::Send(const std::vector<u8> &packet) { return tcp_client_.Send((const char *)packet.data(), packet.size()); }

const std::string &GdxsvBackendTcp::LocalIP() const { return tcp_client_.local_ip(); }

const std::string &GdxsvBackendTcp::RemoteHost() const { return tcp_client_.host(); }

const int GdxsvBackendTcp::RemotePort() const { return tcp_client_.port(); }

u32 GdxsvBackendTcp::OnSockRead(u32 addr, u32 size) {
	int n = std::min<u32>(recv_buf_.size(), size);
	for (int i = 0; i < n; ++i) {
		gdxsv_WriteMem8(addr + i, recv_buf_.front());
		recv_buf_.pop_front();
	}
	return n;
}

u32 GdxsvBackendTcp::OnSockWrite(u32 addr, u32 size) {
	for (int i = 0; i < size; ++i) {
		tx_msg_reader_.Write(gdxsv_ReadMem8(addr + i));
	}

	while (tx_msg_reader_.Read(lbs_msg_)) {
		std::vector<u8> v;
		lbs_msg_.Serialize(v);
		tcp_client_.Send(reinterpret_cast<const char *>(v.data()), v.size());
	}

	return size;
}

u32 GdxsvBackendTcp::OnSockPoll() {
	if (tcp_client_.ReadableSize()) {
		u8 buf[InetBufSize];
		int n = tcp_client_.Recv(reinterpret_cast<char *>(buf), InetBufSize);
		if (0 < n) {
			rx_msg_reader_.Write(reinterpret_cast<char *>(buf), n);

			while (rx_msg_reader_.Read(lbs_msg_)) {
				if (lbs_packet_filter_ && !lbs_packet_filter_(lbs_msg_))
					continue;
				// The guest reader assumes exact reads and has a 768-byte body
				// buffer (disc 2 0x0c3a9bdc, disc 1 0x0c30e49c). Keep staging
				// complete messages, and reject oversize bodies before exposing
				// their header to the guest. Host-only messages (e.g. protobuf
				// patches) were handled by the filter.
				if ((gdxsv.Disk() == 1 || gdxsv.Disk() == 2) && lbs_msg_.body.size() > 768) {
					WARN_LOG(COMMON, "LBS: oversized guest reply command=%04x size=%zu",
						lbs_msg_.command, lbs_msg_.body.size());
					lbs_msg_.body.clear();
					lbs_msg_.body_size = 0;
					lbs_msg_.status = LbsMessage::StatusError;
				}
				lbs_msg_.Serialize(recv_buf_);
			}
		}
	}

	return recv_buf_.size();
}
