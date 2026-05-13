// pset4/tm.cc - JSON (de)serialization for tm types.

#include "tm.hh"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace tmgr {

using json = nlohmann::json;

// ---- enums as strings ----

void to_json(json& j, task_type t) {
    switch (t) {
        case task_type::plan:      j = "plan"; return;
        case task_type::implement: j = "implement"; return;
        case task_type::merge:     j = "merge"; return;
    }
}
void from_json(const json& j, task_type& t) {
    auto s = j.get<std::string>();
    if (s == "plan") t = task_type::plan;
    else if (s == "implement") t = task_type::implement;
    else if (s == "merge") t = task_type::merge;
    else throw std::runtime_error("unknown task_type: " + s);
}

void to_json(json& j, task_status s) {
    switch (s) {
        case task_status::pending:     j = "pending"; return;
        case task_status::in_progress: j = "in_progress"; return;
        case task_status::done:        j = "done"; return;
        case task_status::failed:      j = "failed"; return;
    }
}
void from_json(const json& j, task_status& s) {
    auto v = j.get<std::string>();
    if      (v == "pending")     s = task_status::pending;
    else if (v == "in_progress") s = task_status::in_progress;
    else if (v == "done")        s = task_status::done;
    else if (v == "failed")      s = task_status::failed;
    else throw std::runtime_error("unknown task_status: " + v);
}

void to_json(json& j, errc e) {
    switch (e) {
        case errc::ok:              j = "ok"; return;
        case errc::fenced:          j = "fenced"; return;
        case errc::not_found:       j = "not_found"; return;
        case errc::not_eligible:    j = "not_eligible"; return;
        case errc::depth_exceeded:  j = "depth_exceeded"; return;
        case errc::busy:            j = "busy"; return;
        case errc::invalid:         j = "invalid"; return;
    }
}
void from_json(const json&, errc&) { /* requests don't carry errc */ }

// ---- task_spec ----

void to_json(json& j, const task_spec& s) {
    j = json{
        {"type", s.type},
        {"title", s.title},
        {"prompt", s.prompt},
        {"requires", s.requires_deps},
        {"depth", s.depth}
    };
    if (s.parent_id) j["parent_id"] = *s.parent_id;
    if (s.branch_base) j["branch_base"] = *s.branch_base;
    if (s.implement_branch_sha) j["implement_branch_sha"] = *s.implement_branch_sha;
}
void from_json(const json& j, task_spec& s) {
    s.type = j.value("type", task_type::plan);
    s.title = j.value("title", std::string{});
    s.prompt = j.value("prompt", std::string{});
    s.requires_deps = j.value("requires", std::vector<task_id>{});
    s.depth = j.value("depth", 0);
    if (j.contains("parent_id")) s.parent_id = j["parent_id"].get<std::string>();
    if (j.contains("branch_base")) s.branch_base = j["branch_base"].get<std::string>();
    if (j.contains("implement_branch_sha")) {
        s.implement_branch_sha = j["implement_branch_sha"].get<std::string>();
    }
}

// ---- task_summary (used in task_list responses) ----

void to_json(json& j, const task_summary& s) {
    j = json{
        {"id", s.id},
        {"type", s.type},
        {"title", s.title},
        {"status", s.status},
        {"owner_agent", s.owner_agent}
    };
    if (s.result_branch) j["result_branch"] = *s.result_branch;
}

// ---- helpers for filling request_base ----

namespace {
void fill_base(request_base& r, const json& j) {
    r.serial = j.value("serial", uint64_t{0});
    r.agent_id = j.value("agent_id", std::string{});
}

// Require a fencing-token field. A missing token must not silently
// become 0 — the SM uses 0 as the "unclaimed" sentinel, so a missing
// field would masquerade as a fenced ownership mismatch and obscure
// agent payload bugs (typo "tok" vs "token"). Refuse parse instead.
fencing_token require_token(const json& j, const char* field) {
    if (!j.contains(field)) {
        throw std::runtime_error(std::string("missing required field '") + field + "'");
    }
    return j[field].get<fencing_token>();
}
}  // namespace

