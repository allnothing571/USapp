"""Initialization protocol with explicit fake facade; no cloud or SDK required."""
import copy
from types import SimpleNamespace
import unittest

from tools import deletion_journal as j1
from tools.deletion_journal_publication import State, zero_head
from tools.j4c_initialize import initialize_client, receipt, SetupClient
from tools.j4c_tablestore_sdk import Client as RuntimeClient, ClientError
from test_j4c_metadata import Client, JOURNAL


def plan():
    return dict(version=1, mode="offline_only", run_id="1234abcd", journal_id=JOURNAL,
                region="cn-shanghai", budget_cny=5, declared_owner_uid="1234567890123456",
                bucket="usapp-j4c-log-1234abcd", instance="uj4c1234abcd", table="journal")


class InitClient(Client):
    def __init__(self):
        super().__init__()
        self.rows.clear()
        self.config = SimpleNamespace(journal_id=JOURNAL, instance="uj4c1234abcd", table="journal", region="cn-shanghai")
        self.partial = False

    def preflight(self):
        return {}

    def any_partition_row(self, tx):
        rows = [self.row(*sorted(self.rows)[0])] if self.rows else []
        return None, None, rows, "partial" if self.partial else None


class InitializeTests(unittest.TestCase):
    def test_empty_partition_commits_seed_and_bound_receipt_together(self):
        client = InitClient()
        self.assertEqual(initialize_client(plan(), client), State.COMMITTED)
        self.assertEqual(j1._decode(client.rows["head", "current"]), zero_head(JOURNAL))
        self.assertEqual(j1._decode(client.rows["bootstrap", "current"]), receipt(plan()))
        self.assertEqual(len(client.rows), 2)

    def test_unknown_row_type_or_unreceipted_zero_head_never_repaired(self):
        for key in (("rogue", "x"), ("head", "current")):
            client = InitClient()
            client.rows[key] = j1.canonical(zero_head(JOURNAL)).decode("ascii")
            before = copy.deepcopy(client.rows)
            self.assertEqual(initialize_client(plan(), client), State.CONFLICT)
            self.assertEqual(client.rows, before)

    def test_failed_second_insert_rolls_back_seed(self):
        client = InitClient()
        client.fail_write = "bootstrap"
        self.assertEqual(initialize_client(plan(), client), State.UNKNOWN)
        self.assertFalse(client.rows)
        self.assertEqual(client.calls[-1], "abort")

    def test_lost_commit_does_not_abort_and_receipt_replay_writes_nothing(self):
        client = InitClient()
        client.lose_commit = True
        self.assertEqual(initialize_client(plan(), client), State.UNKNOWN)
        self.assertEqual(client.calls[-1], "commit")
        before = copy.deepcopy(client.rows)
        writes = client.calls.count("write:head") + client.calls.count("write:bootstrap")
        self.assertEqual(initialize_client(plan(), client), State.COMMITTED)
        self.assertEqual(client.rows, before)
        self.assertEqual(client.calls.count("write:head") + client.calls.count("write:bootstrap"), writes)

    def test_replay_never_resets_advanced_head(self):
        client = InitClient()
        self.assertEqual(initialize_client(plan(), client), State.COMMITTED)
        head = dict(zero_head(JOURNAL), batch_seq=1, event_seq=1, batch_head="a" * 64, event_head="b" * 64)
        client.rows["head", "current"] = j1.canonical(head).decode("ascii")
        before = copy.deepcopy(client.rows)
        self.assertEqual(initialize_client(plan(), client), State.COMMITTED)
        self.assertEqual(client.rows, before)

    def test_mismatched_manifest_or_missing_head_stops(self):
        client = InitClient()
        self.assertEqual(initialize_client(plan(), client), State.COMMITTED)
        changed = dict(plan(), declared_owner_uid="1234567890123457")
        self.assertEqual(initialize_client(changed, client), State.CONFLICT)
        del client.rows["head", "current"]
        self.assertEqual(initialize_client(plan(), client), State.UNKNOWN)
        self.assertNotIn(("head", "current"), client.rows)

    def test_partial_probe_cannot_be_empty(self):
        client = InitClient()
        client.partial = True
        self.assertEqual(initialize_client(plan(), client), State.UNKNOWN)
        self.assertFalse(client.rows)

    def test_runtime_facade_does_not_accept_bootstrap_rows(self):
        client = object.__new__(RuntimeClient)
        client.config = SimpleNamespace(journal_id=JOURNAL)
        with self.assertRaises(ClientError):
            client._key("bootstrap", "current")

    def test_setup_facade_refuses_other_rows_payloads_and_updates(self):
        client = object.__new__(SetupClient)
        client.config = SimpleNamespace(journal_id=JOURNAL)
        client.initial_receipt = receipt(plan())
        with self.assertRaises(ClientError):
            client._key("batch", "1")
        with self.assertRaises(ClientError):
            client.insert("unused", "head", "current", {"payload": "wrong"})
        for method in (client.advance_head, client.advance_operation, client.scan):
            with self.assertRaises(ClientError):
                method()


if __name__ == "__main__":
    unittest.main()
