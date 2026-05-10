// pset4/tm-tests.cc - unit tests for tm SM

#include "tm.hh"
#include <nlohmann/json.hpp>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>

// Tiny test framework: count tests; exit nonzero if any failed.
static int tests_run = 0;
static int tests_failed = 0;

#define RUN_TEST(name) do { \
    ++tests_run; \
    std::printf("RUN  %s\n", #name); \
    try { \
        name(); \
        std::printf("OK   %s\n", #name); \
    } catch (const std::exception& e) { \
        ++tests_failed; \
        std::printf("FAIL %s: %s\n", #name, e.what()); \
    } catch (...) { \
        ++tests_failed; \
        std::printf("FAIL %s: unknown exception\n", #name); \
    } \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        throw std::runtime_error("assertion failed: " #cond); \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if (!((a) == (b))) { \
        throw std::runtime_error("assertion failed: " #a " == " #b); \
    } \
} while (0)

// ----- tm types -----

void test_task_spec_default() {
    tmgr::task_spec s;
    ASSERT(s.type == tmgr::task_type::plan);
    ASSERT_EQ(s.title, "");
    ASSERT_EQ(s.depth, 0);
    ASSERT(s.requires_deps.empty());
    ASSERT(!s.parent_id.has_value());
}

#include "task_manager_db.hh"

void test_db_starts_empty() {
    tmgr::task_manager_db db;
    ASSERT(db.tasks().empty());
    ASSERT(db.task_order().empty());
    ASSERT(db.main_lock().holder_agent.empty());
    ASSERT_EQ(db.main_lock().lock_token, 0u);
}

// Helper: make a request and unwrap the response of expected type.
template <typename Resp, typename Req>
Resp call(tmgr::task_manager_db& db, Req r, int64_t now = 1000) {
    auto resp = db.process_req(r, now);
    return std::get<Resp>(std::move(resp));
}

void test_task_create_root() {
    tmgr::task_manager_db db;
    tmgr::task_create_request req;
    req.serial = 1;
    req.agent_id = "human";
    req.spec.type = tmgr::task_type::plan;
    req.spec.title = "build calc";
    req.spec.prompt = "Build a CLI calculator";

    auto resp = call<tmgr::task_create_response>(db, req);
    ASSERT(resp.errcode == tmgr::errc::ok);
    ASSERT_EQ(resp.id, "t/0001");
    ASSERT_EQ(db.tasks().size(), 1u);
    ASSERT(db.tasks().count(resp.id) == 1);
    ASSERT_EQ(db.tasks().at(resp.id).spec.depth, 0);
    ASSERT_EQ(db.tasks().at(resp.id).status, tmgr::task_status::pending);
}

void test_task_create_recursive_requires_creator_token() {
    tmgr::task_manager_db db;
    // Create root
    tmgr::task_create_request root;
    root.serial = 1; root.agent_id = "human"; root.spec.title = "root";
    auto root_resp = call<tmgr::task_create_response>(db, root);

    // Recursive create without creator_token → invalid
    tmgr::task_create_request child;
    child.serial = 2; child.agent_id = "agent-a"; child.spec.title = "child";
    child.creator_task = root_resp.id;
    // creator_token deliberately absent
    auto child_resp = call<tmgr::task_create_response>(db, child);
    ASSERT(child_resp.errcode == tmgr::errc::invalid);
    ASSERT_EQ(db.tasks().size(), 1u);
}

void test_task_create_depth_cap() {
    tmgr::task_manager_db db;
    // Manually construct a task at depth 5 and try to recurse from it.
    // Easiest path: chain root → c1 → c2 → ... → c5 by claiming each in
    // turn. But claim isn't implemented yet, so we synthesize via direct
    // depth on the spec is impossible (root is forced to depth 0).
    //
    // For now test only the root-depth-zero case + that recursive-without-
    // valid-creator fails. Full depth-cap test deferred to after claim
    // works (Task 5).
}

void test_task_claim_finds_pending() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c;
    c.serial = 1; c.agent_id = "human"; c.spec.title = "p";
    auto cr = call<tmgr::task_create_response>(db, c);

    tmgr::task_claim_request q;
    q.serial = 2; q.agent_id = "agent-a";
    auto qr = call<tmgr::task_claim_response>(db, q, /*now=*/1000);
    ASSERT(qr.errcode == tmgr::errc::ok);
    ASSERT(!qr.none);
    ASSERT_EQ(qr.id, cr.id);
    ASSERT(qr.token != 0u);
    ASSERT_EQ(db.tasks().at(qr.id).status, tmgr::task_status::in_progress);
    ASSERT_EQ(db.tasks().at(qr.id).owner_agent, "agent-a");
    ASSERT_EQ(db.tasks().at(qr.id).heartbeat_unix, 1000);
}

void test_task_claim_none_when_empty() {
    tmgr::task_manager_db db;
    tmgr::task_claim_request q; q.serial = 1; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q);
    ASSERT(qr.errcode == tmgr::errc::ok);
    ASSERT(qr.none);
}

void test_task_claim_skips_in_progress_with_fresh_lease() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c; c.serial = 1; c.spec.title = "p";
    auto cr = call<tmgr::task_create_response>(db, c);

    // Agent A claims at t=1000.
    tmgr::task_claim_request qa; qa.serial = 2; qa.agent_id = "agent-a";
    auto qar = call<tmgr::task_claim_response>(db, qa, 1000);
    ASSERT(!qar.none);

    // Agent B claims at t=1030 (within 45s lease) → none.
    tmgr::task_claim_request qb; qb.serial = 3; qb.agent_id = "agent-b";
    auto qbr = call<tmgr::task_claim_response>(db, qb, 1030);
    ASSERT(qbr.none);
    // Owner unchanged.
    ASSERT_EQ(db.tasks().at(cr.id).owner_agent, "agent-a");
}

void test_task_claim_takes_over_expired_lease() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c; c.serial = 1; c.spec.title = "p";
    auto cr = call<tmgr::task_create_response>(db, c);

    // A claims, then disappears.
    tmgr::task_claim_request qa; qa.serial = 2; qa.agent_id = "agent-a";
    auto qar = call<tmgr::task_claim_response>(db, qa, 1000);
    auto a_token = qar.token;

    // B claims at t=1050 (>45s past A's heartbeat) → takes over.
    tmgr::task_claim_request qb; qb.serial = 3; qb.agent_id = "agent-b";
    auto qbr = call<tmgr::task_claim_response>(db, qb, 1050);
    ASSERT(!qbr.none);
    ASSERT_EQ(qbr.id, cr.id);
    ASSERT(qbr.token != a_token);
    ASSERT_EQ(db.tasks().at(cr.id).owner_agent, "agent-b");
    ASSERT_EQ(db.tasks().at(cr.id).heartbeat_unix, 1050);
}

