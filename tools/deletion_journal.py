"""Offline D6 journal validator and conservative replay planner (stdlib only).

This is NOT an online journal adapter or a database replay tool. A generated
plan never permits serving. Its checkpoint must come from a separately trusted
latest-head source; a hash chain alone cannot detect a truncated suffix.
"""
import argparse
import hashlib
import json
import sys
import uuid
from pathlib import Path


ZERO = "0" * 64
FIELDS = {"version", "journal_id", "seq", "event_id", "previous", "intent_id",
          "action", "phase", "target_kind", "target_id", "cycle_id", "hash"}
IDENTITY = ("action", "target_kind", "target_id", "cycle_id")


class JournalError(ValueError):
    """Fixed-label failure, never includes input bytes or file names."""


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def digest(row):
    return hashlib.sha256(canonical({k: v for k, v in row.items() if k != "hash"})).hexdigest()


def _uuid(value):
    if not isinstance(value, str):
        return False
    try:
        return str(uuid.UUID(value)) == value
    except ValueError:
        return False


def _hash(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def _integer(value):
    return type(value) is int


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise JournalError("duplicate_json_key")
        result[key] = value
    return result


def _decode(data):
    try:
        return json.loads(data, object_pairs_hook=_unique,
                          parse_constant=lambda _: _invalid_constant())
    except (ValueError, UnicodeError) as error:
        if isinstance(error, JournalError):
            raise
        raise JournalError("invalid_json") from None


def _invalid_constant():
    raise JournalError("invalid_json")


def make_plan(raw, checkpoint, expected_journal_id):
    """Validate a closed snapshot, then describe hides; do not mutate anything.

    An intent is durable BEFORE attempting its SQL/store action. A committed or
    aborted record is a separate acknowledgement of that exact intent. Aborted
    means proven not applied, never a timeout. No time-based eligibility is
    inferred here. Delete-cycle IDs are permanently unique per target.
    """
    if not _uuid(expected_journal_id):
        raise JournalError("invalid_expected_identity")
    cp_fields = {"version", "journal_id", "seq", "head"}
    if not isinstance(checkpoint, dict) or set(checkpoint) != cp_fields or \
            type(checkpoint["version"]) is not int or checkpoint["version"] != 1 or \
            checkpoint["journal_id"] != expected_journal_id or \
            not _integer(checkpoint["seq"]) or checkpoint["seq"] < 0 or not _hash(checkpoint["head"]):
        raise JournalError("invalid_checkpoint")
    if not isinstance(raw, bytes) or (raw and not raw.endswith(b"\n")):
        raise JournalError("incomplete_record")
    previous = ZERO
    events = set()
    operations = {}
    cycles = {}
    cleanups = {}
    seq = 0
    for line in raw.splitlines():
        if not line or len(line) > 4096:
            raise JournalError("invalid_record_size")
        row = _decode(line)
        if not isinstance(row, dict) or set(row) != FIELDS:
            raise JournalError("invalid_record_fields")
        if type(row["version"]) is not int or row["version"] != 1 or \
                row["journal_id"] != expected_journal_id or not _integer(row["seq"]) or row["seq"] != seq + 1:
            raise JournalError("identity_or_sequence")
        if any(not _uuid(row[k]) for k in ("event_id", "intent_id", "target_id", "cycle_id")):
            raise JournalError("invalid_record_id")
        if row["action"] not in ("delete", "undo", "purge", "cleanup") or \
                row["phase"] not in ("intent", "committed", "aborted") or row["target_kind"] not in ("memory", "media"):
            raise JournalError("invalid_record_enum")
        if row["event_id"] in events:
            raise JournalError("duplicate_event")
        if row["previous"] != previous or not _hash(row["hash"]) or row["hash"] != digest(row):
            raise JournalError("broken_chain")
        events.add(row["event_id"])
        key = (row["target_kind"], row["target_id"], row["cycle_id"])
        operation = operations.get(row["intent_id"])
        if row["phase"] == "intent":
            if operation is not None:
                raise JournalError("duplicate_intent")
            if row["action"] == "cleanup":
                # J2c2: object removal under a cleanup plan, not a user deletion. Media only, needs no delete
                # intent, and repeats: each pass or late removal is a new intent on the same plan identity.
                if row["target_kind"] != "media":
                    raise JournalError("invalid_cleanup")
                operation = {"row": row, "status": "pending"}
                operations[row["intent_id"]] = operation
                cleanups.setdefault(key, []).append(operation)
                seq = row["seq"]
                previous = row["hash"]
                continue
            if row["action"] == "delete":
                if key in cycles:
                    raise JournalError("reused_cycle")
                cycles[key] = []
            elif key not in cycles:
                raise JournalError("unknown_cycle")
            # Terminal acknowledgements can arrive late. Logical order is the
            # intent sequence, not the acknowledgement sequence.
            operation = {"row": row, "status": "pending"}
            operations[row["intent_id"]] = operation
            cycles[key].append(operation)
        else:
            if operation is None or operation["status"] != "pending" or \
                    any(row[k] != operation["row"][k] for k in IDENTITY):
                raise JournalError("invalid_acknowledgement")
            operation["status"] = row["phase"]
        seq = row["seq"]
        previous = row["hash"]
    if seq != checkpoint["seq"] or previous != checkpoint["head"]:
        raise JournalError("checkpoint_mismatch")

    hidden = set()
    irreversible = set()
    replay = []
    pending = []
    for key, entries in sorted(cycles.items()):
        kind, target, cycle = key
        delete = entries[0]
        # Reject histories that claim effects on a proven-aborted deletion.
        if delete["status"] == "aborted":
            if any(op["status"] != "aborted" for op in entries[1:]):
                raise JournalError("action_on_aborted_cycle")
            continue
        purging = any(op["row"]["action"] == "purge" and op["status"] != "aborted" for op in entries)
        undone = any(op["row"]["action"] == "undo" and op["status"] == "committed" for op in entries)
        # An undo of this cycle cannot make another cycle visible. Uncertain
        # delete/purge effects are never dismissed by inference or elapsed time.
        hide = purging or delete["status"] == "pending" or not undone
        if hide:
            hidden.add((kind, target))
        if purging:
            irreversible.add((kind, target))
        replay.append({"target_kind": kind, "target_id": target, "cycle_id": cycle,
                       "hide": hide, "purge_barrier": purging})
    # Bytes may be gone whatever the cleanup's state, unless proven not applied: never a reason to open.
    for (kind, target, _), entries in sorted(cleanups.items()):
        if any(op["status"] != "aborted" for op in entries):
            irreversible.add((kind, target))
    for intent, operation in sorted(operations.items()):
        if operation["status"] == "pending":
            pending.append(intent)
    targets = lambda rows: [{"target_kind": kind, "target_id": target} for kind, target in sorted(rows)]
    return {"version": 1, "journal_id": expected_journal_id, "verified_seq": seq, "verified_head": previous,
            "status": "replay_required", "serving_allowed": False, "cycles": replay,
            "hidden_targets": targets(hidden), "purge_barriers": targets(irreversible),
            "unresolved_intents": pending}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--journal", required=True, type=Path)
    parser.add_argument("--checkpoint", required=True, type=Path)
    parser.add_argument("--expected-journal-id", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    # Never overwrite an earlier plan; failure cleans up only our new artifact.
    created = False
    try:
        plan = make_plan(args.journal.read_bytes(), _decode(args.checkpoint.read_bytes()), args.expected_journal_id)
        with args.output.open("xb") as output:
            created = True
            output.write(canonical(plan) + b"\n")
    except (OSError, JournalError, RecursionError):
        if created:
            try:
                args.output.unlink()
            except OSError:
                pass  # A partial report still cannot grant serving permission.
        print("JOURNAL_PLAN_BLOCKED", file=sys.stderr)
        return 2
    print("JOURNAL_VALIDATED_REPLAY_STILL_REQUIRED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
