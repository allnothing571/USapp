"""CLI dry-run and explicit read-only gates, all live calls substituted."""
import io
import json
import datetime
import os
import tempfile
from pathlib import Path
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import j4c_identity_preflight as driver
from tools import j4c_resource_package as package
from tools.j4c_call_deadline import Result
from tools.deletion_journal_publication import Receipt
from tools.j4c_preflight import report
from test_j4c_initialize import plan
from test_j4c_credentials import raw
from tools.j4c_credentials import parse


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="usapp-j4c-driver-")
        self.addCleanup(self.directory.cleanup)
        self.package = Path(self.directory.name) / "review"
        package.write_package(plan(), self.package)
        self.args = ["--package", str(self.package), "--manifest-sha", report(plan())["manifest_sha256"]]

    def test_dry_run_ignores_credentials_and_sdk_without_read_or_stat(self):
        output = io.StringIO()
        with patch.object(driver, "load", side_effect=AssertionError("credential_read_forbidden")), \
                patch.object(driver, "run", side_effect=AssertionError("worker_forbidden")), redirect_stdout(output):
            self.assertEqual(driver.main(self.args + ["--credentials", "nonexistent-private-file", "--sdk-site", "nonexistent-site"]), 0)
        value = json.loads(output.getvalue())
        for name in ("identity_verified", "resource_ownership_verified", "creation_authorized", "cloud_calls_allowed", "serving_allowed"):
            self.assertFalse(value[name])

    def test_run_requires_both_explicit_read_ack_and_paths_before_loading(self):
        for extra in (["--run"], ["--run", "--ack-read-only-cloud-access"], ["--run", "--credentials", "unused"]):
            with patch.object(driver, "load", side_effect=AssertionError("must_not_read")), redirect_stdout(io.StringIO()):
                self.assertEqual(driver.main(self.args + extra), 2)

    def test_wrong_scope_or_manifest_never_reads_credentials(self):
        with patch.object(driver, "load", side_effect=AssertionError("must_not_read")), redirect_stdout(io.StringIO()):
            self.assertEqual(driver.main(["--package", str(self.package), "--manifest-sha", "a" * 64, "--run"]), 2)

    def test_explicit_fake_run_reports_identity_only_and_never_resource_authority(self):
        site = Path(self.directory.name) / "sdk"
        (site / "tablestore-6.4.8.dist-info").mkdir(parents=True)
        (site / "tablestore-6.4.8.dist-info" / "METADATA").write_text("synthetic", encoding="ascii")
        args = self.args + ["--run", "--ack-read-only-cloud-access", "--credentials", "synthetic-private-file", "--sdk-site", str(site)]
        with patch.object(driver, "load", return_value=object()) as reader, \
                patch.object(driver, "run", return_value=Result(value=Receipt.CONFIRMED)), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(driver.main(args), 0)
        reader.assert_called_once()
        value = json.loads(output.getvalue())
        self.assertTrue(value["identity_verified"])
        self.assertFalse(value["resource_ownership_verified"])
        self.assertFalse(value["creation_authorized"])
        self.assertFalse(value["serving_allowed"])

    def test_unknown_or_unreaped_worker_never_confirms_identity(self):
        site = Path(self.directory.name) / "sdk"
        (site / "tablestore-6.4.8.dist-info").mkdir(parents=True)
        (site / "tablestore-6.4.8.dist-info" / "METADATA").write_text("synthetic", encoding="ascii")
        args = self.args + ["--run", "--ack-read-only-cloud-access", "--credentials", "synthetic", "--sdk-site", str(site)]
        for result in (Result(), Result(value=Receipt.CONFIRMED, worker_reaped=False), Result(timed_out=True)):
            with patch.object(driver, "load", return_value=object()), patch.object(driver, "run", return_value=result), \
                    redirect_stdout(io.StringIO()) as output:
                self.assertEqual(driver.main(args), 2)
            self.assertEqual(output.getvalue(), "J4C_IDENTITY_PREFLIGHT_UNKNOWN_OR_REFUSED\n")


@unittest.skipUnless(os.environ.get("USAPP_J4C_OFFLINE_SDK") == "1", "isolated dependency site required")
class ProbeTests(unittest.TestCase):
    def test_worker_requires_ack_and_verifies_exact_three_then_closes(self):
        bundle = parse(raw(datetime.datetime.now(datetime.timezone.utc)), plan())
        with patch("tools.j4c_identity.Client") as client:
            self.assertEqual(driver._probe(plan(), bundle), Receipt.UNKNOWN)
            client.assert_not_called()
            client.return_value.verify.return_value = True
            self.assertEqual(driver._probe(plan(), bundle, True), Receipt.CONFIRMED)
            self.assertEqual(client.call_count, 3)
            self.assertEqual(client.return_value.close.call_count, 3)

    def test_worker_stops_on_refusal_or_expiry(self):
        bundle = parse(raw(datetime.datetime.now(datetime.timezone.utc)), plan())
        with patch("tools.j4c_identity.Client") as client:
            client.return_value.verify.return_value = False
            self.assertEqual(driver._probe(plan(), bundle, True), Receipt.UNKNOWN)
            self.assertEqual(client.call_count, 1)
        stale = parse(raw(datetime.datetime.now(datetime.timezone.utc)), plan())
        object.__setattr__(stale, "expires_at", datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc))
        with patch("tools.j4c_identity.Client") as client:
            self.assertEqual(driver._probe(plan(), stale, True), Receipt.UNKNOWN)
            client.assert_not_called()
