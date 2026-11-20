"""Fresh-partition bootstrap over real SDK and owned local HTTPS only."""
import os
import unittest

from tools import deletion_journal as j1
from tools.deletion_journal_publication import State, zero_head
from tools.j4c_initialize import initialize, receipt
from test_worker_https import WorkerServerFixture
from test_preflight import plan


class InitializationFixture(WorkerServerFixture):
    def setUp(self):
        super().setUp()
        self.engine.rows.clear()
        original = self.pool.reply_hook
        def reply(api, body, message):
            if api == "GetRange":
                from tablestore.protobuf import table_store_pb2 as pb
                from tablestore import INF_MIN, INF_MAX
                from tablestore.plainbuffer.plain_buffer_builder import PlainBufferBuilder
                request = pb.GetRangeRequest()
                request.ParseFromString(body)
                prefix = [("journal_id", plan()["journal_id"])]
                start = bytes(PlainBufferBuilder.serialize_primary_key(
                    prefix + [("row_type", INF_MIN), ("row_id", INF_MIN)]))
                if request.inclusive_start_primary_key == start:
                    self.assertTrue(self.engine.active)
                    self.assertEqual(request.transaction_id, "offline-tx")
                    self.assertEqual(request.limit, 1)
                    self.assertEqual(request.exclusive_end_primary_key,
                        bytes(PlainBufferBuilder.serialize_primary_key(
                            prefix + [("row_type", INF_MAX), ("row_id", INF_MAX)])))
                    keys = sorted(self.engine.staged)
                    message.rows = self.engine.encode(*keys[0], self.engine.staged[keys[0]]) if keys else b""
                    return
            return original(api, body, message)
        self.pool.reply_hook = reply


@unittest.skipUnless(os.environ.get("USAPP_J4C_LOCAL_TLS_DIR"), "explicit synthetic TLS fixture required")
class InitializationHttpsTests(InitializationFixture, unittest.TestCase):
    def test_empty_initialize_and_idempotent_replay(self):
        result = initialize(plan(), fixture=self.fixture)
        self.assertEqual(result.state, State.COMMITTED)
        self.assertTrue(result.worker_reaped)
        self.assertFalse(result.serving_allowed)
        self.assertEqual(j1._decode(self.engine.rows["bootstrap", "current"]), receipt(plan()))
        before = dict(self.engine.rows)
        self.assertEqual(initialize(plan(), fixture=self.fixture).state, State.COMMITTED)
        self.assertEqual(self.engine.rows, before)
        self.assertEqual(self.requests.count("PutRow"), 2)

    def test_unknown_rows_and_bare_seed_are_not_empty(self):
        for key in (("rogue", "x"), ("head", "current")):
            self.engine.rows.clear()
            self.engine.rows[key] = j1.canonical(zero_head(plan()["journal_id"])).decode("ascii")
            before = dict(self.engine.rows)
            self.assertEqual(initialize(plan(), fixture=self.fixture).state, State.CONFLICT)
            self.assertEqual(self.engine.rows, before)
        self.assertNotIn("PutRow", self.requests)

    def test_lost_commit_then_receipt_replay_preserves_advanced_head(self):
        self.mode = "lost_commit"
        self.assertEqual(initialize(plan(), fixture=self.fixture).state, State.UNKNOWN)
        self.assertEqual(self.requests[-1], "CommitTransaction")
        self.mode = "ok"
        head = dict(zero_head(plan()["journal_id"]), batch_seq=1, event_seq=1,
                    batch_head="a" * 64, event_head="b" * 64)
        self.engine.rows["head", "current"] = j1.canonical(head).decode("ascii")
        before = dict(self.engine.rows)
        self.assertEqual(initialize(plan(), fixture=self.fixture).state, State.COMMITTED)
        self.assertEqual(self.engine.rows, before)
        self.assertEqual(self.requests.count("PutRow"), 2)