void test_task_claim_respects_requires() {
    tmgr::task_manager_db db;
    tmgr::task_create_request a; a.serial = 1; a.spec.title = "a";
    auto ar = call<tmgr::task_create_response>(db, a);

    tmgr::task_create_request b; b.serial = 2; b.spec.title = "b";
    b.spec.requires_deps = { ar.id };
    auto br = call<tmgr::task_create_response>(db, b);

    // Neither A nor B is done, so claim returns A first (creation order).
    tmgr::task_claim_request q; q.serial = 3; q.agent_id = "agent-a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);
    ASSERT(!qr.none);
    ASSERT_EQ(qr.id, ar.id);

    // B is gated on A being done; even after we claim A, B isn't claimable.
    tmgr::task_claim_request q2; q2.serial = 4; q2.agent_id = "agent-b";
    auto qr2 = call<tmgr::task_claim_response>(db, q2, 1001);
    ASSERT(qr2.none);
}

void test_heartbeat_updates_timestamp() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c; c.serial = 1; c.spec.title = "p";
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_heartbeat_request h;
    h.serial = 3; h.agent_id = "a"; h.id = qr.id; h.token = qr.token;
    auto hr = call<tmgr::task_heartbeat_response>(db, h, 1010);
    ASSERT(hr.errcode == tmgr::errc::ok);
    ASSERT_EQ(db.tasks().at(qr.id).heartbeat_unix, 1010);
}

void test_heartbeat_fenced_on_wrong_token() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c; c.serial = 1; c.spec.title = "p";
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_heartbeat_request h;
    h.serial = 3; h.agent_id = "a"; h.id = qr.id; h.token = qr.token + 99;
    auto hr = call<tmgr::task_heartbeat_response>(db, h, 1010);
    ASSERT(hr.errcode == tmgr::errc::fenced);
    ASSERT_EQ(db.tasks().at(qr.id).heartbeat_unix, 1000);  // unchanged
}

