"""Local review artifact tests. No SDK, account, credentials or cloud calls."""
import io
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools import deletion_journal as j1
from tools import j4c_resource_package as package
from tools.j4c_preflight import report
from tools.deletion_journal_publication import zero_head


def plan():
    return dict(version=1, mode="offline_only", run_id="1234abcd",
                journal_id="00000000-0000-0000-0000-000000000041", region="cn-shanghai",
                budget_cny=5, declared_owner_uid="1234567890123456", bucket="usapp-j4c-log-1234abcd",
                instance="uj4c1234abcd", table="journal")


class ResourcePackageTests(unittest.TestCase):
    def test_generated_plan_uses_fresh_paired_names_and_identity(self):
        first, second = package.new_plan("1234567890123456"), package.new_plan("1234567890123456")
        self.assertNotEqual(first["journal_id"], second["journal_id"])
        self.assertEqual(first["bucket"], "usapp-j4c-log-" + first["run_id"])
        self.assertEqual(first["instance"], "uj4c" + first["run_id"])
        self.assertEqual(first["mode"], "offline_only")

    def test_policy_scopes_are_exact_and_have_no_management_or_delete(self):
        policies = package.policies(plan())
        for policy in policies.values():
            for statement in policy["Statement"]:
                self.assertEqual(statement["Effect"], "Allow")
                self.assertEqual(len(statement["Resource"]), 1)
                for action in statement["Action"]:
                    self.assertNotIn("*", action)
                    self.assertFalse(any(word in action for word in ("Delete", "Create", "List", "Attach")))
                resource = statement["Resource"][0]
                if resource.startswith("acs:oss:"):
                    self.assertEqual(resource, "acs:oss:*:*:usapp-j4c-log-1234abcd/journals/" + plan()["journal_id"] + "/batches/*")
                else:
                    self.assertEqual(resource, "acs:ots:cn-shanghai:1234567890123456:instance/uj4c1234abcd/table/journal")

    def test_reader_can_start_abort_but_never_write_or_commit(self):
        actions = {a for s in package.policies(plan())["recovery-reader"]["Statement"] for a in s["Action"]}
        self.assertIn("ots:StartLocalTransaction", actions)
        self.assertIn("ots:AbortTransaction", actions)
        self.assertTrue(actions.isdisjoint({"ots:CommitTransaction", "ots:PutRow", "ots:UpdateRow", "oss:PutObject"}))

    def test_generated_policies_match_corrected_workspace_templates(self):
        directory = Path(__file__).resolve().parents[1] / "docs" / "examples" / "j4c"
        replacements = dict(LOG_BUCKET=plan()["bucket"], JOURNAL_ID=plan()["journal_id"], REGION="cn-shanghai",
                            ACCOUNT_ID=plan()["declared_owner_uid"], INSTANCE=plan()["instance"], TABLE="journal")
        for role, expected in package.policies(plan()).items():
            raw = (directory / (role + ".policy.json")).read_text(encoding="utf-8")
            for key, value in replacements.items():
                raw = raw.replace("<" + key + ">", value)
            self.assertEqual(j1._decode(raw), expected)

    def test_package_roundtrip_never_proves_ownership_or_approval(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-package-") as directory:
            path = Path(directory) / "review"
            result = package.write_package(plan(), path)
            self.assertTrue(result["contents_consistent"])
            self.assertEqual(len(list(path.iterdir())), 8)
            for key in ("ownership_verified", "creation_authorized", "cloud_calls_allowed", "serving_allowed"):
                self.assertFalse(result[key])
            self.assertEqual(result, package.verify_package(path, report(plan())["manifest_sha256"]))

    def test_existing_directory_refused_without_touching_files(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-package-") as directory:
            path = Path(directory) / "review"
            package.write_package(plan(), path)
            before = {p.name: p.read_bytes() for p in path.iterdir()}
            with self.assertRaises(FileExistsError):
                package.write_package(plan(), path)
            self.assertEqual(before, {p.name: p.read_bytes() for p in path.iterdir()})

    def test_changed_bytes_in_any_file_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-package-") as directory:
            path = Path(directory) / "review"
            package.write_package(plan(), path)
            for filename in package.build(plan()):
                target = path / filename
                original = target.read_bytes()
                target.write_bytes(original + b" ")
                with self.subTest(filename=filename), self.assertRaises(ValueError):
                    package.verify_package(path)
                target.write_bytes(original)

    def test_extra_and_incomplete_packages_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-package-") as directory:
            path = Path(directory) / "review"
            package.write_package(plan(), path)
            extra = path / "extra.json"
            extra.write_text("{}", encoding="ascii")
            with self.assertRaises(ValueError):
                package.verify_package(path)
            extra.unlink()
            (path / "package-index.json").unlink()
            with self.assertRaises(ValueError):
                package.verify_package(path)

    def test_external_expected_digest_rejects_internally_consistent_other_plan(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-package-") as directory:
            changed = dict(plan(), declared_owner_uid="1234567890123457")
            path = Path(directory) / "review"
            package.write_package(changed, path)
            self.assertTrue(package.verify_package(path)["contents_consistent"])
            with self.assertRaises(ValueError):
                package.verify_package(path, report(plan())["manifest_sha256"])

    def test_manifest_symlink_is_refused_before_any_read(self):
        with patch("pathlib.Path.is_symlink", side_effect=[False, True]), patch("pathlib.Path.open") as opened:
            with self.assertRaises(ValueError):
                package.verify_package("unused-review-directory")
            opened.assert_not_called()

    def test_cli_invalid_owner_never_creates_directory_and_errors_are_fixed(self):
        with patch("pathlib.Path.mkdir") as mkdir, redirect_stdout(io.StringIO()) as output:
            self.assertEqual(package.main(["--owner-uid", "not-a-uid", "--output-parent", "unused"]), 2)
        mkdir.assert_not_called()
        self.assertEqual(output.getvalue(), "J4C_RESOURCE_PACKAGE_INVALID\n")

    def test_initial_seed_and_settings_are_proposals_only(self):
        files = package.build(plan())
        self.assertEqual(j1._decode(files["proposed-initial-head.json"]), zero_head(plan()["journal_id"]))
        self.assertIn(b"not a trusted latest checkpoint", files["REVIEW.md"])
        settings = j1._decode(files["resource-settings.json"])
        self.assertFalse(settings["creation_authorized"])
        self.assertFalse(settings["test_bounds"]["enforced_by_this_package"])
        self.assertFalse(settings["budget"]["automatic_billing_cap"])
        self.assertEqual(settings["bucket"]["versioning"], "never_enabled")
        self.assertTrue(settings["table"]["local_transactions"])

    def test_prior_review_revision_remains_verifiable_without_rewriting(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-package-") as directory:
            path = Path(directory) / "review"
            path.mkdir()
            prior = package.build(plan(), revision=1)
            for name, data in prior.items():
                (path / name).write_bytes(data)
            self.assertTrue(package.verify_package(path)["contents_consistent"])
            self.assertEqual(prior, {p.name: p.read_bytes() for p in path.iterdir()})
            self.assertIn(b"Offline initializer exists", package.build(plan())["REVIEW.md"])


if __name__ == "__main__":
    unittest.main()
