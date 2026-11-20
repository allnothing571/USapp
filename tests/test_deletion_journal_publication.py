"""J4b reference-model tests. All storage and transactions below are in-memory fakes."""
import copy
import threading
import unittest
import uuid

from tools import deletion_journal as j1
from tools import deletion_journal_batches as batches
from tools import deletion_journal_publication as p


def uid():
    return str(uuid.uuid4())


def operation(count=2, action="delete"):
    return dict(operation_id=uid(), action=action, request_fingerprint="a" * 64,
                subjects=[dict(target_kind="memory" if i == 0 and action != "cleanup" else "media",
                               target_id=uid(), cycle_id=uid(), intent_id=uid()) for i in range(count)])


class Objects(p.Objects):
    def __init__(self):
        self.lock = threading.RLock()
        self.data, self.puts, self.gets = {}, [], []
        self.fail_put = self.lose_put = self.fail_get = self.corrupt_get = False
        self.after_put = None
        self.tx_thread = threading.local()

    def outside_transaction(self):
        if getattr(self.tx_thread, "active", False):
            raise AssertionError("object_io_under_metadata_transaction")

    def put_once(self, key, data):
        self.outside_transaction()
        with self.lock:
            self.puts.append(key)
            if self.fail_put:
                self.fail_put = False
                return p.Receipt.UNKNOWN
            if key in self.data and self.data[key] != data:
                return p.Receipt.CONFLICT
            self.data[key] = data
            if self.lose_put:
                self.lose_put = False
                return p.Receipt.UNKNOWN
        if self.after_put:
            self.after_put()
        return p.Receipt.CONFIRMED

    def get(self, key):
        self.outside_transaction()
        with self.lock:
            self.gets.append(key)
            if self.fail_get or key not in self.data:
                raise p.Unavailable("fixed-fault")
            return b"corrupted" if self.corrupt_get else self.data[key]