void test_complete_marks_done_clears_owner() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c; c.serial = 1; c.spec.title = "p";
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_complete_request done;
    done.serial = 3; done.agent_id = "a"; done.id = qr.id;
    done.token = qr.token; done.result_summary = "summary";
    auto dr = call<tmgr::task_complete_response>(db, done, 1005);
    ASSERT(dr.errcode == tmgr::errc::ok);
    ASSERT_EQ(db.tasks().at(qr.id).status, tmgr::task_status::done);
    ASSERT_EQ(db.tasks().at(qr.id).owner_agent, "");
    ASSERT_EQ(db.tasks().at(qr.id).owner_token, 0u);
    ASSERT(db.tasks().at(qr.id).result_summary.has_value());
    ASSERT(!dr.merge_task_id.has_value());  // plan task, no merge
}

void test_complete_implement_synthesizes_merge() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c;
    c.serial = 1; c.spec.title = "imp";
    c.spec.type = tmgr::task_type::implement;
    c.spec.branch_base = "main";
    auto cr = call<tmgr::task_create_response>(db, c);

    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_complete_request done;
    done.serial = 3; done.agent_id = "a"; done.id = qr.id;
    done.token = qr.token; done.result_summary = "shipped";
    done.result_branch = "abc1234";
    auto dr = call<tmgr::task_complete_response>(db, done, 1005);
    ASSERT(dr.errcode == tmgr::errc::ok);
    ASSERT(dr.merge_task_id.has_value());

    auto& mt = db.tasks().at(*dr.merge_task_id);
    ASSERT(mt.spec.type == tmgr::task_type::merge);
    ASSERT_EQ(mt.spec.requires_deps.size(), 1u);
    ASSERT_EQ(mt.spec.requires_deps[0], qr.id);
    ASSERT(mt.spec.implement_branch_sha.has_value());
    ASSERT_EQ(*mt.spec.implement_branch_sha, "abc1234");
}

void test_complete_with_new_children_appends_them() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c; c.serial = 1; c.spec.title = "plan";
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_complete_request done;
    done.serial = 3; done.agent_id = "a"; done.id = qr.id;
    done.token = qr.token; done.result_summary = "decomposed";
    tmgr::task_spec child;
    child.type = tmgr::task_type::implement;
    child.title = "child-1";
    child.branch_base = "main";
    done.new_children.push_back(child);
    auto dr = call<tmgr::task_complete_response>(db, done);
    ASSERT(dr.errcode == tmgr::errc::ok);
    ASSERT_EQ(dr.child_ids.size(), 1u);
    ASSERT(db.tasks().count(dr.child_ids[0]) == 1);
    ASSERT_EQ(db.tasks().at(dr.child_ids[0]).spec.depth, 1);
}

void test_fail_marks_failed_with_reason() {
    tmgr::task_manager_db db;
    tmgr::task_create_request c; c.serial = 1; c.spec.title = "p";
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_fail_request f;
    f.serial = 3; f.agent_id = "a"; f.id = qr.id;
    f.token = qr.token; f.reason = "boom";
    auto fr = call<tmgr::task_fail_response>(db, f);
    ASSERT(fr.errcode == tmgr::errc::ok);
    ASSERT_EQ(db.tasks().at(qr.id).status, tmgr::task_status::failed);
    ASSERT(db.tasks().at(qr.id).fail_reason.has_value());
    ASSERT_EQ(*db.tasks().at(qr.id).fail_reason, "boom");
    ASSERT_EQ(db.tasks().at(qr.id).owner_agent, "");
}

void test_task_list_filter_by_status() {
    tmgr::task_manager_db db;
    for (int i = 0; i < 3; ++i) {
        tmgr::task_create_request c;
        c.serial = i; c.spec.title = std::format("t{}", i);
        call<tmgr::task_create_response>(db, c);
    }
    // Claim and complete the first one.
    tmgr::task_claim_request q; q.serial = 10; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);
    tmgr::task_complete_request d;
    d.serial = 11; d.agent_id = "a"; d.id = qr.id; d.token = qr.token;
    d.result_summary = "ok";
    call<tmgr::task_complete_response>(db, d, 1001);

    tmgr::task_list_request l; l.serial = 12; l.agent_id = "anyone";
    l.filter_status = tmgr::task_status::pending;
    auto lr = call<tmgr::task_list_response>(db, l);
    ASSERT(lr.errcode == tmgr::errc::ok);
    ASSERT_EQ(lr.tasks.size(), 2u);
    for (auto& s : lr.tasks) ASSERT(s.status == tmgr::task_status::pending);
}

