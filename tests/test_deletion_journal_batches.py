"""J4a offline synthetic batch fixtures only: no provider, DB or credentials."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

from tools import deletion_journal as j1
from tools import deletion_journal_batches as journal


def uid():
    return str(uuid.uuid4())


class Log:
    def __init__(self):
        self.id = uid()
        self.frames = []

    def intent(self, action="delete", subjects=None):
        subjects = subjects or [dict(target_kind="memory", target_id=uid(), cycle_id=uid(), intent_id=uid()),
                                dict(target_kind="media", target_id=uid(), cycle_id=uid(), intent_id=uid())]
        subjects = sorted(subjects, key=lambda s: (0 if s["target_kind"] == "memory" else 1, s["target_id"], s["cycle_id"], s["intent_id"]))
        op = uid()
        self.frames.append(dict(version=2, journal_id=self.id, batch_seq=0, previous=j1.ZERO, hash=j1.ZERO,
                                batch_id=uid(), operation_id=op, action=action, phase="intent",
                                request_fingerprint="a" * 64, subjects=subjects, events=[]))
        self.rebuild()
        return op

    def ack(self, op, phase="committed"):
        source = next(f for f in self.frames if f["operation_id"] == op)
        frame = copy.deepcopy(source)
        frame.update(batch_id=uid(), phase=phase, events=[])
        self.frames.append(frame)
        self.rebuild()

    def rebuild(self):
        """Fixture writer, NOT a production repair or trusted checkpoint builder."""
        event_seq, event_head, batch_head = 0, j1.ZERO, j1.ZERO
        for seq, frame in enumerate(self.frames, 1):
            frame.update(batch_seq=seq, previous=batch_head)
            frame["events"] = []
            for subject in frame["subjects"]:
                event_seq += 1
                event = dict(subject, version=1, journal_id=self.id, seq=event_seq,
                             event_id=str(uuid.uuid5(uuid.UUID(frame["batch_id"]), str(event_seq))),
                             previous=event_head, action=frame["action"], phase=frame["phase"])
                event["hash"] = j1.digest(event)
                event_head = event["hash"]
                frame["events"].append(event)
            frame["hash"] = j1.digest(frame)
            batch_head = frame["hash"]

    def raw(self):
        return b"".join(j1.canonical(frame) + b"\n" for frame in self.frames)

    def checkpoint(self):
        events = [e for f in self.frames for e in f["events"]]
        return dict(version=2, journal_id=self.id, batch_seq=len(self.frames),
                    batch_head=self.frames[-1]["hash"] if self.frames else j1.ZERO,
                    event_seq=len(events), event_head=events[-1]["hash"] if events else j1.ZERO)

    def plan(self):
        return journal.make_plan(self.raw(), self.checkpoint(), self.id)


class BatchTests(unittest.TestCase):
    def setUp(self):
        self.log = Log()

    def blocked(self, raw=None, checkpoint=None):
        with self.assertRaises(j1.JournalError):
            journal.make_plan(self.log.raw() if raw is None else raw,
                              self.log.checkpoint() if checkpoint is None else checkpoint, self.log.id)

    def test_empty_still_requires_checkpoint_and_never_opens(self):
        plan = self.log.plan()
        self.assertFalse(plan["serving_allowed"])
        self.assertEqual(plan["version"], 2)
        self.assertEqual(plan["verified_batch_seq"], 0)
        self.blocked(checkpoint={})

    def test_cascade_intent_is_one_bound_operation_with_all_subjects(self):
        op = self.log.intent()
        plan = self.log.plan()
        self.assertEqual(len(plan["hidden_targets"]), 2)
        self.assertEqual(len(plan["unresolved_intents"]), 2)
        self.assertEqual(plan["unresolved_operations"], [op])
        self.log.ack(op)
        plan = self.log.plan()
        self.assertEqual(plan["unresolved_operations"], [])
        self.assertEqual(len(plan["hidden_targets"]), 2)
        self.assertFalse(plan["serving_allowed"])

    def test_exact_cycle_undo_and_late_delete_ack_keep_j1_semantics(self):
        delete = self.log.intent()
        subjects = copy.deepcopy(self.log.frames[0]["subjects"])
        for s in subjects:
            s["intent_id"] = uid()
        undo = self.log.intent("undo", subjects)
        self.log.ack(undo)
        self.assertEqual(len(self.log.plan()["hidden_targets"]), 2)  # delete still uncertain
        self.log.ack(delete)
        self.assertEqual(self.log.plan()["hidden_targets"], [])
        self.assertFalse(self.log.plan()["serving_allowed"])

    def test_purge_and_cleanup_barriers_preserve_target_scope(self):
        delete = self.log.intent()
        self.log.ack(delete)
        subjects = copy.deepcopy(self.log.frames[0]["subjects"])
        for s in subjects:
            s["intent_id"] = uid()
        self.log.intent("purge", subjects)
        self.assertEqual(len(self.log.plan()["purge_barriers"]), 2)
        other = Log()
        other.intent("cleanup", [dict(target_kind="media", target_id=uid(), cycle_id=uid(), intent_id=uid())])
        self.assertEqual(len(other.plan()["purge_barriers"]), 1)
        self.assertEqual(other.plan()["hidden_targets"], [])
        self.log.intent("cleanup")  # includes memory: refused
        self.blocked()

    def test_proven_aborted_batch_has_no_effect(self):
        op = self.log.intent()
        self.log.ack(op, "aborted")
        self.assertEqual(self.log.plan()["hidden_targets"], [])
        self.assertEqual(self.log.plan()["unresolved_operations"], [])

    def test_partial_terminal_batch_cannot_ack_only_one_photo(self):
        op = self.log.intent()
        self.log.ack(op)
        self.log.frames[-1]["events"].pop()
        self.log.frames[-1]["hash"] = j1.digest(self.log.frames[-1])
        self.blocked()

    def test_rehashed_subset_of_subjects_also_refused(self):
        op = self.log.intent()
        self.log.ack(op)
        self.log.frames[-1]["subjects"].pop()
        self.log.rebuild()
        self.blocked()

    def test_terminal_payload_is_immutable(self):
        for field in ("request_fingerprint", "action", "target_id", "cycle_id", "intent_id"):
            with self.subTest(field=field):
                self.log = Log()
                op = self.log.intent()
                self.log.ack(op)
                if field == "request_fingerprint":
                    self.log.frames[-1][field] = "b" * 64
                elif field == "action":
                    self.log.frames[-1][field] = "purge"
                else:
                    self.log.frames[-1]["subjects"][0][field] = uid()
                self.log.rebuild()
                self.blocked()

    def test_repeated_intent_or_terminal_operation_refused(self):
        op = self.log.intent()
        self.log.frames.append(copy.deepcopy(self.log.frames[0]))
        self.log.frames[-1]["batch_id"] = uid()
        self.log.rebuild()
        self.blocked()
        self.log = Log()
        op = self.log.intent()
        self.log.ack(op)
        self.log.ack(op)
        self.blocked()

    def test_duplicate_batch_identity_refused_even_with_rehashed_chain(self):
        op = self.log.intent()
        self.log.ack(op)
        self.log.frames[-1]["batch_id"] = self.log.frames[0]["batch_id"]
        self.log.rebuild()
        self.blocked()

    def test_subject_shape_uniqueness_order_and_empty_batch(self):
        for variant in ("empty", "duplicate_target", "duplicate_intent", "order", "foreign_kind", "unknown_field"):
            with self.subTest(variant=variant):
                self.log = Log()
                self.log.intent()
                subjects = self.log.frames[0]["subjects"]
                if variant == "empty":
                    subjects.clear()
                elif variant == "duplicate_target":
                    subjects.append(dict(subjects[0], intent_id=uid()))
                elif variant == "duplicate_intent":
                    subjects[1]["intent_id"] = subjects[0]["intent_id"]
                elif variant == "order":
                    subjects.reverse()
                elif variant == "foreign_kind":
                    subjects[0]["target_kind"] = "album"
                else:
                    subjects[0]["title"] = "not-allowed"
                self.log.rebuild()
                self.blocked()

    def test_event_mismatch_and_inner_chain_are_both_checked(self):
        for field, value in (("action", "undo"), ("phase", "committed"), ("journal_id", uid()),
                             ("seq", 99), ("hash", j1.ZERO), ("event_id", "not-a-uuid")):
            with self.subTest(field=field):
                self.log = Log()
                self.log.intent()
                self.log.frames[0]["events"][0][field] = value
                self.log.frames[0]["hash"] = j1.digest(self.log.frames[0])
                self.blocked()

    def test_truncation_missing_tail_reorder_and_wrong_checkpoint(self):
        op = self.log.intent()
        self.log.ack(op)
        raw, cp = self.log.raw(), self.log.checkpoint()
        self.blocked(raw=raw[:-1])
        self.blocked(raw=j1.canonical(self.log.frames[0]) + b"\n", checkpoint=cp)
        self.blocked(raw=b"\n".join(raw.splitlines()[::-1]) + b"\n")
        for field, value in (("batch_seq", 1), ("batch_head", j1.ZERO), ("event_seq", 1),
                             ("event_head", j1.ZERO), ("journal_id", uid()), ("version", True)):
            wrong = dict(cp)
            wrong[field] = value
            self.blocked(raw=raw, checkpoint=wrong)

    def test_untrusted_shape_and_nonfinite_json_are_rejected(self):
        self.log.intent()
        raw = self.log.raw()
        self.blocked(raw=raw.replace(b'"version":2', b'"version":2,"version":2', 1))
        self.blocked(raw=raw.replace(b'"version":2', b'"version":NaN', 1))
        self.blocked(raw=b"[]\n")
        self.blocked(raw=b"\xff\n")
        self.log.frames[0]["filename"] = "private"
        self.log.frames[0]["hash"] = j1.digest(self.log.frames[0])
        self.blocked()

    def test_legacy_empty_fingerprint_is_bound_not_reinterpreted(self):
        op = self.log.intent()
        self.log.frames[0]["request_fingerprint"] = ""
        self.log.rebuild()
        self.log.ack(op)
        self.assertFalse(self.log.plan()["serving_allowed"])
        self.log.frames[-1]["request_fingerprint"] = "a" * 64
        self.log.rebuild()
        self.blocked()

    def test_invalid_envelope_metadata_is_rejected_after_rehash(self):
        for field, value in (("operation_id", "bad"), ("batch_seq", True), ("version", True),
                             ("phase", "unknown"), ("request_fingerprint", "raw-key"),
                             ("subjects", "not-a-list"), ("events", [None, None])):
            with self.subTest(field=field):
                self.log = Log()
                self.log.intent()
                checkpoint = self.log.checkpoint()
                self.log.frames[0][field] = value
                self.log.frames[0]["hash"] = j1.digest(self.log.frames[0])
                self.blocked(checkpoint=checkpoint)

    def test_format_does_not_enroll_unknown_legacy_cycles(self):
        self.log.intent("purge")
        self.blocked()
        self.log.frames[0]["origin"] = "baseline"
        self.log.frames[0]["hash"] = j1.digest(self.log.frames[0])
        self.blocked()

    def test_cli_no_overwrite_and_fixed_failure_without_input_echo(self):
        self.log.intent()
        script = Path(__file__).resolve().parents[1] / "tools" / "deletion_journal_batches.py"
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            log, cp, output = base / "private-name.jsonl", base / "head.json", base / "plan.json"
            log.write_bytes(self.log.raw())
            cp.write_bytes(j1.canonical(self.log.checkpoint()))
            command = [sys.executable, "-B", str(script), "--journal", str(log), "--checkpoint", str(cp),
                       "--expected-journal-id", self.log.id, "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0)
            self.assertFalse(json.loads(output.read_bytes())["serving_allowed"])
            saved = output.read_bytes()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            self.assertEqual(output.read_bytes(), saved)
            output.unlink()
            log.write_bytes(b"private-failure-input\n")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stderr.strip(), "JOURNAL_BATCH_PLAN_BLOCKED")
            self.assertNotIn("private", result.stdout + result.stderr)
            self.assertFalse(output.exists())
            result = subprocess.run(command + ["--unexpected", "private-argument"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stderr.strip(), "JOURNAL_BATCH_PLAN_BLOCKED")


if __name__ == "__main__":
    unittest.main()
