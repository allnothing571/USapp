"""Check the C++ fake protocol's exported events through the J1 validator."""
import json
import sys
import uuid
from pathlib import Path

from deletion_journal import ZERO, canonical, digest, make_plan


def main(path):
    # The final stdout line is a synthetic event export, never live data.
    events = json.loads(Path(path).read_text(encoding="utf-8-sig").splitlines()[-1])
    journal_id = "00000000-0000-0000-0000-000000000500"
    previous = ZERO
    lines = []
    for seq, event in enumerate(events, 1):
        if set(event) != {"action", "phase", "target_kind", "target_id", "cycle_id", "intent_id"}:
            raise ValueError("transcript_fields")
        row = dict(event, version=1, journal_id=journal_id, seq=seq,
                   event_id=str(uuid.uuid5(uuid.UUID(journal_id), str(seq))), previous=previous)
        row["hash"] = digest(row)
        previous = row["hash"]
        lines.append(canonical(row) + b"\n")
    cp = dict(version=1, journal_id=journal_id, seq=len(events), head=previous)
    plan = make_plan(b"".join(lines), cp, journal_id)
    memory = {"target_kind": "memory", "target_id": "00000000-0000-0000-0000-000000000001"}
    if len(events) != 15 or len(plan["cycles"]) != 4 or plan["hidden_targets"] != [memory] or \
            plan["purge_barriers"] != [memory] or len(plan["unresolved_intents"]) != 1 or plan["serving_allowed"]:
        raise ValueError("transcript_semantics")
    print("PROTOCOL_TRANSCRIPT_PASSED: 15 events, 4 cycles, 1 unresolved purge, serving closed")


if __name__ == "__main__":
    try:
        main(sys.argv[1])
    except (ValueError, OSError, IndexError):
        print("PROTOCOL_TRANSCRIPT_BLOCKED", file=sys.stderr)
        sys.exit(2)
