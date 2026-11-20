"""Publication adapter with a stateful fake SDK facade; no cloud I/O."""
import copy
import unittest
import uuid
from types import SimpleNamespace

from tools import deletion_journal as j1
from tools import deletion_journal_publication as p
from tools.j4c_metadata import Metadata


JOURNAL = "00000000-0000-0000-0000-000000000041"


def operation():
    return dict(operation_id=str(uuid.uuid4()), action="delete", request_fingerprint="a" * 64,
                subjects=[dict(target_kind="media", target_id=str(uuid.uuid4()),
                               cycle_id=str(uuid.uuid4()), intent_id=str(uuid.uuid4()))])


class Objects:
    def __init__(self, client):
        self.client, self.data = client, {}

    def get(self, key):
        if self.client.active:
            raise AssertionError("object_io_under_transaction")
        return self.data[key]

    def put_once(self, key, data):
        if self.client.active:
            raise AssertionError("object_io_under_transaction")
        if key in self.data and self.data[key] != data:
            return p.Receipt.CONFLICT
        self.data[key] = data
        return p.Receipt.CONFIRMED


class Client:
    """Explicit fake: transactional replacement is not vendor atomicity evidence."""
    def __init__(self, journal_id=JOURNAL):
        self.config = SimpleNamespace(journal_id=journal_id)
        self.rows = {("head", "current"): j1.canonical(p.zero_head(journal_id)).decode("ascii")}
        self.active = False
        self.calls, self.pending = [], []
        self.fail_write = None
        self.lose_commit = self.reject_commit = False
        self.on_start = self.on_scan = None
        self.bad_continuation = self.partial = False
        self.page_size = 100

    def start(self):
        if self.active:
            raise RuntimeError("active")
        if self.on_start:
            hook, self.on_start = self.on_start, None
            hook()
        self.active, self.pending = True, []
        self.calls.append("start")
        return "tx"

    def finish(self, tx, commit=False):
        if not self.active or tx != "tx":
            raise RuntimeError("unknown")
        self.active = False
        self.calls.append("commit" if commit else "abort")
        if commit:
            if self.reject_commit:
                raise RuntimeError("private commit error")
            following = copy.deepcopy(self.rows)
            for kind, row_id, expected, value in self.pending:
                if following.get((kind, row_id)) != expected:
                    raise RuntimeError("condition")
                following[kind, row_id] = value
            self.rows = following
            if self.lose_commit:
                self.lose_commit = False
                raise RuntimeError("private lost response")

    def row(self, kind, row_id):
        value = self.rows.get((kind, row_id))
        if value is None:
            return None
        return SimpleNamespace(primary_key=[("journal_id", self.config.journal_id), ("row_type", kind), ("row_id", row_id)],
                               attribute_columns=[("payload", value, 1)])

    def get(self, tx, kind, row_id):
        return None, self.row(kind, row_id), None

    def scan(self, tx, kind, start="", limit=20):
        if self.on_scan:
            hook, self.on_scan = self.on_scan, None
            hook()
        keys = sorted(i for k, i in self.rows if k == kind and i >= start)
        selected = keys[:min(limit, self.page_size)]
        following = None
        if len(selected) < len(keys):
            following = [("journal_id", self.config.journal_id), ("row_type", kind), ("row_id", keys[len(selected)])]
        if self.bad_continuation:
            following = [("journal_id", "foreign"), ("row_type", kind), ("row_id", "x")]
        return None, following, [self.row(kind, i) for i in selected], b"partial" if self.partial else None

    def write(self, kind, row_id, expected, value):
        self.calls.append("write:" + kind)
        if self.fail_write == kind:
            raise RuntimeError("private write error")
        self.pending.append((kind, row_id, expected, value))

    def insert(self, tx, kind, row_id, columns):
        self.write(kind, row_id, None, columns["payload"])

    def advance_head(self, tx, expected, following):
        self.write("head", "current", j1.canonical(expected).decode("ascii"), j1.canonical(following).decode("ascii"))

    def advance_operation(self, tx, row_id, previous, following):
        self.write("operation", row_id, previous, following)


