// pset4/tm-paxos-tests.cc - tier-1 sim tests for the replicated SM.
//
// Constructs N pt_paxos_replicas wired together via sim_transport, runs a
// scenario coroutine that drives requests, then checks invariants on the
// resulting replica state. Deterministic per seed.

#include "tm-paxos-internal.hh"
#include <memory>
#include <print>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Owns transports + replicas + their run() background tasks.
struct test_cluster {
    size_t nreplicas;
    random_source rnd;
    std::vector<std::unique_ptr<tmgr::sim_transport<paxos_message>>> transports;
    std::vector<std::unique_ptr<pt_paxos_replica>> replicas;
    // Replicas killed during the run. Mirrors pset3's `failed_replicas` —
    // their DB is stale and gets skipped in the diff check. (Even after
    // restore_replica, we keep them here because they're still behind.)
    std::set<size_t> failed_replicas;

    test_cluster(size_t n, unsigned long seed) : nreplicas(n) {
        rnd.seed(seed);

        for (size_t i = 0; i < n; ++i) {
            transports.emplace_back(
                std::make_unique<tmgr::sim_transport<paxos_message>>(i, n, rnd));
        }
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                if (i == j) continue;
                transports[i]->connect(j, *transports[j]);
            }
        }
        for (size_t i = 0; i < n; ++i) {
            replicas.emplace_back(
                std::make_unique<pt_paxos_replica>(i, *transports[i]));
        }
        for (auto& r : replicas) r->leader_index_ = 0;
    }

    void start() {
        for (auto& r : replicas) {
            r->run().detach();
        }
    }

    // Set baseline message loss rate on every inter-replica channel.
    void set_baseline_loss(double loss) {
        for (size_t i = 0; i < nreplicas; ++i) {
            for (size_t j = 0; j < nreplicas; ++j) {
                if (i == j) continue;
                transports[i]->channel_to(j).set_loss(loss);
            }
        }
    }
};

// Failure-injection helpers. Mirror pset3's kill_replica / restore_replica —
// instead of touching pt_paxos_instance, we manipulate sim_transport channels.
void kill_replica(test_cluster& cluster, size_t r) {
    cluster.failed_replicas.insert(r);
    for (size_t i = 0; i < cluster.nreplicas; ++i) {
        if (i == r) continue;
        cluster.transports[r]->channel_to(i).set_loss(1.0);
        cluster.transports[i]->channel_to(r).set_loss(1.0);
    }
}

void restore_replica(test_cluster& cluster, size_t r, double loss) {
    for (size_t i = 0; i < cluster.nreplicas; ++i) {
        if (i == r) continue;
        cluster.transports[r]->channel_to(i).set_loss(loss);
        cluster.transports[i]->channel_to(r).set_loss(loss);
    }
}

// Failure schedules (ported from pset3, retypled).

// fail_permanent: kill a random non-majority replica permanently.
cot::task<> fail_permanent(test_cluster& cluster) {
    co_await cot::after(2s);
    size_t r = cluster.rnd.uniform(size_t(0), cluster.nreplicas - 1);
    kill_replica(cluster, r);
}

// fail_temporary: leader dies at 2s, recovers at 5s.
cot::task<> fail_temporary(test_cluster& cluster, double loss) {
    co_await cot::after(2s);
    kill_replica(cluster, 0);
    co_await cot::after(3s);
    restore_replica(cluster, 0, loss);
}

// fail_split_brain: link between replicas 0 and 1 severs at 2s. Both can
// still talk to replica 2, but replica 1 wins the election (replica 0 never
// triggers one — leaders don't step down). After that, replica 2 follows
// replica 1, and replica 0 is effectively isolated (its old-round PROPOSEs
// are ignored by replica 2). We mark replica 0 as failed so the invariant
// check exempts its stale state, mirroring the kill_replica convention.
cot::task<> fail_split_brain(test_cluster& cluster) {
    co_await cot::after(2s);
    cluster.transports[0]->channel_to(1).set_loss(1.0);
    cluster.transports[1]->channel_to(0).set_loss(1.0);
    cluster.failed_replicas.insert(0);
    co_return;
}

