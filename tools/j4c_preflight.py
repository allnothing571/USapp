"""Offline manifest checks only. No credentials, cloud calls or resource creation."""
import argparse
import hashlib
import json
import re
from pathlib import Path

from .deletion_journal import _decode, _uuid, canonical


FIELDS = {"version", "mode", "run_id", "journal_id", "region", "budget_cny",
          "declared_owner_uid", "bucket", "instance", "table"}


def validate(value):
    if not isinstance(value, dict) or set(value) != FIELDS or type(value["version"]) is not int or \
            value["version"] != 1 or value["mode"] != "offline_only" or \
            not isinstance(value["run_id"], str) or not re.fullmatch(r"[0-9a-f]{8}", value["run_id"]) or \
            not _uuid(value["journal_id"]) or value["region"] != "cn-shanghai" or \
            type(value["budget_cny"]) is not int or value["budget_cny"] != 5 or \
            not isinstance(value["declared_owner_uid"], str) or \
            not re.fullmatch(r"[0-9]{10,20}", value["declared_owner_uid"]) or \
            value["bucket"] != "usapp-j4c-log-" + value["run_id"] or \
            value["instance"] != "uj4c" + value["run_id"] or value["table"] != "journal":
        raise ValueError("invalid_offline_manifest")
    return _decode(canonical(value))  # Detach from caller mutation before spawning.


def report(value):
    plan = validate(value)
    return dict(configuration_valid=True, mode="offline_only", cloud_calls_allowed=False,
                creation_authorized=False, serving_allowed=False, ownership_verified=False,
                manifest_sha256=hashlib.sha256(canonical(plan)).hexdigest(),
                resources={k: plan[k] for k in ("region", "bucket", "instance", "table", "journal_id")},
                pending=["owner_and_identity_evidence", "exact_permissions_and_inheritance",
                         "bucket_private_and_never_versioned", "table_options_and_local_transactions",
                         "independent_current_checkpoint", "explicit_empty_initialization",
                         "live_atomicity_and_immutability", "unknown_result_recovery",
                         "budget_window_and_exact_cleanup", "separate_creation_approval"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        if not args.manifest.name.endswith(".j4c-plan.json"):
            raise ValueError()
        with args.manifest.open("rb") as stream:
            data = stream.read(8193)
        if len(data) > 8192:
            raise ValueError()
        output = report(_decode(data))
    except Exception:
        print("J4C_PREFLIGHT_INVALID")
        return 2
    print(json.dumps(output, ensure_ascii=True, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