void test_main_lock_acquire_and_release() {
    tmgr::task_manager_db db;
    // Need an in_progress merge task first.
    tmgr::task_create_request c;
    c.serial = 1; c.spec.type = tmgr::task_type::merge; c.spec.title = "m";
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::main_lock_acquire_request acq;
    acq.serial = 3; acq.agent_id = "a";
    acq.merge_task = qr.id; acq.merge_token = qr.token;
    auto ar = call<tmgr::main_lock_acquire_response>(db, acq, 1001);
    ASSERT(ar.errcode == tmgr::errc::ok);
    ASSERT(ar.lock_token != 0u);

    // Re-acquire by another agent: busy.
    tmgr::main_lock_acquire_request acq2;
    acq2.serial = 4; acq2.agent_id = "b";
    acq2.merge_task = qr.id; acq2.merge_token = qr.token;  // wrong agent but
                                                            // we test "busy"
                                                            // path; agent_id
                                                            // mismatch is
                                                            // unenforced for
                                                            // lock.
    auto ar2 = call<tmgr::main_lock_acquire_response>(db, acq2, 1002);
    ASSERT(ar2.errcode == tmgr::errc::busy);

    // Release.
    tmgr::main_lock_release_request rel;
    rel.serial = 5; rel.agent_id = "a"; rel.lock_token = ar.lock_token;
    auto rr = call<tmgr::main_lock_release_response>(db, rel);
    ASSERT(rr.errcode == tmgr::errc::ok);
    ASSERT_EQ(db.main_lock().lock_token, 0u);
}

void test_main_lock_steals_on_stale_merge_lease() {
    tmgr::task_manager_db db;
    // Create + claim merge task A, acquire lock. Then stop heartbeating;
    // a fresh agent B calls main_lock_acquire after lease expires.
    tmgr::task_create_request c;
    c.serial = 1; c.spec.type = tmgr::task_type::merge; c.spec.title = "m";
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request qa; qa.serial = 2; qa.agent_id = "a";
    auto qar = call<tmgr::task_claim_response>(db, qa, 1000);

    tmgr::main_lock_acquire_request acqa;
    acqa.serial = 3; acqa.agent_id = "a";
    acqa.merge_task = qar.id; acqa.merge_token = qar.token;
    auto ara = call<tmgr::main_lock_acquire_response>(db, acqa, 1001);
    ASSERT(ara.errcode == tmgr::errc::ok);

    // Time passes; A's heartbeat is stale. B claims (taking over).
    tmgr::task_claim_request qb; qb.serial = 10; qb.agent_id = "b";
    auto qbr = call<tmgr::task_claim_response>(db, qb, 1100);
    ASSERT(!qbr.none);
    ASSERT_EQ(qbr.id, qar.id);

    // B acquires the lock; previous holder's lease was stale.
    tmgr::main_lock_acquire_request acqb;
    acqb.serial = 11; acqb.agent_id = "b";
    acqb.merge_task = qbr.id; acqb.merge_token = qbr.token;
    auto arb = call<tmgr::main_lock_acquire_response>(db, acqb, 1101);
    ASSERT(arb.errcode == tmgr::errc::ok);
    ASSERT(arb.lock_token != ara.lock_token);
    ASSERT_EQ(db.main_lock().holder_agent, "b");
}

void test_json_task_spec_roundtrip() {
    tmgr::task_spec original;
    original.type = tmgr::task_type::implement;
    original.title = "build calc";
    original.prompt = "Build a CLI calculator";
    original.requires_deps = { "t/0001", "t/0002" };
    original.branch_base = "main";
    original.depth = 2;

    nlohmann::json j = original;
    tmgr::task_spec back = j.get<tmgr::task_spec>();
    ASSERT(back.type == original.type);
    ASSERT_EQ(back.title, original.title);
    ASSERT_EQ(back.prompt, original.prompt);
    ASSERT_EQ(back.requires_deps.size(), 2u);
    ASSERT(back.branch_base == original.branch_base);
    ASSERT_EQ(back.depth, 2);
}

