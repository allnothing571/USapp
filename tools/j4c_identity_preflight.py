"""Dry-run by default. Explicit run supports ONLY three read-only identity probes.

No resource creation, bucket/table inspection, journal initialization or publish.
Operator acknowledgment records intent; it is not authenticated authorization.
The agent must obtain separate user authorization before any real Run invocation.
"""
import argparse
import datetime
import json
from pathlib import Path
import sys

from . import deletion_journal as j1
from .j4c_preflight import validate, report
from .j4c_resource_package import verify_package, POLICY_ROLES
from .j4c_credentials import load
from .j4c_call_deadline import run
from .deletion_journal_publication import Receipt


def dry_run(directory, expected_sha):
    result = verify_package(directory, expected_sha)
    with (Path(directory) / "plan.j4c-plan.json").open("rb") as stream:
        raw = stream.read(8193)
    if len(raw) > 8192:
        raise ValueError("manifest_limit")
    plan = validate(j1._decode(raw))
    if report(plan)["manifest_sha256"] != result["manifest_sha256"]:
        raise ValueError("manifest_changed_after_package_check")
    return plan, dict(mode="dry_run", manifest_sha256=result["manifest_sha256"],
                      identity_verified=False, resource_ownership_verified=False, creation_authorized=False,
                      cloud_calls_allowed=False, serving_allowed=False,
                      proposed_scope="three_STS_GetCallerIdentity_reads_only",
                      expected_usernames=["usapp-j4c-{}-{}".format(plan["run_id"], role) for role in POLICY_ROLES])


def _probe(plan, bundle, read_only_ack=False, fixture=None):
    from .j4c_identity import Client
    try:
        if read_only_ack is not True:
            return Receipt.UNKNOWN
        from urllib3 import PoolManager
        if bundle.manifest_sha256 != report(plan)["manifest_sha256"] or set(bundle.keys) != set(POLICY_ROLES):
            return Receipt.UNKNOWN
        def execute(factory):
            for role in POLICY_ROLES:
                if datetime.datetime.now(datetime.timezone.utc) >= bundle.expires_at:
                    return Receipt.UNKNOWN
                client = Client(bundle.keys[role], plan["declared_owner_uid"],
                                "usapp-j4c-{}-{}".format(plan["run_id"], role), pool_factory=factory)
                try:
                    if client.verify() is not True:
                        return Receipt.UNKNOWN
                finally:
                    client.close()
            return Receipt.CONFIRMED
        if fixture is None:
            return execute(PoolManager)
        from .j4c_local_transport import Fixture, Pools, restricted
        from .j4c_identity import ENDPOINT
        if type(fixture) is not Fixture:
            return Receipt.UNKNOWN
        pools = Pools(fixture)
        try:
            with restricted(fixture):
                return execute(pools.factory(ENDPOINT.rstrip("/"), "identity"))
        finally:
            pools.close()
    except Exception:
        return Receipt.UNKNOWN


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--manifest-sha", required=True)
    parser.add_argument("--credentials", type=Path)
    parser.add_argument("--sdk-site", type=Path)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--ack-read-only-cloud-access", action="store_true")
    args = parser.parse_args(argv)
    try:
        plan, output = dry_run(args.package, args.manifest_sha)
        if not args.run:
            # Do NOT inspect/stat either supplied credential or SDK path in dry-run.
            print(json.dumps(output, ensure_ascii=True, sort_keys=True))
            return 0
        if not args.ack_read_only_cloud_access or args.credentials is None or args.sdk_site is None:
            raise ValueError()
        site = args.sdk_site.resolve()
        if not (site / "tablestore-6.4.8.dist-info" / "METADATA").is_file():
            raise ValueError()
        bundle = load(args.credentials, plan)
        old_path = list(sys.path)
        try:
            sys.path.insert(0, str(site))
            result = run(_probe, args=(plan, bundle, True), timeout=10.0)
        finally:
            sys.path[:] = old_path
        if result.value is not Receipt.CONFIRMED or result.timed_out or not result.worker_reaped:
            print("J4C_IDENTITY_PREFLIGHT_UNKNOWN_OR_REFUSED")
            return 2
        print(json.dumps(dict(mode="read_only_identity_probe", identity_verified=True, verified_principals=3,
                              resource_ownership_verified=False, creation_authorized=False, serving_allowed=False,
                              manifest_sha256=report(plan)["manifest_sha256"]), sort_keys=True))
        return 0
    except Exception:
        print("J4C_IDENTITY_PREFLIGHT_REFUSED")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
