"""Dedicated recovery MAC domain over actual loopback TLS; no production Source/release wiring."""
import json
import os
from pathlib import Path
import threading
import unittest

from tools import deletion_journal as j1
from tools.deletion_journal_bridge import mac as publication_mac
from tools.journal_backend import Credential, Service
from tools.journal_https_server import Config as TlsConfig
from tools.journal_recovery_reader import Context, RecoveryBackend, mac
from tests.test_journal_backend import Fixtures, config
from tests.test_deletion_journal_publication import uid
import tests.test_journal_https_server as tls


class RecoveryTlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pki = Path(os.environ["USAPP_JOURNAL_TLS_TEST_DIR"])

    def setUp(self):
        self.fixtures = Fixtures()
        self.context = Context(config().table.journal_id, uid(), uid())
        self.backend = RecoveryBackend(config(), self.context, Credential("recovery-reader", "synthetic-reader-secret"),
                                       _client_type=self.fixtures.Client, _objects_type=self.fixtures.Objects)
        self.service = Service(TlsConfig(str(self.pki / "server.crt"), str(self.pki / "server.key"), io_timeout=.2), self.backend).start()
        self.releases = []

    def tearDown(self):
        for event in self.releases:
            event.set()
        self.assertTrue(all(self.service.close(2).values()))

    def request(self, method="snapshot", body=None, signature=None):
        if body is None:
            body = j1.canonical(dict(version=1, journal_id=self.context.journal_id, database_id=self.context.database_id,
                                     attempt_id=self.context.attempt_id, nonce="a" * 32, method=method))
        signature = signature or mac("b" * 64, "request", body)
        # Explicit body/signature avoid any publication-side defaults in this reusable HTTP-only fixture helper.
        raw = tls.ServerTests.request(self, body=body, signature=signature)
        with tls.ServerTests.connect(self, self.service.server) as conn:
            conn.sendall(raw)
            status, headers, response = tls.ServerTests.receive(self, conn)
        if status == 200:
            self.assertEqual(headers["x-usapp-journal-mac"], mac("b" * 64, "response", response))
            return status, json.loads(response)
        return status, None

    def test_signed_zero_snapshot_and_current_head_bind_external_context(self):
        for method in ("snapshot", "current_head"):
            status, response = self.request(method)
            self.assertEqual(status, 200)
            self.assertEqual(response["state"], "verified")
            self.assertEqual(response["database_id"], self.context.database_id)
            self.assertEqual(response["attempt_id"], self.context.attempt_id)
            self.assertFalse(response["serving_allowed"])
            if method == "current_head":
                self.assertIsNone(response["snapshot"])
        self.assertEqual(self.fixtures.writer.preflights, 1)
        self.assertEqual(self.fixtures.writer.calls.count("commit"), 0)

    def test_publication_domain_is_rejected_before_preflight(self):
        body = j1.canonical(dict(version=1, journal_id=self.context.journal_id, database_id=self.context.database_id,
                                 attempt_id=self.context.attempt_id, nonce="a" * 32, method="snapshot"))
        self.assertEqual(self.request(body=body, signature=publication_mac("b" * 64, "request", body))[0], 401)
        self.assertEqual(self.fixtures.writer.preflights, 0)

    def test_populated_external_snapshot_preserves_all_barriers_and_intent_ids(self):
        from tools.deletion_journal_publication import Publisher, State
        from tools.j4c_metadata import Metadata
        from tests.test_deletion_journal_publication import operation
        objects = self.backend.clients[1]
        writer = Publisher(self.context.journal_id, objects, Metadata(self.fixtures.writer, objects))
        op = writer._operation(operation())
        self.assertEqual(writer.publish(op), State.READY)  # Separate maintenance writer in this fixture only.
        before = self.fixtures.writer.calls.count("commit")
        status, response = self.request()
        self.assertEqual(status, 200)
        self.assertEqual(response["state"], "verified")
        self.assertEqual(response["snapshot"]["head"]["seq"], 1)
        self.assertEqual(response["checkpoint"]["event_seq"], 2)
        self.assertEqual(len(response["snapshot"]["hidden"]), 2)
        self.assertEqual(response["snapshot"]["unresolved"], sorted(s["intent_id"] for s in op["subjects"]))
        self.assertEqual(self.fixtures.writer.calls.count("commit"), before)
        self.assertEqual(len(objects.puts), 1)

    def test_missing_external_head_is_unknown_not_empty_or_repaired(self):
        self.fixtures.writer.rows.clear()
        status, response = self.request()
        self.assertEqual(status, 200)
        self.assertEqual(response["state"], "unknown")
        self.assertIsNone(response["snapshot"])
        self.assertEqual(self.fixtures.writer.rows, {})

    def test_slow_recovery_read_retains_worker_and_resources_after_http_timeout(self):
        entered, release = threading.Event(), threading.Event()
        self.releases.append(release)
        self.fixtures.writer.on_start = lambda: (entered.set(), release.wait(3))
        self.assertEqual(self.request()[0], 503)
        self.assertTrue(entered.is_set())
        self.assertTrue(self.service.server.snapshot()["backend_busy"])
        result = self.service.close(.1)
        self.assertFalse(result["backend_drained"])
        self.assertFalse(result["resources_drained"])
        self.assertFalse(any(c.closed for c in self.backend.clients))
        release.set()

    def test_unknown_request_cannot_turn_into_a_writer_call(self):
        self.assertEqual(self.request("ensure_intent")[0], 400)
        self.assertEqual(self.fixtures.writer.calls, [])


if __name__ == "__main__":
    unittest.main()
