#pragma once
// pset4/tm-paxos-internal.hh - internal header exposing pt_paxos_replica
// to test harnesses. Production code (tm-server.cc) uses the tmgr::paxos_replica
// facade in tm-paxos.hh; tier-1 sim tests in tm-paxos-tests.cc need the
// inner class directly to construct N replicas and wire them with N
// sim_transports.

#include "task_manager_db.hh"
#include "tm.hh"
#include "tm-paxos.hh"
#include "transport.hh"
#include "cotamer/cotamer.hh"
#include <algorithm>
#include <ctime>
#include <deque>
#include <format>
#include <print>
#include <unordered_map>
#include <vector>

namespace cot = cotamer;

// Inter-replica message type
enum class msg_type { PROPOSE, ACK, PROBE, PREPARE };

struct paxos_message {
    msg_type type;
    size_t sender;                          // replica index of sender
    size_t slot;                            // first slot in values (PROPOSE only)
    std::vector<tmgr::decided_value> values;     // operations (PROPOSE/PREPARE; empty for ACK/PROBE)
    size_t ack_slot;                        // first unacknowledged slot (ACK only)
    size_t decided_slot;                    // how far has been committed (both)
    size_t round = 0;                       // current round of sender
    size_t ack_round = 0;                   // PREPARE only: round of the log being reported
    size_t truncation_slot = 0;             // leader's log_base_ (safe to truncate up to)
};

struct pt_paxos_replica {
    size_t index_;           // index of this replica in the replica set
    size_t nreplicas_;       // number of replicas
    size_t leader_index_ = 0;    // this replica’s idea of the current leader; set externally before run()

    // Inter-replica transport. Phase 2.3a: sim_transport wraps netsim for
    // in-process tests; Phase 2.3b adds tcp_transport for real multi-process
    // demos. Owned by the caller (e.g. paxos_replica::impl).
    tmgr::transport<paxos_message>* transport_ = nullptr;

    tmgr::task_manager_db db_;      // our copy of the database

    // Paxos state
    std::deque<tmgr::decided_value> log_;            // replicated log
    size_t log_base_ = 0;                       // slot number of log_[0]
    size_t next_slot_ = 0;                      // next slot for new client request
    std::vector<size_t> follower_ack_slot_;     // per-replica: first unacked slot
    size_t decided_slot_ = 0;                   // all slots < this are committed
    size_t applied_slot_ = 0;                   // all slots < this applied to db_
    std::unordered_map<size_t, tmgr::decided_value> pending_;  // slot -> request awaiting commit

    // Round / election state
    size_t probe_round_ = 0;           // highest round we've seen (reject anything lower)
    size_t ack_round_ = 0;             // round of last PROPOSE we accepted
    cot::steady_time_point last_leader_msg_;  // when we last heard from leader
    size_t election_round_ = 0;        // round we picked for our election
    std::vector<paxos_message> prepare_responses_;  // collected PREPARE responses

    // Batching
    bool batching_ = false;            // whether batching is enabled
    bool batch_pending_ = false;       // a PROPOSE was sent, batch until next ACK/timer

    // Performance counters
    size_t proposes_sent_ = 0;         // number of send_propose() calls (broadcasts)
    size_t messages_sent_ = 0;         // number of individual PROPOSE messages sent
    size_t values_sent_ = 0;           // total values across all PROPOSEs
    double total_latency_ms_ = 0;      // sum of per-request latencies
    size_t latency_count_ = 0;         // number of latency samples
    std::unordered_map<size_t, cot::steady_time_point> pending_time_;  // slot -> arrival time

    // Per-slot async rendezvous: propose_and_apply registers a pending_propose,
    // suspends, and apply_decided triggers `done` once the slot lands.
    struct pending_propose {
        cot::event done{nullptr};
        tmgr::response resp;
        int64_t now_unix = 0;
    };
    std::unordered_map<size_t, pending_propose*> pending_responses_;

    // Per-apply callback. Fires on EVERY replica inside apply_decided for
    // every applied mutation (path != /task_list — those are reads). Both
    // ok and rejected (e.g. fenced) responses are logged so audits can see
    // every decided request. tm-server uses this to write the on-disk
    // decision log so all replicas produce byte-identical logs (real-paxos
    // invariant).
    tmgr::paxos_replica::on_apply_fn on_apply_;

    void try_decide() {
        std::vector<size_t> sorted_acks(follower_ack_slot_);
        std::sort(sorted_acks.begin(), sorted_acks.end());
        size_t quorum = nreplicas_ / 2 + 1;
        size_t new_decided = sorted_acks[nreplicas_ - quorum];
        decided_slot_ = std::max(decided_slot_, new_decided);
    }

    void try_truncate() {
        size_t min_ack = *std::min_element(follower_ack_slot_.begin(), follower_ack_slot_.end());
        while (log_base_ < min_ack) {
            log_.pop_front();
            ++log_base_;
        }
    }

