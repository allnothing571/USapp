"""Complete spawned publication against owned HTTPS stateful fake services."""
import os
from pathlib import Path
import socket
import ssl
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from tools import deletion_journal as j1
from tools.deletion_journal_publication import State
from tools.j4c_local_transport import Fixture
from tools.j4c_publication_worker import publish
from tools.j4c_tablestore_sdk import Client
from test_j4c_tablestore_sdk import Pool, config
from test_j4c_publication_sdk import Server
from test_j4c_metadata import operation
from test_preflight import plan


class WorkerServerFixture:
    def setUp(self):
        self.path = Path(os.environ["USAPP_J4C_LOCAL_TLS_DIR"])
        self.engine, self.pool = Server(self), Pool()
        # The server uses the real SDK response signer with SYNTHETIC credentials.
        self.pool.sdk = Client(config(), "offline-access-id", "offline-synthetic-secret")._sdk
        self.pool.reply_hook = lambda api, body, message: None if api == "DescribeTable" else self.engine.reply(api, body, message)
        self.objects, self.requests, self.errors = {}, [], []
        self.mode = "ok"
        owner = self
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def handle(self):
                try:
                    super().handle()
                except (ConnectionResetError, BrokenPipeError, ssl.SSLEOFError):
                    pass

            def do_POST(self):
                if not self.path.startswith("/tables/"):
                    self.send_error(404)
                    return
                api = self.path.rsplit("/", 1)[-1]
                owner.requests.append(api)
                size = int(self.headers.get("Content-Length", "0"))
                owner.assertLessEqual(size, 1024 * 1024)
                response = owner.pool.urlopen("POST", config().endpoint + "/" + api,
                            body=self.rfile.read(size), headers={k.lower(): v for k, v in self.headers.items()})
                if owner.mode == "lost_commit" and api == "CommitTransaction":
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                self.send_response(response.status)
                for key, value in response.headers.items():
                    self.send_header(key, value)
                if owner.mode == "encoded_tables":
                    self.send_header("Content-Encoding", "gzip")
                trickle = owner.mode == "trickle_commit" and api == "CommitTransaction"
                self.send_header("Content-Length", str(10000 if trickle else len(response.data)))
                self.end_headers()
                if trickle:
                    for _ in range(100):
                        self.wfile.write(b"x" * 100)
                        self.wfile.flush()
                        time.sleep(0.1)
                elif response.data:
                    self.wfile.write(response.data)

            def do_PUT(self):
                owner.requests.append("OBJECT_PUT")
                owner.assertTrue(self.path.startswith("/objects/journals/"))
                owner.assertFalse(owner.engine.active, "object_call_under_transaction")
                owner.assertEqual(self.headers.get("x-oss-forbid-overwrite"), "true")
                size = int(self.headers.get("Content-Length", "0"))
                owner.assertLessEqual(size, 1024 * 1024)
                data = self.rfile.read(size)
                existed = self.path in owner.objects
                if not existed:
                    owner.objects[self.path] = data
                self.send_response(409 if existed else 200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def do_GET(self):
                owner.requests.append("OBJECT_GET")
                owner.assertFalse(owner.engine.active, "object_call_under_transaction")
                data = owner.objects.get(self.path)
                self.send_response(200 if data is not None else 404)
                self.send_header("Content-Length", str(len(data) if data else 0))
                self.end_headers()
                if data:
                    self.wfile.write(data)
        class OwnedServer(ThreadingHTTPServer):
            daemon_threads = False

            def handle_error(self, request, client_address):
                owner.errors.append(type(sys.exc_info()[1]).__name__)
        self.server = OwnedServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.path / "server.crt"), str(self.path / "server.key"))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)
        self.fixture = Fixture(self.server.server_address[1], str(self.path))

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.errors, [])

    def call(self, op, phase="intent", timeout=5):
        began = time.monotonic()
        result = publish(plan(), op, phase, timeout, fixture=self.fixture)
        print("J4C_WORKER_TEST state={} elapsed={:.3f}".format(result.state.value, time.monotonic() - began), flush=True)
        return result

@unittest.skipUnless(os.environ.get("USAPP_J4C_LOCAL_TLS_DIR"), "explicit synthetic TLS fixture required")
class WorkerHttpsTests(WorkerServerFixture, unittest.TestCase):
    def test_complete_publication_and_replay_in_fresh_workers(self):
        op = operation()
        result = self.call(op)
        self.assertEqual(result.state, State.READY)
        self.assertFalse(result.serving_allowed)
        self.assertTrue(result.worker_reaped)
        self.assertEqual(self.call(op, "committed").state, State.COMMITTED)
        before = dict(self.engine.rows)
        self.assertEqual(self.call(op).state, State.COMMITTED)
        self.assertEqual(self.engine.rows, before)
        self.assertEqual(len(self.objects), 2)
        self.assertIn("DescribeTable", self.requests)
        self.assertIn("UpdateRow", self.requests)

    def test_lost_commit_receipt_never_aborts_and_fresh_worker_recovers(self):
        op = operation()
        self.mode = "lost_commit"
        self.assertEqual(self.call(op).state, State.UNKNOWN)
        self.assertEqual(self.requests[-1], "CommitTransaction")
        self.mode = "ok"
        self.assertEqual(self.call(op).state, State.READY)
        self.assertEqual(len(self.objects), 1)

    def test_full_worker_deadline_after_commit_leaves_unknown_then_recovers(self):
        op = operation()
        self.mode = "trickle_commit"
        result = self.call(op, timeout=5.0)
        self.assertTrue(result.timed_out)
        self.assertTrue(result.worker_reaped)
        self.assertEqual(result.state, State.UNKNOWN)
        self.assertFalse(result.proven_not_applied)
        self.assertEqual(self.requests[-1], "CommitTransaction")
        self.assertEqual(j1._decode(self.engine.rows["head", "current"])["batch_seq"], 1)
        self.mode = "ok"
        self.assertEqual(self.call(op).state, State.READY)
        self.assertEqual(len(self.objects), 1)

    def test_invalid_table_settings_stop_before_publication(self):
        self.pool.bad_table = True
        self.assertEqual(self.call(operation()).state, State.UNAVAILABLE)
        self.assertEqual(self.requests, ["DescribeTable"])
        self.assertFalse(self.objects)

    def test_missing_head_never_initializes_or_writes(self):
        self.engine.rows.clear()
        self.assertEqual(self.call(operation()).state, State.UNAVAILABLE)
        self.assertFalse(self.engine.rows)
        self.assertFalse(self.objects)
        self.assertNotIn("PutRow", self.requests)

    def test_encoded_sdk_response_is_refused_before_parsing(self):
        self.mode = "encoded_tables"
        self.assertEqual(self.call(operation()).state, State.UNAVAILABLE)
        self.assertEqual(self.requests, ["DescribeTable"])
        self.assertFalse(self.objects)


if __name__ == "__main__":
    unittest.main()
