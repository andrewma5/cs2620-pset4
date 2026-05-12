#include "tm-paxos-internal.hh"
#include "tcp_transport.hh"
#include <optional>
#include <set>

using namespace std::chrono_literals;




// Configuration and initialization

pt_paxos_replica::pt_paxos_replica(size_t index, tmgr::transport<paxos_message>& tx)
    : index_(index),
      nreplicas_(tx.nreplicas()),
      transport_(&tx),
      follower_ack_slot_(tx.nreplicas(), 0),
      last_leader_msg_(cot::steady_now()) {
}




// ********** PANCY SERVICE CODE **********

cot::task<> pt_paxos_replica::start_election() {
    // Pick a new round: unique to our index, higher than anything we've seen
    election_round_ = probe_round_ + nreplicas_ - (probe_round_ % nreplicas_) + index_;
    probe_round_ = election_round_;
    prepare_responses_.clear();

    // Send PROBE to all other replicas
    for (size_t i = 0; i < nreplicas_; ++i) {
        if (i == index_) continue;
        co_await transport_->send(i, paxos_message{
            msg_type::PROBE, index_, 0, {}, 0, decided_slot_,
            election_round_, 0
        });
    }

    // Add self as implicit PREPARE response
    prepare_responses_.push_back(paxos_message{
        msg_type::PREPARE, index_, log_base_,
        std::vector<tmgr::decided_value>(log_.begin(), log_.end()),
        log_base_ + log_.size(), decided_slot_,
        election_round_, ack_round_
    });

    // If single replica, we already have quorum
    if (prepare_responses_.size() >= nreplicas_ / 2 + 1) {
        co_await become_leader();
    }
}

cot::task<> pt_paxos_replica::become_leader() {
    // Find PREPARE response with highest ack_round (ties broken by longest log)
    auto& best = *std::max_element(prepare_responses_.begin(),
        prepare_responses_.end(),
        [](auto& a, auto& b) {
            return a.ack_round < b.ack_round ||
                   (a.ack_round == b.ack_round && a.ack_slot < b.ack_slot);
        });

    // Apply our own log entries before adopting (they may be truncated in the new log).
    // Determinism: replay the leader-stamped now_unix carried in decided_value.
    size_t new_log_base = best.slot;
    while (applied_slot_ < new_log_base && applied_slot_ < log_base_ + log_.size()) {
        auto& dv = log_[applied_slot_ - log_base_];
        db_.process_req(dv.req, dv.now_unix);
        ++applied_slot_;
    }
    decided_slot_ = std::max(decided_slot_, new_log_base);
    applied_slot_ = std::max(applied_slot_, new_log_base);

    // Debug: print all PREPARE responses
    std::print(std::clog, "=== become_leader: replica {} round {} ===\n", index_, election_round_);
    std::print(std::clog, "  best: sender={} ack_round={} slot={} len={} ack_slot={}\n",
               best.sender, best.ack_round, best.slot, best.values.size(), best.ack_slot);
    for (auto& resp : prepare_responses_) {
        std::print(std::clog, "  resp: sender={} ack_round={} slot={} len={} ack_slot={} decided={}\n",
                   resp.sender, resp.ack_round, resp.slot, resp.values.size(), resp.ack_slot, resp.decided_slot);
    }
    std::print(std::clog, "  our decided_slot_={} applied_slot_={}\n", decided_slot_, applied_slot_);

    // Check if any response has entries before best.slot that would be lost
    for (auto& resp : prepare_responses_) {
        size_t resp_end = resp.slot + resp.values.size();
        if (resp.slot < best.slot && resp_end > best.slot) {
            std::print(std::clog, "  WARNING: resp from {} has entries {}-{} but best starts at {} — losing slots {}-{}\n",
                       resp.sender, resp.slot, resp_end - 1, best.slot, resp.slot, best.slot - 1);
        }
        if (resp_end > best.slot + best.values.size()) {
            std::print(std::clog, "  WARNING: resp from {} extends to {} but best ends at {} — losing trailing slots\n",
                       resp.sender, resp_end, best.slot + best.values.size());
        }
    }

    // Adopt that log
    log_.clear();
    log_.insert(log_.end(), best.values.begin(), best.values.end());
    log_base_ = best.slot;
    next_slot_ = log_base_ + log_.size();

    // Become leader
    leader_index_ = index_;
    ack_round_ = election_round_;

    // Reset follower tracking (use log_base_, not 0 — old leader truncated there,
    // so all replicas had acked past it before the election)
    std::fill(follower_ack_slot_.begin(), follower_ack_slot_.end(), log_base_);
    follower_ack_slot_[index_] = next_slot_;

    // Send PROPOSE to establish our log
    co_await send_propose();
    co_await apply_decided();
}