    // Apply decided slots to DB. Triggers the per-slot rendezvous so an
    // awaiting propose_and_apply can resume with its response.
    // Determinism invariant (CLAUDE.md hard-rule #3): use the leader-stamped
    // `now_unix` from the decided_value, never std::time(nullptr) here.
    cot::task<> apply_decided() {
        try_decide();
        while (applied_slot_ < decided_slot_ && applied_slot_ < log_base_ + log_.size()) {
            auto& dv = log_[applied_slot_ - log_base_];
            auto resp = db_.process_req(dv.req, dv.now_unix);
            if (on_apply_) {
                auto path = tmgr::request_path(dv.req);
                if (path != "/task_list") {
                    tmgr::errc ec = std::visit(
                        [](auto&& r) { return r.errcode; }, resp);
                    on_apply_(dv, path, ec);
                }
            }
            if (auto it = pending_responses_.find(applied_slot_);
                it != pending_responses_.end()) {
                it->second->resp = std::move(resp);
                it->second->now_unix = dv.now_unix;
                it->second->done.trigger();
            }
            ++applied_slot_;
        }
        if (index_ == leader_index_) {
            try_truncate();
        }
        co_return;
    }

    // Leader: send PROPOSE to followers (carries latest decided_slot_)
    cot::task<> send_propose() {
        ++proposes_sent_;
        for (size_t i = 0; i < nreplicas_; ++i) {
            if (i == index_) continue;
            size_t start = std::clamp(follower_ack_slot_[i], log_base_, log_base_ + log_.size());
            size_t nvalues = log_.size() - (start - log_base_);
            ++messages_sent_;
            values_sent_ += nvalues;
            co_await transport_->send(i, paxos_message{
                msg_type::PROPOSE, index_, start,
                std::vector<tmgr::decided_value>(log_.begin() + (start - log_base_), log_.end()),
                0, decided_slot_, probe_round_, 0, log_base_
            });
        }
    }

    pt_paxos_replica(size_t index, tmgr::transport<paxos_message>& tx);

    cot::task<> run();
    cot::task<> start_election();
    cot::task<> become_leader();

    // Async propose-and-wait. Stamps now_unix (CLAUDE.md hard-rule #3 — this
    // is the ONE place in the module that reads std::time), wraps into a
    // decided_value, appends to the log, sends PROPOSE to peers, and blocks
    // on a per-slot rendezvous until apply_decided lands the response.
    struct apply_result {
        tmgr::response resp;
        int64_t now_unix;
    };
    // True iff this replica believes it's the current leader. Used by clients
    // (and by propose_and_apply itself) to refuse stale proposals.
    bool is_leader() const { return index_ == leader_index_; }

    cot::task<apply_result> propose_and_apply(tmgr::request req) {
        // Reject if we're not the leader. A killed/partitioned old leader
        // still thinks it's the leader (no incoming messages to update
        // leader_index_), so without this check it would keep pushing to
        // its local log and diverge from the elected leader's log.
        // The caller (HTTP layer / test harness) should retry on the
        // current leader.
        if (!is_leader()) {
            tmgr::task_create_response err;
            err.serial = std::visit(
                [](auto& v) -> uint64_t { return v.serial; }, req);
            err.errcode = tmgr::errc::busy;
            err.errmsg = "not leader";
            co_return apply_result{tmgr::response{err}, 0};
        }

        auto& base = std::visit(
            [](auto& v) -> const tmgr::request_base& { return v; }, req);
        tmgr::decided_value dv{std::time(nullptr), base.agent_id, base.serial, req};

        size_t my_slot = next_slot_;
        pending_propose pending;
        pending_responses_[my_slot] = &pending;

        log_.push_back(std::move(dv));
        ++next_slot_;
        follower_ack_slot_[index_] = next_slot_;

        co_await send_propose();
        co_await apply_decided();

        if (applied_slot_ <= my_slot) {
            co_await pending.done.arm();
        }

        pending_responses_.erase(my_slot);
        co_return apply_result{std::move(pending.resp), pending.now_unix};
    }

    const tmgr::task_manager_db& db() const { return db_; }
    tmgr::task_manager_db& db() { return db_; }
};

template <typename CharT>
struct std::formatter<paxos_message, CharT> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    template <typename FormatContext>
    auto format(const paxos_message& m, FormatContext& ctx) const {
        switch (m.type) {
        case msg_type::PROPOSE:
            return std::format_to(ctx.out(), "PROPOSE(r={}, slot={}, nvals={}, decided={}, trunc={})",
                                  m.round, m.slot, m.values.size(), m.decided_slot, m.truncation_slot);
        case msg_type::ACK:
            return std::format_to(ctx.out(), "ACK(r={}, ack={}, decided={})",
                                  m.round, m.ack_slot, m.decided_slot);
        case msg_type::PROBE:
            return std::format_to(ctx.out(), "PROBE(r={}, decided={})",
                                  m.round, m.decided_slot);
        case msg_type::PREPARE:
            return std::format_to(ctx.out(), "PREPARE(r={}, slot={}, nvals={}, ack={}, decided={}, ar={})",
                                  m.round, m.slot, m.values.size(), m.ack_slot, m.decided_slot, m.ack_round);
        }
        return ctx.out();
    }
};
