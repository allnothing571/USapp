"""Local review files only: no credentials, network, resource creation or binding."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import uuid

from . import deletion_journal as j1
from .deletion_journal_publication import zero_head
from .j4c_preflight import validate, report


POLICY_ROLES = ("object-publisher", "checkpoint-writer", "recovery-reader")
TABLE_READ = ["ots:DescribeTable", "ots:GetRow", "ots:GetRange"]
TABLE_TX_READ = TABLE_READ + ["ots:StartLocalTransaction", "ots:AbortTransaction"]
TABLE_WRITE = TABLE_READ + ["ots:PutRow", "ots:UpdateRow", "ots:StartLocalTransaction",
                           "ots:CommitTransaction", "ots:AbortTransaction"]


def new_plan(owner_uid):
    run = secrets.token_hex(4)
    return validate(dict(version=1, mode="offline_only", run_id=run, journal_id=str(uuid.uuid4()),
                         region="cn-shanghai", budget_cny=5, declared_owner_uid=owner_uid,
                         bucket="usapp-j4c-log-" + run, instance="uj4c" + run, table="journal"))


def policies(plan):
    plan = validate(plan)
    obj = "acs:oss:*:*:{}/journals/{}/batches/*".format(plan["bucket"], plan["journal_id"])
    table = "acs:ots:{}:{}:instance/{}/table/{}".format(
        plan["region"], plan["declared_owner_uid"], plan["instance"], plan["table"])
    def allow(actions, resource):
        return dict(Effect="Allow", Action=list(actions), Resource=[resource])
    return {
        "object-publisher": dict(Version="1", Statement=[allow(["oss:PutObject", "oss:GetObject"], obj)]),
        "checkpoint-writer": dict(Version="1", Statement=[allow(["oss:GetObject"], obj), allow(TABLE_WRITE, table)]),
        "recovery-reader": dict(Version="1", Statement=[allow(["oss:GetObject"], obj), allow(TABLE_TX_READ, table)])}


def settings(plan):
    plan = validate(plan)
    return dict(status="proposal_only", cloud_calls_allowed=False, creation_authorized=False,
                declared_owner_uid=plan["declared_owner_uid"],
                bucket=dict(name=plan["bucket"], region=plan["region"], acl="private", block_public_access=True,
                            storage_class="Standard", redundancy="LRS", encryption="AES256",
                            versioning="never_enabled", lifecycle=False, replication=False, cors=False,
                            acceleration=False, retention_policy=False, retention_lock=False),
                table=dict(instance=plan["instance"], name=plan["table"], region=plan["region"],
                           billing="CU", tier="high_performance", local_transactions=True,
                           primary_keys=[[name, "STRING"] for name in ("journal_id", "row_type", "row_id")],
                           time_to_live=-1, max_versions=1, allow_update=True,
                           reserved_read=0, reserved_write=0, secondary_indexes=False),
                identities={role: "usapp-j4c-{}-{}".format(plan["run_id"], role) for role in POLICY_ROLES},
                test_bounds=dict(proposal_only=True, enforced_by_this_package=False, max_operations=10,
                                 max_batches=20, max_object_bytes=2 * 1024 * 1024, window_hours=2),
                budget=dict(cny=5, automatic_billing_cap=False))


def _json(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2) + "\n").encode("ascii")


def build(plan, revision=2):
    if type(revision) is not int or revision not in (1, 2):
        raise ValueError("unknown_review_revision")
    plan = validate(plan)
    files = {"plan.j4c-plan.json": _json(plan), "resource-settings.json": _json(settings(plan)),
             "proposed-initial-head.json": _json(zero_head(plan["journal_id"]))}
    for role, policy in policies(plan).items():
        files[role + ".policy.json"] = _json(policy)
    review = """# J4c resource confirmation - NOT AUTHORIZED

Account UID (owner-confirmed declaration): {owner}
Region: {region}
Bucket: {bucket}
TableStore instance / table: {instance} / {table}
Journal: {journal}
Budget: CNY 5, manual stop threshold, NOT an automatic billing cap.

All files are LOCAL PROPOSALS. Names are NOT checked for cloud availability.
No credentials, network calls, resource creation, binding, retention lock or deletion.
The owner declaration is NOT authenticated account/resource ownership evidence.

Runtime identities must be new, programmatic only, with no inherited/extra grants.
Recovery reader uses Start/Abort for transactional reads, but cannot write or commit.
Transaction control calls may consume capacity; data-read-only is not zero-cost.
UpdateRow permission can bypass client head rules; IAM does not enforce monotonicity.