void test_json_parse_task_create_request() {
    auto j = nlohmann::json::parse(R"({
        "agent_id": "agent-a",
        "serial": 42,
        "spec": {
            "type": "plan",
            "title": "build calc",
            "prompt": "..."
        }
    })");
    auto req = tmgr::parse_request("/task_create", j);
    auto& tc = std::get<tmgr::task_create_request>(req);
    ASSERT_EQ(tc.agent_id, "agent-a");
    ASSERT_EQ(tc.serial, 42u);
    ASSERT_EQ(tc.spec.title, "build calc");
}

// Round-trip: serialize a request, parse it back, verify equality on the
// fields the SM cares about. Locks the JSONL log format to parse_request.
void test_json_request_roundtrip_all_variants() {
    using nlohmann::json;
    auto roundtrip = [](tmgr::request r) {
        auto path = tmgr::request_path(r);
        json body = tmgr::to_json_request_body(r);
        return tmgr::parse_request(path, body);
    };

    {
        tmgr::task_create_request c;
        c.agent_id = "agent-a"; c.serial = 1;
        c.spec.type = tmgr::task_type::implement;
        c.spec.title = "imp"; c.spec.prompt = "do thing";
        c.spec.requires_deps = {"t/0001"};
        c.spec.branch_base = "main";
        c.spec.depth = 2;
        c.creator_task = "t/0001";
        c.creator_token = 42;
        auto back = std::get<tmgr::task_create_request>(roundtrip(c));
        ASSERT_EQ(back.agent_id, "agent-a");
        ASSERT_EQ(back.serial, 1u);
        ASSERT(back.spec.type == tmgr::task_type::implement);
        ASSERT_EQ(back.spec.title, "imp");
        ASSERT_EQ(back.spec.requires_deps.size(), 1u);
        ASSERT(back.spec.branch_base.has_value());
        ASSERT_EQ(*back.spec.branch_base, "main");
        ASSERT_EQ(back.spec.depth, 2);
        ASSERT(back.creator_task.has_value());
        ASSERT_EQ(*back.creator_task, "t/0001");
        ASSERT(back.creator_token.has_value());
        ASSERT_EQ(*back.creator_token, 42u);
    }
    {
        tmgr::task_claim_request c;
        c.agent_id = "a"; c.serial = 5;
        c.prefer_type = tmgr::task_type::merge;
        auto back = std::get<tmgr::task_claim_request>(roundtrip(c));
        ASSERT(back.prefer_type.has_value());
        ASSERT(*back.prefer_type == tmgr::task_type::merge);
    }
    {
        tmgr::task_heartbeat_request h;
        h.agent_id = "a"; h.serial = 6; h.id = "t/0007"; h.token = 99;
        auto back = std::get<tmgr::task_heartbeat_request>(roundtrip(h));
        ASSERT_EQ(back.id, "t/0007");
        ASSERT_EQ(back.token, 99u);
    }
    {
        tmgr::task_complete_request d;
        d.agent_id = "a"; d.serial = 7; d.id = "t/0001"; d.token = 33;
        d.result_summary = "ok"; d.result_branch = "abc1234";
        tmgr::task_spec child; child.type = tmgr::task_type::implement;
        child.title = "kid"; child.branch_base = "main";
        d.new_children.push_back(child);
        auto back = std::get<tmgr::task_complete_request>(roundtrip(d));
        ASSERT_EQ(back.id, "t/0001");
        ASSERT_EQ(back.result_summary, "ok");
        ASSERT(back.result_branch.has_value());
        ASSERT_EQ(*back.result_branch, "abc1234");
        ASSERT_EQ(back.new_children.size(), 1u);
        ASSERT(back.new_children[0].type == tmgr::task_type::implement);
    }
    {
        tmgr::task_fail_request f;
        f.agent_id = "a"; f.serial = 8; f.id = "t/0002"; f.token = 12;
        f.reason = "boom";
        auto back = std::get<tmgr::task_fail_request>(roundtrip(f));
        ASSERT_EQ(back.reason, "boom");
    }
    {
        tmgr::task_list_request l;
        l.agent_id = "a"; l.serial = 9;
        l.filter_status = tmgr::task_status::done; l.only_mine = true;
        auto back = std::get<tmgr::task_list_request>(roundtrip(l));
        ASSERT(back.filter_status.has_value());
        ASSERT(*back.filter_status == tmgr::task_status::done);
        ASSERT(back.only_mine);
    }
    {
        tmgr::main_lock_acquire_request a;
        a.agent_id = "a"; a.serial = 10; a.merge_task = "t/0003"; a.merge_token = 77;
        auto back = std::get<tmgr::main_lock_acquire_request>(roundtrip(a));
        ASSERT_EQ(back.merge_task, "t/0003");
        ASSERT_EQ(back.merge_token, 77u);
    }
    {
        tmgr::main_lock_release_request r;
        r.agent_id = "a"; r.serial = 11; r.lock_token = 88;
        auto back = std::get<tmgr::main_lock_release_request>(roundtrip(r));
        ASSERT_EQ(back.lock_token, 88u);
    }
}

