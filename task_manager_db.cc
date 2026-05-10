// pset4/task_manager_db.cc - SM transitions.

#include "task_manager_db.hh"
#include <format>

namespace tmgr {

response task_manager_db::process_req(const request& req, int64_t now_unix) {
    return std::visit([&](auto&& r) -> response {
        return handle(r, now_unix);
    }, req);
}

task_id task_manager_db::mint_task_id() {
    return std::format("t/{:04}", next_task_seq_++);
}

bool task_manager_db::lease_expired(const task_record& t, int64_t now) const {
    return now - t.heartbeat_unix > LEASE_WINDOW_SECONDS;
}

bool task_manager_db::requires_satisfied(const task_record& t) const {
    for (const auto& dep_id : t.spec.requires_deps) {
        auto it = tasks_.find(dep_id);
        if (it == tasks_.end() || it->second.status != task_status::done) {
            return false;
        }
    }
    return true;
}

// All handlers return "not yet implemented" for now. Each subsequent task
// fills one in.
task_create_response task_manager_db::handle(const task_create_request& r, int64_t /*now*/) {
    int depth = 0;
    if (r.creator_task.has_value()) {
        // Recursive create: require valid creator + token.
        if (!r.creator_token.has_value()) {
            return {{r.serial, errc::invalid, "creator_token required"}, ""};
        }
        auto it = tasks_.find(*r.creator_task);
        if (it == tasks_.end()) {
            return {{r.serial, errc::not_found, "creator_task not found"}, ""};
        }
        const auto& parent = it->second;
        if (parent.status != task_status::in_progress
            || parent.owner_token != *r.creator_token) {
            return {{r.serial, errc::fenced, "creator_token does not match"}, ""};
        }
        depth = parent.spec.depth + 1;
        if (depth > DEPTH_CAP) {
            return {{r.serial, errc::depth_exceeded,
                     std::format("depth {} > cap {}", depth, DEPTH_CAP)}, ""};
        }
    }

    // Validate that every required dep exists.
    for (const auto& dep : r.spec.requires_deps) {
        if (!tasks_.contains(dep)) {
            return {{r.serial, errc::not_found,
                     std::format("requires references unknown task {}", dep)}, ""};
        }
    }

    task_id id = mint_task_id();
    task_record rec;
    rec.id = id;
    rec.spec = r.spec;
    rec.spec.depth = depth;
    if (r.creator_task) rec.spec.parent_id = *r.creator_task;
    rec.status = task_status::pending;
    tasks_.emplace(id, std::move(rec));
    task_order_.push_back(id);

    return {{r.serial, errc::ok, ""}, id};
}
task_claim_response task_manager_db::handle(const task_claim_request& r, int64_t now) {
    // Swarm-halted: refuse new claims, but advertise the cause so the loop
    // can log it and exit. In-progress tasks are unaffected (they keep
    // heartbeating, completing, etc.); we just don't hand out new work.
    if (swarm_halted_) {
        task_claim_response halt_resp;
        halt_resp.serial = r.serial;
        halt_resp.errcode = errc::ok;
        halt_resp.none = true;
        halt_resp.halted = true;
        halt_resp.halted_by = halted_by_;
        halt_resp.halted_reason = halted_reason_;
        return halt_resp;
    }

    auto try_pick = [&](task_record& t) -> bool {
        // Eligibility: requires satisfied AND
        //   (status==pending OR (in_progress AND lease expired)).
        if (!requires_satisfied(t)) return false;
        if (t.status == task_status::pending) return true;
        if (t.status == task_status::in_progress && lease_expired(t, now)) {
            return true;
        }
        return false;
    };

    auto claim = [&](task_record& t) -> task_claim_response {
        // If reclaiming an expired lease, transition back through pending.
        if (t.status == task_status::in_progress) {
            t.status = task_status::pending;
            t.owner_agent.clear();
            t.owner_token = 0;
        }
        t.status = task_status::in_progress;
        t.owner_agent = r.agent_id;
        t.owner_token = mint_token();
        t.heartbeat_unix = now;
        task_claim_response resp;
        resp.serial = r.serial;
        resp.errcode = errc::ok;
        resp.none = false;
        resp.id = t.id;
        resp.spec = t.spec;
        resp.token = t.owner_token;
        return resp;
    };

    // Two passes if prefer_type is set: first only matching, then all.
    auto scan = [&](bool only_matching) -> task_claim_response* {
        for (const auto& tid : task_order_) {
            auto& t = tasks_.at(tid);
            if (only_matching && r.prefer_type
                && t.spec.type != *r.prefer_type) continue;
            if (try_pick(t)) {
                static thread_local task_claim_response cached;
                cached = claim(t);
                return &cached;
            }
        }
        return nullptr;
    };

    if (r.prefer_type) {
        if (auto* p = scan(true))  return *p;
        if (auto* p = scan(false)) return *p;
    } else {
        if (auto* p = scan(false)) return *p;
    }

    task_claim_response none_resp;
    none_resp.serial = r.serial;
    none_resp.errcode = errc::ok;
    none_resp.none = true;
    return none_resp;
}
task_heartbeat_response task_manager_db::handle(const task_heartbeat_request& r, int64_t now) {
    auto it = tasks_.find(r.id);
    if (it == tasks_.end()) {
        return {{r.serial, errc::not_found, "no such task"}};
    }
    auto& t = it->second;
    if (t.owner_token == 0 || t.owner_token != r.token) {
        return {{r.serial, errc::fenced, "fencing token mismatch"}};
    }
    t.heartbeat_unix = now;
    return {{r.serial, errc::ok, ""}};
}

task_complete_response task_manager_db::handle(const task_complete_request& r, int64_t /*now*/) {
    auto it = tasks_.find(r.id);
    if (it == tasks_.end()) {
        return {{r.serial, errc::not_found, "no such task"}, std::nullopt, {}};
    }
    auto& t = it->second;
    if (t.owner_token == 0 || t.owner_token != r.token) {
        return {{r.serial, errc::fenced, "fencing token mismatch"}, std::nullopt, {}};
    }

    // Validate child depths first; reject the whole request on any violation.
    int child_depth = t.spec.depth + 1;
    if (child_depth > DEPTH_CAP && !r.new_children.empty()) {
        return {{r.serial, errc::depth_exceeded,
                 std::format("child depth {} > cap", child_depth)}, std::nullopt, {}};
    }

    // Append children.
    std::vector<task_id> child_ids;
    child_ids.reserve(r.new_children.size());
    for (const auto& spec : r.new_children) {
        task_id cid = mint_task_id();
        task_record rec;
        rec.id = cid;
        rec.spec = spec;
        rec.spec.depth = child_depth;
        rec.spec.parent_id = t.id;
        rec.status = task_status::pending;
        tasks_.emplace(cid, std::move(rec));
        task_order_.push_back(cid);
        child_ids.push_back(cid);
    }

    // Synthesize merge if implement, but ONLY if a result_branch was
    // provided. An implement that completes without a branch (e.g. a
    // scope-too-large mid-flight pivot, a verify-only "run playwright"
    // task, or a hand-off via new_children) has nothing to merge.
    std::optional<task_id> merge_id;
    if (t.spec.type == task_type::implement
        && r.result_branch.has_value()
        && !r.result_branch->empty()) {
        task_id mid = mint_task_id();
        task_record mrec;
        mrec.id = mid;
        mrec.spec.type = task_type::merge;
        mrec.spec.title = std::format("merge {}", t.id);
        mrec.spec.requires_deps = { t.id };
        mrec.spec.branch_base = "main";
        mrec.spec.implement_branch_sha = r.result_branch;
        mrec.spec.parent_id = t.id;
        mrec.spec.depth = t.spec.depth;  // sibling depth, not child
        mrec.status = task_status::pending;
        tasks_.emplace(mid, std::move(mrec));
        task_order_.push_back(mid);
        merge_id = mid;
    }

    // Finalize parent.
    t.status = task_status::done;
    t.owner_agent.clear();
    t.owner_token = 0;
    t.result_branch = r.result_branch;
    t.result_summary = r.result_summary;

    return {{r.serial, errc::ok, ""}, merge_id, child_ids};
}

task_fail_response task_manager_db::handle(const task_fail_request& r, int64_t /*now*/) {
    auto it = tasks_.find(r.id);
    if (it == tasks_.end()) {
        return {{r.serial, errc::not_found, "no such task"}};
    }
    auto& t = it->second;
    if (t.owner_token == 0 || t.owner_token != r.token) {
        return {{r.serial, errc::fenced, "fencing token mismatch"}};
    }
    t.status = task_status::failed;
    t.fail_reason = r.reason;
    t.owner_agent.clear();
    t.owner_token = 0;

    // task_fail is the only path that creates a `failed` task, and by
    // contract it means the goal is unreachable. Halt new claims until a
    // human investigates and calls /swarm_resume. In-progress peers keep
    // running; we just stop handing out NEW work.
    swarm_halted_ = true;
    halted_by_ = t.id;
    halted_reason_ = r.reason;

    return {{r.serial, errc::ok, ""}};
}
task_list_response task_manager_db::handle(const task_list_request& r, int64_t /*now*/) {
    task_list_response out;
    out.serial = r.serial;
    out.errcode = errc::ok;
    out.tasks.reserve(task_order_.size());
    for (const auto& tid : task_order_) {
        const auto& t = tasks_.at(tid);
        if (r.filter_status && t.status != *r.filter_status) continue;
        if (r.only_mine && t.owner_agent != r.agent_id) continue;
        task_summary s;
        s.id = t.id; s.type = t.spec.type; s.title = t.spec.title;
        s.status = t.status; s.owner_agent = t.owner_agent;
        s.result_branch = t.result_branch;
        out.tasks.push_back(std::move(s));
    }
    return out;
}

main_lock_acquire_response task_manager_db::handle(const main_lock_acquire_request& r, int64_t now) {
    auto it = tasks_.find(r.merge_task);
    if (it == tasks_.end()) {
        return {{r.serial, errc::not_found, "no such merge task"}, 0};
    }
    auto& mt = it->second;
    if (mt.status != task_status::in_progress || mt.owner_token != r.merge_token) {
        return {{r.serial, errc::fenced, "merge task not owned"}, 0};
    }

    if (main_lock_.lock_token != 0) {
        // Currently held; check holder's merge task lease.
        // Lock is still valid only if the merge task is in_progress, the
        // lease is fresh, AND the current owner is still the original lock
        // holder. If a new agent has taken over the merge task via an
        // expired-lease claim, the previous lock is stale.
        auto hit = tasks_.find(main_lock_.holder_merge_task);
        if (hit != tasks_.end()
            && hit->second.status == task_status::in_progress
            && !lease_expired(hit->second, now)
            && hit->second.owner_agent == main_lock_.holder_agent) {
            return {{r.serial, errc::busy, "main lock held"}, 0};
        }
        // Stale; clear and grant.
        main_lock_ = {};
    }

    main_lock_.holder_agent = r.agent_id;
    main_lock_.holder_merge_task = r.merge_task;
    main_lock_.lock_token = mint_token();
    return {{r.serial, errc::ok, ""}, main_lock_.lock_token};
}

main_lock_release_response task_manager_db::handle(const main_lock_release_request& r, int64_t /*now*/) {
    if (main_lock_.lock_token == 0 || main_lock_.lock_token != r.lock_token) {
        return {{r.serial, errc::fenced, "lock token mismatch"}};
    }
    main_lock_ = {};
    return {{r.serial, errc::ok, ""}};
}

swarm_resume_response task_manager_db::handle(const swarm_resume_request& r, int64_t /*now*/) {
    swarm_resume_response resp;
    resp.serial = r.serial;
    resp.errcode = errc::ok;
    resp.was_halted = swarm_halted_;
    swarm_halted_ = false;
    halted_by_.reset();
    halted_reason_.reset();
    return resp;
}

}  // namespace tmgr
