"""Read-only recovery facade with real pinned SDK codecs, all connections/DNS forbidden."""
import hashlib
import os
import unittest
from unittest.mock import patch

from tools import deletion_journal as j1
from tools import deletion_journal_publication as publication
from tools.journal_backend import Credential
from tools.journal_recovery_reader import Context, RecoveryBackend
from tests.test_journal_backend import config
from test_j4c_publication_sdk import Server
from test_j4c_tablestore_sdk import Pool as TablePool
from test_j4c_oss_objects import Pool as ObjectPool
from test_j4c_metadata import operation


@unittest.skipUnless(os.environ.get("USAPP_J4C_OFFLINE_SDK") == "1", "explicit pinned SDK runner required")
class RecoverySdkTests(unittest.TestCase):
    def setUp(self):
        for name in ("socket.socket.connect", "socket.create_connection", "socket.getaddrinfo"):
            patcher = patch(name, side_effect=AssertionError("outbound_network_forbidden"))
            patcher.start(); self.addCleanup(patcher.stop)
        self.remote = Server(self)
        def table_factory(**options):
            self.table = TablePool(**options)
            self.table.reply_hook = lambda api, body, message: None if api == "DescribeTable" else self.remote.reply(api, body, message)
            self.table.clear = lambda: None
            return self.table
        def objects_factory(**options):
            self.objects = ObjectPool(**options)
            self.objects.active = lambda: self.remote.active
            self.objects.clear = lambda: None
            return self.objects
        from tests.test_deletion_journal_publication import uid
        self.context = Context(config().table.journal_id, uid(), uid())
        self.backend = RecoveryBackend(config(), self.context, Credential("recovery-reader", "synthetic-reader-secret"),
                                       table_pool=table_factory, objects_pool=objects_factory)
        self.addCleanup(self.backend.close)
        self.table.sdk = self.backend.clients[0]._sdk

    def seed(self):
        # Maintenance fixture only: complete canonical immutable batch and independently derived metadata rows.
        validator = publication.Publisher(self.context.journal_id, None, None)
        self.op = validator._operation(operation())
        frame, head = validator._candidate(self.op, "intent", publication.zero_head(self.context.journal_id))
        key = publication.object_key(self.context.journal_id, frame["hash"])
        self.objects.data[config().objects.endpoint + "/" + key] = j1.canonical(frame) + b"\n"
        operations, cycles = publication.derive_indexes([frame])
        self.remote.rows["head", "current"] = j1.canonical(head).decode("ascii")
        self.remote.rows["batch", "00000000000000000001"] = j1.canonical(dict(key=key)).decode("ascii")
        self.remote.rows["operation", self.op["operation_id"]] = j1.canonical(operations[self.op["operation_id"]]).decode("ascii")
        for cycle in cycles:
            identity = hashlib.sha256(j1.canonical(list(cycle))).hexdigest()
            self.remote.rows["cycle", identity] = j1.canonical(list(cycle)).decode("ascii")
        return head

    def test_complete_snapshot_uses_only_read_transactions_and_reader_principal(self):
        head = self.seed()
        result = self.backend.dispatcher.reader.read("snapshot")
        self.assertIsNotNone(result)
        self.assertEqual(result["checkpoint"], head)
        self.assertEqual(result["snapshot"]["unresolved"], [self.op["subjects"][0]["intent_id"]])
        allowed = {"DescribeTable", "StartLocalTransaction", "AbortTransaction", "GetRow", "GetRange"}
        self.assertTrue(all(call[1].rsplit("/", 1)[-1] in allowed for call in self.table.calls))
        self.assertTrue(self.objects.calls)
        self.assertTrue(all(c[0] == "GET" and "Credential=recovery-reader/" in c[2]["headers"]["authorization"] for c in self.objects.calls))

    def test_mutating_facade_calls_and_private_transport_calls_are_denied_before_io(self):
        client = self.backend.clients[0]
        tx = client.start()
        before = len(self.table.calls)
        for call in (lambda: client.finish(tx, True), lambda: client.insert(tx, "head", "current", {}),
                     lambda: client.advance_head(tx, {}, {}), lambda: client.advance_operation(tx, "id", "x", "y"),
                     lambda: client._sdk.connection.send_receive("/CommitTransaction", {}, b"")):
            with self.assertRaises(Exception):
                call()
        self.assertEqual(len(self.table.calls), before)
        client.finish(tx)
        objects = self.backend.clients[1]
        self.assertEqual(objects.put_once("invalid", b"private"), publication.Receipt.CONFLICT)
        with self.assertRaises(publication.Unavailable):
            objects._request("PUT", "invalid", b"private")
        self.assertEqual(self.objects.calls, [])
        self.assertEqual(self.backend.dispatcher.reader.metadata.commit(None, None, None, None), publication.Receipt.CONFLICT)

    def test_missing_head_never_initializes(self):
        self.remote.rows.clear()
        self.assertIsNone(self.backend.dispatcher.reader.read("snapshot"))
        self.assertEqual(self.remote.rows, {})
        self.assertEqual(self.objects.calls, [])

    def test_signature_failure_is_unknown_not_zero(self):
        self.seed()
        self.table.bad_signature = True
        self.assertIsNone(self.backend.dispatcher.reader.read("snapshot"))
        self.assertEqual(self.objects.calls, [])


if __name__ == "__main__":
    unittest.main()