void test_json_response_serialization() {
    tmgr::task_create_response r;
    r.serial = 7;
    r.errcode = tmgr::errc::ok;
    r.id = "t/0001";
    nlohmann::json j = tmgr::to_json_response(tmgr::response{r});
    ASSERT_EQ(j["ok"].get<bool>(), true);
    ASSERT_EQ(j["serial"].get<uint64_t>(), 7u);
    ASSERT_EQ(j["task_id"].get<std::string>(), "t/0001");
}

// ----- new behavior: fail halts swarm, hand-off via complete -----

void test_fail_halts_swarm() {
    // Two pending tasks; A gets claimed and failed. After that, even though
    // B is eligible, claim must return none + halted=true.
    tmgr::task_manager_db db;
    tmgr::task_create_request a; a.serial = 1; a.spec.title = "a";
    auto ar = call<tmgr::task_create_response>(db, a);
    tmgr::task_create_request b; b.serial = 2; b.spec.title = "b";
    auto br = call<tmgr::task_create_response>(db, b);

    tmgr::task_claim_request q; q.serial = 3; q.agent_id = "agent-a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);
    ASSERT_EQ(qr.id, ar.id);

    tmgr::task_fail_request f;
    f.serial = 4; f.agent_id = "agent-a"; f.id = qr.id;
    f.token = qr.token; f.reason = "ABANDON: contradictory prompt";
    auto fr = call<tmgr::task_fail_response>(db, f);
    ASSERT(fr.errcode == tmgr::errc::ok);
    ASSERT(db.swarm_halted());

    // B is otherwise eligible (pending, no requires) but the swarm is halted.
    tmgr::task_claim_request q2; q2.serial = 5; q2.agent_id = "agent-b";
    auto q2r = call<tmgr::task_claim_response>(db, q2, 1001);
    ASSERT(q2r.none);
    ASSERT(q2r.halted);
    ASSERT(q2r.halted_by.has_value());
    ASSERT_EQ(*q2r.halted_by, ar.id);
    ASSERT(q2r.halted_reason.has_value());
    ASSERT_EQ(*q2r.halted_reason, "ABANDON: contradictory prompt");
    ASSERT_EQ(db.tasks().at(br.id).status, tmgr::task_status::pending);
}

void test_fail_does_not_disturb_running_tasks() {
    // A and B both claimed; A fails. B's heartbeat and complete still work.
    tmgr::task_manager_db db;
    tmgr::task_create_request a; a.serial = 1; a.spec.title = "a";
    auto ar = call<tmgr::task_create_response>(db, a);
    tmgr::task_create_request b; b.serial = 2; b.spec.title = "b";
    auto br = call<tmgr::task_create_response>(db, b);

    tmgr::task_claim_request qa; qa.serial = 3; qa.agent_id = "agent-a";
    auto qar = call<tmgr::task_claim_response>(db, qa, 1000);
    tmgr::task_claim_request qb; qb.serial = 4; qb.agent_id = "agent-b";
    auto qbr = call<tmgr::task_claim_response>(db, qb, 1001);
    ASSERT_EQ(qar.id, ar.id);
    ASSERT_EQ(qbr.id, br.id);

    // A fails -> swarm halts.
    tmgr::task_fail_request f;
    f.serial = 5; f.agent_id = "agent-a"; f.id = qar.id;
    f.token = qar.token; f.reason = "ABANDON: dead end";
    call<tmgr::task_fail_response>(db, f);
    ASSERT(db.swarm_halted());

    // B's heartbeat continues to work.
    tmgr::task_heartbeat_request h;
    h.serial = 6; h.agent_id = "agent-b"; h.id = qbr.id; h.token = qbr.token;
    auto hr = call<tmgr::task_heartbeat_response>(db, h, 1010);
    ASSERT(hr.errcode == tmgr::errc::ok);
    ASSERT_EQ(db.tasks().at(qbr.id).heartbeat_unix, 1010);

    // B can still complete.
    tmgr::task_complete_request d;
    d.serial = 7; d.agent_id = "agent-b"; d.id = qbr.id; d.token = qbr.token;
    d.result_summary = "ok";
    auto dr = call<tmgr::task_complete_response>(db, d, 1011);
    ASSERT(dr.errcode == tmgr::errc::ok);
    ASSERT_EQ(db.tasks().at(qbr.id).status, tmgr::task_status::done);
}

