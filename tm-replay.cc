// pset4/tm-replay.cc - replay decision logs into a sequence of snapshots.
//
// Multi-replica mode: pass -L once per replica log (e.g. -L logs/tm-0.log
// -L logs/tm-1.log -L logs/tm-2.log). Each replica's log carries the same
// (seq, now_unix, request) values — those are the chosen Paxos decisions —
// but a per-replica applied_at_unix that records when *this replica*
// locally applied the entry. A partition shows up as applied_at_unix
// lagging behind now_unix on the affected replica.
//
// The visualizer asks "what does replica i think the world looks like at
// wall-clock time T?", so we maintain N parallel task_manager_db instances
// and advance replica i's SM by every entry with applied_at_unix[i] <= T.
// Each snapshot embeds a per_replica_state map keyed by replica id; the
// UI picks one to render based on which replica chip is selected.
//
// "Interesting events" are derived from owner_agent transitions (lease
// reclaim) and main_lock holder transitions (lock acquire/release), and
// give the UI a "next interesting event" hotkey target separate from raw
// step-by-step navigation.

#include "task_manager_db.hh"
#include "tm.hh"
#include "tm_dump.hh"
#include <nlohmann/json.hpp>
#include <xxhash.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <print>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <variant>
#include <vector>

using json = nlohmann::json;

namespace {

struct op_view {
    std::string summary;
    std::string affected_task_id;
};

struct resp_status {
    tmgr::errc errcode;
    std::string errmsg;
};
resp_status status_of(const tmgr::response& r) {
    return std::visit([](auto&& v) -> resp_status {
        return {v.errcode, v.errmsg};
    }, r);
}

std::string_view errc_name(tmgr::errc e) {
    switch (e) {
        case tmgr::errc::ok:             return "ok";
        case tmgr::errc::fenced:         return "fenced";
        case tmgr::errc::not_found:      return "not_found";
        case tmgr::errc::not_eligible:   return "not_eligible";
        case tmgr::errc::depth_exceeded: return "depth_exceeded";
        case tmgr::errc::busy:           return "busy";
        case tmgr::errc::invalid:        return "invalid";
    }
    return "?";
}

op_view summarize(const tmgr::request& req,
                  const tmgr::response& resp,
                  std::string_view path) {
    op_view ov;
    std::visit([&](auto&& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, tmgr::task_create_request>) {
            auto& cr = std::get<tmgr::task_create_response>(resp);
            ov.affected_task_id = cr.id;
            std::string ttype;
            switch (r.spec.type) {
                case tmgr::task_type::plan:      ttype = "plan"; break;
                case tmgr::task_type::implement: ttype = "implement"; break;
                case tmgr::task_type::merge:     ttype = "merge"; break;
            }
            ov.summary = std::format("task_create {} ({}) \"{}\"",
                                     cr.id, ttype, r.spec.title);
        } else if constexpr (std::is_same_v<T, tmgr::task_claim_request>) {
            auto& cr = std::get<tmgr::task_claim_response>(resp);
            if (cr.none) {
                ov.summary = std::format("task_claim {} -> none", r.agent_id);
            } else {
                ov.affected_task_id = cr.id;
                ov.summary = std::format("task_claim {} -> {} (token {})",
                                         r.agent_id, cr.id, cr.token);
            }
        } else if constexpr (std::is_same_v<T, tmgr::task_heartbeat_request>) {
            ov.affected_task_id = r.id;
            ov.summary = std::format("task_heartbeat {} ({})", r.id, r.agent_id);
        } else if constexpr (std::is_same_v<T, tmgr::task_complete_request>) {
            ov.affected_task_id = r.id;
            auto& cr = std::get<tmgr::task_complete_response>(resp);
            std::string extra;
            if (cr.merge_task_id) {
                extra = std::format(" + merge {}", *cr.merge_task_id);
            }
            if (!cr.child_ids.empty()) {
                extra += std::format(" + {} child(ren)", cr.child_ids.size());
            }
            ov.summary = std::format("task_complete {} ({}){}",
                                     r.id, r.agent_id, extra);
        } else if constexpr (std::is_same_v<T, tmgr::task_fail_request>) {
            ov.affected_task_id = r.id;
            ov.summary = std::format("task_fail {} ({}): {}",
                                     r.id, r.agent_id, r.reason);
        } else if constexpr (std::is_same_v<T, tmgr::task_list_request>) {
            ov.summary = std::format("task_list {}", r.agent_id);
        } else if constexpr (std::is_same_v<T, tmgr::main_lock_acquire_request>) {
            ov.affected_task_id = r.merge_task;
            auto& cr = std::get<tmgr::main_lock_acquire_response>(resp);
            if (cr.errcode == tmgr::errc::ok) {
                ov.summary = std::format("main_lock_acquire {} (token {})",
                                         r.agent_id, cr.lock_token);
            } else {
                ov.summary = std::format("main_lock_acquire {} -> busy", r.agent_id);
            }
        } else if constexpr (std::is_same_v<T, tmgr::main_lock_release_request>) {
            ov.summary = std::format("main_lock_release ({})", r.agent_id);
        } else if constexpr (std::is_same_v<T, tmgr::swarm_resume_request>) {
            ov.summary = std::format("swarm_resume ({})", r.agent_id);
        }
    }, req);
    if (ov.summary.empty()) {
        ov.summary = std::string(path);
    }
    return ov;
}