class MetadataTests(unittest.TestCase):
    def setUp(self):
        self.client = Client()
        self.objects = Objects(self.client)
        self.meta = Metadata(self.client, self.objects)
        self.publisher = p.Publisher(JOURNAL, self.objects, self.meta)

    def test_publication_terminal_and_replay(self):
        op = operation()
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.assertEqual(self.publisher.publish(op, "committed"), p.State.COMMITTED)
        before = copy.deepcopy(self.client.rows)
        self.assertEqual(self.publisher.publish(op), p.State.COMMITTED)
        self.assertEqual(self.client.rows, before)
        self.assertEqual(self.meta.read(JOURNAL)["checkpoint"]["batch_seq"], 2)
        self.assertFalse(self.publisher.inspect()["serving_allowed"])

    def test_missing_head_is_not_empty(self):
        self.client.rows.clear()
        self.assertEqual(self.publisher.publish(operation()), p.State.UNAVAILABLE)
        self.assertFalse(self.objects.data)

    def test_foreign_journal_never_starts_transaction(self):
        with self.assertRaises(p.Unavailable):
            self.meta.read(str(uuid.uuid4()))
        self.assertFalse(self.client.calls)

    def test_each_write_failure_leaves_no_partial_indexes(self):
        for kind in ("batch", "operation", "cycle", "head"):
            with self.subTest(kind=kind):
                self.setUp()
                before = copy.deepcopy(self.client.rows)
                self.client.fail_write = kind
                self.assertEqual(self.publisher.publish(operation()), p.State.UNKNOWN)
                self.assertEqual(self.client.rows, before)
                self.assertFalse(self.client.active)
                self.assertNotIn("commit", self.client.calls)

    def test_lost_applied_commit_is_unknown_and_recovers_by_read(self):
        op = operation()
        self.client.lose_commit = True
        self.assertEqual(self.publisher.publish(op), p.State.UNKNOWN)
        commit = self.client.calls.index("commit")
        self.assertEqual(self.client.calls[commit:], ["commit"])
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.assertEqual(self.meta.read(JOURNAL)["checkpoint"]["batch_seq"], 1)

    def test_unknown_unapplied_commit_does_not_abort_or_claim_success(self):
        self.client.reject_commit = True
        self.assertEqual(self.publisher.publish(operation()), p.State.UNKNOWN)
        self.assertEqual(self.client.calls[-1], "commit")
        self.assertEqual(len(self.client.rows), 1)

    def test_pages_are_followed_without_dropping_indexes(self):
        self.client.page_size = 1
        for _ in range(3):
            self.assertEqual(self.publisher.publish(operation()), p.State.READY)
        snapshot = self.meta.read(JOURNAL)
        self.assertEqual([len(snapshot[k]) for k in ("keys", "operations", "cycles")], [3, 3, 3])

    def test_partial_or_foreign_continuation_refuses_read(self):
        for flag in ("bad_continuation", "partial"):
            with self.subTest(flag=flag):
                setattr(self.client, flag, True)
                with self.assertRaises(p.Unavailable):
                    self.meta.read(JOURNAL)
                setattr(self.client, flag, False)
                self.assertFalse(self.client.active)

    def test_changed_head_during_scan_is_not_a_snapshot(self):
        def change():
            head = p.zero_head(JOURNAL)
            head.update(batch_seq=1, event_seq=1, batch_head="a" * 64, event_head="b" * 64)
            self.client.rows["head", "current"] = j1.canonical(head).decode("ascii")
        self.client.on_scan = change
        with self.assertRaises(p.Unavailable):
            self.meta.read(JOURNAL)

    def test_head_changed_after_validation_refuses_write(self):
        op = self.publisher._operation(operation())
        old = p.zero_head(JOURNAL)
        frame, head = self.publisher._candidate(op, "intent", old)
        key = p.object_key(JOURNAL, frame["hash"])
        self.objects.data[key] = j1.canonical(frame) + b"\n"
        original = self.objects.get
        def get(k):
            value = original(k)
            def change():
                self.client.rows["head", "current"] = j1.canonical(head).decode("ascii")
            self.client.on_start = change
            return value
        self.objects.get = get
        self.assertEqual(self.meta.commit(JOURNAL, old, key, head), p.Receipt.HEAD_CHANGED)
        self.assertFalse(any(c.startswith("write:") for c in self.client.calls))

    def test_corrupt_operation_index_stops_next_write(self):
        op = operation()
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.client.rows["operation", op["operation_id"]] = "{}"
        self.assertEqual(self.publisher.publish(operation()), p.State.UNAVAILABLE)

    def test_gap_noncanonical_or_oversized_head_stops(self):
        for payload in ('{}', ' {"version":2}', j1.canonical(dict(p.zero_head(JOURNAL), batch_seq=2**60)).decode("ascii")):
            with self.subTest(payload=payload):
                self.client.rows["head", "current"] = payload
                with self.assertRaises(p.Unavailable):
                    self.meta.read(JOURNAL)

    def test_read_limits_fail_closed(self):
        self.assertEqual(self.publisher.publish(operation()), p.State.READY)
        with self.assertRaises(p.Unavailable):
            Metadata(self.client, self.objects, max_rows=1).read(JOURNAL)
        with self.assertRaises(p.Unavailable):
            Metadata(self.client, self.objects, max_bytes=1).read(JOURNAL)

    def test_candidate_corruption_never_opens_write_transaction(self):
        self.assertEqual(self.meta.commit(JOURNAL, p.zero_head(JOURNAL), "bad", {}), p.Receipt.UNKNOWN)
        self.assertNotIn("commit", self.client.calls)
        self.assertFalse(any(c.startswith("write:") for c in self.client.calls))


if __name__ == "__main__":
    unittest.main()
