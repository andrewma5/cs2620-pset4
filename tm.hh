#pragma once
// pset4/tm.hh - task manager types (specs, records, request/response variants)

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tmgr {

enum class task_type   : uint8_t { plan, implement, merge };
enum class task_status : uint8_t { pending, in_progress, done, failed };

using fencing_token = uint64_t;          // 0 = unset; nonzero monotonic
using task_id       = std::string;       // SM-assigned, e.g. "t/0001"

struct task_spec {
    task_type type = task_type::plan;
    std::string title;
    std::string prompt;
    std::vector<task_id> requires_deps;
    std::optional<task_id> parent_id;
    std::optional<std::string> branch_base;            // for implement/merge
    std::optional<std::string> implement_branch_sha;   // for merge tasks
    int depth = 0;
};

struct task_record {
    task_id id;
    task_spec spec;
    task_status status = task_status::pending;
    std::string owner_agent;          // empty = unowned
    fencing_token owner_token = 0;    // 0 = unowned
    int64_t heartbeat_unix = 0;       // last heartbeat wall-clock
    std::optional<std::string> result_branch;
    std::optional<std::string> result_summary;
    std::optional<std::string> fail_reason;
};

struct main_lock_state {
    std::string holder_agent;     // empty = unheld
    task_id holder_merge_task;    // task whose lease backs this lock
    fencing_token lock_token = 0; // monotonic
};

// ----- Requests and responses -----

struct request_base {
    uint64_t serial = 0;
    std::string agent_id;
};

// Generic error envelope shared by every response. errcode == 0 means OK.
enum class errc : int8_t {
    ok = 0,
    fenced = 1,
    not_found = 2,
    not_eligible = 3,    // claim found nothing (also encoded by `none` in claim_resp)
    depth_exceeded = 4,
    busy = 5,            // main_lock_acquire when held by someone else
    invalid = 6,         // malformed request, missing fields, etc.
};

struct response_base {
    uint64_t serial = 0;
    errc errcode = errc::ok;
    std::string errmsg;  // human-readable; only set for errc != ok
};

// task_create: create a new task. Root tasks (no creator_task) are depth 0.
// Recursive calls (creator_task set) compute depth = creator.depth + 1; rejected
// if > 5 (constant DEPTH_CAP). creator_token must match the creator task's
// owner_token.
struct task_create_request : request_base {
    task_spec spec;
    std::optional<task_id> creator_task;
    std::optional<fencing_token> creator_token;
};
struct task_create_response : response_base {
    task_id id;  // assigned by SM; empty on error
};

// task_claim: returns the first eligible task. eligibility = requires all done
// AND (status==pending OR (in_progress AND lease expired)).
struct task_claim_request : request_base {
    std::optional<task_type> prefer_type;
};
struct task_claim_response : response_base {
    bool none = false;  // true if no eligible task; id/spec/token unset
    task_id id;
    task_spec spec;
    fencing_token token = 0;
    // Swarm-halt signal: set when none==true because the swarm is halted
    // (a task previously called task_fail). The agent loop should log the
    // cause and exit instead of polling. Cleared by /swarm_resume.
    bool halted = false;
    std::optional<task_id> halted_by;
    std::optional<std::string> halted_reason;
};

struct task_heartbeat_request : request_base {
    task_id id;
    fencing_token token = 0;
};
struct task_heartbeat_response : response_base {};

struct task_complete_request : request_base {
    task_id id;
    fencing_token token = 0;
    std::optional<std::string> result_branch;
    std::string result_summary;
    std::vector<task_spec> new_children;  // recursive decomposition payload
};
struct task_complete_response : response_base {
    std::optional<task_id> merge_task_id;  // present iff this was an implement task
    std::vector<task_id> child_ids;        // assigned IDs for new_children, in order
};

struct task_fail_request : request_base {
    task_id id;
    fencing_token token = 0;
    std::string reason;
};
struct task_fail_response : response_base {};

struct task_summary {
    task_id id;
    task_type type;
    std::string title;
    task_status status;
    std::string owner_agent;
    std::optional<std::string> result_branch;
};

struct task_list_request : request_base {
    std::optional<task_status> filter_status;
    bool only_mine = false;  // if true, restrict to tasks owned by agent_id
};
struct task_list_response : response_base {
    std::vector<task_summary> tasks;
};

struct main_lock_acquire_request : request_base {
    task_id merge_task;
    fencing_token merge_token = 0;
};
struct main_lock_acquire_response : response_base {
    fencing_token lock_token = 0;
};

struct main_lock_release_request : request_base {
    fencing_token lock_token = 0;
};
struct main_lock_release_response : response_base {};

// swarm_resume: human-triggered RPC that clears the halt flag. No fencing —
// this is operator action. Returns ok even if the swarm wasn't halted.
struct swarm_resume_request : request_base {};
struct swarm_resume_response : response_base {
    bool was_halted = false;  // whether the flag was actually cleared
};

using request = std::variant<
    task_create_request,
    task_claim_request,
    task_heartbeat_request,
    task_complete_request,
    task_fail_request,
    task_list_request,
    main_lock_acquire_request,
    main_lock_release_request,
    swarm_resume_request
>;

using response = std::variant<
    task_create_response,
    task_claim_response,
    task_heartbeat_response,
    task_complete_response,
    task_fail_response,
    task_list_response,
    main_lock_acquire_response,
    main_lock_release_response,
    swarm_resume_response
>;

// Constants
constexpr int DEPTH_CAP = 5;
constexpr int64_t LEASE_WINDOW_SECONDS = 45;

}  // namespace tmgr

// nlohmann::json hooks. Keep declarations here so consumers only need
// `#include "tm.hh"` (and `#include <nlohmann/json.hpp>` for json itself).
namespace nlohmann { template <typename, typename> struct adl_serializer; }

#include <nlohmann/json_fwd.hpp>

namespace tmgr {

// Tag types are serialized as strings.
void to_json(nlohmann::json&, task_type);
void from_json(const nlohmann::json&, task_type&);
void to_json(nlohmann::json&, task_status);
void from_json(const nlohmann::json&, task_status&);
void to_json(nlohmann::json&, errc);
void from_json(const nlohmann::json&, errc&);

void to_json(nlohmann::json&, const task_spec&);
void from_json(const nlohmann::json&, task_spec&);

void to_json(nlohmann::json&, const task_summary&);

// Request parsers (not via to_json since requests come in over the wire and
// we want explicit field validation).
//
// parse_request: dispatched on the URL path; returns a fully-populated
// `request` variant or throws std::runtime_error on malformed input.
request parse_request(std::string_view path, const nlohmann::json& body);

// Serialize any response to JSON.
nlohmann::json to_json_response(const response&);

// Serialize a request to its body JSON (the inverse of parse_request).
// The returned object is what parse_request expects as its `body` argument.
nlohmann::json to_json_request_body(const request&);

// Returns the URL path tag for a request variant, e.g. "/task_create".
std::string_view request_path(const request&);

}  // namespace tmgr
