"""J4a offline atomic-batch envelope validator. No provider or serving permission.

Version 2 wraps complete J2 operations around the unchanged J1 logical events.
The latest checkpoint MUST be independently trusted. This does not implement
durable publication, signatures, legacy enrollment or database recovery.
"""
import argparse
import sys
from pathlib import Path

try:
    from . import deletion_journal as j1
except ImportError:  # direct CLI execution
    import deletion_journal as j1


BATCH_FIELDS = {"version", "journal_id", "batch_seq", "previous", "hash", "batch_id",
                "operation_id", "action", "phase", "request_fingerprint", "subjects", "events"}
SUBJECT_FIELDS = {"target_kind", "target_id", "cycle_id", "intent_id"}
CHECKPOINT_FIELDS = {"version", "journal_id", "batch_seq", "batch_head", "event_seq", "event_head"}


class _Parser(argparse.ArgumentParser):
    def error(self, _message):
        self.exit(2, "JOURNAL_BATCH_PLAN_BLOCKED\n")


def _subjects(value):
    if not isinstance(value, list) or not value:
        raise j1.JournalError("invalid_batch_subjects")
    identities, intents, targets = [], set(), set()
    for item in value:
        if not isinstance(item, dict) or set(item) != SUBJECT_FIELDS or item["target_kind"] not in ("memory", "media"):
            raise j1.JournalError("invalid_batch_subject")
        if not all(j1._uuid(item[name]) for name in ("target_id", "cycle_id", "intent_id")):
            raise j1.JournalError("invalid_batch_subject")
        target = (item["target_kind"], item["target_id"])
        if item["intent_id"] in intents or target in targets:
            raise j1.JournalError("duplicate_batch_subject")
        intents.add(item["intent_id"])
        targets.add(target)
        # Same ordering as J2 Kind::Memory, Kind::Media, then the UUID fields.
        identities.append((0 if item["target_kind"] == "memory" else 1,
                           item["target_id"], item["cycle_id"], item["intent_id"]))
    if identities != sorted(identities):
        raise j1.JournalError("noncanonical_batch_order")


def make_plan(raw, checkpoint, expected_journal_id):
    if not j1._uuid(expected_journal_id):
        raise j1.JournalError("invalid_expected_identity")
    if not isinstance(checkpoint, dict) or set(checkpoint) != CHECKPOINT_FIELDS or \
            type(checkpoint["version"]) is not int or checkpoint["version"] != 2 or \
            checkpoint["journal_id"] != expected_journal_id or \
            not all(j1._integer(checkpoint[k]) and checkpoint[k] >= 0 for k in ("batch_seq", "event_seq")) or \
            not all(j1._hash(checkpoint[k]) for k in ("batch_head", "event_head")):
        raise j1.JournalError("invalid_batch_checkpoint")
    if not isinstance(raw, bytes) or (raw and not raw.endswith(b"\n")):
        raise j1.JournalError("incomplete_batch")
    seq, previous, batches, operations, events = 0, j1.ZERO, set(), {}, []
    for line in raw.splitlines():
        batch = j1._decode(line)
        if not isinstance(batch, dict) or set(batch) != BATCH_FIELDS or \
                type(batch["version"]) is not int or batch["version"] != 2 or \
                batch["journal_id"] != expected_journal_id or \
                not j1._integer(batch["batch_seq"]) or batch["batch_seq"] != seq + 1 or \
                not j1._hash(batch["previous"]) or batch["previous"] != previous or \
                not j1._hash(batch["hash"]) or batch["hash"] != j1.digest(batch) or \
                not j1._uuid(batch["batch_id"]) or batch["batch_id"] in batches or \
                not j1._uuid(batch["operation_id"]) or \
                batch["action"] not in ("delete", "undo", "purge", "cleanup") or \
                batch["phase"] not in ("intent", "committed", "aborted") or \
                not isinstance(batch["request_fingerprint"], str) or \
                (batch["request_fingerprint"] != "" and not j1._hash(batch["request_fingerprint"])):
            raise j1.JournalError("invalid_batch")
        _subjects(batch["subjects"])
        if batch["action"] == "cleanup" and any(s["target_kind"] != "media" for s in batch["subjects"]):
            raise j1.JournalError("invalid_cleanup")
        rows = batch["events"]
        if not isinstance(rows, list) or len(rows) != len(batch["subjects"]):
            raise j1.JournalError("partial_batch")
        for subject, row in zip(batch["subjects"], rows):
            if not isinstance(row, dict) or set(row) != j1.FIELDS or \
                    any(row[k] != subject[k] for k in SUBJECT_FIELDS) or \
                    row["action"] != batch["action"] or row["phase"] != batch["phase"] or \
                    row["journal_id"] != expected_journal_id:
                raise j1.JournalError("batch_event_mismatch")
        operation = batch["operation_id"]
        identity = (batch["action"], batch["request_fingerprint"], j1.canonical(batch["subjects"]))
        if batch["phase"] == "intent":
            if operation in operations:
                raise j1.JournalError("reused_operation")
            operations[operation] = (identity, "pending")
        else:
            if operation not in operations or operations[operation] != (identity, "pending"):
                raise j1.JournalError("invalid_batch_acknowledgement")
            operations[operation] = (identity, batch["phase"])
        batches.add(batch["batch_id"])
        events.extend(rows)
        seq, previous = batch["batch_seq"], batch["hash"]
    if seq != checkpoint["batch_seq"] or previous != checkpoint["batch_head"]:
        raise j1.JournalError("batch_checkpoint_mismatch")
    # Reuse the original strict J1 validator for the logical event chain,
    # deletion cycles, exact-cycle undo and irreversible barriers.
    event_checkpoint = dict(version=1, journal_id=expected_journal_id,
                            seq=checkpoint["event_seq"], head=checkpoint["event_head"])
    plan = j1.make_plan(b"".join(j1.canonical(row) + b"\n" for row in events), event_checkpoint, expected_journal_id)
    plan.update(version=2, verified_batch_seq=seq, verified_batch_head=previous,
                unresolved_operations=sorted(op for op, (_, state) in operations.items() if state == "pending"))
    return plan


def main(argv=None):
    parser = _Parser(description=__doc__)
    parser.add_argument("--journal", required=True, type=Path)
    parser.add_argument("--checkpoint", required=True, type=Path)
    parser.add_argument("--expected-journal-id", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    created = False
    try:
        plan = make_plan(args.journal.read_bytes(), j1._decode(args.checkpoint.read_bytes()), args.expected_journal_id)
        with args.output.open("xb") as stream:
            created = True
            stream.write(j1.canonical(plan) + b"\n")
    except (OSError, j1.JournalError, RecursionError):
        if created:
            try:
                args.output.unlink()
            except OSError:
                pass
        print("JOURNAL_BATCH_PLAN_BLOCKED", file=sys.stderr)
        return 2
    print("JOURNAL_BATCH_VALIDATED_REPLAY_STILL_REQUIRED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
