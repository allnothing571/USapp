"""Real SDK request/response codecs + stateful in-memory server; no network."""
import copy
import os
import unittest
from unittest.mock import patch

from tools import deletion_journal as j1
from tools import deletion_journal_publication as p
from tools.j4c_metadata import Metadata
from tools.j4c_oss_objects import Objects, Config as ObjectConfig
from tools.j4c_tablestore_sdk import Client
from test_j4c_tablestore_sdk import Pool, config, JOURNAL
from test_j4c_oss_objects import Pool as ObjectPool, STAMP
from test_j4c_metadata import operation


class Server:
    """Transactional fake. Not evidence of vendor isolation, durability or conflict behavior."""
    def __init__(self, testcase):
        self.case = testcase
        self.rows = {("head", "current"): j1.canonical(p.zero_head(JOURNAL)).decode("ascii")}
        self.active, self.staged = False, None
        self.lose_commit = False
        self.fail_kind = None

    @staticmethod
    def decode(data):
        from tablestore.plainbuffer.plain_buffer_stream import PlainBufferInputStream
        from tablestore.plainbuffer.plain_buffer_coded_stream import PlainBufferCodedInputStream
        return PlainBufferCodedInputStream(PlainBufferInputStream(data)).read_row()

    @staticmethod
    def encode(kind, row_id, value):
        from tablestore.plainbuffer.plain_buffer_builder import PlainBufferBuilder
        return bytes(PlainBufferBuilder.serialize_for_put_row(
            [("journal_id", JOURNAL), ("row_type", kind), ("row_id", row_id)], [("payload", value, 1)]))

    def reply(self, api, body, message):
        from tablestore.protobuf import table_store_pb2 as pb
        request = getattr(pb, api + "Request")()
        request.ParseFromString(body)
        if api == "StartLocalTransaction":
            self.case.assertFalse(self.active)
            self.active, self.staged = True, copy.deepcopy(self.rows)
            self.case.assertEqual(request.table_name, "journal")
            return
        self.case.assertTrue(self.active)
        self.case.assertEqual(request.transaction_id, "offline-tx")
        if api == "CommitTransaction":
            self.rows, self.staged, self.active = self.staged, None, False
            if self.lose_commit:
                self.lose_commit = False
                raise TimeoutError("synthetic-lost-commit-response")
            return
        if api == "AbortTransaction":
            self.staged, self.active = None, False
            return
        self.case.assertEqual(request.table_name, "journal")
        if api == "GetRange":
            start, _ = self.decode(request.inclusive_start_primary_key)
            self.case.assertEqual(start[:2], [("journal_id", JOURNAL), ("row_type", start[1][1])])
            kind, first = start[1][1], start[2][1]
            keys = sorted(k for k in self.staged if k[0] == kind and k[1] >= first)
            # Force pagination even for two-row fixtures.
            page = keys[:1]
            encoded = [self.encode(k, i, self.staged[k, i]) for k, i in page]
            message.rows = encoded[0] if encoded else b""
            if len(keys) > 1:
                from tablestore.plainbuffer.plain_buffer_builder import PlainBufferBuilder
                message.next_start_primary_key = bytes(PlainBufferBuilder.serialize_primary_key(
                    [("journal_id", JOURNAL), ("row_type", kind), ("row_id", keys[1][1])]))
            return
        binary = request.primary_key if api == "GetRow" else (request.row if api == "PutRow" else request.row_change)
        key, attrs = self.decode(binary)
        self.case.assertEqual(key[:2], [("journal_id", JOURNAL), ("row_type", key[1][1])])
        kind, row_id = key[1][1], key[2][1]
        if api == "GetRow":
            value = self.staged.get((kind, row_id))
            message.row = self.encode(kind, row_id, value) if value is not None else b""
            return
        self.case.assertIn(api, ("PutRow", "UpdateRow"))
        if self.fail_kind == kind:
            raise RuntimeError("synthetic-index-write-failure")
        self.case.assertEqual(attrs[0][0], "payload")
        self.case.assertEqual(len(attrs), 1)
        if api == "PutRow":
            self.case.assertEqual(request.condition.row_existence, pb.EXPECT_NOT_EXIST)
            self.case.assertNotIn((kind, row_id), self.staged)
        else:
            self.case.assertEqual(request.condition.row_existence, pb.EXPECT_EXIST)
            self.case.assertIn(self.staged[kind, row_id].encode("ascii"), request.condition.column_condition)
        self.staged[kind, row_id] = attrs[0][1]


@unittest.skipUnless(os.environ.get("USAPP_J4C_OFFLINE_SDK") == "1", "run isolated SDK verification runner")
class PublicationSdkTests(unittest.TestCase):
    def setUp(self):
        for name in ("socket.socket.connect", "socket.create_connection", "socket.getaddrinfo"):
            patcher = patch(name, side_effect=AssertionError("outbound_network_forbidden"))
            patcher.start()
            self.addCleanup(patcher.stop)
        self.server = Server(self)
        self.pool = None
        def sdk_pool(**kwargs):
            self.pool = Pool(**kwargs)
            self.pool.reply_hook = self.server.reply
            return self.pool
        self.client = Client(config(), "offline-access-id", "offline-synthetic-secret", pool_factory=sdk_pool)
        self.pool.sdk = self.client._sdk
        def object_pool(**kwargs):
            self.object_pool = ObjectPool(**kwargs)
            self.object_pool.active = lambda: self.server.active
            return self.object_pool
        self.objects = Objects(ObjectConfig("usapp-j4c-log-1234abcd", JOURNAL), "offline-id", "offline-secret",
                               pool_factory=object_pool, clock=lambda: STAMP)
        self.metadata = Metadata(self.client, self.objects)
        self.publisher = p.Publisher(JOURNAL, self.objects, self.metadata)

    def test_full_intent_terminal_chain_uses_real_sdk_codecs(self):
        op = operation()
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.assertEqual(self.publisher.publish(op, "committed"), p.State.COMMITTED)
        snapshot = self.metadata.read(JOURNAL)
        self.assertEqual(snapshot["checkpoint"]["batch_seq"], 2)
        self.assertEqual(snapshot["operations"][op["operation_id"]]["phase"], "committed")
        self.assertEqual(len(snapshot["cycles"]), 1)
        self.assertFalse(self.publisher.inspect()["serving_allowed"])

    def test_lost_commit_response_recovers_from_real_sdk_reads(self):
        op = operation()
        self.server.lose_commit = True
        self.assertEqual(self.publisher.publish(op), p.State.UNKNOWN)
        self.assertEqual(self.pool.calls[-1][1].rsplit("/", 1)[-1], "CommitTransaction")
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.assertEqual(self.metadata.read(JOURNAL)["checkpoint"]["batch_seq"], 1)

    def test_index_error_aborts_all_metadata_and_leaves_unreferenced_object(self):
        before = copy.deepcopy(self.server.rows)
        self.server.fail_kind = "cycle"
        self.assertEqual(self.publisher.publish(operation()), p.State.UNKNOWN)
        self.assertEqual(self.server.rows, before)
        self.assertFalse(self.server.active)
        self.assertEqual(len(self.object_pool.data), 1)
        self.assertEqual(self.pool.calls[-1][1].rsplit("/", 1)[-1], "AbortTransaction")


if __name__ == "__main__":
    unittest.main()