void test_swarm_resume_unblocks_claim() {
    tmgr::task_manager_db db;
    tmgr::task_create_request a; a.serial = 1; a.spec.title = "a";
    auto ar = call<tmgr::task_create_response>(db, a);
    tmgr::task_create_request b; b.serial = 2; b.spec.title = "b";
    auto br = call<tmgr::task_create_response>(db, b);

    tmgr::task_claim_request qa; qa.serial = 3; qa.agent_id = "agent-a";
    auto qar = call<tmgr::task_claim_response>(db, qa, 1000);
    tmgr::task_fail_request f;
    f.serial = 4; f.agent_id = "agent-a"; f.id = qar.id;
    f.token = qar.token; f.reason = "ABANDON: stuck";
    call<tmgr::task_fail_response>(db, f);
    ASSERT(db.swarm_halted());

    // Resume.
    tmgr::swarm_resume_request sr; sr.serial = 5; sr.agent_id = "human";
    auto srr = call<tmgr::swarm_resume_response>(db, sr);
    ASSERT(srr.errcode == tmgr::errc::ok);
    ASSERT(srr.was_halted);
    ASSERT(!db.swarm_halted());

    // B is now claimable again.
    tmgr::task_claim_request q2; q2.serial = 6; q2.agent_id = "agent-b";
    auto q2r = call<tmgr::task_claim_response>(db, q2, 1010);
    ASSERT(!q2r.none);
    ASSERT_EQ(q2r.id, br.id);
}

void test_swarm_resume_when_not_halted_is_noop() {
    tmgr::task_manager_db db;
    tmgr::swarm_resume_request sr; sr.serial = 1; sr.agent_id = "human";
    auto srr = call<tmgr::swarm_resume_response>(db, sr);
    ASSERT(srr.errcode == tmgr::errc::ok);
    ASSERT(!srr.was_halted);
}

void test_complete_implement_without_branch_skips_merge() {
    // The new gating: an implement that completes without a result_branch
    // (e.g. because it's handing off via new_children, or it's a
    // verify-only task) must NOT auto-synthesize a merge task.
    tmgr::task_manager_db db;
    tmgr::task_create_request c;
    c.serial = 1; c.spec.title = "imp-no-branch";
    c.spec.type = tmgr::task_type::implement;
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "agent-a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_complete_request done;
    done.serial = 3; done.agent_id = "agent-a"; done.id = qr.id;
    done.token = qr.token;
    done.result_summary = "scope too large; decomposed instead";
    // result_branch deliberately absent
    tmgr::task_spec child;
    child.type = tmgr::task_type::implement;
    child.title = "smaller-piece";
    child.branch_base = "main";
    done.new_children.push_back(child);

    auto dr = call<tmgr::task_complete_response>(db, done, 1005);
    ASSERT(dr.errcode == tmgr::errc::ok);
    ASSERT(!dr.merge_task_id.has_value());  // no merge synthesized
    ASSERT_EQ(dr.child_ids.size(), 1u);
    // Sanity: only the parent + one child exist; no extra merge task.
    ASSERT_EQ(db.tasks().size(), 2u);
}

void test_complete_implement_with_empty_branch_skips_merge() {
    // Wire-level edge: result_branch present but empty string. Treat as
    // "no branch" — same as absent.
    tmgr::task_manager_db db;
    tmgr::task_create_request c;
    c.serial = 1; c.spec.title = "imp-empty";
    c.spec.type = tmgr::task_type::implement;
    auto cr = call<tmgr::task_create_response>(db, c);
    tmgr::task_claim_request q; q.serial = 2; q.agent_id = "a";
    auto qr = call<tmgr::task_claim_response>(db, q, 1000);

    tmgr::task_complete_request done;
    done.serial = 3; done.agent_id = "a"; done.id = qr.id;
    done.token = qr.token; done.result_summary = "no branch";
    done.result_branch = "";  // empty
    auto dr = call<tmgr::task_complete_response>(db, done, 1005);
    ASSERT(dr.errcode == tmgr::errc::ok);
    ASSERT(!dr.merge_task_id.has_value());
}

