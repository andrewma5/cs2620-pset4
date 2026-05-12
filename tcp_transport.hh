#pragma once
// pset4/tcp_transport.hh - real-socket transport for paxos_message.
//
// One outbound TCP connection per peer (length-prefixed JSON frames) plus
// one listen socket on this replica's port. Inbound frames are decoded into
// paxos_message and pushed onto a queue that receive() pops. Used by
// tm-server in multi-process docker demo mode (Phase 4); tier-1 sim tests
// keep using sim_transport.

#include "tm-paxos-internal.hh"
#include <deque>
#include <string>
#include <vector>

class tcp_transport : public tmgr::transport<paxos_message> {
public:
    // `peer_addrs[i]` is the "host:port" of replica i. The entry at
    // index my_index_ is OUR listen address. peer_addrs.size() == nreplicas.
    tcp_transport(size_t my_index, std::vector<std::string> peer_addrs);
    ~tcp_transport() override = default;
    tcp_transport(const tcp_transport&) = delete;
    tcp_transport& operator=(const tcp_transport&) = delete;

    // Launch the listen-loop + per-peer connect-loop coroutines. Returns
    // immediately; the background tasks persist for the lifetime of *this.
    cot::task<> start();

    cot::task<paxos_message> receive() override;
    cot::task<> send(size_t peer, paxos_message msg) override;
    size_t nreplicas() const override { return peer_addrs_.size(); }

private:
    size_t my_index_;
    std::vector<std::string> peer_addrs_;

    // outbound_fds_[i] is our connection to peer i (or invalid if not yet
    // connected / disconnected). send() writes a frame on it.
    std::vector<cot::fd> outbound_fds_;

    // Inbound queue + signal — read_loop_ pushes parsed messages; receive()
    // arms on inbound_ready_ when empty.
    std::deque<paxos_message> inbound_queue_;
    cot::event inbound_ready_{nullptr};

    cot::task<> listen_loop_();
    cot::task<> connect_loop_(size_t peer);
    cot::task<> read_loop_(cot::fd conn);
};
