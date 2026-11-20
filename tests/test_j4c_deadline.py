import sys
import unittest

from tools.j4c_sdk_offline import bounded_process


class DeadlineTests(unittest.TestCase):
    def test_finished_child_is_reaped_but_is_not_remote_commit_proof(self):
        result = bounded_process([sys.executable, "-I", "-B", "-c", "print('synthetic-finished')"], 3)
        self.assertFalse(result.timed_out)
        self.assertEqual(result.exit_code, 0)
        self.assertIn(b"synthetic-finished", result.output)
        self.assertFalse(result.proven_not_applied)

    def test_timeout_terminates_only_owned_child_and_stays_unknown(self):
        result = bounded_process([sys.executable, "-I", "-B", "-c", "import time; time.sleep(10)"], 0.2)
        self.assertTrue(result.timed_out)
        self.assertIsNotNone(result.exit_code)
        self.assertFalse(result.proven_not_applied)

    def test_invalid_timeout_does_not_start_process(self):
        for value in (0, -1, True, 31):
            with self.assertRaises(ValueError):
                bounded_process([sys.executable], value)


if __name__ == "__main__":
    unittest.main()