// Heuristic: pick the replica that thinks it's the leader and has the
// highest probe_round_. Round number monotonically increases through
// elections, so the most-recently-elected leader always wins ties — even
// if an old killed leader still thinks it's leader at a lower round.
size_t find_leader(test_cluster& cluster) {
    size_t best = 0;
    size_t best_round = 0;
    bool found = false;
    for (size_t i = 0; i < cluster.nreplicas; ++i) {
        if (!cluster.replicas[i]->is_leader()) continue;
        if (!found || cluster.replicas[i]->probe_round_ > best_round) {
            best = i;
            best_round = cluster.replicas[i]->probe_round_;
            found = true;
        }
    }
    return best;
}

// Send a request to the current leader, retrying on `busy` (not-leader)
// responses with brief randomized backoff. Returns the final response.
cot::task<tmgr::response> send_to_leader(test_cluster& cluster, tmgr::request req,
                                         random_source& rnd) {
    constexpr size_t max_retries = 5;
    for (size_t attempt = 0; attempt < max_retries; ++attempt) {
        size_t leader = find_leader(cluster);
        auto result = co_await cluster.replicas[leader]->propose_and_apply(req);
        auto& base = std::visit(
            [](auto& v) -> const tmgr::response_base& { return v; }, result.resp);
        if (base.errcode != tmgr::errc::busy) {
            co_return std::move(result.resp);
        }
        co_await cot::after(rnd.uniform(20ms, 80ms));
    }
    // Last shot — return whatever we get.
    size_t leader = find_leader(cluster);
    auto result = co_await cluster.replicas[leader]->propose_and_apply(req);
    co_return std::move(result.resp);
}

// Build request helpers — keep verbose construction out of agent logic.
tmgr::task_create_request make_plan_create(uint64_t serial, std::string_view agent_id,
                                           std::string title) {
    tmgr::task_create_request req;
    req.serial = serial;
    req.agent_id = std::string(agent_id);
    req.spec.type = tmgr::task_type::plan;
    req.spec.title = std::move(title);
    req.spec.prompt = "test-prompt";
    return req;
}

// Synthetic agent coroutine. Drives the §7 lifecycle:
//   claim → simulate work → complete (with child synthesis for plan tasks
//   → implement, implement → merge handled by the SM).
//
// `live` is a stop signal; when false, the agent exits its loop.
struct agent_state {
    std::string id;
    uint64_t serial = 0;
    size_t completed = 0;
    size_t claim_misses = 0;
};
cot::task<> synthetic_agent(test_cluster& cluster, agent_state& state,
                            random_source& rnd, const bool& live) {
    while (live) {
        // Claim
        tmgr::task_claim_request claim;
        claim.serial = ++state.serial;
        claim.agent_id = state.id;
        auto resp = co_await send_to_leader(cluster, tmgr::request{claim}, rnd);
        auto& cr = std::get<tmgr::task_claim_response>(resp);
        if (cr.none) {
            ++state.claim_misses;
            co_await cot::after(rnd.uniform(20ms, 80ms));
            continue;
        }

        auto task_id = cr.id;
        auto token = cr.token;
        auto task_type = cr.spec.type;

        // Simulate work (random duration). Real agents heartbeat here, but
        // for sim tests we run fast and don't need leases — we'll add
        // heartbeats once we test the lease-expiry path.
        co_await cot::after(rnd.uniform(30ms, 80ms));

        // For merge tasks, acquire + release main_lock before completing.
        if (task_type == tmgr::task_type::merge) {
            tmgr::main_lock_acquire_request lock;
            lock.serial = ++state.serial;
            lock.agent_id = state.id;
            lock.merge_task = task_id;
            lock.merge_token = token;
            auto lockresp = co_await send_to_leader(cluster, tmgr::request{lock}, rnd);
            auto& lr = std::get<tmgr::main_lock_acquire_response>(lockresp);
            if (lr.errcode == tmgr::errc::ok) {
                tmgr::main_lock_release_request rel;
                rel.serial = ++state.serial;
                rel.agent_id = state.id;
                rel.lock_token = lr.lock_token;
                co_await send_to_leader(cluster, tmgr::request{rel}, rnd);
            }
        }

        // Complete. Plan tasks spawn 1 implement child; implement provides
        // a result_branch so the SM synthesizes a merge.
        tmgr::task_complete_request done;
        done.serial = ++state.serial;
        done.agent_id = state.id;
        done.id = task_id;
        done.token = token;
        done.result_summary = "ok";
        if (task_type == tmgr::task_type::plan) {
            tmgr::task_spec child;
            child.type = tmgr::task_type::implement;
            child.title = "impl";
            child.prompt = "do thing";
            child.branch_base = "main";
            done.new_children = {child};
        } else if (task_type == tmgr::task_type::implement) {
            done.result_branch = "abc1234";
        }
        co_await send_to_leader(cluster, tmgr::request{done}, rnd);
        ++state.completed;
    }
}