cot::task<> pt_paxos_replica::run() {
    while (true) {
        // Pset4: client requests no longer arrive via netsim — they enter
        // through propose_and_apply() (called by tm-server's HTTP layer).
        // run() only handles peer-to-peer paxos messages and timers.
        auto ret = co_await cot::first(
            transport_->receive(),       // index 0: inter-replica message
            cot::after(100ms)            // index 1: retransmit timer
        );

        if (ret.index() == 0) {
            auto& msg = std::get<0>(ret);
            if (msg.round > probe_round_) {
                // Higher round: step down
                probe_round_ = msg.round;
                leader_index_ = msg.sender;
                last_leader_msg_ = cot::steady_now();
                pending_.clear();
                pending_time_.clear();
                batch_pending_ = false;
            }

            if (msg.round < probe_round_) {
                // Stale message: ignore
            } else if (msg.type == msg_type::PROBE) {
                // Someone wants to be leader — send PREPARE with our log
                co_await transport_->send(msg.sender, paxos_message{
                    msg_type::PREPARE, index_, log_base_,
                    std::vector<tmgr::decided_value>(log_.begin(), log_.end()),
                    log_base_ + log_.size(), decided_slot_,
                    msg.round, ack_round_
                });
            } else if (msg.type == msg_type::PREPARE) {
                // PREPARE response for our election
                if (msg.round == election_round_ && probe_round_ == election_round_) {
                    prepare_responses_.push_back(msg);
                    if (prepare_responses_.size() == nreplicas_ / 2 + 1) {
                        co_await become_leader();
                    }
                }
            } else if (index_ == leader_index_) {
                // Leader: handle ACK from follower
                if (msg.type == msg_type::ACK && msg.round == probe_round_) {
                    follower_ack_slot_[msg.sender] = std::max(
                        follower_ack_slot_[msg.sender], msg.ack_slot
                    );
                }
                co_await apply_decided();
                if (batching_) batch_pending_ = false;
            } else if (msg.type == msg_type::PROPOSE) {
                // Follower: handle PROPOSE from leader
                leader_index_ = msg.sender;

                // Same-round-shorter check: ignore stale retransmit that would
                // shorten our log within the same round (Multi-Paxos spec §5)
                size_t propose_end = msg.slot + msg.values.size();
                if (msg.round == ack_round_ && propose_end < log_base_ + log_.size()) {
                    // Stale PROPOSE within same round — ignore
                    last_leader_msg_ = cot::steady_now();
                    co_await transport_->send(msg.sender, paxos_message{
                        msg_type::ACK, index_, 0, {}, log_base_ + log_.size(), decided_slot_,
                        probe_round_, 0
                    });
                    continue;
                }

                bool new_round = (msg.round > ack_round_);
                ack_round_ = msg.round;

                // If there's a gap, we're too far behind — reset log
                if (msg.slot > log_base_ + log_.size()) {
                    log_.clear();
                    log_base_ = msg.slot;
                    applied_slot_ = std::max(applied_slot_, msg.slot);
                }

                // Apply values to local log (overwrite or append)
                for (size_t i = 0; i < msg.values.size(); ++i) {
                    size_t s = msg.slot + i;
                    if (s < log_base_) continue;  // already truncated
                    size_t idx = s - log_base_;
                    if (idx < log_.size()) {
                        log_[idx] = msg.values[i];  // overwrite stale entry
                    } else if (idx == log_.size()) {
                        log_.push_back(msg.values[i]);  // append new entry
                    }
                }

                // On round change, truncate stale entries beyond what the new
                // leader sent (Multi-Paxos spec §5: AV_s ← V replaces entirely)
                if (new_round) {
                    while (log_base_ + log_.size() > propose_end) {
                        log_.pop_back();
                    }
                }

                // Update decided_slot from leader and apply
                decided_slot_ = std::max(decided_slot_, msg.decided_slot);
                last_leader_msg_ = cot::steady_now();
                co_await apply_decided();

                // Truncate log up to leader's truncation point (safe — all replicas have acked)
                size_t trunc = std::min(msg.truncation_slot, applied_slot_);
                while (log_base_ < trunc && !log_.empty()) {
                    log_.pop_front();
                    ++log_base_;
                }

                // Send ACK to leader
                co_await transport_->send(msg.sender, paxos_message{
                    msg_type::ACK, index_, 0, {}, log_base_ + log_.size(), decided_slot_,
                    probe_round_, 0
                });
            }
        } else {
            // Timeout
            if (index_ == leader_index_) {
                // Leader: retransmit
                batch_pending_ = false;
                co_await send_propose();
                co_await apply_decided();
            } else {
                // Follower: check if leader is dead
                if (cot::steady_now() - last_leader_msg_ > 500ms) {
                    co_await start_election();
                }
            }
        }
    }
}

