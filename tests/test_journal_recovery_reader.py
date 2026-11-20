"""Recovery material with injected authoritative-read fakes; never permission to serve restored data."""
import copy
import hashlib
import json
import threading
import time
import unittest
from unittest.mock import patch

from tools import deletion_journal as j1
from tools import deletion_journal_publication as publication
from tools.deletion_journal_bridge import mac as publication_mac
from tools.journal_recovery_reader import CheckpointObserver, Context, Reader, RecoveryDispatcher, RecoveryBackend, mac
from tools.journal_backend import Credential
from tests.test_deletion_journal_publication import Objects, Metadata, operation, uid
from tests.test_journal_backend import Fixtures, config


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.context = Context(uid(), uid(), uid())
        self.key = "d" * 64
        self.objects = Objects()
        self.metadata = Metadata(self.context.journal_id, self.objects)
        self.writer = publication.Publisher(self.context.journal_id, self.objects, self.metadata)
        self.reader = Reader(self.context, self.objects, self.metadata)
        self.dispatcher = RecoveryDispatcher(self.context, self.key, self.reader)
        self.operation = self.writer._operation(operation())

    def frame(self, method="snapshot"):
        return dict(version=1, journal_id=self.context.journal_id, database_id=self.context.database_id,
                    attempt_id=self.context.attempt_id, nonce="e" * 32, method=method)

    def call(self, method="snapshot"):
        raw = j1.canonical(self.frame(method))
        status, body, signature = self.dispatcher.handle(raw, mac(self.key, "request", raw))
        self.assertEqual(status, 200)
        self.assertEqual(signature, mac(self.key, "response", body))
        value = json.loads(body)
        self.assertEqual(value["request_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertFalse(value["serving_allowed"])
        for field in ("journal_id", "database_id", "attempt_id"):
            self.assertEqual(value[field], getattr(self.context, field))
        return value

    def seed(self, phase="intent"):
        self.assertEqual(self.writer.publish(self.operation), publication.State.READY)
        if phase != "intent":
            expected = publication.State.COMMITTED if phase == "committed" else publication.State.ABORTED
            self.assertEqual(self.writer.publish(self.operation, phase), expected)

    def test_existing_empty_partition_is_verified_zero_not_inferred_missing(self):
        value = self.call()
        self.assertEqual(value["state"], "verified")
        self.assertEqual(value["checkpoint"], publication.zero_head(self.context.journal_id))
        self.assertEqual(value["snapshot"]["head"]["seq"], 0)
        self.assertEqual(value["snapshot"]["hidden"], [])
        self.assertEqual(value["snapshot"]["unresolved"], [])
        self.assertEqual(self.metadata.commits, 0)

    def test_pending_delete_projects_all_hidden_targets_and_subject_intents(self):
        self.seed()
        value = self.call()
        self.assertEqual(value["state"], "verified")
        snapshot = value["snapshot"]
        expected = sorted(((s["target_kind"], s["target_id"]) for s in self.operation["subjects"]),
                          key=lambda row: (0 if row[0] == "memory" else 1, row[1]))
        self.assertEqual([(r["kind"], r["id"]) for r in snapshot["hidden"]], expected)
        self.assertEqual(snapshot["unresolved"], sorted(s["intent_id"] for s in self.operation["subjects"]))
        self.assertNotIn(self.operation["operation_id"], snapshot["unresolved"])
        self.assertEqual(snapshot["head"]["hash"], value["checkpoint"]["batch_head"])

    def test_committed_delete_and_aborted_delete_remain_distinct(self):
        self.seed("committed")
        value = self.call()
        self.assertEqual(len(value["snapshot"]["hidden"]), 2)
        self.assertEqual(value["snapshot"]["unresolved"], [])
        self.assertEqual(value["snapshot"]["head"]["seq"], 2)
        other = self.writer._operation(operation())
        self.assertEqual(self.writer.publish(other), publication.State.READY)
        self.assertEqual(self.writer.publish(other, "aborted"), publication.State.ABORTED)
        self.assertEqual(len(self.call()["snapshot"]["hidden"]), 2)

    def test_pending_cleanup_is_a_purge_barrier_even_without_delete(self):
        op = self.writer._operation(operation(action="cleanup"))
        self.assertEqual(self.writer.publish(op), publication.State.READY)
        snapshot = self.call()["snapshot"]
        self.assertEqual(snapshot["hidden"], [])
        self.assertEqual(len(snapshot["purge_barriers"]), 2)
        self.assertEqual(snapshot["unresolved"], sorted(s["intent_id"] for s in op["subjects"]))

    def test_current_head_checks_chain_without_returning_snapshot(self):
        self.seed()
        value = self.call("current_head")
        self.assertEqual(value["state"], "verified")
        self.assertIsNone(value["snapshot"])
        self.objects.corrupt_get = True
        self.assertEqual(self.call("current_head")["state"], "unknown")

    def test_unavailable_corrupt_or_contradictory_state_never_means_empty(self):
        self.seed()
        for mode in ("read", "object", "index"):
            with self.subTest(mode=mode):
                self.metadata.fail_read = mode == "read"
                self.objects.corrupt_get = mode == "object"
                before = copy.deepcopy(self.metadata.state)
                if mode == "index":
                    self.metadata.state["operations"] = {}
                value = self.call()
                self.assertEqual(value["state"], "unknown")
                self.assertIsNone(value["checkpoint"])
                self.assertIsNone(value["snapshot"])
                self.metadata.state = before
        self.assertEqual(self.metadata.commits, 1)

    def test_checkpoint_advance_between_validation_and_recheck_is_unknown(self):
        self.seed()
        read = self.metadata.read
        count = [0]
        def changing(journal):
            value = read(journal)
            count[0] += 1
            if count[0] == 2:
                value["checkpoint"]["batch_head"] = "f" * 64
            return value
        with patch.object(self.metadata, "read", side_effect=changing):
            self.assertEqual(self.call()["state"], "unknown")

    def test_changed_index_on_second_read_is_unknown_even_with_same_head(self):
        self.seed()
        read = self.metadata.read
        count = [0]
        def changing(journal):
            value = read(journal)
            count[0] += 1
            if count[0] == 2:
                value["operations"] = {}
            return value
        with patch.object(self.metadata, "read", side_effect=changing):
            self.assertEqual(self.call()["state"], "unknown")

    def test_absolute_budget_is_checked_after_synchronous_dependencies_return(self):
        reader = Reader(self.context, self.objects, self.metadata, .05)
        read = self.metadata.read
        def delayed(journal):
            time.sleep(.04)
            return read(journal)
        with patch.object(self.metadata, "read", side_effect=delayed):
            self.assertIsNone(reader.read("snapshot"))

    def test_expired_read_does_not_start_another_dependency_call(self):
        reader = Reader(self.context, self.objects, self.metadata, .05)
        read = self.metadata.read
        def delayed(journal):
            time.sleep(.08)
            return read(journal)
        with patch.object(self.metadata, "read", side_effect=delayed) as calls:
            self.assertIsNone(reader.read("snapshot"))
            self.assertEqual(calls.call_count, 1)
        self.assertEqual(self.objects.gets, [])

    def test_foreign_context_and_strict_frame_fail_before_backend_reads(self):
        requests = []
        for field in ("journal_id", "database_id", "attempt_id"):
            value = self.frame(); value[field] = uid(); requests.append(j1.canonical(value))
        for field, value in (("version", True), ("nonce", "F" * 32), ("method", "publish")):
            changed = self.frame(); changed[field] = value; requests.append(j1.canonical(changed))
        changed = self.frame(); changed["extra"] = True; requests.append(j1.canonical(changed))
        requests += [b'{"version":1,"version":1}', b'[]']
        with patch.object(self.metadata, "read", wraps=self.metadata.read) as read:
            for body in requests:
                self.assertEqual(self.dispatcher.handle(body, mac(self.key, "request", body))[0], 400)
            read.assert_not_called()

    def test_mac_domains_directions_and_frame_caps_are_separate(self):
        raw = j1.canonical(self.frame())
        for signature in ("", "a" * 64, publication_mac(self.key, "request", raw), mac(self.key, "response", raw)):
            self.assertEqual(self.dispatcher.handle(raw, signature), (401, b"", ""))
        self.assertEqual(self.dispatcher.handle(b"x" * 65537, "")[0], 413)
        self.assertEqual(self.objects.gets, [])

    def test_oversized_projection_is_refused_whole(self):
        oversized = dict(checkpoint=publication.zero_head(self.context.journal_id),
                         snapshot={"hidden": [dict(kind="memory", id=uid()) for _ in range(1500)]})
        with patch.object(self.reader, "read", return_value=oversized):
            raw = j1.canonical(self.frame())
            self.assertEqual(self.dispatcher.handle(raw, mac(self.key, "request", raw)), (503, b"", ""))

    def test_serial_reader_refuses_another_direct_flight(self):
        self.reader.guard.acquire()
        try:
            self.assertIsNone(self.reader.read("current_head"))
        finally:
            self.reader.guard.release()

    def test_managed_endpoint_blocks_closed_and_concurrent_requests(self):
        self.dispatcher.active = True
        raw = j1.canonical(self.frame())
        self.assertEqual(self.dispatcher.handle(raw, mac(self.key, "request", raw))[0], 503)
        self.dispatcher.active = False
        self.dispatcher.closing = True
        self.assertEqual(self.dispatcher.handle(raw, mac(self.key, "request", raw))[0], 503)

    def test_config_validation(self):
        for budget in (0, 11, True, float("nan")):
            with self.assertRaises(ValueError):
                Reader(self.context, self.objects, self.metadata, budget)
        with self.assertRaises(ValueError):
            Context(uid(), "not-an-external-incarnation", uid()).validate()

    def test_restricted_backend_never_initializes_missing_head_and_drains(self):
        fixtures = Fixtures()
        backend = RecoveryBackend(config(), Context(config().table.journal_id, uid(), uid()),
                                  Credential("recovery-reader", "synthetic-reader-secret"),
                                  _client_type=fixtures.Client, _objects_type=fixtures.Objects)
        try:
            fixtures.writer.rows.clear()
            self.assertIsNone(backend.dispatcher.reader.read("snapshot"))
            self.assertEqual(fixtures.writer.rows, {})
            self.assertEqual(fixtures.data, {})
            self.assertEqual(fixtures.writer.preflights, 1)
        finally:
            self.assertTrue(backend.close())
        self.assertTrue(all(client.closed for client in backend.clients))

    def test_direct_reader_flight_also_retains_owned_clients_until_done(self):
        fixtures = Fixtures()
        backend = RecoveryBackend(config(), Context(config().table.journal_id, uid(), uid()),
                                  Credential("recovery-reader", "synthetic-reader-secret"),
                                  _client_type=fixtures.Client, _objects_type=fixtures.Objects)
        reader = backend.dispatcher.reader
        reader.guard.acquire()
        try:
            self.assertFalse(backend.close())
            self.assertFalse(any(client.closed for client in backend.clients))
        finally:
            reader.guard.release()
            self.assertTrue(backend.close())
        self.assertIsNone(reader.read("snapshot"))


class CheckpointObserverTests(unittest.TestCase):
    def setUp(self):
        self.fixtures = Fixtures()
        configuration = config()
        self.journal = configuration.table.journal_id
        self.observer = CheckpointObserver(configuration.table, configuration.objects,
                                           Context(self.journal, uid(), uid()),
                                           Credential("authority-observer", "synthetic-observer-secret"),
                                           _client_type=self.fixtures.Client, _objects_type=self.fixtures.Objects)
        self.addCleanup(self.observer.close)

    def test_observation_uses_fresh_reads_and_no_write_or_pause_claim(self):
        self.assertIsNone(self.observer.observe(uid()))
        self.assertEqual(self.fixtures.writer.calls, [])
        with patch("tools.journal_recovery_reader.time.time", return_value=123):
            self.assertEqual(self.observer.observe(self.journal), {
                "checkpoint": {"journalId": self.journal, "seq": "0", "hash": j1.ZERO}, "observedAt": 123,
            })
        self.assertEqual(self.fixtures.writer.calls.count("start"), 2)
        self.assertEqual(self.fixtures.writer.calls.count("abort"), 2)
        self.assertNotIn("commit", self.fixtures.writer.calls)
        self.assertEqual(self.fixtures.object_instances[0].puts, [])

    def test_missing_or_changed_provider_head_refuses_without_fallback(self):
        provider = self.observer.reader.metadata
        original = provider.read
        observed = []

        def changing(journal):
            value = original(journal)
            observed.append(value)
            if len(observed) == 2:
                value["checkpoint"]["batch_head"] = "a" * 64
            return value

        with patch.object(provider, "read", side_effect=changing):
            self.assertIsNone(self.observer.observe(self.journal))
        self.fixtures.writer.rows.clear()
        self.assertIsNone(self.observer.observe(self.journal))
        self.assertEqual(self.fixtures.writer.rows, {})

    def test_rejected_preflight_is_latched_and_handles_close(self):
        self.fixtures.fail_preflight = True
        self.assertIsNone(self.observer.observe(self.journal))
        self.fixtures.fail_preflight = False
        self.assertIsNone(self.observer.observe(self.journal))
        self.assertEqual(self.fixtures.writer.preflights, 1)
        self.assertEqual(self.fixtures.writer.calls, [])
        owned = list(self.observer.clients)
        self.assertTrue(self.observer.close())
        self.assertTrue(all(client.closed for client in owned))
        self.assertIsNone(self.observer.observe(self.journal))

    def test_handles_stay_open_while_observer_owns_the_read_slot(self):
        with self.observer.reader.guard:
            self.assertFalse(self.observer.close())
            self.assertIsNone(self.observer.observe(self.journal))
            self.assertFalse(any(client.closed for client in self.observer.clients))
        self.assertTrue(self.observer.close())


if __name__ == "__main__":
    unittest.main()
