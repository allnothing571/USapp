"""Durable local budget shared by fresh synthetic publication workers."""
import os
import tempfile
import time
import unittest

from tools.deletion_journal_publication import State, zero_head
from tools import deletion_journal as j1
from tools.j4c_trial import Trial, MAX_REQUESTS
from test_initialize_worker import InitializationFixture
from test_preflight import plan
from test_j4c_metadata import operation


@unittest.skipUnless(os.environ.get("USAPP_J4C_LOCAL_TLS_DIR"), "explicit synthetic TLS fixture required")
class TrialHttpsTests(InitializationFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.directory = tempfile.TemporaryDirectory(prefix="usapp-j4c-trial-net-")
        self.addCleanup(self.directory.cleanup)
        self.trial = Trial(plan(), self.directory.name)

    def test_initialize_publish_and_lost_commit_recovery_share_budget(self):
        self.assertEqual(self.trial.initialize(fixture=self.fixture).state, State.COMMITTED)
        op = operation()
        self.mode = "lost_commit"
        self.assertEqual(self.trial.publish(op, fixture=self.fixture).state, State.UNKNOWN)
        before = self.trial.stats()
        self.assertEqual(before["puts"], 1)
        self.assertGreater(before["requests"], 0)
        self.mode = "ok"
        self.assertEqual(self.trial.publish(op, fixture=self.fixture).state, State.READY)
        after = self.trial.stats()
        self.assertEqual(after["puts"], 1)
        self.assertEqual(after["operations"], 1)
        self.assertGreater(after["requests"], before["requests"])

    def test_exhausted_request_budget_stops_before_any_network(self):
        self.assertEqual(self.trial.initialize(fixture=self.fixture).state, State.COMMITTED)
        self.requests.clear()
        with self.trial.ledger.transaction() as (db, _, __):
            db.execute("UPDATE budget SET requests=? WHERE id=1", (MAX_REQUESTS,))
        self.assertEqual(self.trial.publish(operation(), fixture=self.fixture).state, State.UNAVAILABLE)
        self.assertEqual(self.requests, [])
        self.assertEqual(self.trial.stats()["requests"], MAX_REQUESTS)

    def test_expired_session_refuses_initialization_and_publication(self):
        with self.trial.ledger.transaction() as (db, _, __):
            db.execute("UPDATE budget SET end_ns=? WHERE id=1", (time.monotonic_ns() - 1,))
        self.assertEqual(self.trial.initialize(fixture=self.fixture).state, State.UNAVAILABLE)
        self.assertEqual(self.trial.publish(operation(), fixture=self.fixture).state, State.UNAVAILABLE)
        self.assertEqual(self.requests, [])

    def test_mid_read_budget_exhaustion_leaves_uncommitted_remote_transaction(self):
        self.assertEqual(self.trial.initialize(fixture=self.fixture).state, State.COMMITTED)
        self.requests.clear()
        with self.trial.ledger.transaction() as (db, _, __):
            db.execute("UPDATE budget SET requests=? WHERE id=1", (MAX_REQUESTS - 7,))
        self.assertEqual(self.trial.publish(operation(), fixture=self.fixture).state, State.UNAVAILABLE)
        self.assertTrue(self.engine.active)  # Fake lease expiry is deliberately NOT simulated.
        self.assertNotIn("CommitTransaction", self.requests)
        self.assertNotIn("PutRow", self.requests)
        self.assertEqual(self.trial.stats()["requests"], MAX_REQUESTS)