The initial-head file is a PROPOSED SEED, not a trusted latest checkpoint.
Initialize only a NEW, verified-empty whole partition while writers are stopped.
Unknown rows, missing head on an existing journal, old cycles or restored state: STOP.
Initializer and initialization authority/fresh-lineage proof remain unimplemented.

Before creation: review exact config/identities/budget/window/cleanup, obtain separate approval.
After approval: independently verify owner, permissions, never-enabled bucket versioning,
private settings, no lifecycle/replication/retention, exact table options and local transactions.
No live runner, credential management or live initialization is provided by this package.

Cleanup is NOT automated or authorized: enumerate EXACT test keys/rows/resources first,
exclude all pre-existing resources, then obtain the exact cleanup authorization.
Proposed test bounds are review values, not enforced by this package.
Hash verification proves internal consistency, not authenticity, authorization or anti-rollback.
J3, restore/replay, J5 and release gates remain open.
""".format(owner=plan["declared_owner_uid"], region=plan["region"], bucket=plan["bucket"],
           instance=plan["instance"], table=plan["table"], journal=plan["journal_id"])
    if revision == 2:
        review = review.replace("Initializer and initialization authority/fresh-lineage proof remain unimplemented.",
                                "Offline initializer exists; live authority/fresh-lineage verification remains unimplemented.")
    files["REVIEW.md"] = review.encode("ascii")
    index = dict(version=revision, manifest_sha256=report(plan)["manifest_sha256"],
                 creation_authorized=False, ownership_verified=False,
                 files={name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
    files["package-index.json"] = _json(index)
    return files


def write_package(plan, directory):
    files = build(plan)
    directory = Path(directory)
    directory.mkdir()  # Exclusive; never reuse or overwrite a review directory.
    for name, data in files.items():
        with (directory / name).open("xb") as stream:
            stream.write(data)
    return verify_package(directory)


def verify_package(directory, expected_manifest_sha=None):
    directory = Path(directory)
    if directory.is_symlink():
        raise ValueError("package_link_forbidden")
    manifest = directory / "plan.j4c-plan.json"
    if manifest.is_symlink() or not manifest.is_file():
        raise ValueError("manifest_link_forbidden")
    with manifest.open("rb") as stream:
        raw = stream.read(8193)
    if len(raw) > 8192:
        raise ValueError("manifest_limit")
    plan = validate(j1._decode(raw))
    index_path = directory / "package-index.json"
    if index_path.is_symlink() or not index_path.is_file():
        raise ValueError("invalid_package_index")
    with index_path.open("rb") as stream:
        index_raw = stream.read(8193)
    if len(index_raw) > 8192:
        raise ValueError("package_index_limit")
    index = j1._decode(index_raw)
    if not isinstance(index, dict):
        raise ValueError("invalid_package_index")
    expected = build(plan, revision=index.get("version"))
    if {p.name for p in directory.iterdir()} != set(expected):
        raise ValueError("unexpected_package_files")
    for name, data in expected.items():
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("invalid_package_file")
        with path.open("rb") as stream:
            if stream.read(len(data) + 1) != data:
                raise ValueError("package_content_mismatch")
    digest = report(plan)["manifest_sha256"]
    if expected_manifest_sha is not None and (not isinstance(expected_manifest_sha, str) or
            not re.fullmatch(r"[0-9a-f]{64}", expected_manifest_sha) or digest != expected_manifest_sha):
        raise ValueError("unexpected_manifest")
    return dict(contents_consistent=True, manifest_sha256=digest, ownership_verified=False,
                creation_authorized=False, cloud_calls_allowed=False, serving_allowed=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--owner-uid")
    operation.add_argument("--verify", type=Path)
    parser.add_argument("--output-parent", type=Path)
    parser.add_argument("--expected-manifest-sha")
    args = parser.parse_args(argv)
    try:
        if args.verify is not None:
            output = verify_package(args.verify, args.expected_manifest_sha)
        else:
            if args.output_parent is None or args.expected_manifest_sha is not None:
                raise ValueError()
            plan = new_plan(args.owner_uid)
            args.output_parent.mkdir(parents=True, exist_ok=True)
            directory = args.output_parent / ("j4c-" + plan["run_id"])
            output = write_package(plan, directory)
            output["directory"] = str(directory)
    except Exception:
        print("J4C_RESOURCE_PACKAGE_INVALID")
        return 2
    print(json.dumps(output, ensure_ascii=True, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