// Phase 3.2 scenario: spawn N agents working on M initial plan tasks.
// Runs for `duration` simulated time, then signals agents to stop.
cot::task<> scenario_multi_agent(test_cluster& cluster, size_t nagents,
                                 size_t ntasks, cot::duration duration,
                                 std::vector<agent_state>& states,
                                 bool& live) {
    co_await cot::after(50ms);  // let replicas settle

    // Create initial plan tasks from a "human" agent.
    for (size_t i = 0; i < ntasks; ++i) {
        co_await send_to_leader(cluster, tmgr::request{
            make_plan_create(i + 1, "human", std::format("plan-{}", i))},
            cluster.rnd);
    }

    // Spawn agents.
    states.clear();
    states.reserve(nagents);
    for (size_t i = 0; i < nagents; ++i) {
        states.emplace_back(agent_state{std::format("agent-{}", i), 0, 0, 0});
    }
    for (size_t i = 0; i < nagents; ++i) {
        synthetic_agent(cluster, states[i], cluster.rnd, live).detach();
    }

    // Run for `duration`, then signal stop and let any in-flight requests
    // settle.
    co_await cot::after(duration);
    live = false;
    co_await cot::after(500ms);  // drain in-flight + final retransmit cycle
    cot::clear();
}

// Invariant check, modeled on pset3's "lag-tolerant diff against baseline".
//
//   Strict:
//     (a) No replica's applied_slot_ exceeds the baseline's. The most
//         advanced replica IS the reference.
//     (b) Every replica's tasks are a SUBSET of the baseline's. A replica
//         can lag, but can never have a task the baseline doesn't know.
//
//   Lag-tolerant (mirrors pset3's `behind + 50` budget):
//     (c) Count disagreements between this replica and the baseline:
//           * each task the baseline has but this replica doesn't (= 1)
//           * each shared task where status/owner_token/owner_agent differ (= 1)
//         Allow up to (behind + 50) disagreements, where
//           behind = baseline.applied_slot_ - this.applied_slot_.
//         Each missed slot can change at most one task, so the lag accounts
//         for the legitimate divergence; the +50 is slack for the cases
//         where a single applied slot updates multiple records (e.g.,
//         task_complete that synthesizes a merge child).
//
// We do NOT exempt failed_replicas — under lag tolerance, even a killed
// replica's state should be a VALID prefix of the baseline (whatever it
// had at kill time is correct, just stale).
bool check_invariants(test_cluster& cluster, unsigned long seed) {
    size_t baseline = 0;
    for (size_t i = 1; i < cluster.nreplicas; ++i) {
        if (cluster.replicas[i]->applied_slot_ >
            cluster.replicas[baseline]->applied_slot_) {
            baseline = i;
        }
    }
    auto& tasks_b = cluster.replicas[baseline]->db().tasks();
    auto& lock_b = cluster.replicas[baseline]->db().main_lock();
    size_t baseline_applied = cluster.replicas[baseline]->applied_slot_;

    for (size_t i = 0; i < cluster.nreplicas; ++i) {
        if (i == baseline) continue;

        auto& tasks_i = cluster.replicas[i]->db().tasks();
        auto& lock_i = cluster.replicas[i]->db().main_lock();
        size_t applied_i = cluster.replicas[i]->applied_slot_;

        // (a) No replica may be ahead of the baseline.
        if (applied_i > baseline_applied) {
            std::print(stderr,
                "seed {}: replica {} applied_slot_={} > baseline {} ({})\n",
                seed, i, applied_i, baseline, baseline_applied);
            return false;
        }

        // (b) Subset check — replica i may not know a task the baseline
        // doesn't. (Same fencing token, same owner — replicas cannot
        // invent state.)
        for (auto& [id, rec_i] : tasks_i) {
            if (!tasks_b.contains(id)) {
                std::print(stderr,
                    "seed {}: replica {} has task {} not in baseline {}\n",
                    seed, i, id, baseline);
                return false;
            }
        }

        // (c) Lag-tolerant disagreement count.
        size_t behind = baseline_applied - applied_i;
        size_t budget = behind + 50;
        size_t disagreements = 0;
        for (auto& [id, rec_b] : tasks_b) {
            auto it = tasks_i.find(id);
            if (it == tasks_i.end()) {
                ++disagreements;
                continue;
            }
            auto& rec_i = it->second;
            if (rec_i.status != rec_b.status ||
                rec_i.owner_token != rec_b.owner_token ||
                rec_i.owner_agent != rec_b.owner_agent) {
                ++disagreements;
            }
        }
        if (lock_i.lock_token != lock_b.lock_token ||
            lock_i.holder_agent != lock_b.holder_agent) {
            ++disagreements;
        }
        if (disagreements > budget) {
            std::print(stderr,
                "seed {}: replica {} has {} disagreements vs baseline {} "
                "(behind={}, budget={})\n",
                seed, i, disagreements, baseline, behind, budget);
            return false;
        }
    }
    return true;
}

