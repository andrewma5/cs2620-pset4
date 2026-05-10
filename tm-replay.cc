// pset4/tm-replay.cc - replay a decision log into a sequence of snapshots.
//
// Reads JSONL (one decision per line) written by tm-server, replays each
// entry through the real task_manager_db::process_req, and emits a JSON
// array of post-state snapshots — one per log entry. The UI visualizer
// reads that file and lets you scrub through time.
//
// This binary deliberately *reuses* the SM (task_manager_db) so the
// timeline reflects exactly what the live server computed. Same code
// path, same answers.

#include "task_manager_db.hh"
#include "tm.hh"
#include "tm_dump.hh"
#include <nlohmann/json.hpp>
#include <xxhash.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <variant>

using json = nlohmann::json;

namespace {

// Build a one-line, human-readable summary of the operation for the UI
// header, plus the task_id (if any) the operation primarily affected, so
// the UI can highlight that card on step.
struct op_view {
    std::string summary;
    std::string affected_task_id;  // empty if none
};

op_view summarize(const tmgr::request& req,
                  const tmgr::response& resp,
                  std::string_view path) {
    op_view ov;
    std::visit([&](auto&& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, tmgr::task_create_request>) {
            // Affected = the new task. The id was minted by the SM; pull
            // from the response.
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

void usage() {
    std::print(std::cerr,
        "Usage: tm-replay [-L LOG_PATH] [-o OUT_PATH]\n"
        "  -L PATH   path to tm-server JSONL log (default: tm-server.log)\n"
        "  -o PATH   path for snapshots output (default: snapshots.json)\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string log_path = "tm-server.log";
    std::string out_path = "snapshots.json";
    int opt;
    while ((opt = getopt(argc, argv, "hL:o:")) != -1) {
        switch (opt) {
        case 'h': usage(); return 0;
        case 'L': log_path = optarg; break;
        case 'o': out_path = optarg; break;
        default:  usage(); return 1;
        }
    }
    if (optind != argc) { usage(); return 1; }

    std::ifstream in(log_path);
    if (!in) {
        std::print(std::cerr, "tm-replay: cannot open {}: {}\n",
                   log_path, std::strerror(errno));
        return 1;
    }

    tmgr::task_manager_db db;
    json snapshots = json::array();
    std::string line;
    int64_t line_no = 0;
    int64_t replayed = 0;

    // Hash a "structural" view of the state for dedup. We strip
    // heartbeat_unix from each task before hashing because a vanilla
    // heartbeat (one that doesn't change ownership/status) only bumps
    // that field — without stripping, every heartbeat would look like
    // a state change and never collapse. A heartbeat that DOES recover
    // an expired lease changes owner_agent/owner_token/status, which
    // remain in the hashed view, so it still shows up.
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

    // Always start with an empty-state "seq 0" snapshot so the UI can
    // show the world before the first decision.
    json init_state = tmgr::dump_state(db);
    snapshots.push_back({
        {"seq", 0},
        {"now_unix", 0},
        {"op", ""},
        {"op_summary", "(initial state)"},
        {"affected_task_id", ""},
        {"state", init_state}
    });
    uint64_t last_kept_hash = hash_state(init_state);

    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        json entry;
        try {
            entry = json::parse(line);
        } catch (const json::parse_error& e) {
            std::print(std::cerr, "tm-replay: line {}: bad JSON: {}\n",
                       line_no, e.what());
            return 1;
        }

        std::string path = entry.value("path", std::string{});
        int64_t seq = entry.value("seq", int64_t{0});
        int64_t now = entry.value("now_unix", int64_t{0});
        if (!entry.contains("request")) {
            std::print(std::cerr, "tm-replay: line {}: missing 'request'\n", line_no);
            return 1;
        }

        tmgr::request req;
        try {
            req = tmgr::parse_request(path, entry["request"]);
        } catch (const std::exception& e) {
            std::print(std::cerr, "tm-replay: line {}: parse_request failed: {}\n",
                       line_no, e.what());
            return 1;
        }

        tmgr::response resp = db.process_req(req, now);
        op_view ov = summarize(req, resp, path);

        json post_state = tmgr::dump_state(db);
        uint64_t h = hash_state(post_state);
        ++replayed;
        if (h == last_kept_hash) {
            continue;
        }
        snapshots.push_back({
            {"seq", seq},
            {"now_unix", now},
            {"op", path},
            {"op_summary", ov.summary},
            {"affected_task_id", ov.affected_task_id},
            {"state", post_state}
        });
        last_kept_hash = h;
    }

    std::ofstream out(out_path);
    if (!out) {
        std::print(std::cerr, "tm-replay: cannot write {}: {}\n",
                   out_path, std::strerror(errno));
        return 1;
    }
    out << snapshots.dump(2) << "\n";

    // snapshots includes the synthetic seq-0 init entry, so the count
    // of "real" kept snapshots is size()-1.
    int64_t kept = static_cast<int64_t>(snapshots.size()) - 1;
    std::print(std::cerr,
               "tm-replay: {} entries replayed -> {} snapshots kept ({} no-ops collapsed) -> {}\n",
               replayed, kept, replayed - kept, out_path);
    return 0;
}
