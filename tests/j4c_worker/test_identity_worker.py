"""Synthetic bundle IPC and owned STS-shaped HTTPS server; no vendor requests."""
import datetime
import json
import os
from pathlib import Path
import re
import ssl
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from tools.j4c_call_deadline import run
from tools.j4c_credentials import parse
from tools.j4c_identity_preflight import _probe
from tools.j4c_local_transport import Fixture
from tools.j4c_resource_package import POLICY_ROLES
from tools.deletion_journal_publication import Receipt
from test_j4c_credentials import raw
from test_j4c_initialize import plan


@unittest.skipUnless(os.environ.get("USAPP_J4C_LOCAL_TLS_DIR"), "explicit synthetic TLS fixture required")
class IdentityWorkerTests(unittest.TestCase):
    def setUp(self):
        self.calls, self.errors, self.bad_role = [], [], None
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                owner.assertEqual(self.path, "/identity/")
                owner.assertEqual(self.headers.get("x-acs-action"), "GetCallerIdentity")
                owner.assertEqual(self.headers.get("x-acs-version"), "2015-04-01")
                owner.assertEqual(int(self.headers.get("Content-Length", "0")), 0)
                key_id = re.search(r"Credential=synthetic-id-(\d+),", self.headers.get("authorization", "")).group(1)
                role = POLICY_ROLES[int(key_id)]
                owner.calls.append((role, self.headers.get("x-acs-signature-nonce")))
                value = dict(IdentityType="RAMUser", AccountId=plan()["declared_owner_uid"],
                             Arn="acs:ram::{}:user/usapp-j4c-{}-{}".format(plan()["declared_owner_uid"], plan()["run_id"], role))
                if role == owner.bad_role:
                    value["IdentityType"] = "Account"
                body = json.dumps(value).encode("ascii")
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        class Server(ThreadingHTTPServer):
            daemon_threads = False

            def handle_error(self, request, client_address):
                owner.errors.append(type(sys.exc_info()[1]).__name__)
        self.server = Server(("127.0.0.1", 0), Handler)
        self.path = Path(os.environ["USAPP_J4C_LOCAL_TLS_DIR"])
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.path / "server.crt"), str(self.path / "server.key"))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)
        self.fixture = Fixture(self.server.server_address[1], str(self.path))
        self.bundle = parse(raw(datetime.datetime.now(datetime.timezone.utc)), plan())

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.errors, [])

    def test_all_three_specific_identities_verified_in_owned_worker(self):
        result = run(_probe, args=(plan(), self.bundle, True, self.fixture), timeout=5)
        self.assertEqual(result.value, Receipt.CONFIRMED)
        self.assertTrue(result.worker_reaped)
        self.assertEqual([role for role, _ in self.calls], list(POLICY_ROLES))
        self.assertEqual(len({nonce for _, nonce in self.calls}), 3)

    def test_wrong_identity_stops_before_third_call(self):
        self.bad_role = POLICY_ROLES[1]
        result = run(_probe, args=(plan(), self.bundle, True, self.fixture), timeout=5)
        self.assertEqual(result.value, Receipt.UNKNOWN)
        self.assertEqual(len(self.calls), 2)
        self.assertTrue(result.worker_reaped)

    def test_missing_ack_never_connects(self):
        result = run(_probe, args=(plan(), self.bundle, False, self.fixture), timeout=5)
        self.assertEqual(result.value, Receipt.UNKNOWN)
        self.assertEqual(self.calls, [])
