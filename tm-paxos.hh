#pragma once
// pset4/tm-paxos.hh - public surface of the Multi-Paxos replication layer.
//
// The state machine (`tmgr::task_manager_db::process_req(req, now_unix)`)
// is a pure function of (request, wall-clock). To replicate it via Paxos,
// the *leader* must stamp `now_unix` at propose time and that exact value
// must ride inside the decided value, so every replica replays the same
// timestamp at apply time. If a replica pulls its own clock at apply
// time, the state machines diverge silently (CLAUDE.md hard-rule #3).
//
// `decided_value` is the value type Paxos operates over. `paxos_replica`
// is the facade tm-server uses; the heavy implementation (pt_paxos_replica
// plus netsim plumbing) is hidden behind a pImpl pointer so this header
// stays light.

#include "tm.hh"
#include "cotamer/cotamer.hh"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tmgr {

class task_manager_db;

// The value decided by one Paxos slot. Replicated identically across
// replicas; applied to the SM in slot order.
struct decided_value {
    int64_t now_unix = 0;       // leader-stamped at propose time
    std::string agent_id;       // lifted from request_base for dedup
    uint64_t serial = 0;        // lifted from request_base for dedup
    request req;                // the SM input
};

// Public surface of the replication layer. Phase 2.4 wires this into
// tm-server.cc in single-replica degenerate mode (no network, instant
// decide). Phase 2.3 adds multi-replica behind the same call shape.
class paxos_replica {
public:
    // Single-replica degenerate mode (Phase 2.4). Uses sim_transport with
    // nreplicas=1, propose_and_apply applies synchronously in effect.
    paxos_replica();

    // Multi-replica TCP mode (Phase 2.3b + 4). `peer_addrs[i]` is the
    // "host:port" of replica i; the entry at `my_index` is OUR listen
    // address. Spawns the listen + connect-to-peers background coroutines
    // and the replica's run() loop.
    paxos_replica(size_t my_index, std::vector<std::string> peer_addrs);

    ~paxos_replica();
    paxos_replica(const paxos_replica&) = delete;
    paxos_replica& operator=(const paxos_replica&) = delete;

    struct apply_result {
        response resp;
        int64_t now_unix;   // the value Paxos stamped (and what was logged)
    };

    // Multi-replica: blocks (co_awaits) until quorum decides the slot
    // carrying this request. Single-replica: returns as soon as the slot
    // applies (effectively synchronous).
    cotamer::task<apply_result> propose_and_apply(request req);
    const task_manager_db& db() const;

    // Which replica this server thinks is currently the leader. Used by
    // tm-server to 307-redirect non-leader HTTP requests.
    size_t leader_index() const;

    // Per-apply callback. Fires on EVERY replica (not just leader) inside
    // apply_decided, once the SM has processed the decided value AND the
    // response was errcode::ok AND path != "/task_list". This is where
    // tm-server writes the on-disk decision log so all replicas produce
    // byte-identical logs (real-paxos invariant; powers tm-replay).
    using on_apply_fn =
        std::function<void(const decided_value& dv, std::string_view path)>;
    void set_decision_logger(on_apply_fn cb);

private:
    struct impl;
    std::unique_ptr<impl> p_;
};

}  // namespace tmgr