// ---- parse_request: dispatch on URL path ----

request parse_request(std::string_view path, const json& j) {
    if (path == "/task_create") {
        task_create_request r;
        fill_base(r, j);
        if (!j.contains("spec")) throw std::runtime_error("missing 'spec'");
        r.spec = j["spec"].get<task_spec>();
        if (j.contains("creator_task")) r.creator_task = j["creator_task"].get<std::string>();
        if (j.contains("creator_token")) r.creator_token = j["creator_token"].get<fencing_token>();
        return r;
    }
    if (path == "/task_claim") {
        task_claim_request r;
        fill_base(r, j);
        if (j.contains("prefer_type")) {
            r.prefer_type = j["prefer_type"].get<task_type>();
        }
        return r;
    }
    if (path == "/task_heartbeat") {
        task_heartbeat_request r;
        fill_base(r, j);
        r.id = j.value("id", std::string{});
        r.token = require_token(j, "token");
        return r;
    }
    if (path == "/task_complete") {
        task_complete_request r;
        fill_base(r, j);
        r.id = j.value("id", std::string{});
        r.token = require_token(j, "token");
        if (j.contains("result_branch")) r.result_branch = j["result_branch"].get<std::string>();
        r.result_summary = j.value("result_summary", std::string{});
        if (j.contains("new_children")) {
            for (auto& c : j["new_children"]) {
                r.new_children.push_back(c.get<task_spec>());
            }
        }
        return r;
    }
    if (path == "/task_fail") {
        task_fail_request r;
        fill_base(r, j);
        r.id = j.value("id", std::string{});
        r.token = require_token(j, "token");
        r.reason = j.value("reason", std::string{});
        return r;
    }
    if (path == "/task_list") {
        task_list_request r;
        fill_base(r, j);
        if (j.contains("filter_status")) {
            r.filter_status = j["filter_status"].get<task_status>();
        }
        r.only_mine = j.value("only_mine", false);
        return r;
    }
    if (path == "/main_lock_acquire") {
        main_lock_acquire_request r;
        fill_base(r, j);
        r.merge_task = j.value("merge_task", std::string{});
        r.merge_token = require_token(j, "merge_token");
        return r;
    }
    if (path == "/main_lock_release") {
        main_lock_release_request r;
        fill_base(r, j);
        r.lock_token = require_token(j, "lock_token");
        return r;
    }
    if (path == "/swarm_resume") {
        swarm_resume_request r;
        fill_base(r, j);
        return r;
    }
    throw std::runtime_error(std::string("unknown path: ") + std::string(path));
}

// ---- response serialization ----

namespace {

json envelope(const response_base& b) {
    json j = {
        {"ok", b.errcode == errc::ok},
        {"serial", b.serial}
    };
    if (b.errcode != errc::ok) {
        j["error"] = b.errcode;
        j["errmsg"] = b.errmsg;
    }
    return j;
}

}  // namespace

// ---- request serialization (inverse of parse_request) ----

namespace {

void put_base(json& j, const request_base& b) {
    j["agent_id"] = b.agent_id;
    j["serial"] = b.serial;
}

}  // namespace

std::string_view request_path(const request& r) {
    return std::visit([](auto&& v) -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, task_create_request>)            return "/task_create";
        else if constexpr (std::is_same_v<T, task_claim_request>)        return "/task_claim";
        else if constexpr (std::is_same_v<T, task_heartbeat_request>)    return "/task_heartbeat";
        else if constexpr (std::is_same_v<T, task_complete_request>)     return "/task_complete";
        else if constexpr (std::is_same_v<T, task_fail_request>)         return "/task_fail";
        else if constexpr (std::is_same_v<T, task_list_request>)         return "/task_list";
        else if constexpr (std::is_same_v<T, main_lock_acquire_request>) return "/main_lock_acquire";
        else if constexpr (std::is_same_v<T, main_lock_release_request>) return "/main_lock_release";
        else                                                              return "/swarm_resume";
    }, r);
}

