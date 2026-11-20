"""J4b synchronous publication reference model, injected adapters only.

No OSS/Tablestore client, HTTP service, credentials, production configuration or
serving authorization. Real adapters still need bounded calls and live proof.
"""
import copy
from enum import Enum
import uuid

from . import deletion_journal as j1
from . import deletion_journal_batches as batches


class State(Enum):
    READY = "ready"
    COMMITTED = "committed"
    ABORTED = "aborted"
    UNKNOWN = "unknown"
    UNAVAILABLE = "unavailable"
    CONFLICT = "conflict"
    INVALID = "invalid"


class Receipt(Enum):
    CONFIRMED = "confirmed"
    UNKNOWN = "unknown"
    CONFLICT = "conflict"
    HEAD_CHANGED = "head_changed"


class Unavailable(Exception):
    """Fixed category; callers never print adapter exception text."""


def zero_head(journal_id):
    return dict(version=2, journal_id=journal_id, batch_seq=0, batch_head=j1.ZERO,
                event_seq=0, event_head=j1.ZERO)


def object_key(journal_id, batch_hash):
    return "journals/{}/batches/{}.json".format(journal_id, batch_hash)


def operation_of(frame):
    return dict(operation_id=frame["operation_id"], action=frame["action"],
                request_fingerprint=frame["request_fingerprint"], subjects=copy.deepcopy(frame["subjects"]))


def derive_indexes(frames):
    """Only call after complete v2/J1 validation; not a legacy enrollment path."""
    operations, cycles = {}, set()
    for frame in frames:
        operations[frame["operation_id"]] = dict(operation=operation_of(frame), phase=frame["phase"])
        if frame["action"] == "delete" and frame["phase"] == "intent":
            cycles.update((s["target_kind"], s["target_id"], s["cycle_id"]) for s in frame["subjects"])
    return operations, sorted(cycles)


class Objects:
    def put_once(self, key, data):
        """CONFIRMED only if immutable complete bytes are durable, or identical bytes already exist."""
        raise NotImplementedError

    def get(self, key):
        """Return exact immutable bytes, or raise Unavailable. Never infer from listing/ETag."""
        raise NotImplementedError


class Metadata:
    def read(self, journal_id):
        """Atomic current checkpoint, referenced keys, operation and cycle indexes; no cached fallback."""
        raise NotImplementedError

    def commit(self, journal_id, expected_head, candidate_key, candidate_head):
        """Independently validate immutable candidate OUTSIDE the metadata transaction, then atomically
        compare full predecessor and commit checkpoint + all indexes in one journal partition.
        No object-store/other-service I/O under the metadata transaction; only the
        metadata store's own transactional RPCs. Unknown commit never means rollback.
        """
        raise NotImplementedError


