// pset4/tcp_transport.cc - tcp_transport implementation.

#include "tcp_transport.hh"
#include <nlohmann/json.hpp>
#include <arpa/inet.h>   // ntohl, htonl
#include <chrono>
#include <cstdint>
#include <utility>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// paxos_message <-> JSON. The `request` inside each decided_value rides as
// {req_path, req_body} so we can route through the existing tmgr parser.
json to_json(const paxos_message& m) {
    const char* type_name = "?";
    switch (m.type) {
    case msg_type::PROPOSE: type_name = "PROPOSE"; break;
    case msg_type::ACK:     type_name = "ACK";     break;
    case msg_type::PROBE:   type_name = "PROBE";   break;
    case msg_type::PREPARE: type_name = "PREPARE"; break;
    }
    json values = json::array();
    for (auto& dv : m.values) {
        values.push_back(json{
            {"now_unix", dv.now_unix},
            {"agent_id", dv.agent_id},
            {"serial",   dv.serial},
            {"req_path", std::string(tmgr::request_path(dv.req))},
            {"req",      tmgr::to_json_request_body(dv.req)},
        });
    }
    return json{
        {"type",            type_name},
        {"sender",          m.sender},
        {"slot",            m.slot},
        {"values",          std::move(values)},
        {"ack_slot",        m.ack_slot},
        {"decided_slot",    m.decided_slot},
        {"round",           m.round},
        {"ack_round",       m.ack_round},
        {"truncation_slot", m.truncation_slot},
    };
}

paxos_message from_json(const json& j) {
    paxos_message m;
    std::string type = j.at("type");
    if      (type == "PROPOSE") m.type = msg_type::PROPOSE;
    else if (type == "ACK")     m.type = msg_type::ACK;
    else if (type == "PROBE")   m.type = msg_type::PROBE;
    else if (type == "PREPARE") m.type = msg_type::PREPARE;
    m.sender          = j.at("sender");
    m.slot            = j.at("slot");
    m.ack_slot        = j.at("ack_slot");
    m.decided_slot    = j.at("decided_slot");
    m.round           = j.at("round");
    m.ack_round       = j.at("ack_round");
    m.truncation_slot = j.at("truncation_slot");
    for (auto& dvj : j.at("values")) {
        tmgr::decided_value dv;
        dv.now_unix = dvj.at("now_unix");
        dv.agent_id = dvj.at("agent_id");
        dv.serial   = dvj.at("serial");
        std::string path = dvj.at("req_path");
        dv.req = tmgr::parse_request(path, dvj.at("req"));
        m.values.push_back(std::move(dv));
    }
    return m;
}

}  // namespace

tcp_transport::tcp_transport(size_t my_index, std::vector<std::string> peer_addrs)
    : my_index_(my_index),
      peer_addrs_(std::move(peer_addrs)),
      outbound_fds_(peer_addrs_.size()) {
}

cot::task<> tcp_transport::start() {
    listen_loop_().detach();
    for (size_t i = 0; i < peer_addrs_.size(); ++i) {
        if (i == my_index_) continue;
        connect_loop_(i).detach();
    }
    co_return;
}

cot::task<paxos_message> tcp_transport::receive() {
    while (inbound_queue_.empty()) {
        co_await inbound_ready_.arm();
    }
    paxos_message msg = std::move(inbound_queue_.front());
    inbound_queue_.pop_front();
    co_return msg;
}

// Long enough to absorb a normal slow write under load, short enough to
// keep run() responsive (election timeout 500ms, retransmit cadence 100ms).
// See docs/BUGS.md "OPEN — Killing the elected leader wedges write quorum"
// for the failure mode this guards against: without a timeout, cot::write
// suspends forever on a full kernel send buffer to a dead peer in CLOSE-WAIT.
constexpr auto PEER_SEND_TIMEOUT = 500ms;

cot::task<> tcp_transport::send(size_t peer, paxos_message msg) {
    if (peer >= peer_addrs_.size() || peer == my_index_) co_return;
    if (!outbound_fds_[peer].valid()) co_return;  // not connected yet

    std::string body = to_json(msg).dump();
    uint32_t len_be = htonl(static_cast<uint32_t>(body.size()));

    auto r1 = co_await cot::first(
        cot::write(outbound_fds_[peer], &len_be, sizeof(len_be)),
        cot::after(PEER_SEND_TIMEOUT)
    );
    if (r1.index() == 1 || !std::get<0>(r1)) {
        outbound_fds_[peer].close();   // wakes connect_loop_ via cot::closed
        co_return;
    }
    auto r2 = co_await cot::first(
        cot::write(outbound_fds_[peer], body.data(), body.size()),
        cot::after(PEER_SEND_TIMEOUT)
    );
    if (r2.index() == 1 || !std::get<0>(r2)) {
        outbound_fds_[peer].close();
    }
}

cot::task<> tcp_transport::listen_loop_() {
    auto listen_fd = co_await cot::tcp_listen(peer_addrs_[my_index_]);
    if (!listen_fd.valid()) co_return;
    while (true) {
        auto conn = co_await cot::tcp_accept(listen_fd);
        if (!conn.valid()) co_return;
        read_loop_(std::move(conn)).detach();
    }
}

// Persistent reconnect loop. After a successful connect, parks on
// cot::closed(outbound_fds_[peer]) — fires when send() closes the fd on
// write timeout/error, or when the peer's container drops the connection.
// Then loops back to reconnect. See docs/BUGS.md for the dead-peer-wedge
// failure mode this avoids.
cot::task<> tcp_transport::connect_loop_(size_t peer) {
    while (true) {
        if (!outbound_fds_[peer].valid()) {
            try {
                auto conn = co_await cot::tcp_connect(peer_addrs_[peer]);
                if (conn.valid()) {
                    outbound_fds_[peer] = std::move(conn);
                }
            } catch (...) {
                // peer not up yet — back off and retry
            }
        }

        if (outbound_fds_[peer].valid()) {
            // Wait for this fd to close (either send() timed out and
            // closed it, or the peer dropped the connection). When fired,
            // loop back and re-establish.
            co_await cot::closed(outbound_fds_[peer]).arm();
            outbound_fds_[peer] = cot::fd{};   // drop refcount on closed fd
        } else {
            // 100ms backoff on connect failure — faster than the 500ms
            // election timeout so replicas can re-establish all-to-all
            // connectivity before the next election round.
            co_await cot::after(100ms);
        }
    }
}

cot::task<> tcp_transport::read_loop_(cot::fd conn) {
    while (true) {
        uint32_t len_be;
        auto r1 = co_await cot::read(conn, &len_be, sizeof(len_be));
        if (!r1 || *r1 != sizeof(len_be)) co_return;
        uint32_t len = ntohl(len_be);
        if (len == 0 || len > (1u << 24)) co_return;  // sanity: 16MB cap
        std::string body(len, '\0');
        auto r2 = co_await cot::read(conn, body.data(), len);
        if (!r2 || *r2 != len) co_return;

        try {
            auto j = json::parse(body);
            inbound_queue_.push_back(from_json(j));
            inbound_ready_.trigger();
        } catch (...) {
            // malformed frame — skip
        }
    }
}