// One parsed log entry. The same (seq, now_unix, path, request) appears
// in every replica's log; only applied_at_unix differs per replica.
struct log_entry {
    int64_t seq;
    int64_t now_unix;
    int64_t applied_at_unix;  // -1 if absent
    std::string path;
    tmgr::request req;
};

// Read one replica's log file into a vector of entries. Errors are fatal.
std::vector<log_entry> read_log(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::print(std::cerr, "tm-replay: cannot open {}: {}\n",
                   path, std::strerror(errno));
        std::exit(1);
    }
    std::vector<log_entry> out;
    std::string line;
    int64_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        json entry;
        try {
            entry = json::parse(line);
        } catch (const json::parse_error& e) {
            std::print(std::cerr, "tm-replay: {}:{}: bad JSON: {}\n",
                       path, line_no, e.what());
            std::exit(1);
        }
        log_entry e;
        e.seq = entry.value("seq", int64_t{0});
        e.now_unix = entry.value("now_unix", int64_t{0});
        e.applied_at_unix = entry.value("applied_at_unix", int64_t{-1});
        e.path = entry.value("path", std::string{});
        if (!entry.contains("request")) {
            std::print(std::cerr, "tm-replay: {}:{}: missing 'request'\n",
                       path, line_no);
            std::exit(1);
        }
        try {
            e.req = tmgr::parse_request(e.path, entry["request"]);
        } catch (const std::exception& ex) {
            std::print(std::cerr, "tm-replay: {}:{}: parse_request: {}\n",
                       path, line_no, ex.what());
            std::exit(1);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// Pluck a derived "replica id" from a log path: logs/tm-0.log -> tm-0.
// Falls back to the path itself if the pattern doesn't match.
std::string replica_id_from_path(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

// Index of (task_id -> owner_agent, owner_token) for change detection.
// Used to derive lease_reclaim events without re-running the SM.
struct owner_index {
    std::map<std::string, std::pair<std::string, int64_t>> by_task;
    void load(const json& state) {
        by_task.clear();
        if (!state.contains("tasks") || !state["tasks"].is_array()) return;
        for (const auto& t : state["tasks"]) {
            std::string id = t.value("id", std::string{});
            std::string owner = t.value("owner_agent", std::string{});
            int64_t tok = t.value("owner_token", int64_t{0});
            if (!id.empty()) by_task[id] = {owner, tok};
        }
    }
};

void usage() {
    std::print(std::cerr,
        "Usage: tm-replay [-L LOG_PATH ...] [-o OUT_PATH]\n"
        "  -L PATH   path to tm-server JSONL log; repeat for multi-replica\n"
        "            mode (e.g. -L logs/tm-0.log -L logs/tm-1.log)\n"
        "  -o PATH   path for snapshots output (default: snapshots.json)\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> log_paths;
    std::string out_path = "snapshots.json";
    int opt;
    while ((opt = getopt(argc, argv, "hL:o:")) != -1) {
        switch (opt) {
        case 'h': usage(); return 0;
        case 'L': log_paths.push_back(optarg); break;
        case 'o': out_path = optarg; break;
        default:  usage(); return 1;
        }
    }
    if (optind != argc) { usage(); return 1; }
    if (log_paths.empty()) log_paths.push_back("tm-server.log");

    // Read every replica's log. They must agree on the (seq, now_unix,
    // path, request) prefix — that's the SMR invariant — but applied_at
    // can diverge. We use replica 0 as the authoritative event stream.
    const size_t N = log_paths.size();
    std::vector<std::string> replica_ids;
    std::vector<std::vector<log_entry>> per_replica_entries(N);
    for (size_t i = 0; i < N; ++i) {
        per_replica_entries[i] = read_log(log_paths[i]);
        replica_ids.push_back(replica_id_from_path(log_paths[i]));
    }

    const auto& entries0 = per_replica_entries[0];
    const size_t E = entries0.size();

    // Sanity-check: every replica should have logged the same seqs.
    // Differences would mean the logs aren't from the same Paxos run.
    for (size_t i = 1; i < N; ++i) {
        if (per_replica_entries[i].size() != E) {
            std::print(std::cerr,
                "tm-replay: WARNING: {} has {} entries but {} has {} - "
                "logs may be from different runs\n",
                log_paths[0], E, log_paths[i], per_replica_entries[i].size());
        }
    }

    // Build seq -> applied_at_unix map per replica. Missing entries get
    // -1 (treated as "this replica never applied this seq within the
    // captured window"). The map approach lets us look up cheaply.
    std::vector<std::map<int64_t, int64_t>> applied_at_by_seq(N);
    for (size_t i = 0; i < N; ++i) {
        for (const auto& e : per_replica_entries[i]) {
            applied_at_by_seq[i][e.seq] = e.applied_at_unix;
        }
    }

    // Per-replica SM state. We advance each replica's SM lazily as wall
    // time progresses (driven by event.now_unix). next_idx[i] is the
    // next entry in replica i's log that hasn't been applied yet.
    std::vector<tmgr::task_manager_db> dbs(N);
    std::vector<size_t> next_idx(N, 0);

    auto advance_replica_to = [&](size_t i, int64_t T) {
        // Apply every entry j with applied_at_unix[i] <= T (in seq order,
        // which equals log-file order). If applied_at_unix is missing
        // (-1), fall back to now_unix so old logs without the field
        // still replay deterministically.
        auto& entries = per_replica_entries[i];
        while (next_idx[i] < entries.size()) {
            const auto& e = entries[next_idx[i]];
            int64_t when = e.applied_at_unix >= 0 ? e.applied_at_unix : e.now_unix;
            if (when > T) break;
            dbs[i].process_req(e.req, e.now_unix);
            ++next_idx[i];
        }
    };

    auto last_applied_seq = [&](size_t i) -> int64_t {
        if (next_idx[i] == 0) return 0;
        return per_replica_entries[i][next_idx[i] - 1].seq;
    };
    auto last_applied_at = [&](size_t i) -> int64_t {
        if (next_idx[i] == 0) return -1;
        const auto& e = per_replica_entries[i][next_idx[i] - 1];
        return e.applied_at_unix >= 0 ? e.applied_at_unix : e.now_unix;
    };

    // Hash a structural view of the leader-replica state to dedup vanilla
    // heartbeats. Per-replica states are derived from advancing-to-T, so
    // a heartbeat-only step still leaves the leader's state unchanged
    // and we can collapse it.
    auto hash_state = [](const json& s) -> uint64_t {
        json view = s;
        if (view.contains("tasks") && view["tasks"].is_array()) {
            for (auto& t : view["tasks"]) {
                t.erase("heartbeat_unix");
            }
        }
        std::string blob = view.dump();
        return XXH3_64bits(blob.data(), blob.size());
    };

    // A dedicated leader SM mirrors what every replica eventually
    // converges to. We use it to capture each event's response (for
    // op_summary / affected_task) and to derive lease-reclaim and
    // lock-transition events without coupling those to a specific
    // replica's lagged view.
    json snapshots = json::array();
    json interesting = json::array();
    owner_index prev_owners;
    std::string prev_lock_holder;
    int64_t prev_lock_token = 0;
    int64_t replayed = 0;
    uint64_t last_kept_hash = 0;

    {
        json init_state = tmgr::dump_state(dbs[0]);
        json per_replica = json::object();
        for (size_t i = 0; i < N; ++i) {
            per_replica[replica_ids[i]] = {
                {"state", tmgr::dump_state(dbs[i])},
                {"last_applied_seq", 0},
                {"applied_at_unix", -1}
            };
        }
        snapshots.push_back({
            {"seq", 0},
            {"now_unix", 0},
            {"op", ""},
            {"op_summary", "(initial state)"},
            {"affected_task_id", ""},
            {"errcode", "ok"},
            {"state", init_state},
            {"per_replica_state", per_replica}
        });
        last_kept_hash = hash_state(init_state);
    }

    tmgr::task_manager_db leader_sm;
    prev_owners.load(tmgr::dump_state(leader_sm));

    for (size_t k = 0; k < E; ++k) {
        const auto& e = entries0[k];
        ++replayed;

        // Apply on the dedicated leader SM to capture the response.
        tmgr::response resp = leader_sm.process_req(e.req, e.now_unix);
        op_view ov = summarize(e.req, resp, e.path);
        resp_status rs = status_of(resp);
        if (rs.errcode != tmgr::errc::ok) {
            ov.summary = std::format("{} -> {}{}",
                                     ov.summary, errc_name(rs.errcode),
                                     rs.errmsg.empty() ? "" :
                                         std::format(" ({})", rs.errmsg));
        }

        // Advance every replica's SM by wall-clock. Replica 0 will
        // catch up to seq k+1 (since its applied_at <= e.now_unix by
        // construction). Lagging replicas may stop short.
        for (size_t i = 0; i < N; ++i) {
            advance_replica_to(i, e.now_unix);
        }

        json leader_state = tmgr::dump_state(leader_sm);
        uint64_t h = hash_state(leader_state);
        bool state_changed = (h != last_kept_hash);
        bool rejected = (rs.errcode != tmgr::errc::ok);

        // Lease reclaim detection: any task whose owner_agent changed
        // (and the new owner is non-empty) is a handover. Run against
        // the leader_state so we observe the canonical transitions.
        owner_index now_owners;
        now_owners.load(leader_state);
        std::vector<json> reclaims;
        for (const auto& [tid, no] : now_owners.by_task) {
            auto it = prev_owners.by_task.find(tid);
            std::string old_owner = (it == prev_owners.by_task.end()) ? "" : it->second.first;
            if (!no.first.empty() && no.first != old_owner && !old_owner.empty()) {
                reclaims.push_back({
                    {"kind", "lease_reclaim"},
                    {"seq", e.seq},
                    {"now_unix", e.now_unix},
                    {"task_id", tid},
                    {"from_agent", old_owner},
                    {"to_agent", no.first}
                });
            }
        }
        prev_owners = std::move(now_owners);

        // Main lock transition.
        std::string cur_lock_holder;
        int64_t cur_lock_token = 0;
        if (leader_state.contains("main_lock") &&
            leader_state["main_lock"].is_object()) {
            const auto& ml = leader_state["main_lock"];
            cur_lock_holder = ml.value("holder_agent", std::string{});
            cur_lock_token = ml.value("lock_token", int64_t{0});
        }
        bool lock_changed = (cur_lock_holder != prev_lock_holder ||
                             cur_lock_token != prev_lock_token);
        if (lock_changed) {
            interesting.push_back({
                {"kind", cur_lock_holder.empty() ? "lock_release" : "lock_acquire"},
                {"seq", e.seq},
                {"now_unix", e.now_unix},
                {"holder", cur_lock_holder},
                {"token", cur_lock_token}
            });
        }
        prev_lock_holder = cur_lock_holder;
        prev_lock_token = cur_lock_token;

        // Always record lease reclaims as interesting.
        for (auto& r : reclaims) interesting.push_back(std::move(r));
        // Rejects are interesting.
        if (rejected) {
            interesting.push_back({
                {"kind", "rejected"},
                {"seq", e.seq},
                {"now_unix", e.now_unix},
                {"op_summary", ov.summary},
                {"errcode", errc_name(rs.errcode)}
            });
        }

        // Keep snapshot if state changed, request was rejected, or any
        // interesting derived event was emitted this step.
        bool any_interesting = !reclaims.empty() || lock_changed;
        if (!state_changed && !rejected && !any_interesting) {
            continue;
        }

        // Build per-replica state snapshot.
        json per_replica = json::object();
        for (size_t i = 0; i < N; ++i) {
            per_replica[replica_ids[i]] = {
                {"state", tmgr::dump_state(dbs[i])},
                {"last_applied_seq", last_applied_seq(i)},
                {"applied_at_unix", last_applied_at(i)}
            };
        }

        snapshots.push_back({
            {"seq", e.seq},
            {"now_unix", e.now_unix},
            {"op", e.path},
            {"op_summary", ov.summary},
            {"affected_task_id", ov.affected_task_id},
            {"errcode", errc_name(rs.errcode)},
            {"state", leader_state},  // legacy: replica-0/leader view
            {"per_replica_state", per_replica}
        });
        last_kept_hash = h;
    }

    json top = {
        {"replicas", replica_ids},
        {"snapshots", snapshots},
        {"interesting_events", interesting}
    };

    std::ofstream out(out_path);
    if (!out) {
        std::print(std::cerr, "tm-replay: cannot write {}: {}\n",
                   out_path, std::strerror(errno));
        return 1;
    }
    out << top.dump(2) << "\n";

    int64_t kept = static_cast<int64_t>(snapshots.size()) - 1;
    std::print(std::cerr,
               "tm-replay: {} replica(s), {} entries replayed -> {} snapshots kept "
               "({} no-ops collapsed), {} interesting events -> {}\n",
               N, replayed, kept, replayed - kept,
               interesting.size(), out_path);
    return 0;
}
