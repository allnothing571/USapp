"""Actual TLS composition/lifecycle, stateful metadata facade fake; PKI supplied by owned harness."""
import os
from pathlib import Path
import threading
import time
import unittest
from unittest.mock import patch

from tools.journal_backend import Service
from tools.journal_https_server import Config
from tests.test_journal_backend import Fixtures, config
from tests.test_j4c_metadata import operation
import tests.test_journal_https_server as tls


class BackendTlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pki = Path(os.environ["USAPP_JOURNAL_TLS_TEST_DIR"])

    def setUp(self):
        self.fixture = Fixtures()
        self.backend = self.fixture.build()
        self.journal, self.key = config().table.journal_id, "b" * 64
        self.op = self.backend.dispatcher.publisher._operation(operation())
        self.service = Service(Config(str(self.pki / "server.crt"), str(self.pki / "server.key"), io_timeout=.3), self.backend).start()
        self.releases = []

    def tearDown(self):
        for event in self.releases:
            event.set()
        self.assertTrue(all(self.service.close(2).values()))

    def body(self, *args):
        return tls.ServerTests.body(self, *args)

    def call(self, raw=None):
        with tls.ServerTests.connect(self, self.service.server) as conn:
            conn.sendall(tls.ServerTests.request(self) if raw is None else raw)
            return tls.ServerTests.receive(self, conn)

    def test_https_publish_and_client_resources_drain(self):
        import json
        self.assertEqual(self.fixture.writer.preflights, 0)
        status, _, body = self.call()
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["state"], "ready")
        self.assertEqual(self.fixture.writer.preflights, 1)
        self.assertTrue(all(self.service.close(1).values()))
        self.assertTrue(all(client.closed for client in self.backend.clients))

    def test_bad_auth_does_not_run_preflight(self):
        raw = tls.ServerTests.request(self, signature="c" * 64)
        self.assertEqual(self.call(raw)[0], 401)
        self.assertEqual(self.fixture.writer.preflights, 0)

    def test_missing_head_is_unavailable_without_initialization(self):
        import json
        self.fixture.writer.rows.clear()
        self.assertEqual(json.loads(self.call()[2])["state"], "unavailable")
        self.assertEqual(self.fixture.writer.rows, {})
        self.assertEqual(self.fixture.data, {})

    def test_active_publication_cannot_close_pools(self):
        entered, release = threading.Event(), threading.Event()
        self.releases.append(release)
        self.fixture.object_instances[0].after_put = lambda: (entered.set(), release.wait(3))
        conn = tls.ServerTests.connect(self, self.service.server)
        try:
            conn.sendall(tls.ServerTests.request(self))
            self.assertTrue(entered.wait(1))
            result = self.service.close(.15)
            self.assertFalse(result["backend_drained"])
            self.assertFalse(result["resources_drained"])
            self.assertFalse(any(c.closed for c in self.backend.clients))
            release.set()
        finally:
            conn.close()

    def test_slow_pool_cleanup_has_one_worker_and_same_close_deadline(self):
        entered, release = threading.Event(), threading.Event()
        self.releases.append(release)
        client = self.backend.clients[-1]
        close = client.close
        def stalled():
            entered.set()
            release.wait(3)
            close()
        with patch.object(client, "close", side_effect=stalled) as mocked:
            start = time.monotonic()
            self.assertFalse(self.service.close(.2)["resources_drained"])
            self.assertLess(time.monotonic() - start, .7)
            self.assertTrue(entered.is_set())
            self.assertFalse(self.service.close(.1)["resources_drained"])
            self.assertEqual(mocked.call_count, 1)
            release.set()
            self.assertTrue(self.service.close(1)["resources_drained"])

    def test_backend_cannot_attach_to_two_services(self):
        with self.assertRaisesRegex(ValueError, "already_attached_or_closed"):
            Service(self.service.server.config, self.backend)

    def test_closed_backend_cannot_start_a_listener(self):
        fixture = Fixtures()
        backend = fixture.build()
        service = Service(self.service.server.config, backend)
        self.assertTrue(backend.close())
        with self.assertRaisesRegex(ValueError, "journal_backend_closed"):
            service.start()
        self.assertIsNone(service.server.listener)
        self.assertTrue(all(service.close(1).values()))

    def test_duplicate_start_closes_the_original_listener(self):
        with self.assertRaises(ValueError):
            self.service.start()
        self.assertTrue(self.service.server.closing.is_set())
        self.assertTrue(all(self.service.close(1).values()))

    def test_invalid_tls_configuration_closes_constructed_backend(self):
        fixture = Fixtures()
        backend = fixture.build()
        with self.assertRaisesRegex(ValueError, "^journal_service_construction_failed$"):
            Service(Config(str(self.pki / "missing-private-cert"), str(self.pki / "server.key")), backend)
        self.assertTrue(all(client.closed for client in backend.clients))


if __name__ == "__main__":
    unittest.main()
