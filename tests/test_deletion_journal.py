"""Synthetic, offline D6 replay tests: no database, network or credentials."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("deletion_journal", ROOT / "tools" / "deletion_journal.py")
journal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(journal)


def uid():
    return str(uuid.uuid4())


class Log:
    """In-memory fake writer, intentionally not a production durability adapter."""
    def __init__(self):
        self.id = uid()
        self.rows = []
        self.target = uid()
        self.cycle = uid()

    def intent(self, action="delete", target=None, cycle=None, kind="memory"):
        intent = uid()
        self.append(intent, action, "intent", target or self.target, cycle or self.cycle, kind)
        return intent

    def append(self, intent, action, phase, target, cycle, kind):
        row = dict(version=1, journal_id=self.id, seq=len(self.rows) + 1, event_id=uid(),
                   previous=self.rows[-1]["hash"] if self.rows else journal.ZERO,
                   intent_id=intent, action=action, phase=phase,
                   target_kind=kind, target_id=target, cycle_id=cycle)
        row["hash"] = journal.digest(row)
        self.rows.append(row)

    def ack(self, intent, phase="committed"):
        source = next(row for row in self.rows if row["intent_id"] == intent)
        self.append(intent, source["action"], phase, source["target_id"], source["cycle_id"], source["target_kind"])

    def raw(self):
        return b"".join(journal.canonical(row) + b"\n" for row in self.rows)

    def checkpoint(self):
        return dict(version=1, journal_id=self.id, seq=len(self.rows),
                    head=self.rows[-1]["hash"] if self.rows else journal.ZERO)

    def plan(self):
        return journal.make_plan(self.raw(), self.checkpoint(), self.id)


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.log = Log()

    def blocked(self, raw=None, checkpoint=None):
        with self.assertRaises(journal.JournalError):
            journal.make_plan(self.log.raw() if raw is None else raw,
                              self.log.checkpoint() if checkpoint is None else checkpoint, self.log.id)

    def test_empty_valid_snapshot_never_opens_serving(self):
        self.assertFalse(self.log.plan()["serving_allowed"])

    def test_unconfirmed_delete_hides_after_database_loss(self):
        key = self.log.intent()
        plan = self.log.plan()
        self.assertEqual(plan["unresolved_intents"], [key])
        self.assertEqual(plan["hidden_targets"], [{"target_kind": "memory", "target_id": self.log.target}])

    def test_proven_aborted_delete_does_not_hide(self):
        self.log.ack(self.log.intent(), "aborted")
        self.assertEqual(self.log.plan()["hidden_targets"], [])

    def test_unknown_undo_stays_hidden(self):
        self.log.ack(self.log.intent())
        self.log.intent("undo")
        self.assertTrue(self.log.plan()["hidden_targets"])

    def test_committed_undo_resolves_only_its_cycle(self):
        self.log.ack(self.log.intent())
        self.log.ack(self.log.intent("undo"))
        self.assertEqual(self.log.plan()["hidden_targets"], [])
        self.log.ack(self.log.intent(cycle=uid()))
        self.assertEqual(len(self.log.plan()["hidden_targets"]), 1)

    def test_late_delete_ack_does_not_override_later_undo(self):
        deletion = self.log.intent()
        self.log.ack(self.log.intent("undo"))
        self.log.ack(deletion)
        self.assertEqual(self.log.plan()["hidden_targets"], [])

    def test_pending_delete_is_conservative_even_with_confirmed_undo(self):
        self.log.intent()
        self.log.ack(self.log.intent("undo"))
        self.assertTrue(self.log.plan()["hidden_targets"])

    def test_unknown_purge_cannot_be_reversed_by_undo(self):
        self.log.ack(self.log.intent())
        self.log.intent("purge")
        self.log.ack(self.log.intent("undo"))
        plan = self.log.plan()
        self.assertEqual(plan["hidden_targets"], plan["purge_barriers"])
        self.assertEqual(len(plan["purge_barriers"]), 1)

    def test_aborted_purge_does_not_prevent_confirmed_undo(self):
        self.log.ack(self.log.intent())
        self.log.ack(self.log.intent("purge"), "aborted")
        self.log.ack(self.log.intent("undo"))
        self.assertEqual(self.log.plan()["hidden_targets"], [])

    def test_cascade_records_are_independent_and_cover_history_photos(self):
        self.log.ack(self.log.intent())
        photos = [uid(), uid()]
        for photo in photos:
            self.log.intent(target=photo, cycle=uid(), kind="media")
        self.log.ack(self.log.intent("undo"))
        self.assertEqual({row["target_id"] for row in self.log.plan()["hidden_targets"]}, set(photos))

    def test_terminal_effect_on_proven_aborted_cycle_is_invalid(self):
        self.log.ack(self.log.intent(), "aborted")
        self.log.intent("purge")
        self.blocked()

    def test_unknown_cycle_and_reused_cycle_are_invalid(self):
        self.log.intent("undo")
        self.blocked()
        self.log = Log()
        self.log.intent()
        self.log.intent()
        self.blocked()

    def test_missing_middle_reorder_and_duplicate_are_invalid(self):
        self.log.ack(self.log.intent())
        rows = self.log.raw().splitlines(keepends=True)
        for raw in (rows[1], b"".join(reversed(rows)), rows[0] + rows[0] + rows[1]):
            self.blocked(raw)

    def test_tail_truncation_detected_by_independent_checkpoint(self):
        self.log.ack(self.log.intent())
        self.blocked(self.log.raw().splitlines(keepends=True)[0])

    def test_extra_tail_refused_against_stale_checkpoint(self):
        cp = self.log.checkpoint()
        self.log.intent()
        self.blocked(checkpoint=cp)

    def test_torn_record_and_mutated_bytes_refused(self):
        self.log.intent()
        self.blocked(self.log.raw()[:-1])
        self.blocked(self.log.raw().replace(b'"action":"delete"', b'"action":"undo"'))

    def test_foreign_journal_identity_refused(self):
        self.log.intent()
        cp = self.log.checkpoint()
        cp["journal_id"] = uid()
        self.blocked(checkpoint=cp)

    def test_malformed_checkpoints_and_boolean_version_refused(self):
        cp = self.log.checkpoint()
        for changes in ({"seq": True}, {"version": True}, {"head": "not-a-hash"}, {"extra": "private"}):
            self.blocked(checkpoint=dict(cp, **changes))

    def test_duplicate_event_id_is_refused_even_with_matching_hash_chain(self):
        self.log.ack(self.log.intent())
        self.log.rows[1]["event_id"] = self.log.rows[0]["event_id"]
        self.log.rows[1]["hash"] = journal.digest(self.log.rows[1])
        self.blocked()

    def test_purge_of_an_old_cycle_blocks_target_after_later_cycle_undo(self):
        self.log.ack(self.log.intent())
        self.log.ack(self.log.intent("purge"))
        new_cycle = uid()
        self.log.ack(self.log.intent(cycle=new_cycle))
        self.log.ack(self.log.intent("undo", cycle=new_cycle))
        self.assertEqual(len(self.log.plan()["purge_barriers"]), 1)
        self.assertEqual(len(self.log.plan()["hidden_targets"]), 1)

    def test_private_fields_duplicate_keys_and_nonfinite_numbers_refused(self):
        self.log.intent()
        row = self.log.rows[0]
        altered = dict(row, title="private sentinel")
        self.blocked(journal.canonical(altered) + b"\n")
        self.blocked(self.log.raw().replace(b'"seq":1', b'"seq":1,"seq":1'))
        self.blocked(self.log.raw().replace(b'"seq":1', b'"seq":NaN'))
        self.blocked(self.log.raw().replace(b'"seq":1', b'"seq":true'))

    def test_acknowledgement_must_match_and_can_only_happen_once(self):
        key = self.log.intent()
        self.log.ack(key)
        self.log.ack(key)
        self.blocked()
        self.log.rows.pop()
        self.log.rows[-1]["target_id"] = uid()
        self.log.rows[-1]["hash"] = journal.digest(self.log.rows[-1])
        self.blocked()

    def test_no_allow_list_and_no_text_or_object_keys_in_plan(self):
        self.log.ack(self.log.intent())
        result = self.log.plan()
        self.assertFalse(result["serving_allowed"])
        self.assertNotIn("allow", result)
        self.assertEqual(set(result["cycles"][0]), {"target_kind", "target_id", "cycle_id", "hide", "purge_barrier"})

    def test_deterministic_plan_does_not_mutate_inputs(self):
        self.log.intent()
        rows = copy.deepcopy(self.log.rows)
        self.assertEqual(self.log.plan(), self.log.plan())
        self.assertEqual(self.log.rows, rows)


    def test_cleanup_needs_no_deletion_repeats_and_blocks_opening_without_hiding(self):
        media = uid()
        plan_cycle = uid()
        first = self.log.intent("cleanup", media, plan_cycle, kind="media")
        self.log.ack(first)
        sweep = self.log.intent("cleanup", media, plan_cycle, kind="media")
        plan = self.log.plan()
        self.assertEqual(plan["purge_barriers"], [{"target_kind": "media", "target_id": media}])
        self.assertEqual(plan["hidden_targets"], [])
        self.assertEqual(plan["unresolved_intents"], [sweep])
        self.assertEqual(plan["cycles"], [])
        self.assertFalse(plan["serving_allowed"])

    def test_proven_aborted_cleanup_alone_is_no_barrier_and_memory_cleanup_is_invalid(self):
        media = uid()
        aborted = self.log.intent("cleanup", media, uid(), kind="media")
        self.log.ack(aborted, "aborted")
        self.assertEqual(self.log.plan()["purge_barriers"], [])
        self.log.intent("cleanup", uid(), uid(), kind="memory")
        with self.assertRaises(journal.JournalError):
            self.log.plan()

    def test_cleanup_acknowledgement_must_match_its_own_intent(self):
        media = uid()
        intent = self.log.intent("cleanup", media, uid(), kind="media")
        self.log.append(intent, "purge", "committed", media, uid(), "media")
        with self.assertRaises(journal.JournalError):
            self.log.plan()


class CliTests(unittest.TestCase):
    def test_success_missing_input_and_exclusive_output(self):
        log = Log()
        log.intent(kind="media")
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source, cp, out = base / "journal.jsonl", base / "head.json", base / "plan.json"
            source.write_bytes(log.raw())
            cp.write_bytes(journal.canonical(log.checkpoint()))
            args = [sys.executable, str(ROOT / "tools" / "deletion_journal.py"), "--journal", str(source),
                    "--checkpoint", str(cp), "--expected-journal-id", log.id, "--output", str(out)]
            first = subprocess.run(args, capture_output=True)
            self.assertEqual(first.returncode, 0)
            self.assertFalse(json.loads(out.read_bytes())["serving_allowed"])
            saved = out.read_bytes()
            self.assertEqual(subprocess.run(args, capture_output=True).returncode, 2)
            self.assertEqual(out.read_bytes(), saved)
            out.unlink()
            source.write_bytes(b'{"private":"must-not-be-logged"}\n')
            corrupt = subprocess.run(args, capture_output=True)
            self.assertEqual(corrupt.returncode, 2)
            self.assertNotIn(b"must-not-be-logged", corrupt.stdout + corrupt.stderr)
            self.assertFalse(out.exists())
            source.unlink()
            failure = subprocess.run(args, capture_output=True)
            self.assertEqual(failure.returncode, 2)
            self.assertEqual(failure.stderr.strip(), b"JOURNAL_PLAN_BLOCKED")
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
