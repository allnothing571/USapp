import unittest

from tools.journal_fenced_publisher import FencedPublisher, Receipt


JOURNAL = "00000000-0000-0000-0000-000000000001"
ADMISSION = "00000000-0000-0000-0000-000000000002"
FOREIGN = "00000000-0000-0000-0000-000000000003"


class FencedPublisherTests(unittest.TestCase):
    def setUp(self):
        self.trace = []
        self.local = {"network_drained": True, "backend_drained": True, "resources_drained": True}
        self.receipts = {kind: Receipt(JOURNAL, ADMISSION, kind) for kind in ("admitted", "reconciled", "drained")}
        self.publisher = FencedPublisher(self, JOURNAL, ADMISSION, self.observe("admitted"),
                                         self.observe("reconciled"), self.observe("drained"))

    def start(self):
        self.trace.append("start")
        if getattr(self, "start_error", False):
            raise RuntimeError("listener failed")

    def close(self, timeout):
        self.trace.append("close")
        return self.local

    def observe(self, kind):
        def call(journal, admission):
            self.assertEqual((journal, admission), (JOURNAL, ADMISSION))
            self.trace.append(kind)
            return self.receipts[kind]
        return call

    def test_exact_committed_receipts_gate_listener_and_drain(self):
        self.publisher.start()
        self.assertTrue(self.publisher.close()["coordinator_drained"])
        self.assertTrue(self.publisher.close()["coordinator_drained"])
        self.assertEqual(self.trace, ["admitted", "start", "close", "reconciled", "drained", "close"])

    def test_unconfirmed_or_foreign_admission_never_starts(self):
        self.receipts["admitted"] = Receipt(JOURNAL, FOREIGN, "admitted")
        with self.assertRaisesRegex(RuntimeError, "admission_unconfirmed"):
            self.publisher.start()
        self.assertFalse(self.publisher.close()["coordinator_drained"])
        self.assertEqual(self.trace, ["admitted", "close"])

    def test_local_and_external_uncertainty_hold_the_admission(self):
        self.publisher.start()
        self.local["resources_drained"] = False
        self.assertFalse(self.publisher.close()["coordinator_drained"])
        self.assertNotIn("reconciled", self.trace)
        self.local["resources_drained"] = True
        self.receipts["reconciled"] = None
        self.assertFalse(self.publisher.close()["coordinator_drained"])
        self.assertNotIn("drained", self.trace)
        self.receipts["reconciled"] = Receipt(JOURNAL, ADMISSION, "reconciled")
        self.receipts["drained"] = None
        self.assertFalse(self.publisher.close()["coordinator_drained"])
        self.receipts["drained"] = Receipt(JOURNAL, ADMISSION, "drained")
        self.assertTrue(self.publisher.close()["coordinator_drained"])

    def test_failed_start_still_has_a_durable_admission_to_drain(self):
        self.start_error = True
        with self.assertRaisesRegex(RuntimeError, "listener failed"):
            self.publisher.start()
        with self.assertRaisesRegex(RuntimeError, "already_attempted"):
            self.publisher.start()
        self.assertTrue(self.publisher.close()["coordinator_drained"])
        self.assertEqual(self.trace, ["admitted", "start", "close", "reconciled", "drained"])

    def test_missing_admission_reply_is_not_retried(self):
        def unavailable(_journal, _admission):
            raise RuntimeError("private coordinator details")

        self.publisher.admit = unavailable
        with self.assertRaisesRegex(RuntimeError, "^publisher_admission_unconfirmed$"):
            self.publisher.start()
        with self.assertRaisesRegex(RuntimeError, "already_attempted"):
            self.publisher.start()
        self.assertFalse(self.publisher.close()["coordinator_drained"])
        self.assertEqual(self.trace, ["close"])