json to_json_request_body(const request& r) {
    return std::visit([](auto&& v) -> json {
        using T = std::decay_t<decltype(v)>;
        json j = json::object();
        put_base(j, v);
        if constexpr (std::is_same_v<T, task_create_request>) {
            j["spec"] = v.spec;
            if (v.creator_task) j["creator_task"] = *v.creator_task;
            if (v.creator_token) j["creator_token"] = *v.creator_token;
        } else if constexpr (std::is_same_v<T, task_claim_request>) {
            if (v.prefer_type) j["prefer_type"] = *v.prefer_type;
        } else if constexpr (std::is_same_v<T, task_heartbeat_request>) {
            j["id"] = v.id;
            j["token"] = v.token;
        } else if constexpr (std::is_same_v<T, task_complete_request>) {
            j["id"] = v.id;
            j["token"] = v.token;
            j["result_summary"] = v.result_summary;
            if (v.result_branch) j["result_branch"] = *v.result_branch;
            if (!v.new_children.empty()) {
                json kids = json::array();
                for (auto& c : v.new_children) kids.push_back(c);
                j["new_children"] = std::move(kids);
            }
        } else if constexpr (std::is_same_v<T, task_fail_request>) {
            j["id"] = v.id;
            j["token"] = v.token;
            j["reason"] = v.reason;
        } else if constexpr (std::is_same_v<T, task_list_request>) {
            if (v.filter_status) j["filter_status"] = *v.filter_status;
            if (v.only_mine) j["only_mine"] = true;
        } else if constexpr (std::is_same_v<T, main_lock_acquire_request>) {
            j["merge_task"] = v.merge_task;
            j["merge_token"] = v.merge_token;
        } else if constexpr (std::is_same_v<T, main_lock_release_request>) {
            j["lock_token"] = v.lock_token;
        } else if constexpr (std::is_same_v<T, swarm_resume_request>) {
            // no fields beyond base
        }
        return j;
    }, r);
}

json to_json_response(const response& r) {
    return std::visit([](auto&& v) -> json {
        using T = std::decay_t<decltype(v)>;
        json j = envelope(v);
        if constexpr (std::is_same_v<T, task_create_response>) {
            j["task_id"] = v.id;
        } else if constexpr (std::is_same_v<T, task_claim_response>) {
            j["none"] = v.none;
            if (!v.none) {
                j["task_id"] = v.id;
                j["spec"] = v.spec;
                j["fencing_token"] = v.token;
            }
            if (v.halted) {
                j["halted"] = true;
                if (v.halted_by) j["halted_by"] = *v.halted_by;
                if (v.halted_reason) j["halted_reason"] = *v.halted_reason;
            }
        } else if constexpr (std::is_same_v<T, task_heartbeat_response>) {
            // envelope is enough
        } else if constexpr (std::is_same_v<T, task_complete_response>) {
            if (v.merge_task_id) j["merge_task_id"] = *v.merge_task_id;
            j["child_ids"] = v.child_ids;
        } else if constexpr (std::is_same_v<T, task_fail_response>) {
            // envelope is enough
        } else if constexpr (std::is_same_v<T, task_list_response>) {
            j["tasks"] = v.tasks;
        } else if constexpr (std::is_same_v<T, main_lock_acquire_response>) {
            if (v.errcode == errc::ok) j["lock_token"] = v.lock_token;
        } else if constexpr (std::is_same_v<T, main_lock_release_response>) {
            // envelope is enough
        } else if constexpr (std::is_same_v<T, swarm_resume_response>) {
            j["was_halted"] = v.was_halted;
        }
        return j;
    }, r);
}

}  // namespace tmgr
