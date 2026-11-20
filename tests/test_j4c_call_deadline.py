"""Spawned worker receipts, sanitized failures and non-application non-claims."""
import time
import unittest
from tools.deletion_journal_publication import Receipt
from tools.j4c_call_deadline import run


def confirmed():
    return Receipt.CONFIRMED


def private_error():
    raise RuntimeError("private-synthetic-provider-message")


def blocking():
    time.sleep(10)


class CallDeadlineTests(unittest.TestCase):
    def test_exact_enum_receipt_and_sanitized_failure(self):
        result = run(confirmed, timeout=3)
        self.assertEqual(result.value, Receipt.CONFIRMED)
        self.assertFalse(result.proven_not_applied)
        self.assertIsNone(run(private_error, timeout=3).value)

    def test_owned_worker_timeout_is_unknown_not_remote_rollback(self):
        result = run(blocking, timeout=0.2)
        self.assertTrue(result.timed_out)
        self.assertTrue(result.worker_reaped)
        self.assertFalse(result.proven_not_applied)
        self.assertIsNone(result.value)

    def test_invalid_budget_never_launches_worker(self):
        for timeout in (0, -1, True, 21, float("nan")):
            with self.assertRaises(ValueError):
                run(confirmed, timeout=timeout)
