"""Separate offline worker target: no package marker, not silently folded into old budget."""
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import j4c_preflight as preflight
from tools.j4c_publication_worker import publish
from tools.deletion_journal_publication import State
from test_j4c_metadata import operation


def plan():
    return dict(version=1, mode="offline_only", run_id="1234abcd",
                journal_id="00000000-0000-0000-0000-000000000041", region="cn-shanghai",
                budget_cny=5, declared_owner_uid="1234567890123456", bucket="usapp-j4c-log-1234abcd",
                instance="uj4c1234abcd", table="journal")


class PreflightTests(unittest.TestCase):
    def test_valid_config_never_authorizes_or_claims_verified_ownership(self):
        report = preflight.report(plan())
        self.assertTrue(report["configuration_valid"])
        for field in ("cloud_calls_allowed", "creation_authorized", "serving_allowed", "ownership_verified"):
            self.assertFalse(report[field])
        self.assertIn("explicit_empty_initialization", report["pending"])
        self.assertEqual(len(report["manifest_sha256"]), 64)

    def test_manifest_rejects_mixed_resources_flags_or_unapproved_budget(self):
        changes = (("version", True), ("mode", "live"), ("bucket", "existing"), ("instance", "uj4c00000000"),
                   ("region", "cn-hangzhou"), ("budget_cny", 6), ("budget_cny", True),
                   ("table", "production"), ("declared_owner_uid", "owner"), ("journal_id", "bad"))
        for key, value in changes:
            changed = dict(plan(), **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                preflight.report(changed)
        with self.assertRaises(ValueError):
            preflight.report(dict(plan(), enable_network=True))

    def test_plan_is_detached_and_hash_binds_identity(self):
        original = plan()
        detached = preflight.validate(original)
        original["bucket"] = "changed"
        self.assertEqual(detached, plan())
        changed = dict(plan(), declared_owner_uid="1234567890123457")
        self.assertNotEqual(preflight.report(plan())["manifest_sha256"], preflight.report(changed)["manifest_sha256"])

    def test_cli_reads_only_named_small_manifest_and_errors_are_fixed(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-plan-") as directory:
            filename = Path(directory) / "example.j4c-plan.json"
            filename.write_text(json.dumps(plan()), encoding="utf-8")
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(preflight.main([str(filename)]), 0)
            self.assertFalse(json.loads(output.getvalue())["ownership_verified"])
            filename.write_bytes(b"private-synthetic-error" * 500)
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(preflight.main([str(filename)]), 2)
            self.assertEqual(output.getvalue(), "J4C_PREFLIGHT_INVALID\n")
            with patch("pathlib.Path.open", side_effect=AssertionError("wrong_file_must_not_be_read")):
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(preflight.main([str(Path(directory) / "credentials.json")]), 2)

    def test_invalid_requests_never_spawn_a_worker(self):
        with patch("tools.j4c_publication_worker.run", side_effect=AssertionError("must_not_spawn")):
            for request in ((dict(plan(), mode="live"), operation(), "intent", 5),
                            (plan(), {}, "intent", 5), (plan(), operation(), "wrong", 5),
                            (plan(), operation(), "intent", True)):
                self.assertEqual(publish(*request).state, State.INVALID)

    def test_default_worker_network_is_blocked_even_for_valid_plan(self):
        result = publish(plan(), operation())  # Published 5 s budget; this is not the timeout test.
        self.assertFalse(result.timed_out)
        self.assertEqual(result.state, State.UNAVAILABLE)
        self.assertTrue(result.worker_reaped)
        self.assertFalse(result.serving_allowed)
        self.assertFalse(result.proven_not_applied)


if __name__ == "__main__":
    unittest.main()
