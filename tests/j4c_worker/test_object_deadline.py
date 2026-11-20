"""Separate budget for post-acceptance trickle; preserve all outcome assertions."""
import os
import time
import unittest
from tools.j4c_call_deadline import run
from test_j4c_local_tls import LocalTlsFixture, worker_put


@unittest.skipUnless(os.environ.get("USAPP_J4C_LOCAL_TLS_DIR"), "explicit synthetic TLS fixture required")
class ObjectDeadlineTests(LocalTlsFixture, unittest.TestCase):
    def test_whole_call_watchdog_stops_trickle_after_server_accepted_put(self):
        self.mode, self.applied = "trickle", False
        start = time.monotonic()
        result = run(worker_put, args=(str(self.path), self.server.server_address[1], self.key, self.data), timeout=5.0)
        self.assertLess(time.monotonic() - start, 8.0)  # 5 s execution + 2 s reap + scheduling allowance.
        self.assertTrue(self.applied, "timeout={},reaped={},value={}".format(result.timed_out, result.worker_reaped, result.value))
        self.assertTrue(result.timed_out)
        self.assertTrue(result.worker_reaped)
        self.assertFalse(result.proven_not_applied)
        self.assertIsNone(result.value)
        self.assertEqual(self.requests, [("PUT", "/" + self.key)])
