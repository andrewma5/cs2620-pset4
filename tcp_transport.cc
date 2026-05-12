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

cot::task<> tcp_transport::send(size_t peer, paxos_message msg) {
    if (peer >= peer_addrs_.size() || peer == my_index_) co_return;
    if (!outbound_fds_[peer].valid()) co_return;  // not connected yet

    std::string body = to_json(msg).dump();
    uint32_t len_be = htonl(static_cast<uint32_t>(body.size()));

    auto w1 = co_await cot::write(outbound_fds_[peer], &len_be, sizeof(len_be));
    if (!w1) {
        outbound_fds_[peer].close();
        co_return;
    }
    auto w2 = co_await cot::write(outbound_fds_[peer], body.data(), body.size());
    if (!w2) {
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

cot::task<> tcp_transport::connect_loop_(size_t peer) {
    while (true) {
        try {
            auto conn = co_await cot::tcp_connect(peer_addrs_[peer]);
            if (conn.valid()) {
                outbound_fds_[peer] = std::move(conn);
                co_return;
            }
        } catch (...) {
            // peer not up yet — back off and retry
        }
        // 100ms retry — faster than the 500ms election timeout so replicas
        // can establish all-to-all connectivity before the first election.
        co_await cot::after(100ms);
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