void test_json_swarm_resume_roundtrip() {
    using nlohmann::json;
    tmgr::swarm_resume_request s;
    s.agent_id = "human"; s.serial = 99;
    json body = tmgr::to_json_request_body(s);
    auto path = tmgr::request_path(s);
    ASSERT_EQ(std::string(path), "/swarm_resume");
    auto back = std::get<tmgr::swarm_resume_request>(tmgr::parse_request(path, body));
    ASSERT_EQ(back.agent_id, "human");
    ASSERT_EQ(back.serial, 99u);
}

void test_json_complete_request_without_branch_roundtrip() {
    // Re-roundtrip a task_complete_request with result_branch absent to
    // make sure the wire format handles it cleanly.
    using nlohmann::json;
    tmgr::task_complete_request d;
    d.agent_id = "a"; d.serial = 1; d.id = "t/0001"; d.token = 5;
    d.result_summary = "decomposed";
    // result_branch deliberately absent
    tmgr::task_spec child;
    child.type = tmgr::task_type::plan;
    child.title = "kid";
    d.new_children.push_back(child);

    json body = tmgr::to_json_request_body(d);
    ASSERT(!body.contains("result_branch"));  // absent on the wire
    auto back = std::get<tmgr::task_complete_request>(tmgr::parse_request("/task_complete", body));
    ASSERT(!back.result_branch.has_value());
    ASSERT_EQ(back.new_children.size(), 1u);
}

void test_json_claim_response_halted_serializes() {
    // task_claim_response with halted=true should put halted_by and
    // halted_reason on the wire so the loop can display them.
    tmgr::task_claim_response c;
    c.serial = 1; c.errcode = tmgr::errc::ok;
    c.none = true; c.halted = true;
    c.halted_by = "t/0007";
    c.halted_reason = "ABANDON: contradiction";
    nlohmann::json j = tmgr::to_json_response(tmgr::response{c});
    ASSERT_EQ(j["none"].get<bool>(), true);
    ASSERT_EQ(j["halted"].get<bool>(), true);
    ASSERT_EQ(j["halted_by"].get<std::string>(), "t/0007");
    ASSERT_EQ(j["halted_reason"].get<std::string>(), "ABANDON: contradiction");
}

int main() {
    RUN_TEST(test_task_spec_default);
    RUN_TEST(test_db_starts_empty);
    RUN_TEST(test_task_create_root);
    RUN_TEST(test_task_create_recursive_requires_creator_token);
    RUN_TEST(test_task_claim_finds_pending);
    RUN_TEST(test_task_claim_none_when_empty);
    RUN_TEST(test_task_claim_skips_in_progress_with_fresh_lease);
    RUN_TEST(test_task_claim_takes_over_expired_lease);
    RUN_TEST(test_task_claim_respects_requires);
    RUN_TEST(test_heartbeat_updates_timestamp);
    RUN_TEST(test_heartbeat_fenced_on_wrong_token);
    RUN_TEST(test_complete_marks_done_clears_owner);
    RUN_TEST(test_complete_implement_synthesizes_merge);
    RUN_TEST(test_complete_with_new_children_appends_them);
    RUN_TEST(test_fail_marks_failed_with_reason);
    RUN_TEST(test_task_list_filter_by_status);
    RUN_TEST(test_main_lock_acquire_and_release);
    RUN_TEST(test_main_lock_steals_on_stale_merge_lease);
    RUN_TEST(test_json_task_spec_roundtrip);
    RUN_TEST(test_json_parse_task_create_request);
    RUN_TEST(test_json_request_roundtrip_all_variants);
    RUN_TEST(test_json_response_serialization);
    RUN_TEST(test_fail_halts_swarm);
    RUN_TEST(test_fail_does_not_disturb_running_tasks);
    RUN_TEST(test_swarm_resume_unblocks_claim);
    RUN_TEST(test_swarm_resume_when_not_halted_is_noop);
    RUN_TEST(test_complete_implement_without_branch_skips_merge);
    RUN_TEST(test_complete_implement_with_empty_branch_skips_merge);
    RUN_TEST(test_json_swarm_resume_roundtrip);
    RUN_TEST(test_json_complete_request_without_branch_roundtrip);
    RUN_TEST(test_json_claim_response_halted_serializes);
    std::printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
