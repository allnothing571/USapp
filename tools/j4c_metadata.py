"""J4c bounded TableStore publication adapter; not a live-service entry point.

Requires a provisioned, explicitly initialized head. Missing head never means an
empty journal. Object I/O and chain validation precede the write transaction.
The injected client implements j4c_tablestore_sdk.Client's restricted interface.
"""
import hashlib
import threading

from . import deletion_journal as j1
from . import deletion_journal_batches as batches
from . import deletion_journal_publication as p


class Metadata(p.Metadata):
    def __init__(self, client, objects, max_rows=4096, max_bytes=8 * 1024 * 1024):
        if type(max_rows) is not int or not 1 <= max_rows <= 4096 or \
                type(max_bytes) is not int or not 1 <= max_bytes <= 8 * 1024 * 1024:
            raise ValueError("invalid_adapter_limits")
        self.client, self.objects = client, objects
        self.id = client.config.journal_id
        self.max_rows, self.max_bytes = max_rows, max_bytes
        # One Client owns at most one transaction; concurrent callers serialize
        # locally. Other processes are protected by the conditional head write.
        self.lock = threading.RLock()

    def _identity(self, journal_id):
        if journal_id != self.id:
            raise p.Unavailable("journal_identity")

    def _row(self, row, kind, expected_id=None):
        key = row.primary_key
        if not isinstance(key, list) or len(key) != 3 or \
                key[:2] != [("journal_id", self.id), ("row_type", kind)] or \
                key[2][0] != "row_id" or not isinstance(key[2][1], str) or \
                not 1 <= len(key[2][1]) <= 256 or \
                (expected_id is not None and key[2][1] != expected_id):
            raise p.Unavailable("invalid_metadata_key")
        columns = row.attribute_columns
        if not isinstance(columns, list) or len(columns) != 1 or \
                len(columns[0]) != 3 or columns[0][0] != "payload" or \
                not isinstance(columns[0][1], str):
            raise p.Unavailable("invalid_metadata_payload")
        raw = columns[0][1].encode("utf-8")
        if len(raw) > 65536:
            raise p.Unavailable("metadata_payload_limit")
        value = j1._decode(raw)
        if raw != j1.canonical(value):
            raise p.Unavailable("noncanonical_metadata")
        return key[2][1], value, len(raw)

    def _head(self, tx):
        _, row, token = self.client.get(tx, "head", "current")
        if row is None or token:
            raise p.Unavailable("missing_or_partial_head")
        _, head, _ = self._row(row, "head", "current")
        # Includes strict zero/nonzero checkpoint validation, without any I/O.
        if not isinstance(head, dict) or set(head) != batches.CHECKPOINT_FIELDS or \
                type(head["version"]) is not int or head["version"] != 2 or head["journal_id"] != self.id or \
                not all(j1._integer(head[k]) and 0 <= head[k] < 2**63 for k in ("batch_seq", "event_seq")) or \
                not all(j1._hash(head[k]) for k in ("batch_head", "event_head")) or head["batch_seq"] > 1024:
            raise p.Unavailable("invalid_head")
        if head["batch_seq"] == 0:
            if head != p.zero_head(self.id):
                raise p.Unavailable("invalid_zero_head")
        elif head["event_seq"] < head["batch_seq"] or j1.ZERO in (head["batch_head"], head["event_head"]):
            raise p.Unavailable("invalid_head")
        return head

    def _scan(self, tx, kind, budget):
        start, rows = "", {}
        while True:
            _, following, page, token = self.client.scan(tx, kind, start=start, limit=100)
            if token or not isinstance(page, list) or len(page) > 100:
                raise p.Unavailable("partial_metadata_page")
            for row in page:
                row_id, value, size = self._row(row, kind)
                if row_id < start or row_id in rows or (rows and row_id <= next(reversed(rows))):
                    raise p.Unavailable("metadata_order")
                rows[row_id] = value
                budget[0] += 1
                budget[1] += size
                if budget[0] > self.max_rows or budget[1] > self.max_bytes:
                    raise p.Unavailable("metadata_limit")
            if not following:
                return rows
            prefix = [("journal_id", self.id), ("row_type", kind)]
            if not isinstance(following, list) or len(following) != 3 or following[:2] != prefix or \
                    following[2][0] != "row_id" or not isinstance(following[2][1], str) or \
                    not page or following[2][1] <= next(reversed(rows)) or len(following[2][1]) > 256:
                raise p.Unavailable("invalid_metadata_continuation")
            start = following[2][1]

    @staticmethod
    def _cycle_id(cycle):
        return hashlib.sha256(j1.canonical(list(cycle))).hexdigest()

    def _snapshot(self, tx):
        head = self._head(tx)
        budget = [0, 0]
        rows = {kind: self._scan(tx, kind, budget) for kind in ("batch", "operation", "cycle")}
        # Local transactions are not assumed to provide repeatable-read snapshots.
        # Monotonic conditional heads bracket reads; changed head fails closed.
        if self._head(tx) != head:
            raise p.Unavailable("head_changed_during_read")
        if list(rows["batch"]) != ["{:020d}".format(n) for n in range(1, head["batch_seq"] + 1)]:
            raise p.Unavailable("batch_index_gap")
        keys = []
        for value in rows["batch"].values():
            if not isinstance(value, dict) or set(value) != {"key"} or not isinstance(value["key"], str):
                raise p.Unavailable("invalid_batch_index")
            keys.append(value["key"])
        cycles = []
        for row_id, value in rows["cycle"].items():
            if not isinstance(value, list) or len(value) != 3 or \
                    value[0] not in ("memory", "media") or not all(j1._uuid(v) for v in value[1:]) or \
                    self._cycle_id(value) != row_id:
                raise p.Unavailable("invalid_cycle_index")
            cycles.append(tuple(value))
        return dict(checkpoint=head, keys=keys, operations=rows["operation"], cycles=sorted(cycles))

    def _abort(self, tx):
        try:
            self.client.finish(tx)
        except Exception:
            pass  # Abort is best effort; no write transaction has been committed.

    def read(self, journal_id):
        with self.lock:
            tx = None
            try:
                self._identity(journal_id)
                tx = self.client.start()
                snapshot = self._snapshot(tx)
                self.client.finish(tx)
                tx = None
                return snapshot
            except Exception:
                if tx is not None:
                    self._abort(tx)
                raise p.Unavailable("metadata_read_unknown") from None

    def commit(self, journal_id, expected_head, candidate_key, candidate_head):
        with self.lock:
            tx = None
            try:
                self._identity(journal_id)
                # Independently validate the full immutable chain and indexes.
                # Publisher._load uses read(), whose transaction has already ended.
                verifier = p.Publisher(self.id, self.objects, self)
                snapshot, frames, raw, _ = verifier._load()
                if snapshot["checkpoint"] != expected_head:
                    return p.Receipt.HEAD_CHANGED
                data = self.objects.get(candidate_key)
                if not isinstance(data, bytes) or len(data) > verifier.max_bytes or \
                        len(raw) + len(data) > self.max_bytes or len(frames) >= verifier.max_batches:
                    return p.Receipt.CONFLICT
                frame = j1._decode(data)
                if not isinstance(frame, dict) or candidate_key != p.object_key(self.id, frame.get("hash")) or \
                        data != j1.canonical(frame) + b"\n":
                    return p.Receipt.CONFLICT
                batches.make_plan(raw + data, candidate_head, self.id)
                operations, cycles = p.derive_indexes(frames + [frame])
                if len(snapshot["keys"]) + 1 + len(operations) + len(cycles) > self.max_rows:
                    return p.Receipt.CONFLICT
                tx = self.client.start()
                if self._head(tx) != expected_head:
                    self._abort(tx)
                    tx = None
                    return p.Receipt.HEAD_CHANGED
                self.client.insert(tx, "batch", "{:020d}".format(candidate_head["batch_seq"]),
                                   {"payload": j1.canonical({"key": candidate_key}).decode("ascii")})
                op_id = frame["operation_id"]
                previous = snapshot["operations"].get(op_id)
                payload = j1.canonical(operations[op_id]).decode("ascii")
                if previous is None:
                    self.client.insert(tx, "operation", op_id, {"payload": payload})
                else:
                    self.client.advance_operation(tx, op_id, j1.canonical(previous).decode("ascii"), payload)
                for cycle in sorted(set(cycles) - set(snapshot["cycles"])):
                    self.client.insert(tx, "cycle", self._cycle_id(cycle),
                                       {"payload": j1.canonical(list(cycle)).decode("ascii")})
                self.client.advance_head(tx, expected_head, candidate_head)
                # Detach before sending commit: even an exception may mean applied.
                committing, tx = tx, None
                self.client.finish(committing, commit=True)
                return p.Receipt.CONFIRMED
            except Exception:
                if tx is not None:
                    self._abort(tx)
                return p.Receipt.UNKNOWN
