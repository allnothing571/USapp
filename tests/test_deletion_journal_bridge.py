"""Synthetic injected stores only; dispatcher has no HTTP/SDK/network factory."""
import copy
import hashlib
import unittest
from tools import deletion_journal as j1
from tools import deletion_journal_bridge as b
from tools import deletion_journal_publication as p
from tests.test_deletion_journal_publication import Objects, Metadata, operation, uid


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.journal_id, self.key = uid(), "b" * 64
        self.objects = Objects()
        self.meta = Metadata(self.journal_id, self.objects)
        self.publisher = p.Publisher(self.journal_id, self.objects, self.meta)
        self.dispatch = b.Dispatcher(self.journal_id, self.key, self.publisher)
        self.op = self.publisher._operation(operation())

    def request(self, method="ensure_intent", op=None, phase="intent", nonce="c" * 32):
        op = self.op if op is None else op
        return dict(version=1, journal_id=self.journal_id, nonce=nonce, method=method,
                    operation_id=op["operation_id"], operation=op, phase=phase)

    def call(self, request):
        raw = j1.canonical(request)
        status, body, signature = self.dispatch.handle(raw, b.mac(self.key, "request", raw))
        self.assertEqual(status, 200)
        self.assertEqual(signature, b.mac(self.key, "response", body))
        reply = j1._decode(body)
        self.assertEqual(reply["request_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(reply["nonce"], request["nonce"])
        self.assertEqual(reply["journal_id"], self.journal_id)
        return reply

    def lookup(self, op=None):
        request = self.request("lookup", op)
        request["operation"] = request["phase"] = None
        return self.call(request)

    def test_publish_read_terminal_and_same_key_replay(self):
        self.assertEqual(self.lookup()["state"], "missing")
        self.assertEqual(self.call(self.request())["state"], "ready")
        self.assertEqual(self.lookup()["operation"], self.op)
        before = self.meta.commits
        self.assertEqual(self.call(self.request())["state"], "ready")
        self.assertEqual(self.meta.commits, before)
        self.assertEqual(self.call(self.request("settle", phase="committed"))["state"], "committed")
        self.assertEqual(self.call(self.request())["state"], "committed")
        before = self.meta.commits
        self.assertEqual(self.call(self.request("settle", phase="aborted"))["state"], "conflict")
        self.assertEqual(self.meta.commits, before)
        self.assertFalse(self.publisher.inspect()["serving_allowed"])

    def test_changed_payload_and_missing_terminal_never_write(self):
        self.call(self.request())
        op = copy.deepcopy(self.op); op["request_fingerprint"] = "d" * 64
        before = self.meta.commits
        self.assertEqual(self.call(self.request(op=op))["state"], "conflict")
        new = self.publisher._operation(operation())
        self.assertEqual(self.call(self.request("settle", new, "aborted"))["state"], "conflict")
        self.assertEqual(self.meta.commits, before)

    def test_unauthenticated_and_direction_reflection_do_not_touch_backend(self):
        raw = j1.canonical(self.request())
        for signature in (None, "", "a" * 64, b.mac(self.key, "response", raw)):
            self.assertEqual(self.dispatch.handle(raw, signature)[0], 401)
        self.assertEqual(self.meta.commits, 0); self.assertEqual(self.objects.gets, [])

    def test_strict_shape_version_scope_phase_and_normalized_subjects(self):
        bad = []
        for field, value in (("version", True), ("version", 1.0), ("journal_id", uid()), ("nonce", "A" * 32),
                             ("method", "open"), ("phase", "aborted"), ("operation_id", uid())):
            v = self.request(); v[field] = value; bad.append(j1.canonical(v))
        v = self.request(); v["operation"]["subjects"] = list(reversed(v["operation"]["subjects"])); bad.append(j1.canonical(v))
        v = self.request(); v["secret"] = "must-not-escape"; bad.append(j1.canonical(v))
        bad += [b'{"version":1,"version":1}', b'[]', b'{' + b'"x":[' * 1000]
        for raw in bad:
            status, body, signature = self.dispatch.handle(raw, b.mac(self.key, "request", raw))
            self.assertEqual(status, 400); self.assertEqual(body, b""); self.assertEqual(signature, "")
        self.assertEqual(self.meta.commits, 0); self.assertEqual(self.objects.gets, [])

    def test_size_limit_rejects_before_auth_or_backend(self):
        self.assertEqual(self.dispatch.handle(b"x" * (b.MAX_BYTES + 1), "")[0], 413)
        self.assertEqual(self.meta.commits, 0)

    def test_unavailable_corrupt_or_stale_indexes_never_mean_missing(self):
        self.meta.fail_read = True
        self.assertEqual(self.lookup()["state"], "unknown")
        self.meta.fail_read = False
        self.call(self.request())
        self.objects.corrupt_get = True
        self.assertEqual(self.lookup()["state"], "unknown")
        self.objects.corrupt_get = False
        self.meta.state["operations"] = {}
        self.assertEqual(self.lookup()["state"], "unknown")

    def test_lost_commit_receipt_recovered_by_same_operation_without_new_effect(self):
        self.meta.failure = "after"
        reply = self.call(self.request())
        self.assertIn(reply["state"], ("unknown", "unavailable"))
        self.meta.failure = None
        before = self.meta.commits
        self.assertEqual(self.lookup()["operation"], self.op)
        self.assertEqual(self.call(self.request())["state"], "ready")
        self.assertEqual(self.meta.commits, before)

    def test_constructor_rejects_cross_journal_or_unbounded_key(self):
        for journal, key in ((uid(), self.key), (self.journal_id, ""), (self.journal_id, "x" * 64)):
            with self.assertRaises(ValueError): b.Dispatcher(journal, key, self.publisher)


if __name__ == "__main__":
    unittest.main()
