// pset4/tm-server.cc - HTTP/JSON front for the task manager.
//
// Modeled on examples/jsond.cc. One coroutine per connection; each request
// is parsed, dispatched to db.process_req(), serialized back as JSON.

#include "task_manager_db.hh"
#include "tm.hh"
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
#include <print>
#include <string>
#include <unistd.h>
#include <variant>

namespace cot = cotamer;
using json = nlohmann::json;

namespace {

bool verbose = false;

// JSONL decision log. Owned by main(); nullptr means logging disabled.
// Single-threaded event loop, so plain int64_t is fine.
std::FILE* decision_log = nullptr;
int64_t decision_seq = 0;

void log_decision(std::string_view path, int64_t now, const tmgr::request& req) {
    if (!decision_log) return;
    json entry = {
        {"seq", ++decision_seq},
        {"now_unix", now},
        {"path", path},
        {"request", tmgr::to_json_request_body(req)}
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

cot::http_message handle(const cot::http_message& req, tmgr::task_manager_db& db) {
    auto path = std::string(req.path());

    if (req.method() == HTTP_GET && path == "/dump") {
        return make_response(200, tmgr::dump_state(db));
    }

    if (req.method() != HTTP_POST) {
        return error_response(405, "method not allowed");
    }

    json body;
    try {
        body = json::parse(req.body());
    } catch (const json::parse_error& e) {
        return error_response(400, std::string("invalid JSON: ") + e.what());
    }

    tmgr::request tmreq;
    try {
        tmreq = tmgr::parse_request(path, body);
    } catch (const std::exception& e) {
        return error_response(404, e.what());
    }

    int64_t now = std::time(nullptr);
    tmgr::response tmresp = db.process_req(tmreq, now);

    // Log the decided request iff (a) it actually mutated the SM (errcode==ok)
    // and (b) it is not a read-only path. task_list is read-only; everything
    // else mutates state on success. This matches what Multi-Paxos would
    // decide and replicate in phase B.
    if (base_of(tmresp).errcode == tmgr::errc::ok && path != "/task_list") {
        log_decision(path, now, tmreq);
    }

    return make_response(200, tmgr::to_json_response(tmresp));
}

cot::task<> handle_connection(cot::fd cfd, tmgr::task_manager_db& db) {
    cot::http_parser hp(std::move(cfd), cot::http_parser::server);
    cot::event sends{nullptr};
    while (true) {
        auto req = co_await hp.receive();
        if (!hp.ok()) break;
        if (verbose) {
            std::print(std::cerr, "{} {}\n", req.method_name(), req.url());
        }
        auto res = handle(req, db);
        auto task = hp.send(std::move(res));
        sends = cot::all(sends, task.resolution());
        task.detach();
        if (!hp.should_keep_alive()) break;
    }
    co_await sends;
}

cot::task<> serve(std::string address, tmgr::task_manager_db& db) {
    auto lfd = co_await cot::tcp_listen(address);
    std::print(std::cerr, "tm-server listening on {}\n", address);
    while (true) {
        handle_connection(co_await cot::tcp_accept(lfd), db).detach();
    }
}

void usage() {
    std::print(std::cerr,
        "Usage: tm-server [-p PORT] [-V] [-L LOG_PATH] [-A]\n"
        "  -L PATH   write JSONL decision log (default: tm-server.log).\n"
        "            Pass empty string or '-' to disable logging.\n"
        "  -A        append to existing log instead of truncating.\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    int opt;
    int port = 8080;
    std::string log_path = "tm-server.log";
    bool append_log = false;
    while ((opt = getopt(argc, argv, "hp:VL:A")) != -1) {
        switch (opt) {
        case 'h': usage(); return 0;
        case 'p': port = std::strtol(optarg, nullptr, 0); break;
        case 'V': verbose = true; break;
        case 'L': log_path = optarg; break;
        case 'A': append_log = true; break;
        default:  usage(); return 1;
        }
    }
    if (optind != argc) { usage(); return 1; }

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

    tmgr::task_manager_db db;
    cot::set_clock(cot::clock::real_time);
    cot::task<> t = serve(std::format("0.0.0.0:{}", port), db);
    cot::loop();
}
