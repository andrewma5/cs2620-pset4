#pragma once
// pset4/task_manager_db.hh - the typed state machine.
//
// task_manager_db::process_req(request, now_unix) is the SM's only mutation
// entry point. It is a pure function of (state, request, now_unix). All
// transitions are atomic; failures return an error response and don't mutate.

#include "tm.hh"
#include <map>
#include <vector>

namespace tmgr {

class task_manager_db {
public:
    task_manager_db() = default;
    task_manager_db(const task_manager_db&) = delete;
    task_manager_db& operator=(const task_manager_db&) = delete;

    // The single mutation entry point.
    response process_req(const request& req, int64_t now_unix);

    // Read-only inspectors (used by tests, /dump, future state-equality checks).
    const std::map<task_id, task_record>& tasks() const { return tasks_; }
    const std::vector<task_id>& task_order() const { return task_order_; }
    const main_lock_state& main_lock() const { return main_lock_; }
    bool swarm_halted() const { return swarm_halted_; }
    const std::optional<task_id>& halted_by() const { return halted_by_; }
    const std::optional<std::string>& halted_reason() const { return halted_reason_; }

private:
    // Per-request handlers
    task_create_response   handle(const task_create_request&, int64_t now);
    task_claim_response    handle(const task_claim_request&, int64_t now);
    task_heartbeat_response handle(const task_heartbeat_request&, int64_t now);
    task_complete_response handle(const task_complete_request&, int64_t now);
    task_fail_response     handle(const task_fail_request&, int64_t now);
    task_list_response     handle(const task_list_request&, int64_t now);
    main_lock_acquire_response handle(const main_lock_acquire_request&, int64_t now);
    main_lock_release_response handle(const main_lock_release_request&, int64_t now);
    swarm_resume_response  handle(const swarm_resume_request&, int64_t now);

    // Helpers
    task_id mint_task_id();
    fencing_token mint_token() { return next_token_++; }
    bool lease_expired(const task_record&, int64_t now) const;
    bool requires_satisfied(const task_record&) const;

    std::map<task_id, task_record> tasks_;
    std::vector<task_id> task_order_;
    main_lock_state main_lock_;
    fencing_token next_token_ = 1;
    uint64_t next_task_seq_ = 1;

    // Swarm-halt: set by any task_fail; halts new task_claim until cleared
    // by swarm_resume. In-progress tasks are unaffected.
    bool swarm_halted_ = false;
    std::optional<task_id> halted_by_;
    std::optional<std::string> halted_reason_;
};

}  // namespace tmgr
