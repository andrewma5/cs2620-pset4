#pragma once
// pset4/tm_dump.hh - shared serializer for the full SM state.
//
// Used by both tm-server's GET /dump endpoint and tm-replay's snapshot
// emission, so the visualizer sees the exact same shape the live /dump
// endpoint exposes.

#include "task_manager_db.hh"
#include <nlohmann/json.hpp>

namespace tmgr {

inline nlohmann::json dump_state(const task_manager_db& db) {
    nlohmann::json j = nlohmann::json::object();
    j["main_lock"] = {
        {"holder_agent", db.main_lock().holder_agent},
        {"holder_merge_task", db.main_lock().holder_merge_task},
        {"lock_token", db.main_lock().lock_token}
    };
    j["swarm_halted"] = db.swarm_halted();
    j["halted_by"] = db.halted_by().value_or("");
    j["halted_reason"] = db.halted_reason().value_or("");
    nlohmann::json arr = nlohmann::json::array();
    for (auto& tid : db.task_order()) {
        const auto& t = db.tasks().at(tid);
        arr.push_back({
            {"id", t.id},
            {"spec", t.spec},
            {"status", t.status},
            {"owner_agent", t.owner_agent},
            {"owner_token", t.owner_token},
            {"heartbeat_unix", t.heartbeat_unix},
            {"result_branch", t.result_branch.value_or("")},
            {"result_summary", t.result_summary.value_or("")},
            {"fail_reason", t.fail_reason.value_or("")}
        });
    }
    j["tasks"] = arr;
    return j;
}

}  // namespace tmgr
