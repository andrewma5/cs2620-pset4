"""Unit tests for the unified `tm` CLI.

Run from the pset4/ directory:

    py -3 -m unittest tests.test_tm_cli
    # or
    python3 -m unittest tests.test_tm_cli

The CLI lives at
  pset4/.claude/skills/swarm-deploy/assets/agent-skills/task-loop/tm
and is stdlib-only Python. We load it by path (the filename has no .py
suffix) and exercise it against a stub HTTP server running in a thread.
"""

import http.server
import importlib.util
import io
import json
import socket
import socketserver
import subprocess
import sys
import threading
import time
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
TM_CLI_PATH = (REPO_ROOT / ".claude" / "skills" / "swarm-deploy" / "assets"
               / "agent-skills" / "task-loop" / "tm")


def load_tm_module():
    """Import the tm CLI as a module so we can call main() directly.

    The CLI file has no .py suffix, so we provide an explicit
    SourceFileLoader instead of relying on filename-based detection.
    Disable bytecode caching so we don't litter __pycache__ inside the
    swarm-deploy bundle (it would get copied to every agent on deploy).
    """
    sys.dont_write_bytecode = True
    from importlib.machinery import SourceFileLoader
    loader = SourceFileLoader("tm_cli", str(TM_CLI_PATH))
    spec = importlib.util.spec_from_loader("tm_cli", loader)
    if spec is None:
        raise RuntimeError(f"cannot load {TM_CLI_PATH}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["tm_cli"] = mod
    loader.exec_module(mod)
    return mod


tm_cli = load_tm_module()


# ---------------- stub HTTP server scaffolding ----------------

class StubHandler(http.server.BaseHTTPRequestHandler):
    """Records the request and returns whatever the server's `responder`
    function says. Path is matched by the test."""

    def log_message(self, *args, **kwargs):
        # silence stderr logging during tests
        pass

    def _serve(self):
        clen = int(self.headers.get("Content-Length", "0") or "0")
        body_bytes = self.rfile.read(clen) if clen else b""
        try:
            body = json.loads(body_bytes) if body_bytes else None
        except json.JSONDecodeError:
            body = body_bytes.decode("utf-8", errors="replace")
        self.server.recorded.append({
            "method": self.command,
            "path": self.path,
            "body": body,
            "headers": dict(self.headers.items()),
        })
        status, headers, resp_body = self.server.responder(
            self.command, self.path, body)
        self.send_response(status)
        for k, v in headers.items():
            self.send_header(k, v)
        if "Content-Type" not in headers and "content-type" not in headers:
            self.send_header("Content-Type", "application/json")
        self.end_headers()
        if isinstance(resp_body, (dict, list)):
            payload = json.dumps(resp_body).encode("utf-8")
        elif isinstance(resp_body, str):
            payload = resp_body.encode("utf-8")
        else:
            payload = resp_body or b""
        self.wfile.write(payload)

    def do_GET(self):
        self._serve()

    def do_POST(self):
        self._serve()


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


@contextmanager
def run_stub(responder):
    """Spin up one stub on a fresh port. Yields (url, server)."""
    port = _free_port()
    server = ThreadedHTTPServer(("127.0.0.1", port), StubHandler)
    server.responder = responder
    server.recorded = []
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    try:
        yield f"http://127.0.0.1:{port}", server
    finally:
        server.shutdown()
        server.server_close()


def run_cli(argv, env_extra=None):
    """Run the CLI in-process. Returns (exit_code, stdout, stderr)."""
    env_extra = env_extra or {}
    base_env = {
        "AGENT_ID": "test-agent",
    }
    base_env.update(env_extra)
    stdout = io.StringIO()
    stderr = io.StringIO()
    with mock.patch.dict("os.environ", base_env, clear=False), \
         mock.patch("sys.stdout", stdout), \
         mock.patch("sys.stderr", stderr):
        try:
            rc = tm_cli.main(argv)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 1
    return rc, stdout.getvalue(), stderr.getvalue()


# ---------------- the actual tests ----------------

class CompleteUsesTokenKey(unittest.TestCase):
    """The whole reason this CLI exists: the JSON key must be `token`,
    NOT `tok`. Regression-guards against the bug found in agent-2's
    transcript that motivated this rewrite."""

    def test_complete_sends_token_field_not_tok(self):
        def responder(method, path, body):
            return 200, {}, {"ok": True, "serial": body.get("serial", 0),
                             "child_ids": []}
        with run_stub(responder) as (url, server):
            rc, out, err = run_cli(
                ["complete", "--id", "t/0003", "--token", "7",
                 "--branch", "abcdef", "--summary", "shipped"],
                env_extra={"TM_URL_LIST": url})
            self.assertEqual(rc, 0, msg=f"stderr: {err}")
            self.assertEqual(len(server.recorded), 1)
            rec = server.recorded[0]
            self.assertEqual(rec["path"], "/task_complete")
            self.assertEqual(rec["method"], "POST")
            body = rec["body"]
            self.assertIn("token", body, "token must be in payload")
            self.assertNotIn("tok", body,
                             "must NOT use 'tok' key — that's the bug")
            self.assertEqual(body["token"], 7)
            self.assertEqual(body["id"], "t/0003")
            self.assertEqual(body["result_branch"], "abcdef")
            self.assertEqual(body["result_summary"], "shipped")
            self.assertEqual(body["new_children"], [])

    def test_hb_once_sends_token_field_not_tok(self):
        def responder(method, path, body):
            return 200, {}, {"ok": True, "serial": body.get("serial", 0)}
        with run_stub(responder) as (url, server):
            rc, out, err = run_cli(
                ["hb-once", "--id", "t/0001", "--token", "3"],
                env_extra={"TM_URL_LIST": url})
            self.assertEqual(rc, 0)
            body = server.recorded[0]["body"]
            self.assertEqual(body["token"], 3)
            self.assertNotIn("tok", body)

    def test_fail_sends_token_field_not_tok(self):
        def responder(method, path, body):
            return 200, {}, {"ok": True, "serial": body.get("serial", 0)}
        with run_stub(responder) as (url, server):
            rc, out, err = run_cli(
                ["fail", "--id", "t/0001", "--token", "5",
                 "--reason", "ABANDON: testing"],
                env_extra={"TM_URL_LIST": url})
            self.assertEqual(rc, 0)
            body = server.recorded[0]["body"]
            self.assertEqual(body["token"], 5)
            self.assertNotIn("tok", body)


class FailRequiresAbandonPrefix(unittest.TestCase):
    def test_fail_without_abandon_prefix_refuses(self):
        # Server should never be hit. Use a non-routable URL just in
        # case (the CLI must short-circuit before any HTTP).
        rc, out, err = run_cli(
            ["fail", "--id", "t/X", "--token", "1",
             "--reason", "this is bad"],
            env_extra={"TM_URL_LIST": "http://127.0.0.1:1"})
        self.assertEqual(rc, 2)
        self.assertIn("ABANDON:", err)

    def test_fail_with_abandon_prefix_sends(self):
        def responder(method, path, body):
            return 200, {}, {"ok": True, "serial": body.get("serial", 0)}
        with run_stub(responder) as (url, server):
            rc, out, err = run_cli(
                ["fail", "--id", "t/X", "--token", "1",
                 "--reason", "ABANDON: corrupt repo"],
                env_extra={"TM_URL_LIST": url})
            self.assertEqual(rc, 0)
            self.assertEqual(len(server.recorded), 1)


class FailoverBehavior(unittest.TestCase):
    def test_failover_on_connection_refused(self):
        # URL 1: port that nothing listens on. URL 2: real stub.
        dead_port = _free_port()
        # Don't bind it; the port is just nominally free.
        def responder(method, path, body):
            return 200, {}, {"ok": True, "serial": 1, "tasks": []}
        with run_stub(responder) as (url, server):
            urls = f"http://127.0.0.1:{dead_port},{url}"
            rc, out, err = run_cli(
                ["dump"], env_extra={"TM_URL_LIST": urls})
            self.assertEqual(rc, 0, msg=f"stderr: {err}")
            self.assertEqual(len(server.recorded), 1,
                             "second replica should have served the call")
            self.assertIn("tasks", out)

    def test_no_failover_on_http_400(self):
        # First replica returns HTTP 400 (a payload bug). The CLI must
        # surface that, NOT try the second replica — retrying would
        # mask the bug.
        def responder_400(method, path, body):
            return 400, {}, {"ok": False, "error": "missing required "
                             "field 'token'", "status": 400}
        def responder_200(method, path, body):
            return 200, {}, {"ok": True}
        with run_stub(responder_400) as (url1, server1), \
             run_stub(responder_200) as (url2, server2):
            urls = f"{url1},{url2}"
            rc, out, err = run_cli(
                ["dump"], env_extra={"TM_URL_LIST": urls})
            self.assertEqual(rc, 0, "HTTP 4xx is a successful HTTP call "
                             "from the CLI's POV; exit 0")
            self.assertEqual(len(server1.recorded), 1,
                             "first replica should have been hit once")
            self.assertEqual(len(server2.recorded), 0,
                             "second replica must NOT be tried after 4xx")
            self.assertIn("missing required field", out)

    def test_307_redirect_followed_once(self):
        # Replica 1 returns 307 → replica 2. Replica 2 returns 200.
        # The CLI should follow once and return replica 2's body.
        def responder_200(method, path, body):
            return 200, {}, {"ok": True, "tasks": [{"id": "via-redirect"}]}
        with run_stub(responder_200) as (url2, server2):
            def responder_307(method, path, body):
                return 307, {"Location": url2 + path}, {"ok": False,
                                                       "error": "not leader"}
            with run_stub(responder_307) as (url1, server1):
                # Single URL in list; we're testing the redirect path,
                # not failover.
                rc, out, err = run_cli(
                    ["dump"], env_extra={"TM_URL_LIST": url1})
                self.assertEqual(rc, 0, msg=f"stderr: {err}")
                self.assertEqual(len(server1.recorded), 1)
                self.assertEqual(len(server2.recorded), 1)
                self.assertIn("via-redirect", out)

    def test_all_replicas_down_exits_nonzero(self):
        # Two free ports, nothing bound to them.
        p1 = _free_port()
        p2 = _free_port()
        urls = f"http://127.0.0.1:{p1},http://127.0.0.1:{p2}"
        rc, out, err = run_cli(["dump"], env_extra={"TM_URL_LIST": urls})
        self.assertNotEqual(rc, 0)
        self.assertIn("all replicas failed", err)


class TokenMustBeInteger(unittest.TestCase):
    def test_complete_rejects_non_int_token(self):
        # argparse type=int — must fail at parse time, no HTTP attempt.
        rc, out, err = run_cli(
            ["complete", "--id", "t/X", "--token", "foo", "--summary", "x"],
            env_extra={"TM_URL_LIST": "http://127.0.0.1:1"})
        self.assertEqual(rc, 2)  # argparse uses exit 2 for parse errors
        self.assertIn("invalid int value", err)


class EnvValidation(unittest.TestCase):
    def test_missing_agent_id_exits(self):
        # No AGENT_ID in env.
        with mock.patch.dict("os.environ",
                             {"TM_URL_LIST": "http://x"}, clear=True), \
             mock.patch("sys.stdout", io.StringIO()), \
             mock.patch("sys.stderr", io.StringIO()) as err:
            try:
                rc = tm_cli.main(["dump"])
            except SystemExit as e:
                rc = e.code if isinstance(e.code, int) else 1
            self.assertNotEqual(rc, 0)

    def test_missing_url_list_exits(self):
        with mock.patch.dict("os.environ", {"AGENT_ID": "a"}, clear=True), \
             mock.patch("sys.stdout", io.StringIO()), \
             mock.patch("sys.stderr", io.StringIO()):
            try:
                rc = tm_cli.main(["dump"])
            except SystemExit as e:
                rc = e.code if isinstance(e.code, int) else 1
            self.assertNotEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