class Publisher:
    """Bounded offline model. Full-history reads are intentionally NOT a production scalability design.

    Limits are prototype safeguards, not approved production service quotas.
    Adapters must bound wall time themselves; the model bounds work/call counts only.
    """
    def __init__(self, journal_id, objects, metadata, max_rounds=3, max_batches=1024,
                 max_subjects=256, max_bytes=1024 * 1024, max_snapshot_bytes=8 * 1024 * 1024):
        if not j1._uuid(journal_id) or any(type(n) is not int or n <= 0 for n in
                                         (max_rounds, max_batches, max_subjects, max_bytes, max_snapshot_bytes)):
            raise ValueError("invalid_model_configuration")
        self.journal_id, self.objects, self.metadata = journal_id, objects, metadata
        self.max_rounds, self.max_batches, self.max_subjects = max_rounds, max_batches, max_subjects
        self.max_bytes, self.max_snapshot_bytes = max_bytes, max_snapshot_bytes

    def _operation(self, value):
        fields = {"operation_id", "action", "request_fingerprint", "subjects"}
        if not isinstance(value, dict) or set(value) != fields or not j1._uuid(value["operation_id"]) or \
                value["action"] not in ("delete", "undo", "purge", "cleanup") or \
                not isinstance(value["request_fingerprint"], str) or \
                (value["request_fingerprint"] != "" and not j1._hash(value["request_fingerprint"])) or \
                not isinstance(value["subjects"], list) or not 0 < len(value["subjects"]) <= self.max_subjects:
            raise j1.JournalError("invalid_operation")
        normalized = copy.deepcopy(value)
        # Validate before sorting to avoid arbitrary malformed comparisons.
        for s in normalized["subjects"]:
            batches._subjects([s])
        normalized["subjects"].sort(key=lambda s: (0 if s["target_kind"] == "memory" else 1,
                                                  s["target_id"], s["cycle_id"], s["intent_id"]))
        batches._subjects(normalized["subjects"])
        if normalized["action"] == "cleanup" and any(s["target_kind"] != "media" for s in normalized["subjects"]):
            raise j1.JournalError("invalid_operation")
        return normalized

    def _load(self):
        snapshot = self.metadata.read(self.journal_id)
        if not isinstance(snapshot, dict) or set(snapshot) != {"checkpoint", "keys", "operations", "cycles"} or \
                not isinstance(snapshot["keys"], list) or len(snapshot["keys"]) > self.max_batches:
            raise Unavailable
        frames, raw = [], b""
        for key in snapshot["keys"]:
            if not isinstance(key, str):
                raise Unavailable
            data = self.objects.get(key)
            if not isinstance(data, bytes) or len(data) > self.max_bytes or len(raw) + len(data) > self.max_snapshot_bytes:
                raise Unavailable
            frame = j1._decode(data)
            if not isinstance(frame, dict) or not j1._hash(frame.get("hash")) or \
                    key != object_key(self.journal_id, frame["hash"]) or data != j1.canonical(frame) + b"\n":
                raise Unavailable
            frames.append(frame)
            raw += data
        plan = batches.make_plan(raw, snapshot["checkpoint"], self.journal_id)
        operations, cycles = derive_indexes(frames)
        if operations != snapshot["operations"] or cycles != snapshot["cycles"]:
            raise Unavailable
        return snapshot, frames, raw, plan

    def _candidate(self, operation, phase, head):
        batch_id = str(uuid.uuid5(uuid.UUID(operation["operation_id"]), phase + ":" + head["batch_head"]))
        frame = dict(operation, version=2, journal_id=self.journal_id, batch_seq=head["batch_seq"] + 1,
                     batch_id=batch_id, previous=head["batch_head"], phase=phase, events=[])
        previous = head["event_head"]
        for ordinal, subject in enumerate(operation["subjects"], 1):
            row = dict(subject, version=1, journal_id=self.journal_id, seq=head["event_seq"] + ordinal,
                       event_id=str(uuid.uuid5(uuid.UUID(batch_id), subject["intent_id"])),
                       previous=previous, action=operation["action"], phase=phase)
            row["hash"] = j1.digest(row)
            previous = row["hash"]
            frame["events"].append(row)
        frame["hash"] = j1.digest(frame)
        checkpoint = dict(version=2, journal_id=self.journal_id, batch_seq=frame["batch_seq"],
                          batch_head=frame["hash"], event_seq=head["event_seq"] + len(operation["subjects"]), event_head=previous)
        return frame, checkpoint

    def publish(self, value, phase="intent"):
        try:
            operation = self._operation(value)
            if phase not in ("intent", "committed", "aborted"):
                return State.INVALID
        except (j1.JournalError, TypeError, ValueError, RecursionError):
            return State.INVALID
        for _ in range(self.max_rounds):
            try:
                snapshot, frames, raw, _ = self._load()
                known = snapshot["operations"].get(operation["operation_id"])
                if known:
                    if known["operation"] != operation:
                        return State.CONFLICT
                    if phase == "intent" or known["phase"] != "intent":
                        if phase != "intent" and known["phase"] != phase:
                            return State.CONFLICT
                        return {"intent": State.READY, "committed": State.COMMITTED, "aborted": State.ABORTED}[known["phase"]]
                elif phase != "intent":
                    return State.CONFLICT
                if len(frames) >= self.max_batches:
                    return State.UNAVAILABLE
                frame, head = self._candidate(operation, phase, snapshot["checkpoint"])
                data = j1.canonical(frame) + b"\n"
                if len(data) > self.max_bytes or len(raw) + len(data) > self.max_snapshot_bytes:
                    return State.UNAVAILABLE
                try:
                    batches.make_plan(raw + data, head, self.journal_id)
                except j1.JournalError:
                    return State.CONFLICT  # e.g. unknown deletion cycle: refuse BEFORE any object write
                key = object_key(self.journal_id, frame["hash"])
                uploaded = self.objects.put_once(key, data)
                if uploaded != Receipt.CONFIRMED:
                    return State.CONFLICT if uploaded == Receipt.CONFLICT else State.UNKNOWN
                if self.objects.get(key) != data:
                    return State.UNKNOWN
                committed = self.metadata.commit(self.journal_id, snapshot["checkpoint"], key, head)
                if committed == Receipt.HEAD_CHANGED:
                    continue  # bounded reread, same operation identity; candidate stays unreferenced
                if committed == Receipt.CONFLICT:
                    return State.CONFLICT
                if committed != Receipt.CONFIRMED:
                    return State.UNKNOWN
                return {"intent": State.READY, "committed": State.COMMITTED, "aborted": State.ABORTED}[phase]
            except Exception:
                # No adapter details, object keys or credentials escape as an outcome.
                return State.UNAVAILABLE
        return State.UNAVAILABLE

    def inspect(self):
        """Offline preparation report only; validates the pinned current chain, never serving permission."""
        return self._load()[3]

    def lookup(self, operation_id):
        """Authoritative only under Metadata.read's contract; validate full chain/indexes, never cached fallback.

        This is an operation binding read, NOT a recovery Source or serving authorization.
        """
        if not j1._uuid(operation_id):
            return "unknown", None
        try:
            snapshot, _, _, _ = self._load()
            known = snapshot["operations"].get(operation_id)
            if known is None:
                return "missing", None
            return "found", copy.deepcopy(known["operation"])
        except Exception:
            return "unknown", None
