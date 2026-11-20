"""Opt-in, owned loopback HTTPS server. No vendor address can be connected."""
import os
from pathlib import Path
import socket
import ssl
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

from tools import deletion_journal_publication as p
from tools.j4c_oss_objects import Config, Objects
from test_j4c_oss_objects import JOURNAL, STAMP, candidate


def worker_put(path, port, key, data):
    """Spawnable synthetic client, allow only this test's owned loopback port."""
    from urllib3 import HTTPSConnectionPool
    original_connect, original_resolve = socket.socket.connect, socket.getaddrinfo
    def connect(sock, address):
        if address != ("127.0.0.1", port):
            raise AssertionError("non_owned_connection_forbidden")
        return original_connect(sock, address)
    def resolve(host, *args, **kwargs):
        if host != "127.0.0.1":
            raise AssertionError("external_dns_forbidden")
        return original_resolve(host, *args, **kwargs)
    pool = HTTPSConnectionPool("127.0.0.1", port, cert_reqs="CERT_REQUIRED", retries=False,
                ca_certs=str(Path(path) / "server.crt"), assert_hostname="journal-test.invalid",
                server_hostname="journal-test.invalid", maxsize=1, block=True)
    class Route:
        def urlopen(self, method, url, **kwargs):
            prefix = Config("usapp-j4c-log-1234abcd", JOURNAL).endpoint
            if not url.startswith(prefix + "/journals/"):
                raise AssertionError("object_scope")
            return pool.urlopen(method, url[len(prefix):], **kwargs)
    try:
        with patch("socket.socket.connect", connect), patch("socket.getaddrinfo", resolve):
            return Objects(Config("usapp-j4c-log-1234abcd", JOURNAL), "offline-id", "offline-secret",
                           pool_factory=lambda **_: Route(), clock=lambda: STAMP).put_once(key, data)
    finally:
        pool.close()


class LocalTlsFixture:
    def setUp(self):
        self.path = Path(os.environ["USAPP_J4C_LOCAL_TLS_DIR"])
        self.key, self.data = candidate()
        self.requests, self.mode, self.pools, self.server_errors = [], "ok", [], []
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def handle(self):
                try:
                    super().handle()
                except (ConnectionResetError, BrokenPipeError, ssl.SSLEOFError):
                    # Negative TLS/timeout clients may close their own socket.
                    pass

            def handle_request(self):
                owner.requests.append((self.command, self.path))
                if self.command == "PUT":
                    incoming = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                    if incoming != owner.data or self.headers.get("x-oss-forbid-overwrite") != "true":
                        self.send_error(400)
                        return
                    owner.applied = True
                if owner.mode == "trickle":
                    self.send_response(200)
                    self.send_header("Content-Length", "200000")
                    self.end_headers()
                    for _ in range(100):
                        self.wfile.write(b"x" * 1000)
                        self.wfile.flush()
                        time.sleep(0.1)
                    return
                if owner.mode == "stall":
                    # Longer than the actual configured 3 s read timeout.
                    time.sleep(3.5)
                    return
                body = owner.data if self.command == "GET" else b""
                self.send_response(307 if owner.mode == "redirect" else 200)
                if owner.mode == "redirect":
                    self.send_header("Location", "https://vendor-must-not-be-contacted.invalid/")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if body:
                    self.wfile.write(body)

            do_GET = handle_request
            do_PUT = handle_request
        class Server(ThreadingHTTPServer):
            daemon_threads = False

            def handle_error(self, request, client_address):
                owner.server_errors.append(type(sys.exc_info()[1]).__name__)
        self.server = Server(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.path / "server.crt"), str(self.path / "server.key"))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)
        original_connect, original_resolve = socket.socket.connect, socket.getaddrinfo
        def connect(sock, address):
            if address != self.server.server_address:
                raise AssertionError("non_owned_connection_forbidden")
            return original_connect(sock, address)
        def resolve(host, *args, **kwargs):
            if host != "127.0.0.1":
                raise AssertionError("external_dns_forbidden")
            return original_resolve(host, *args, **kwargs)
        for name, function in (("socket.socket.connect", connect), ("socket.getaddrinfo", resolve)):
            patcher = patch(name, function)
            patcher.start()
            self.addCleanup(patcher.stop)

    def stop(self):
        for pool in self.pools:
            pool.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.server_errors, [])

    def objects(self, trusted=True, hostname="journal-test.invalid"):
        from urllib3 import HTTPSConnectionPool
        owner = self
        def factory(**options):
            owner.assertEqual(options["cert_reqs"], "CERT_REQUIRED")
            owner.assertIs(options["retries"], False)
            pool = HTTPSConnectionPool("127.0.0.1", self.server.server_address[1],
                        cert_reqs=options["cert_reqs"], assert_hostname=hostname, server_hostname=hostname,
                        ca_certs=str(self.path / "server.crt") if trusted else options["ca_certs"],
                        timeout=options["timeout"], retries=False, maxsize=1, block=True)
            owner.pools.append(pool)
            class Route:
                def urlopen(self, method, url, **kwargs):
                    prefix = Config("usapp-j4c-log-1234abcd", JOURNAL).endpoint
                    owner.assertTrue(url.startswith(prefix + "/journals/"))
                    # Test-only routing: fixed logical endpoint -> this owned socket.
                    return pool.urlopen(method, url[len(prefix):], **kwargs)
            return Route()
        return Objects(Config("usapp-j4c-log-1234abcd", JOURNAL), "offline-id", "offline-secret",
                       pool_factory=factory, clock=lambda: STAMP)

@unittest.skipUnless(os.environ.get("USAPP_J4C_LOCAL_TLS_DIR"), "explicit synthetic TLS fixture required")
class LocalTlsTests(LocalTlsFixture, unittest.TestCase):
    def test_actual_tls_and_exact_readback(self):
        objects = self.objects()
        self.assertEqual(objects.put_once(self.key, self.data), p.Receipt.CONFIRMED)
        self.assertEqual(self.requests, [("PUT", "/" + self.key), ("GET", "/" + self.key)])

    def test_untrusted_ca_and_wrong_hostname_never_send_http(self):
        for objects in (self.objects(trusted=False), self.objects(hostname="wrong.invalid")):
            self.assertEqual(objects.put_once(self.key, self.data), p.Receipt.UNKNOWN)
        self.assertEqual(self.requests, [])

    def test_real_pool_does_not_follow_redirect(self):
        self.mode = "redirect"
        self.assertEqual(self.objects().put_once(self.key, self.data), p.Receipt.UNKNOWN)
        self.assertEqual(self.requests, [("PUT", "/" + self.key)])

    def test_real_stalled_read_times_out_without_resending_put(self):
        self.mode = "stall"
        start = time.monotonic()
        self.assertEqual(self.objects().put_once(self.key, self.data), p.Receipt.UNKNOWN)
        elapsed = time.monotonic() - start
        self.assertGreaterEqual(elapsed, 2.5)
        self.assertLess(elapsed, 5.0)
        self.assertEqual(self.requests, [("PUT", "/" + self.key)])

if __name__ == "__main__":
    unittest.main()
