"""Real pinned SDK codecs through composition; in-memory HTTP responses, all socket/DNS blocked."""
import os
import unittest
from unittest.mock import patch

from tools import deletion_journal as j1
from tools.deletion_journal_bridge import mac
from tools.journal_backend import Backend, Credential
from tools import deletion_journal_publication as publication
from tests.test_journal_backend import config
from test_j4c_tablestore_sdk import Pool as TablePool
from test_j4c_oss_objects import Pool as ObjectPool, candidate
from test_j4c_publication_sdk import Server
from test_j4c_metadata import operation


@unittest.skipUnless(os.environ.get("USAPP_J4C_OFFLINE_SDK") == "1", "explicit isolated pinned SDK runner required")
class BackendSdkTests(unittest.TestCase):
    def setUp(self):
        for name in ("socket.socket.connect", "socket.create_connection", "socket.getaddrinfo"):
            patcher = patch(name, side_effect=AssertionError("outbound_network_forbidden"))
            patcher.start()
            self.addCleanup(patcher.stop)
        self.remote = Server(self)
        self.table_pool = None
        self.object_pools = []
        shared = {}
        def table_factory(**options):
            self.table_pool = TablePool(**options)
            self.table_pool.reply_hook = lambda api, body, message: None if api == "DescribeTable" else self.remote.reply(api, body, message)
            self.table_pool.clears = 0
            def clear():
                self.table_pool.clears += 1
            self.table_pool.clear = clear
            return self.table_pool
        def objects_factory(**options):
            pool = ObjectPool(**options)
            pool.data = shared
            pool.active = lambda: self.remote.active
            pool.clears = 0
            def clear():
                pool.clears += 1
            pool.clear = clear
            self.object_pools.append(pool)
            return pool
        self.backend = Backend(config(), Credential("object-publisher", "synthetic-publisher-secret"),
                               Credential("checkpoint-writer", "synthetic-writer-secret"), table_pool=table_factory,
                               publisher_pool=objects_factory, validator_pool=objects_factory)
        self.table_pool.sdk = self.backend.clients[0]._sdk
        self.addCleanup(self.backend.close)
        self.op = self.backend.dispatcher.publisher._operation(operation())

    def call(self, phase="intent", method="ensure_intent"):
        import json
        raw = j1.canonical(dict(version=1, journal_id=config().table.journal_id, nonce="c" * 32, method=method,
                                operation_id=self.op["operation_id"], operation=None if method == "lookup" else self.op,
                                phase=None if method == "lookup" else phase))
        status, body, signature = self.backend.dispatcher.handle(raw, mac("b" * 64, "request", raw))
        self.assertEqual(status, 200)
        self.assertEqual(signature, mac("b" * 64, "response", body))
        return json.loads(body)

    def test_full_service_backend_uses_sdk_and_separate_validation_principal(self):
        self.assertEqual(self.table_pool.calls, [])
        self.assertEqual(self.call()["state"], "ready")
        self.assertEqual(self.call("committed", "settle")["state"], "committed")
        self.assertEqual(self.call(method="lookup")["operation"], self.op)
        self.assertEqual(sum(c[1].endswith("/DescribeTable") for c in self.table_pool.calls), 1)
        output, validator = self.object_pools
        self.assertEqual(sum(c[0] == "PUT" for c in output.calls), 2)
        self.assertTrue(validator.calls)
        self.assertTrue(all(c[0] == "GET" for c in validator.calls))
        self.assertTrue(all("Credential=checkpoint-writer/" in c[2]["headers"]["authorization"] for c in validator.calls))
        self.assertTrue(all("Credential=object-publisher/" in c[2]["headers"]["authorization"] for c in output.calls))

    def test_bad_table_is_latched_before_object_io(self):
        self.table_pool.bad_table = True
        for _ in range(2):
            self.assertIn(self.call()["state"], ("unknown", "unavailable"))
        self.assertEqual(len(self.table_pool.calls), 1)
        self.assertTrue(all(not p.calls for p in self.object_pools))

    def test_sdk_lost_commit_is_not_aborted_or_repeated(self):
        self.remote.lose_commit = True
        self.assertEqual(self.call()["state"], "unknown")
        self.assertTrue(self.table_pool.calls[-1][1].endswith("/CommitTransaction"))
        puts = sum(c[0] == "PUT" for c in self.object_pools[0].calls)
        self.assertEqual(self.call()["state"], "ready")
        self.assertEqual(sum(c[0] == "PUT" for c in self.object_pools[0].calls), puts)

    def test_close_clears_each_pool_once_and_blocks_reuse(self):
        self.assertTrue(self.backend.close())
        self.assertTrue(self.backend.close())
        self.assertEqual(self.table_pool.clears, 1)
        self.assertEqual([p.clears for p in self.object_pools], [1, 1])
        before = len(self.table_pool.calls)
        with self.assertRaisesRegex(Exception, "client_closed"):
            self.backend.clients[0].describe()
        self.assertEqual(len(self.table_pool.calls), before)
        self.assertEqual(self.backend.dispatcher.handle(b"{}", "b" * 64)[0], 503)
        key, data = candidate()
        output = self.backend.clients[1]
        object_calls = len(self.object_pools[0].calls)
        with self.assertRaises(publication.Unavailable):
            output.get(key)
        self.assertEqual(output.put_once(key, data), publication.Receipt.UNKNOWN)
        self.assertEqual(len(self.object_pools[0].calls), object_calls)

    def test_client_close_refuses_an_active_transaction_without_aborting(self):
        client = self.backend.clients[0]
        tx = client.start()
        before = len(self.table_pool.calls)
        with self.assertRaisesRegex(Exception, "transaction_still_active"):
            client.close()
        self.assertEqual(len(self.table_pool.calls), before)
        self.assertEqual(self.table_pool.clears, 0)
        client.finish(tx)
        client.close()

    def test_missing_pool_cleanup_cannot_claim_resources_drained(self):
        clear = self.table_pool.clear
        del self.table_pool.clear
        try:
            self.assertFalse(self.backend.close())
            self.assertFalse(self.backend.clients[0]._closed)
        finally:
            self.table_pool.clear = clear
        self.assertTrue(self.backend.close())
        self.assertEqual(self.table_pool.clears, 1)


if __name__ == "__main__":
    unittest.main()