class Metadata(p.Metadata):
    def __init__(self, journal_id, objects):
        self.id, self.objects = journal_id, objects
        self.lock = threading.RLock()
        self.state = dict(checkpoint=p.zero_head(journal_id), keys=[], operations={}, cycles=[])
        self.frames = []
        self.fail_read = self.changed = self.disabled = False
        self.failure = None
        self.commits = 0

    def read(self, journal_id):
        with self.lock:
            if self.fail_read or self.disabled or journal_id != self.id:
                raise p.Unavailable("fixed-fault")
            return copy.deepcopy(self.state)

    def commit(self, journal_id, expected_head, candidate_key, candidate_head):
        # Independent candidate read is outside the critical section; a real
        # adapter must prove immutable bytes cannot change after this read.
        candidate = self.objects.get(candidate_key)
        frame = j1._decode(candidate)
        with self.lock:
            self.objects.tx_thread.active = True
            try:
                self.commits += 1
                if journal_id != self.id:
                    return p.Receipt.CONFLICT
                if self.changed or expected_head != self.state["checkpoint"]:
                    return p.Receipt.HEAD_CHANGED
                if self.failure == "before":
                    self.failure = None
                    return p.Receipt.UNKNOWN
                if candidate_key != p.object_key(self.id, frame["hash"]) or candidate != j1.canonical(frame) + b"\n":
                    return p.Receipt.CONFLICT
                frames = self.frames + [frame]
                raw = b"".join(j1.canonical(f) + b"\n" for f in frames)
                try:
                    batches.make_plan(raw, candidate_head, self.id)
                except j1.JournalError:
                    return p.Receipt.CONFLICT
                operations, cycles = p.derive_indexes(frames)
                # One atomic state replacement; read() uses the same lock.
                self.state = dict(checkpoint=copy.deepcopy(candidate_head), keys=self.state["keys"] + [candidate_key],
                                  operations=operations, cycles=cycles)
                self.frames = frames
                if self.failure == "after":
                    self.failure = None
                    return p.Receipt.UNKNOWN
                return p.Receipt.CONFIRMED
            finally:
                self.objects.tx_thread.active = False


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.id = uid()
        self.objects = Objects()
        self.meta = Metadata(self.id, self.objects)
        self.publisher = p.Publisher(self.id, self.objects, self.meta)

    def test_complete_operation_and_terminal_are_atomic_and_deduplicated(self):
        op = operation(3)
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        state = self.meta.read(self.id)
        self.assertEqual(state["checkpoint"]["batch_seq"], 1)
        self.assertEqual(state["checkpoint"]["event_seq"], 3)
        self.assertEqual(len(state["cycles"]), 3)
        self.assertEqual(len(state["operations"]), 1)
        self.assertEqual(len(self.publisher.inspect()["hidden_targets"]), 3)
        self.assertFalse(self.publisher.inspect()["serving_allowed"])
        self.assertEqual(self.publisher.publish(op, "committed"), p.State.COMMITTED)
        before = self.meta.read(self.id)
        calls = len(self.objects.puts)
        self.assertEqual(self.publisher.publish(op), p.State.COMMITTED)
        self.assertEqual(self.publisher.publish(op, "committed"), p.State.COMMITTED)
        self.assertEqual(len(self.objects.puts), calls)
        self.assertEqual(self.meta.read(self.id), before)

    def test_normalization_and_changed_payload_keep_operation_binding(self):
        op = operation(3)
        original = copy.deepcopy(op)
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        op["subjects"].reverse()
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.assertEqual(self.meta.read(self.id)["checkpoint"]["batch_seq"], 1)
        for field in ("fingerprint", "subjects", "action"):
            changed = copy.deepcopy(original)
            if field == "fingerprint":
                changed["request_fingerprint"] = "b" * 64
            elif field == "subjects":
                changed["subjects"][0]["cycle_id"] = uid()
            else:
                changed["action"] = "undo"
            self.assertEqual(self.publisher.publish(changed), p.State.CONFLICT)
        self.assertEqual(self.meta.read(self.id)["checkpoint"]["batch_seq"], 1)

    def test_unknown_cycle_or_terminal_without_intent_writes_nothing(self):
        self.assertEqual(self.publisher.publish(operation(), "committed"), p.State.CONFLICT)
        self.assertEqual(self.publisher.publish(operation(action="purge")), p.State.CONFLICT)
        self.assertEqual(self.objects.puts, [])
        self.assertEqual(self.meta.read(self.id)["operations"], {})

    def test_aborted_terminal_never_flips(self):
        op = operation()
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.assertEqual(self.publisher.publish(op, "aborted"), p.State.ABORTED)
        self.assertEqual(self.publisher.publish(op), p.State.ABORTED)
        self.assertEqual(self.publisher.publish(op, "committed"), p.State.CONFLICT)
        self.assertEqual(self.publisher.inspect()["hidden_targets"], [])

    def test_cleanup_needs_no_fake_delete_and_preserves_barrier_semantics(self):
        op = operation(action="cleanup")
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        plan = self.publisher.inspect()
        self.assertEqual(len(plan["purge_barriers"]), 2)
        self.assertEqual(plan["hidden_targets"], [])
        self.assertEqual(self.meta.read(self.id)["cycles"], [])

    def test_undo_and_late_delete_ack_are_validated_through_j1(self):
        delete = operation()
        self.assertEqual(self.publisher.publish(delete), p.State.READY)
        undo = copy.deepcopy(delete)
        undo.update(operation_id=uid(), action="undo")
        for s in undo["subjects"]:
            s["intent_id"] = uid()
        self.assertEqual(self.publisher.publish(undo), p.State.READY)
        self.assertEqual(self.publisher.publish(undo, "committed"), p.State.COMMITTED)
        self.assertEqual(len(self.publisher.inspect()["hidden_targets"]), 2)
        self.assertEqual(self.publisher.publish(delete, "committed"), p.State.COMMITTED)
        self.assertEqual(self.publisher.inspect()["hidden_targets"], [])

    def test_read_failure_and_missing_namespace_do_not_bootstrap_from_objects(self):
        self.meta.fail_read = True
        self.assertEqual(self.publisher.publish(operation()), p.State.UNAVAILABLE)
        self.assertEqual(self.objects.puts, [])
        self.meta.fail_read = False
        op = operation()
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.meta.disabled = True
        fresh = p.Publisher(self.id, self.objects, self.meta)
        self.assertEqual(fresh.publish(op), p.State.UNAVAILABLE)
        self.assertEqual(len(self.objects.data), 1)

    def test_lost_object_reply_never_advances_checkpoint_and_retry_reuses_bytes(self):
        for lost in (False, True):
            with self.subTest(lost=lost):
                self.setUp()
                op = operation()
                self.objects.lose_put, self.objects.fail_put = lost, not lost
                self.assertEqual(self.publisher.publish(op), p.State.UNKNOWN)
                self.assertEqual(self.meta.commits, 0)
                self.assertEqual(self.meta.read(self.id)["checkpoint"], p.zero_head(self.id))
                first = self.objects.puts[0]
                self.assertEqual(self.publisher.publish(op), p.State.READY)
                self.assertEqual(self.objects.puts[-1], first)
                self.assertEqual(len(self.objects.data), 1)

    def test_unverified_uploaded_bytes_never_advance_metadata(self):
        self.objects.corrupt_get = True
        self.assertEqual(self.publisher.publish(operation()), p.State.UNKNOWN)
        self.assertEqual(self.meta.commits, 0)
        self.assertEqual(self.meta.read(self.id)["checkpoint"], p.zero_head(self.id))

    def test_commit_receipt_loss_preserves_whole_index_and_retry_is_safe(self):
        for when in ("before", "after"):
            with self.subTest(when=when):
                self.setUp()
                op = operation(4)
                self.meta.failure = when
                self.assertEqual(self.publisher.publish(op), p.State.UNKNOWN)
                observed = self.meta.read(self.id)
                self.assertEqual(len(observed["operations"]), 0 if when == "before" else 1)
                self.assertEqual(len(observed["cycles"]), 0 if when == "before" else 4)
                self.assertEqual(observed["checkpoint"]["event_seq"], 0 if when == "before" else 4)
                restarted = p.Publisher(self.id, self.objects, self.meta)
                self.assertEqual(restarted.publish(op), p.State.READY)
                self.assertEqual(len(self.meta.read(self.id)["keys"]), 1)
                self.assertEqual(len(self.meta.read(self.id)["cycles"]), 4)

    def test_lost_terminal_commit_is_classified_not_republished(self):
        op = operation()
        self.assertEqual(self.publisher.publish(op), p.State.READY)
        self.meta.failure = "after"
        self.assertEqual(self.publisher.publish(op, "committed"), p.State.UNKNOWN)
        calls = len(self.objects.puts)
        fresh = p.Publisher(self.id, self.objects, self.meta)
        self.assertEqual(fresh.publish(op, "committed"), p.State.COMMITTED)
        self.assertEqual(len(self.objects.puts), calls)
        self.assertEqual(self.meta.read(self.id)["checkpoint"]["batch_seq"], 2)

    def test_exception_after_metadata_commit_is_not_rollback_proof(self):
        op = operation()
        commit = self.meta.commit
        def lose_response(*args):
            result = commit(*args)
            if result == p.Receipt.CONFIRMED:
                raise TimeoutError("fixed-lost-response")
            return result
        self.meta.commit = lose_response
        self.assertEqual(self.publisher.publish(op), p.State.UNAVAILABLE)
        self.assertEqual(self.meta.read(self.id)["checkpoint"]["batch_seq"], 1)
        self.meta.commit = commit
        calls = len(self.objects.puts)
        self.assertEqual(p.Publisher(self.id, self.objects, self.meta).publish(op), p.State.READY)
        self.assertEqual(len(self.objects.puts), calls)

    def test_stale_read_cannot_commit_over_current_predecessor(self):
        old = self.meta.read(self.id)
        existing = operation()
        self.assertEqual(self.publisher.publish(existing), p.State.READY)
        current_read = self.meta.read
        actual = current_read(self.id)
        self.meta.read = lambda _: copy.deepcopy(old)
        bounded = p.Publisher(self.id, self.objects, self.meta, max_rounds=2)
        self.assertEqual(bounded.publish(operation()), p.State.UNAVAILABLE)
        self.assertEqual(current_read(self.id), actual)

    def concurrent(self, operations):
        barrier, lock, arrivals = threading.Barrier(2), threading.Lock(), [0]
        def after_put():
            with lock:
                arrivals[0] += 1
                wait = arrivals[0] <= 2
            if wait:
                barrier.wait(timeout=5)
        self.objects.after_put = after_put
        results = [None, None]
        def run(index):
            publisher = p.Publisher(self.id, self.objects, self.meta)
            results[index] = publisher.publish(operations[index])
        threads = [threading.Thread(target=run, args=(i,)) for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=10)
            self.assertFalse(t.is_alive())
        self.objects.after_put = None
        return results

    def test_two_publishers_with_same_operation_commit_once(self):
        op = operation()
        self.assertEqual(self.concurrent([op, copy.deepcopy(op)]), [p.State.READY, p.State.READY])
        state = self.meta.read(self.id)
        self.assertEqual(state["checkpoint"]["batch_seq"], 1)
        self.assertEqual(len(state["operations"]), 1)
        self.assertEqual(len(self.objects.data), 1)

    def test_two_distinct_operations_rebase_but_never_adopt_orphan_candidate(self):
        self.assertEqual(self.concurrent([operation(), operation()]), [p.State.READY, p.State.READY])
        state = self.meta.read(self.id)
        self.assertEqual(state["checkpoint"]["batch_seq"], 2)
        self.assertEqual(len(state["operations"]), 2)
        self.assertEqual(len(self.objects.data), 3)  # one losing predecessor candidate
        self.assertEqual(len(state["keys"]), 2)
        self.assertEqual(len(self.publisher.inspect()["hidden_targets"]), 4)

    def test_same_operation_competing_payloads_cannot_both_publish(self):
        first = operation()
        second = copy.deepcopy(first)
        second["request_fingerprint"] = "b" * 64
        self.assertCountEqual(self.concurrent([first, second]), [p.State.READY, p.State.CONFLICT])
        self.assertEqual(self.meta.read(self.id)["checkpoint"]["batch_seq"], 1)
        self.assertEqual(len(self.objects.data), 2)

    def test_malformed_or_partial_index_and_missing_objects_fail_closed(self):
        for broken in ("operation", "cycle", "head", "object"):
            with self.subTest(broken=broken):
                self.setUp()
                op = operation()
                self.assertEqual(self.publisher.publish(op), p.State.READY)
                if broken == "operation":
                    self.meta.state["operations"].clear()
                elif broken == "cycle":
                    self.meta.state["cycles"].clear()
                elif broken == "head":
                    self.meta.state["checkpoint"]["batch_head"] = "b" * 64
                else:
                    self.objects.data.clear()
                self.assertEqual(self.publisher.publish(op), p.State.UNAVAILABLE)

    def test_bounded_contention_and_quota_refusal_do_not_split_operations(self):
        self.meta.changed = True
        publisher = p.Publisher(self.id, self.objects, self.meta, max_rounds=2)
        self.assertEqual(publisher.publish(operation()), p.State.UNAVAILABLE)
        self.assertEqual(self.meta.commits, 2)
        self.assertEqual(self.meta.read(self.id)["operations"], {})
        self.meta.changed = False
        for limits, expected in (({"max_subjects": 1}, p.State.INVALID), ({"max_bytes": 1}, p.State.UNAVAILABLE),
                                 ({"max_snapshot_bytes": 1}, p.State.UNAVAILABLE)):
            calls = len(self.objects.puts)
            self.assertEqual(p.Publisher(self.id, self.objects, self.meta, **limits).publish(operation()), expected)
            self.assertEqual(len(self.objects.puts), calls)
        bounded = p.Publisher(self.id, self.objects, self.meta, max_batches=1)
        self.assertEqual(bounded.publish(operation()), p.State.READY)
        self.assertEqual(bounded.publish(operation()), p.State.UNAVAILABLE)
        self.assertEqual(len(self.meta.read(self.id)["keys"]), 1)

    def test_immutable_object_conflict_and_denied_adapter_never_publish(self):
        op = self.publisher._operation(operation())
        candidate, _ = self.publisher._candidate(op, "intent", p.zero_head(self.id))
        key = p.object_key(self.id, candidate["hash"])
        self.objects.data[key] = b"different-immutable-bytes"
        self.assertEqual(self.publisher.publish(op), p.State.CONFLICT)
        self.assertEqual(self.meta.commits, 0)
        def denied():
            raise PermissionError("private-adapter-failure-not-for-output")
        self.objects.after_put = denied
        result = self.publisher.publish(operation())
        self.assertEqual(result, p.State.UNAVAILABLE)
        self.assertNotIn("private", result.value)
        self.assertEqual(self.meta.read(self.id)["checkpoint"], p.zero_head(self.id))

    def test_unknown_fields_invalid_config_and_foreign_identity_are_refused(self):
        value = operation()
        value["title"] = "not-a-journal-field"
        self.assertEqual(self.publisher.publish(value), p.State.INVALID)
        self.assertEqual(self.publisher.publish(operation(), "timeout-aborted"), p.State.INVALID)
        self.assertEqual(p.Publisher(uid(), self.objects, self.meta).publish(operation()), p.State.UNAVAILABLE)
        for limits in ({"max_rounds": 0}, {"max_bytes": True}):
            with self.assertRaises(ValueError):
                p.Publisher(self.id, self.objects, self.meta, **limits)
        self.assertEqual(self.objects.puts, [])


if __name__ == "__main__":
    unittest.main()
