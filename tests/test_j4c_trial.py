"""Conservative local quotas, crash reservations and lifetime checks; no network."""
import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import j4c_trial as trial
from test_j4c_initialize import plan
from test_j4c_metadata import operation


class TrialTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="usapp-j4c-trial-")
        self.addCleanup(self.directory.cleanup)
        self.session = trial.Trial(plan(), self.directory.name)

    def test_existing_ledger_cannot_reset_quotas(self):
        self.session.ledger.request("GET", None)
        with self.assertRaises(FileExistsError):
            trial.Trial(plan(), self.directory.name)
        self.assertEqual(self.session.stats()["requests"], 1)

    def test_ten_operations_and_same_payload_retries(self):
        op = operation()
        self.session.admit(op, "intent")
        self.session.admit(op, "intent")
        self.session.admit(op, "committed")
        self.session.admit(op, "committed")
        for _ in range(9):
            self.session.admit(operation(), "intent")
        with self.assertRaises(trial.QuotaError):
            self.session.admit(operation(), "intent")
        self.assertEqual(self.session.stats()["operations"], 10)

    def test_payload_rebinding_and_third_phase_refused(self):
        op = operation()
        self.session.admit(op, "intent")
        changed = copy.deepcopy(op)
        changed["request_fingerprint"] = "b" * 64
        with self.assertRaises(trial.QuotaError):
            self.session.admit(changed, "intent")
        self.session.admit(op, "aborted")
        with self.assertRaises(trial.QuotaError):
            self.session.admit(op, "committed")

    def test_request_and_object_attempt_caps_charge_before_failure(self):
        with patch.object(trial, "MAX_REQUESTS", 2):
            self.session.ledger.request("GET", None)
            self.session.ledger.request("POST", b"body")
            with self.assertRaises(trial.QuotaError):
                self.session.ledger.request("GET", None)
        self.assertEqual(self.session.stats()["requests"], 2)
        with patch.object(trial, "MAX_PUTS", 1):
            self.session.ledger.request("PUT", b"data")
            with self.assertRaises(trial.QuotaError):
                self.session.ledger.request("PUT", b"data")
        self.assertEqual(self.session.stats()["puts"], 1)

    def test_upload_and_request_body_caps(self):
        with patch.object(trial, "MAX_UPLOAD", 3):
            with self.assertRaises(trial.QuotaError):
                self.session.ledger.request("PUT", b"four")
        with patch.object(trial, "MAX_WIRE", 3):
            with self.assertRaises(trial.QuotaError):
                self.session.ledger.request("POST", b"four")
        self.assertEqual(self.session.stats()["requests"], 0)

    def test_receive_reservation_is_conservative_and_settles_once(self):
        identifier = self.session.ledger.reserve_read(100)
        self.assertEqual(self.session.stats()["receive"], 100)
        self.session.ledger.settle_read(identifier, 3)
        self.assertEqual(self.session.stats()["receive"], 3)
        with self.assertRaises(trial.QuotaError):
            self.session.ledger.settle_read(identifier, 3)
        with patch.object(trial, "MAX_RECEIVE", 10):
            with self.assertRaises(trial.QuotaError):
                self.session.ledger.reserve_read(8)

    def test_failed_read_keeps_full_reservation(self):
        class Response:
            def read(self, *_args, **_kwargs):
                raise TimeoutError("synthetic-lost-read")
        class Pool:
            def urlopen(self, *_args, **_kwargs):
                return Response()
        pool = self.session.ledger.factory(lambda **_: Pool())()
        response = pool.urlopen("GET", "unused")
        with self.assertRaises(TimeoutError):
            response.read(100, decode_content=False)
        self.assertEqual(self.session.stats()["receive"], 100)

    def test_failed_request_is_not_refunded_and_reopened_worker_shares_counts(self):
        class Pool:
            def urlopen(self, *_args, **_kwargs):
                raise TimeoutError("synthetic-lost-put")
        pool = self.session.ledger.factory(lambda **_: Pool())()
        with self.assertRaises(TimeoutError):
            pool.urlopen("PUT", "unused", body=b"abc")
        other = trial.Ledger(self.session.ledger.path, self.session.nonce, self.session.manifest)
        other.request("GET", None)
        stats = self.session.stats()
        self.assertEqual((stats["requests"], stats["puts"], stats["upload"]), (2, 1, 3))

    def test_expired_or_backward_monotonic_clock_refuses_admission(self):
        now = trial.time.monotonic_ns()
        for value in (0, now + trial.WINDOW_NS + 10**9):
            with patch.object(trial.time, "monotonic_ns", return_value=value):
                with self.assertRaises(trial.QuotaError):
                    self.session.admit(operation(), "intent")

    def test_missing_corrupt_or_wrong_session_cannot_create_new_budget(self):
        wrong = trial.Ledger(self.session.ledger.path, "wrong", self.session.manifest)
        with self.assertRaises(trial.QuotaError):
            wrong.request("GET", None)
        missing = trial.Ledger(Path(self.directory.name) / "missing" / self.session.ledger.path.name,
                               self.session.nonce, self.session.manifest)
        with self.assertRaises(trial.QuotaError):
            missing.request("GET", None)
        self.assertFalse(missing.path.exists())
        corrupt = Path(self.directory.name) / "corrupt"
        corrupt.mkdir()
        path = corrupt / self.session.ledger.path.name
        path.write_bytes(b"not-a-sqlite-budget")
        with self.assertRaises(trial.QuotaError):
            trial.Ledger(path, self.session.nonce, self.session.manifest).request("GET", None)

    def test_uninitialized_publish_or_changed_manifest_never_spawns(self):
        with patch("tools.j4c_call_deadline.run", side_effect=AssertionError("must_not_spawn")):
            self.assertEqual(self.session.publish(operation()).state, trial.p.State.UNAVAILABLE)
            self.session.plan["declared_owner_uid"] = "1234567890123457"
            self.assertEqual(self.session.initialize().state, trial.p.State.UNAVAILABLE)
        with self.assertRaises(trial.QuotaError):
            self.session.admit(operation(), "intent")


if __name__ == "__main__":
    unittest.main()
