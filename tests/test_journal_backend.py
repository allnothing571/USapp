"""Restricted adapter composition with stateful conditional metadata fakes, no SDK/network dependency."""
import copy
import json
import threading
import unittest
from unittest.mock import patch

from tools import deletion_journal as j1
from tools.deletion_journal_bridge import mac
from tools.journal_backend import Backend, Config, Credential
from tools.j4c_oss_objects import Config as ObjectConfig
from tools.j4c_tablestore_sdk import Config as TableConfig
from tests.test_j4c_metadata import Client as FakeClient, JOURNAL, operation
from tests.test_deletion_journal_publication import Objects as FakeObjects


def config(journal_id=JOURNAL):
    return Config(TableConfig("cn-shanghai", "uj4c1234abcd", "journal", journal_id,
                              "https://uj4c1234abcd.cn-shanghai.ots.aliyuncs.com"),
                  ObjectConfig("usapp-j4c-log-1234abcd", journal_id), "b" * 64)


class Fixtures:
    def __init__(self):
        self.writer = None
        self.object_instances = []
        self.data = {}
        self.fail_preflight = self.fail_head = self.fail_constructor = False
        fixture = self
        class Client(FakeClient):
            def __init__(self, cfg, key_id, secret, pool_factory=None):
                super().__init__(cfg.journal_id)
                self.config, self.key_id, self.secret = cfg, key_id, secret
                self.preflights = 0
                self.closed = False
                fixture.writer = self
                if fixture.fail_head:
                    self.rows.clear()

            def preflight(self):
                self.preflights += 1
                if fixture.fail_preflight:
                    raise RuntimeError("private credentials do not escape")
                return dict(table_verified=True, transaction_probe=True, serving_allowed=False)

            def close(self):
                if self.active:
                    raise RuntimeError("active transaction")
                self.closed = True

        class Objects(FakeObjects):
            def __init__(self, cfg, key_id, secret, pool_factory=None):
                if fixture.fail_constructor:
                    raise RuntimeError("private-constructor-value")
                super().__init__()
                self.config, self.key_id = cfg, key_id
                self.data = fixture.data
                self.closed = False
                fixture.object_instances.append(self)

            def get(self, key):
                if fixture.writer.active:
                    raise AssertionError("object_io_under_transaction")
                return super().get(key)

            def put_once(self, key, data):
                if fixture.writer.active:
                    raise AssertionError("object_io_under_transaction")
                return super().put_once(key, data)

            def close(self):
                self.closed = True

        self.Client, self.Objects = Client, Objects

    def build(self, configuration=None):
        return Backend(configuration or config(), Credential("object-publisher", "synthetic-publisher-secret"),
                       Credential("checkpoint-writer", "synthetic-writer-secret"),
                       _client_type=self.Client, _objects_type=self.Objects)


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixtures()
        self.backend = self.fixture.build()
        self.addCleanup(self.backend.close)
        self.op = self.backend.dispatcher.publisher._operation(operation())

    def call(self, method="ensure_intent", phase="intent", signature=None):
        request = dict(version=1, journal_id=JOURNAL, nonce="a" * 32, method=method,
                       operation_id=self.op["operation_id"], operation=None if method == "lookup" else self.op,
                       phase=None if method == "lookup" else phase)
        raw = j1.canonical(request)
        status, body, digest = self.backend.dispatcher.handle(raw, signature or mac("b" * 64, "request", raw))
        if status == 200:
            self.assertEqual(digest, mac("b" * 64, "response", body))
            return json.loads(body)
        return status

    def test_construction_and_auth_do_not_probe_or_initialize(self):
        self.assertEqual(self.fixture.writer.calls, [])
        self.assertEqual(self.fixture.writer.preflights, 0)
        self.assertEqual(self.call(signature="c" * 64), 401)
        self.assertEqual(self.fixture.writer.calls, [])
        self.assertEqual(self.fixture.writer.preflights, 0)
        raw = b"{}"
        self.assertEqual(self.backend.dispatcher.handle(raw, mac("b" * 64, "request", raw))[0], 400)
        self.assertEqual(self.fixture.writer.preflights, 0)

    def test_role_separation_and_real_metadata_publication(self):
        self.assertEqual(self.call()["state"], "ready")
        self.assertEqual(self.fixture.writer.preflights, 1)
        self.assertEqual(self.call("settle", "committed")["state"], "committed")
        self.assertEqual(self.call("lookup")["operation"], self.op)
        self.assertEqual(self.fixture.writer.preflights, 1)
        output, validator = self.fixture.object_instances
        self.assertEqual((output.key_id, validator.key_id, self.fixture.writer.key_id),
                         ("object-publisher", "checkpoint-writer", "checkpoint-writer"))
        self.assertEqual(len(output.puts), 2)
        self.assertEqual(validator.puts, [])
        self.assertTrue(validator.gets)

    def test_preflight_failure_is_latched_and_never_writes(self):
        self.fixture.fail_preflight = True
        before = copy.deepcopy(self.fixture.writer.rows)
        for _ in range(2):
            self.assertIn(self.call()["state"], ("unavailable", "unknown"))
        self.assertEqual(self.fixture.writer.preflights, 1)
        self.assertEqual(self.fixture.writer.rows, before)
        self.assertEqual(self.fixture.data, {})

    def test_missing_existing_head_never_bootstraps(self):
        self.fixture.writer.rows.clear()
        self.assertIn(self.call()["state"], ("unavailable", "unknown"))
        self.assertEqual(self.fixture.writer.rows, {})
        self.assertEqual(self.fixture.data, {})

    def test_loss_of_commit_reply_is_unknown_then_same_key_reads_original(self):
        self.fixture.writer.lose_commit = True
        self.assertEqual(self.call()["state"], "unknown")
        self.assertEqual(self.fixture.writer.calls[-1], "commit")
        puts = len(self.fixture.object_instances[0].puts)
        self.assertEqual(self.call()["state"], "ready")
        self.assertEqual(len(self.fixture.object_instances[0].puts), puts)

    def test_partial_index_write_aborts_metadata_only(self):
        self.fixture.writer.fail_write = "cycle"
        before = copy.deepcopy(self.fixture.writer.rows)
        self.assertIn(self.call()["state"], ("unknown", "unavailable"))
        self.assertEqual(self.fixture.writer.rows, before)
        self.assertFalse(self.fixture.writer.active)
        self.assertEqual(len(self.fixture.data), 1)

    def test_active_dispatch_retains_clients_through_close(self):
        entered, release = threading.Event(), threading.Event()
        output = self.fixture.object_instances[0]
        output.after_put = lambda: (entered.set(), release.wait(2))
        results = []
        thread = threading.Thread(target=lambda: results.append(self.call()))
        thread.start()
        try:
            self.assertTrue(entered.wait(1))
            self.assertFalse(self.backend.close())
            self.assertFalse(any(c.closed for c in self.backend.clients))
            self.assertEqual(self.call(), 503)
        finally:
            release.set()
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual([r["state"] for r in results], ["ready"])
        self.assertTrue(self.backend.close())
        self.assertTrue(all(c.closed for c in self.backend.clients))
        self.assertTrue(self.backend.close())
        self.assertEqual(self.call(), 503)

    def test_cleanup_failure_is_reported_and_can_be_retried(self):
        with patch.object(self.fixture.object_instances[0], "close", side_effect=RuntimeError("private")):
            self.assertFalse(self.backend.close())
        self.assertTrue(self.backend.close())

    def test_constructor_failure_closes_already_created_client(self):
        fixture = Fixtures()
        fixture.fail_constructor = True
        with self.assertRaisesRegex(ValueError, "^journal_backend_construction_failed$"):
            fixture.build()
        self.assertTrue(fixture.writer.closed)

    def test_config_credentials_repr_and_binding_refusals(self):
        self.assertNotIn("b" * 64, repr(config()))
        credential = Credential("private-id", "private-secret")
        self.assertNotIn("private", repr(credential))
        with self.assertRaises(ValueError):
            Backend(config(), credential, credential)
        bad = Config(config().table, ObjectConfig("usapp-j4c-log-deadbeef", JOURNAL), "b" * 64)
        with self.assertRaisesRegex(ValueError, "^invalid_journal_backend_configuration$"):
            bad.validate()
        with self.assertRaises(ValueError):
            Credential("newline\n", "secret").validate()
        with self.assertRaisesRegex(ValueError, "^invalid_journal_backend_configuration$"):
            Backend(None, credential, credential)


if __name__ == "__main__":
    unittest.main()