// ******** end Pancy service code ********


// Facade implementation. Two modes:
//   - sim (single-replica): nreplicas=1 sim_transport, no peers, propose
//     applies synchronously via self-ack quorum. Default tm-server mode.
//   - tcp (multi-replica): peers given as host:port list. Used by the
//     docker-compose demo (Phase 4). Each replica runs the real protocol
//     against peers over TCP frames.
struct tmgr::paxos_replica::impl {
    std::optional<random_source> sim_rnd_;
    std::unique_ptr<tmgr::transport<paxos_message>> tx_;
    std::unique_ptr<pt_paxos_replica> inner_;

    static std::unique_ptr<impl> make_sim() {
        auto p = std::make_unique<impl>();
        p->sim_rnd_.emplace();
        auto sim = std::make_unique<tmgr::sim_transport<paxos_message>>(
            0, 1, *p->sim_rnd_);
        p->inner_ = std::make_unique<pt_paxos_replica>(0, *sim);
        p->tx_ = std::move(sim);
        p->inner_->leader_index_ = 0;
        p->inner_->run().detach();
        return p;
    }

    static std::unique_ptr<impl> make_tcp(size_t my_index,
                                          std::vector<std::string> peer_addrs) {
        auto p = std::make_unique<impl>();
        auto tcp = std::make_unique<tcp_transport>(my_index, std::move(peer_addrs));
        tcp->start().detach();
        p->inner_ = std::make_unique<pt_paxos_replica>(my_index, *tcp);
        p->tx_ = std::move(tcp);
        p->inner_->leader_index_ = 0;
        p->inner_->run().detach();
        return p;
    }
};

tmgr::paxos_replica::paxos_replica() : p_(impl::make_sim()) {}
tmgr::paxos_replica::paxos_replica(size_t my_index,
                                   std::vector<std::string> peer_addrs)
    : p_(impl::make_tcp(my_index, std::move(peer_addrs))) {}
tmgr::paxos_replica::~paxos_replica() = default;

cot::task<tmgr::paxos_replica::apply_result>
tmgr::paxos_replica::propose_and_apply(tmgr::request req) {
    auto r = co_await p_->inner_->propose_and_apply(std::move(req));
    co_return apply_result{std::move(r.resp), r.now_unix};
}

const tmgr::task_manager_db& tmgr::paxos_replica::db() const {
    return p_->inner_->db();
}

size_t tmgr::paxos_replica::leader_index() const {
    return p_->inner_->leader_index_;
}