struct test_options {
    size_t nreplicas = 3;          // paxos quorum is nreplicas/2 + 1
    double loss = 0.0;             // baseline message-loss rate on all channels
    int failure_schedule = 0;      // 0=none, 1=permanent, 2=temporary, 3=split-brain
    cot::duration duration = 5000ms;
    size_t nagents = 3;
    size_t ntasks = 5;
};

bool try_one_seed(const test_options& opts, unsigned long seed) {
    cot::reset();
    test_cluster cluster(opts.nreplicas, seed);
    cluster.set_baseline_loss(opts.loss);
    cluster.start();

    std::vector<agent_state> states;
    bool live = true;
    scenario_multi_agent(cluster, opts.nagents, opts.ntasks,
                         opts.duration, states, live).detach();

    if (opts.failure_schedule == 1) {
        fail_permanent(cluster).detach();
    } else if (opts.failure_schedule == 2) {
        fail_temporary(cluster, opts.loss).detach();
    } else if (opts.failure_schedule == 3) {
        fail_split_brain(cluster).detach();
    }

    cot::loop();
    return check_invariants(cluster, seed);
}

}  // namespace

int main(int argc, char* argv[]) {
    unsigned long first_seed = 1;
    unsigned long seed_count = 1;
    test_options opts;

    // Parse flags + positional seed args.
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.starts_with("--nreplicas=")) {
            opts.nreplicas = std::stoul(a.substr(12));
        } else if (a.starts_with("--loss=")) {
            opts.loss = std::stod(a.substr(7));
        } else if (a.starts_with("--failure=")) {
            opts.failure_schedule = std::stoi(a.substr(10));
        } else if (a.starts_with("--duration-ms=")) {
            opts.duration = std::chrono::milliseconds(std::stoi(a.substr(14)));
        } else if (a == "-h" || a == "--help") {
            std::print(
                "Usage: tm-paxos-tests [flags] [first_seed [seed_count]]\n"
                "  --nreplicas=N       number of paxos replicas (default 3)\n"
                "  --loss=F            baseline message loss rate (0.0..1.0)\n"
                "  --failure=N         failure schedule:\n"
                "                        0 = none (default)\n"
                "                        1 = fail_permanent\n"
                "                        2 = fail_temporary (leader 2s/3s)\n"
                "                        3 = fail_split_brain\n"
                "  --duration-ms=N     simulated test duration (default 5000)\n");
            return 0;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() > 0) first_seed = std::stoul(positional[0]);
    if (positional.size() > 1) seed_count = std::stoul(positional[1]);

    // Failure schedules need longer runtime to allow election + recovery.
    if (opts.failure_schedule != 0 && opts.duration < 10000ms) {
        opts.duration = 10000ms;
    }

    size_t pass = 0, fail = 0;
    for (unsigned long s = first_seed; s < first_seed + seed_count; ++s) {
        if (try_one_seed(opts, s)) {
            ++pass;
        } else {
            ++fail;
            if (fail >= 3) {
                std::print(stderr, "...stopping after 3 failures\n");
                break;
            }
        }
    }
    std::print("{} pass, {} fail\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
