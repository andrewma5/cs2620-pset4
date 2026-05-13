// pset4/tm-server.cc - HTTP/JSON front for the task manager.
//
// Modeled on examples/jsond.cc. One coroutine per connection; each request
// is parsed, dispatched to db.process_req(), serialized back as JSON.

#include "task_manager_db.hh"
#include "tm.hh"
#include "tm-paxos.hh"
#include "tm_dump.hh"
#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

namespace cot = cotamer;
using json = nlohmann::json;

namespace {

bool verbose = false;

// JSONL decision log. Owned by main(); nullptr means logging disabled.
// Single-threaded event loop, so plain int64_t is fine.
std::FILE* decision_log = nullptr;
int64_t decision_seq = 0;

// HTTP URLs of all replicas in index order. Empty in single-replica mode.
// When non-empty, handle() returns 307 redirects on busy (non-leader) so
// clients can find the actual leader.
std::vector<std::string> http_peers;

// Append one JSONL entry to the decision log. Called from the paxos
// apply_decided callback on EVERY replica (so all replicas produce
// byte-identical logs — real-paxos invariant; see Phase 4.6 in docs/PLAN.md).
// The filter (errcode==ok && path != /task_list) is applied inside
// apply_decided, so this is unconditionally a write.
void log_decided(const tmgr::decided_value& dv, std::string_view path) {
    if (!decision_log) return;
    json entry = {
        {"seq", ++decision_seq},
        {"now_unix", dv.now_unix},
        {"path", path},
        {"request", tmgr::to_json_request_body(dv.req)}
    };
    auto s = entry.dump();
    s.push_back('\n');
    std::fputs(s.c_str(), decision_log);
    std::fflush(decision_log);
}

// Pull the response_base out of any response variant so we can read errcode.
const tmgr::response_base& base_of(const tmgr::response& r) {
    return std::visit([](auto&& v) -> const tmgr::response_base& {
        return v;
    }, r);
}

cot::http_message make_response(unsigned status, const json& body) {
    cot::http_message res;
    std::string s = body.dump(2);
    s.push_back('\n');
    res.status_code(status)
        .header("Content-Type", "application/json")
        .body(std::move(s));
    return res;
}

cot::http_message error_response(unsigned status, std::string_view msg) {
    return make_response(status, json{{"ok", false}, {"error", msg},
                                      {"status", status}});
}

// Phase 2.6: 307 Temporary Redirect to the elected leader's HTTP URL. Path
// (and query, if any) is preserved. Client must replay the request body.
cot::http_message redirect_response(std::string_view leader_url,
                                    std::string_view path) {
    std::string location = std::string(leader_url) + std::string(path);
    cot::http_message res;
    res.status_code(307)
        .header("Location", location)
        .header("Content-Type", "application/json")
        .body(json{{"ok", false}, {"error", "not leader"},
                   {"redirect", location}}.dump() + "\n");
    return res;
}

cot::task<cot::http_message> handle(cot::http_message req, tmgr::paxos_replica& paxos) {
    auto path = std::string(req.path());

    if (req.method() == HTTP_GET && path == "/dump") {
        co_return make_response(200, tmgr::dump_state(paxos.db()));
    }

    if (req.method() != HTTP_POST) {
        co_return error_response(405, "method not allowed");
    }

    json body;
    try {
        body = json::parse(req.body());
    } catch (const json::parse_error& e) {
        co_return error_response(400, std::string("invalid JSON: ") + e.what());
    }

    tmgr::request tmreq;
    try {
        tmreq = tmgr::parse_request(path, body);
    } catch (const std::exception& e) {
        co_return error_response(400, e.what());
    }

    // Phase 2.3c: propose_and_apply is async. It blocks until paxos decides
    // and applies the slot carrying this request, then returns the response.
    // (The leader-stamped now_unix is captured inside the decided_value and
    // logged by paxos.apply_decided() via the decision_logger callback.)
    auto result = co_await paxos.propose_and_apply(tmreq);
    tmgr::response& tmresp = result.resp;

    // Phase 2.6: if we're not the leader and we know the leader's HTTP URL,
    // redirect the client instead of returning the not-leader error.
    if (base_of(tmresp).errcode == tmgr::errc::busy &&
        !http_peers.empty()) {
        size_t leader = paxos.leader_index();
        if (leader < http_peers.size() && !http_peers[leader].empty()) {
            co_return redirect_response(http_peers[leader], path);
        }
    }

    // Logging is now driven from paxos.apply_decided() via the
    // decision_logger callback registered in main(). Every replica logs
    // when it applies; the filter (errcode==ok && path != /task_list)
    // is enforced inside apply_decided.

    co_return make_response(200, tmgr::to_json_response(tmresp));
}

cot::task<> handle_connection(cot::fd cfd, tmgr::paxos_replica& paxos) {
    cot::http_parser hp(std::move(cfd), cot::http_parser::server);
    cot::event sends{nullptr};
    while (true) {
        auto req = co_await hp.receive();
        if (!hp.ok()) break;
        if (verbose) {
            std::print(std::cerr, "{} {}\n", req.method_name(), req.url());
        }
        auto res = co_await handle(std::move(req), paxos);
        auto task = hp.send(std::move(res));
        sends = cot::all(sends, task.resolution());
        task.detach();
        if (!hp.should_keep_alive()) break;
    }
    co_await sends;
}

cot::task<> serve(std::string address, tmgr::paxos_replica& paxos) {
    auto lfd = co_await cot::tcp_listen(address);
    std::print(std::cerr, "tm-server listening on {}\n", address);
    while (true) {
        handle_connection(co_await cot::tcp_accept(lfd), paxos).detach();
    }
}

void usage() {
    std::print(std::cerr,
        "Usage: tm-server [-p PORT] [-V] [-L LOG_PATH] [-A]\n"
        "                 [--replica-index N --peers host:port,host:port,...]\n"
        "  -p PORT              HTTP port (default 8080)\n"
        "  -V                   verbose request logging\n"
        "  -L PATH              JSONL decision log (default tm-server.log;\n"
        "                       pass empty or '-' to disable)\n"
        "  -A                   append to log instead of truncate\n"
        "  --replica-index N    this replica's index in --peers (default 0)\n"
        "  --peers a,b,c        host:port for each replica's paxos listen\n"
        "                       address. Without --peers, run single-replica\n"
        "                       sim mode (the default).\n"
        "  --http-peers a,b,c   HTTP URL for each replica (e.g. http://host:8080).\n"
        "                       If set, non-leader requests get 307 redirected\n"
        "                       to the leader's URL.\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string log_path = "tm-server.log";
    bool append_log = false;
    size_t replica_index = 0;
    std::vector<std::string> peers;  // host:port per replica; empty = single-replica sim

    // Manual flag parse so we can support both short (-p) and long
    // (--replica-index, --peers) flags. Positional args are rejected.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "-V") { verbose = true; }
        else if (a == "-A") { append_log = true; }
        else if (a == "-p") {
            if (i + 1 >= argc) { usage(); return 1; }
            port = std::strtol(argv[++i], nullptr, 0);
        }
        else if (a.starts_with("-p") && a.size() > 2) {
            port = std::strtol(a.c_str() + 2, nullptr, 0);
        }
        else if (a == "-L") {
            if (i + 1 >= argc) { usage(); return 1; }
            log_path = argv[++i];
        }
        else if (a.starts_with("-L") && a.size() > 2) {
            log_path = a.substr(2);
        }
        else if (a.starts_with("--replica-index=")) {
            replica_index = std::strtoul(a.c_str() + 16, nullptr, 0);
        }
        else if (a == "--replica-index" && i + 1 < argc) {
            replica_index = std::strtoul(argv[++i], nullptr, 0);
        }
        else if (a.starts_with("--peers=")) {
            std::string list = a.substr(8);
            size_t start = 0;
            while (start < list.size()) {
                size_t comma = list.find(',', start);
                peers.push_back(list.substr(start, comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        else if (a == "--peers" && i + 1 < argc) {
            std::string list = argv[++i];
            size_t start = 0;
            while (start < list.size()) {
                size_t comma = list.find(',', start);
                peers.push_back(list.substr(start, comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        else if (a.starts_with("--http-peers=") ||
                 (a == "--http-peers" && i + 1 < argc)) {
            std::string list = a.starts_with("--http-peers=")
                ? a.substr(13) : std::string(argv[++i]);
            size_t start = 0;
            while (start < list.size()) {
                size_t comma = list.find(',', start);
                http_peers.push_back(list.substr(start, comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        else { usage(); return 1; }
    }

    if (!peers.empty() && replica_index >= peers.size()) {
        std::print(std::cerr, "tm-server: --replica-index ({}) out of range "
                              "for {} peers\n", replica_index, peers.size());
        return 1;
    }

    if (!log_path.empty() && log_path != "-") {
        decision_log = std::fopen(log_path.c_str(), append_log ? "a" : "w");
        if (!decision_log) {
            std::print(std::cerr, "tm-server: cannot open log {}: {}\n",
                       log_path, std::strerror(errno));
            return 1;
        }
        std::print(std::cerr, "tm-server: decision log -> {} ({})\n",
                   log_path, append_log ? "append" : "truncate");
    } else {
        std::print(std::cerr, "tm-server: decision log disabled\n");
    }

    cot::set_clock(cot::clock::real_time);
    // Don't die on SIGPIPE — when a peer disconnects mid-write, we want
    // the write() call to fail with EPIPE, not signal-terminate the
    // process.
    cot::ignore_sigpipe();

    // Pick mode. If --peers given, run multi-replica TCP. Otherwise
    // single-replica sim (current default).
    std::unique_ptr<tmgr::paxos_replica> paxos;
    if (peers.empty()) {
        paxos = std::make_unique<tmgr::paxos_replica>();
        std::print(std::cerr, "tm-server: paxos = single-replica (sim)\n");
    } else {
        paxos = std::make_unique<tmgr::paxos_replica>(replica_index, peers);
        std::print(std::cerr, "tm-server: paxos = multi-replica TCP "
                              "(replica {} of {}, peers={})\n",
                   replica_index, peers.size(), peers[replica_index]);
    }

    // Register per-apply log callback. Fires on every replica when it
    // applies a decided value (real-paxos invariant — see docs/docs/PLAN.md §4.6).
    paxos->set_decision_logger(log_decided);

    cot::task<> t = serve(std::format("0.0.0.0:{}", port), *paxos);
    cot::loop();
}
