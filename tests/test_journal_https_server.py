"""Real loopback TLS, synthetic publisher only. PKI is created by the owned C++ harness."""
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import threading
import time
import unittest
from unittest.mock import patch

from tools import deletion_journal as j1
from tools import deletion_journal_bridge as bridge
from tools.deletion_journal_publication import Publisher
from tools.journal_https_server import Config, JournalHttpsServer
from tests.test_deletion_journal_publication import Objects, Metadata, operation, uid


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pki = Path(os.environ["USAPP_JOURNAL_TLS_TEST_DIR"])
        for name in ("server.crt", "server.key", "ca.crt"):
            if not (cls.pki / name).is_file():
                raise RuntimeError("synthetic_test_pki_required")

    def setUp(self):
        self.journal, self.key = uid(), "b" * 64
        self.objects = Objects()
        self.meta = Metadata(self.journal, self.objects)
        self.publisher = Publisher(self.journal, self.objects, self.meta)
        self.dispatch = bridge.Dispatcher(self.journal, self.key, self.publisher)
        self.op = self.publisher._operation(operation())
        self.servers = []
        self.releases = []

    def tearDown(self):
        for release in self.releases:
            release.set()
        for server in self.servers:
            self.assertEqual(server.close(2), dict(network_drained=True, backend_drained=True))

    def config(self, **kw):
        values = dict(certificate=str(self.pki / "server.crt"), private_key=str(self.pki / "server.key"))
        values.update(kw)
        return Config(**values)

    def start(self, **kw):
        server = JournalHttpsServer(self.config(**kw), self.dispatch)
        self.servers.append(server)
        return server.start()

    def connect(self, server, hostname="journal.test.invalid"):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.load_verify_locations(str(self.pki / "ca.crt"))
        raw = socket.create_connection(("127.0.0.1", server.port), timeout=2)
        try:
            return context.wrap_socket(raw, server_hostname=hostname)
        except BaseException:
            raw.close()
            raise

    def body(self, method="ensure_intent", phase="intent"):
        return j1.canonical(dict(version=1, journal_id=self.journal, nonce="c" * 32, method=method,
                                 operation_id=self.op["operation_id"], operation=self.op if method != "lookup" else None,
                                 phase=phase if method != "lookup" else None))

    def request(self, body=None, signature=None, extra=b"", omit=(), line=b"POST /v1/journal HTTP/1.1"):
        body = self.body() if body is None else body
        signature = bridge.mac(self.key, "request", body) if signature is None else signature
        headers = [("Host", "journal.test.invalid"), ("Content-Type", "application/json"),
                   ("Content-Length", str(len(body))), ("X-Usapp-Journal-Mac", signature)]
        return line + b"\r\n" + b"".join((k + ": " + v + "\r\n").encode("ascii") for k, v in headers if k not in omit) + extra + b"\r\n" + body

    def receive(self, conn):
        result = b""
        while True:
            data = conn.recv(4096)
            if not data:
                break
            result += data
            self.assertLessEqual(len(result), 70000)
        if not result:
            return None, {}, b""
        head, body = result.split(b"\r\n\r\n", 1)
        lines = head.decode("ascii").split("\r\n")
        headers = dict(line.lower().split(": ", 1) for line in lines[1:])
        self.assertEqual(int(headers["content-length"]), len(body))
        self.assertEqual(headers["connection"], "close")
        self.assertEqual(headers["cache-control"], "no-store")
        self.assertNotIn("server", headers)
        return int(lines[0].split()[1]), headers, body

    def call(self, server, raw=None):
        with self.connect(server) as conn:
            conn.sendall(self.request() if raw is None else raw)
            return self.receive(conn)

    def signed(self, server, method="ensure_intent", phase="intent"):
        raw = self.body(method, phase)
        status, headers, body = self.call(server, self.request(raw))
        self.assertEqual(status, 200)
        self.assertEqual(headers["x-usapp-journal-mac"], bridge.mac(self.key, "response", body))
        reply = json.loads(body)
        self.assertEqual(reply["request_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(reply["nonce"], "c" * 32)
        self.assertEqual(reply["journal_id"], self.journal)
        return reply

    def wait(self, condition, budget=1):
        end = time.monotonic() + budget
        while not condition() and time.monotonic() < end:
            threading.Event().wait(.005)
        self.assertTrue(condition())

    def test_configuration_rejects_public_wildcard_invalid_limits(self):
        bad = [dict(bind=v) for v in ("0.0.0.0", "8.8.8.8", "localhost", "::1", None)]
        bad += [dict(port=v) for v in (-1, 65536, True)]
        bad += [dict(io_timeout=v) for v in (0, 11, True, float("nan"), float("inf"))]
        bad += [dict(max_connections=v) for v in (0, 9, True)]
        bad += [dict(private_key="hidden\0path")]
        for values in bad:
            with self.subTest(values=values), self.assertRaisesRegex(ValueError, "^invalid_journal_server_configuration$"):
                self.config(**values).validate()
        for address in ("127.0.0.1", "10.0.0.1", "172.16.0.1", "192.168.1.1"):
            self.config(bind=address).validate()

    def test_missing_material_has_fixed_error_without_path(self):
        with self.assertRaisesRegex(ValueError, "^invalid_journal_tls_material$"):
            JournalHttpsServer(self.config(private_key=str(self.pki / "private-hidden-missing")), self.dispatch)

    def test_signed_publication_lookup_replay_terminal(self):
        server = self.start()
        self.assertEqual(self.signed(server, "lookup")["state"], "missing")
        self.assertEqual(self.signed(server)["state"], "ready")
        self.assertEqual(self.signed(server, "lookup")["operation"], self.op)
        before = self.meta.commits
        self.assertEqual(self.signed(server)["state"], "ready")
        self.assertEqual(self.meta.commits, before)
        self.assertEqual(self.signed(server, "settle", "committed")["state"], "committed")
        self.assertEqual(self.signed(server, "settle", "aborted")["state"], "conflict")
        self.assertEqual(self.meta.commits, 2)

    def test_bad_auth_never_enters_dispatch_or_storage(self):
        server = self.start()
        with patch.object(self.dispatch, "handle", wraps=self.dispatch.handle) as handle:
            for signature in ("", "a" * 64, bridge.mac(self.key, "response", self.body())):
                status, headers, body = self.call(server, self.request(signature=signature))
                self.assertEqual(status, 401)
                self.assertEqual(body, b"")
                self.assertNotIn("x-usapp-journal-mac", headers)
            handle.assert_not_called()
        self.assertEqual(self.objects.gets, [])
        self.assertEqual(self.meta.commits, 0)

    def test_strict_http_refusals_have_no_backend_effect(self):
        server = self.start()
        cases = [(self.request(extra=b"Content-Length: 1\r\n"), 400),
                 (self.request(extra=b"Transfer-Encoding: chunked\r\n"), 400),
                 (self.request(extra=b"Expect: 100-continue\r\n"), 400),
                 (self.request(extra=b"Upgrade: websocket\r\n"), 400),
                 (self.request(extra=b" folded: no\r\n"), 400),
                 (self.request(omit=("Host",)), 400), (self.request(omit=("Content-Length",)), 411),
                 (self.request(omit=("Content-Type",)), 415),
                 (self.request(extra=b"Content-Encoding: gzip\r\n"), 415),
                 (self.request(line=b"GET /v1/journal HTTP/1.1"), 404),
                 (self.request(line=b"POST /other HTTP/1.1"), 404),
                 (self.request(line=b"POST /v1/journal HTTP/1.0"), 404),
                 (self.request(extra=b"X-Usapp-Journal-Mac: no\r\n"), 400),
                 (self.request(extra=b"X-Oversize: " + b"x" * 8200 + b"\r\n"), 431),
                 (self.request(extra=b"".join(("X-{}: y\r\n".format(i)).encode() for i in range(65))), 431),
                 (self.request(body=b"x" * 65537).split(b"\r\n\r\n")[0] + b"\r\n\r\n", 413)]
        with patch.object(self.dispatch, "handle", wraps=self.dispatch.handle) as handle:
            for raw, expected in cases:
                with self.subTest(expected=expected, prefix=raw[:30]):
                    status, _, body = self.call(server, raw)
                    self.assertEqual(status, expected)
                    self.assertEqual(body, b"")
            handle.assert_not_called()

    def test_authenticated_malformed_body_is_refused(self):
        server = self.start()
        status, _, body = self.call(server, self.request(b'{"version":1,"version":1}'))
        self.assertEqual((status, body), (400, b""))
        self.assertEqual(self.meta.commits, 0)

    def test_exact_frame_limit_accepts_and_one_byte_over_refuses(self):
        server = self.start()
        raw = self.body()
        raw += b" " * (65536 - len(raw))
        status, headers, body = self.call(server, self.request(raw))
        self.assertEqual(status, 200)
        self.assertEqual(headers["x-usapp-journal-mac"], bridge.mac(self.key, "response", body))
        self.assertEqual(json.loads(body)["state"], "ready")
        over = self.request(raw + b" ").split(b"\r\n\r\n")[0] + b"\r\n\r\n"
        self.assertEqual(self.call(server, over)[0], 413)
        self.assertEqual(self.meta.commits, 1)

    def test_backend_exception_and_invalid_success_are_empty_unknown(self):
        server = self.start()
        for result in (RuntimeError("private exception must not escape"), None, (200, b"private-result", "bad"), (200, "not-bytes", "a" * 64)):
            with patch.object(self.dispatch, "handle", side_effect=result if isinstance(result, Exception) else None,
                              return_value=result):
                status, headers, body = self.call(server)
                self.assertEqual((status, body), (503, b""))
                self.assertNotIn("x-usapp-journal-mac", headers)
        self.assertEqual(self.signed(server)["state"], "ready")
        self.assertEqual(self.meta.commits, 1)

    def test_prefetched_pipeline_is_refused_before_dispatch(self):
        # Deterministic single-buffer transport seam; real TLS record coalescing is deliberately not assumed.
        from tools.journal_https_server import _read, _Refusal
        class Buffered:
            def settimeout(self, _):
                pass
            def recv(inner, size):
                return (self.request() + b"POST /other HTTP/1.1\r\n\r\n")[:size]
        with self.assertRaises(_Refusal) as error:
            _read(Buffered(), time.monotonic() + 1)
        self.assertEqual(error.exception.status, 400)

    def test_header_trickle_does_not_extend_absolute_deadline(self):
        server = self.start(io_timeout=.25)
        start = time.monotonic()
        with self.connect(server) as conn:
            for _ in range(12):
                try:
                    conn.sendall(b"P")
                except OSError:
                    break
                threading.Event().wait(.04)
            try:
                self.assertEqual(conn.recv(1), b"")
            except OSError:
                pass
        self.assertLess(time.monotonic() - start, .9)
        self.assertEqual(self.meta.commits, 0)

    def test_body_trickle_does_not_extend_absolute_deadline(self):
        server = self.start(io_timeout=.25)
        with self.connect(server) as conn:
            head = self.request().split(b"\r\n\r\n")[0] + b"\r\n\r\n"
            conn.sendall(head + b"{")
            threading.Event().wait(.35)
            try:
                self.assertEqual(conn.recv(1), b"")
            except OSError:
                pass
        self.assertEqual(self.objects.gets, [])

    def test_expired_authentication_cannot_start_publication(self):
        server = self.start(io_timeout=.15)
        authenticate = self.dispatch.authenticate
        def delayed(body, signature):
            result = authenticate(body, signature)
            threading.Event().wait(.25)
            return result
        with patch.object(self.dispatch, "authenticate", side_effect=delayed), \
                patch.object(self.dispatch, "handle", wraps=self.dispatch.handle) as handle:
            self.assertIsNone(self.call(server)[0])
            handle.assert_not_called()
        self.assertEqual(self.objects.gets, [])
        self.assertEqual(self.meta.commits, 0)

    def test_tls_stalls_are_bounded_and_close_drains_network(self):
        server = self.start(max_connections=2)
        raw = [socket.create_connection(("127.0.0.1", server.port), timeout=1) for _ in range(2)]
        try:
            self.wait(lambda: server.snapshot()["connections"] == 2)
            with socket.create_connection(("127.0.0.1", server.port), timeout=1) as extra:
                self.assertEqual(extra.recv(1), b"")
            self.assertEqual(server.snapshot()["connections"], 2)
            self.assertEqual(server.close(1), dict(network_drained=True, backend_drained=True))
            self.assertEqual(self.meta.commits, 0)
        finally:
            for conn in raw:
                conn.close()

    def stalled_publisher(self):
        entered, release = threading.Event(), threading.Event()
        self.releases.append(release)
        def stall():
            entered.set()
            release.wait(4)
        self.objects.after_put = stall
        return entered, release

    def test_timed_out_job_holds_slot_until_publication_really_ends(self):
        entered, release = self.stalled_publisher()
        server = self.start(io_timeout=.3)
        with patch.object(self.dispatch, "handle", wraps=self.dispatch.handle) as handle:
            self.assertEqual(self.call(server)[0], 503)
            self.assertTrue(entered.is_set())
            self.assertTrue(server.snapshot()["backend_busy"])
            self.assertEqual(self.meta.commits, 0)
            self.assertEqual(self.call(server)[0], 503)
            self.assertEqual(handle.call_count, 1)
            release.set()
            self.wait(lambda: not server.snapshot()["backend_busy"])
            self.assertEqual(self.meta.commits, 1)
            before = len(self.objects.puts)
            self.assertEqual(self.signed(server)["state"], "ready")
            self.assertEqual(len(self.objects.puts), before)
            self.assertEqual(self.meta.commits, 1)

    def test_close_reports_undrained_work_without_inventing_abort(self):
        entered, release = self.stalled_publisher()
        server = self.start(io_timeout=1)
        conn = self.connect(server)
        conn.sendall(self.request())
        try:
            self.assertTrue(entered.wait(1))
            start = time.monotonic()
            result = server.close(.15)
            self.assertLess(time.monotonic() - start, .6)
            self.assertEqual(result, dict(network_drained=True, backend_drained=False))
            self.assertTrue(server.snapshot()["backend_busy"])
            self.assertEqual(self.meta.commits, 0)
            release.set()
            self.wait(lambda: not server.snapshot()["backend_busy"])
            self.assertEqual(self.meta.commits, 1)
        finally:
            conn.close()

    def test_disconnected_client_does_not_cancel_or_duplicate_publication(self):
        entered, release = self.stalled_publisher()
        server = self.start(io_timeout=.4)
        conn = self.connect(server)
        conn.sendall(self.request())
        self.assertTrue(entered.wait(1))
        conn.close()
        self.assertTrue(server.snapshot()["backend_busy"])
        release.set()
        self.wait(lambda: not server.snapshot()["backend_busy"])
        self.assertEqual(self.meta.commits, 1)
        before = len(self.objects.puts)
        self.assertEqual(self.signed(server)["state"], "ready")
        self.assertEqual(len(self.objects.puts), before)
        self.assertEqual(self.meta.commits, 1)

    def test_plaintext_and_wrong_hostname_cannot_publish(self):
        server = self.start()
        with socket.create_connection(("127.0.0.1", server.port), timeout=1) as conn:
            conn.sendall(self.request())
            try:
                self.assertEqual(conn.recv(1), b"")
            except ConnectionResetError:
                pass
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.connect(server, "wrong.test.invalid")
        self.assertEqual(self.meta.commits, 0)

    def test_start_failure_and_repeated_close_are_safe(self):
        server = JournalHttpsServer(self.config(), self.dispatch)
        self.servers.append(server)
        with patch.object(threading.Thread, "start", side_effect=RuntimeError("not printed")):
            with self.assertRaisesRegex(ValueError, "^journal_server_start_failed$"):
                server.start()
        self.assertEqual(server.close(1), dict(network_drained=True, backend_drained=True))
        with self.assertRaises(ValueError):
            server.start()
        other = self.start()
        with self.assertRaises(ValueError):
            other.start()


if __name__ == "__main__":
    unittest.main()
