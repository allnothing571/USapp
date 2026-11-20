"""Real pinned SDK + in-memory HTTP pool. Never connects to a service."""
import base64
import datetime
import hashlib
import io
import logging
import os
import socket
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch

from tools.j4c_tablestore_sdk import Client, ClientError, Config


JOURNAL = "00000000-0000-0000-0000-000000000041"


def config():
    return Config("cn-shanghai", "uj4c1234abcd", "journal", JOURNAL,
                  "https://uj4c1234abcd.cn-shanghai.ots.aliyuncs.com")


class ConfigTests(unittest.TestCase):
    def test_config_exact_test_endpoint_only(self):
        config().validate()
        for field, value in (("endpoint", "http://uj4c1234abcd.cn-shanghai.ots.aliyuncs.com"),
                             ("endpoint", config().endpoint + "/extra"),
                             ("endpoint", "https://127.0.0.1:443"), ("table", "production"),
                             ("instance", "usapp"), ("region", "cn-hangzhou"), ("journal_id", "bad")):
            changed = dict(config().__dict__)
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(ClientError):
                Config(**changed).validate()


class Response:
    def __init__(self, data, headers, status=200):
        self.data, self.headers, self.status = data, headers, status
        self.reason = "synthetic"
        self.released = self.closed = False

    def read(self, amount, decode_content=False):
        if decode_content:
            raise AssertionError("decompression_forbidden")
        return self.data[:amount]

    def release_conn(self):
        self.released = True

    def close(self):
        self.closed = True


class Pool:
    def __init__(self, **kwargs):
        self.options, self.calls, self.responses = kwargs, [], []
        self.sdk = None
        self.status, self.bad_signature, self.bad_md5 = 200, False, False
        self.oversize = False
        self.error = None
        self.bad_table = False
        self.reply_hook = None

    def urlopen(self, method, url, **kwargs):
        from tablestore.protobuf import table_store_pb2 as pb
        from tablestore.auth import RequestContext
        from tablestore import consts
        self.calls.append((method, url, kwargs))
        if self.error:
            raise self.error
        api = url.rsplit("/", 1)[-1]
        if self.status != 200:
            message = pb.Error(code="OTSInternalServerError", message="private-synthetic-error")
        else:
            message = getattr(pb, api + "Response")()
            if api == "StartLocalTransaction":
                message.transaction_id = "offline-tx"
            if api == "DescribeTable":
                message.table_meta.table_name = "journal"
                for key in ("journal_id", "row_type", "row_id"):
                    message.table_meta.primary_key.add(name=key, type=pb.STRING)
                message.reserved_throughput_details.capacity_unit.read = 0
                message.reserved_throughput_details.capacity_unit.write = 0
                message.reserved_throughput_details.last_increase_time = 0
                message.table_options.time_to_live = 3600 if self.bad_table else -1
                message.table_options.max_versions = 1
                message.table_options.allow_update = True
            if api in ("GetRow", "GetRange", "PutRow", "UpdateRow"):
                message.consumed.capacity_unit.read = 0
                message.consumed.capacity_unit.write = 1
            if api == "GetRow":
                message.row = b""
            if api == "GetRange":
                message.rows = b""
        if self.reply_hook and self.status == 200:
            self.reply_hook(api, kwargs["body"], message)
        body = message.SerializeToString()
        if self.oversize:
            body = b"x" * 100
        headers = {"x-ots-requestid": "offline-request", "x-ots-contenttype": "application/x-protobuf",
                   "x-ots-date": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
                   "x-ots-contentmd5": base64.b64encode(hashlib.md5(body).digest()).decode("ascii")}
        if self.status == 200:
            context = RequestContext(self.sdk.credentials_provider.get_credentials(),
                                     sign_date=kwargs["headers"][consts.OTS_HEADER_SIGN_DATE])
            signature = self.sdk._signer.make_response_signature("/" + api, headers, self.sdk._signer.get_signing_key(context))
            headers["authorization"] = "OTS " + self.sdk.credentials_provider.get_credentials().get_access_key_id() + ":" + signature
        if self.bad_signature:
            headers["authorization"] = "OTS " + self.sdk.credentials_provider.get_credentials().get_access_key_id() + ":bad-signature"
        if self.bad_md5:
            headers["x-ots-contentmd5"] = "bad-checksum"
        response = Response(body, headers, self.status)
        self.responses.append(response)
        return response


@unittest.skipUnless(os.environ.get("USAPP_J4C_OFFLINE_SDK") == "1", "run isolated SDK verification runner")
class SdkTests(unittest.TestCase):
    def setUp(self):
        # Import and execution are both under an explicit socket/DNS prohibition.
        for method in ("socket.socket.connect", "socket.create_connection", "socket.getaddrinfo"):
            patcher = patch(method, side_effect=AssertionError("outbound_network_forbidden"))
            patcher.start()
            self.addCleanup(patcher.stop)
        self.pool = None
        def factory(**kwargs):
            self.pool = Pool(**kwargs)
            return self.pool
        self.client = Client(config(), "offline-access-id", "offline-synthetic-secret", pool_factory=factory)
        self.pool.sdk = self.client._sdk

    def test_real_sdk_factory_disables_both_retry_layers_and_native_codecs(self):
        from tablestore.retry import NoRetryPolicy
        self.assertIsInstance(self.client._sdk.retry_policy, NoRetryPolicy)
        self.assertFalse(self.client._sdk.enable_native)
        self.assertFalse(self.client._sdk.native_fallback)
        self.assertEqual(self.pool.options["cert_reqs"], "CERT_REQUIRED")
        self.assertTrue(os.path.isfile(self.pool.options["ca_certs"]))
        self.assertIs(self.pool.options["retries"], False)
        self.assertEqual(self.pool.options["maxsize"], 1)
        self.assertEqual(self.client._sdk.socket_timeout, 3)

    def test_default_transport_cannot_contact_cloud(self):
        blocked = Client(config(), "offline-access-id", "offline-synthetic-secret")
        with self.assertRaisesRegex(ClientError, "^sdk_call_unknown$"):
            blocked.start()
        self.assertEqual(self.pool.calls, [])

    def test_preflight_decodes_table_and_probes_start_abort_without_writing(self):
        report = self.client.preflight()
        self.assertTrue(report["table_verified"])
        self.assertTrue(report["transaction_probe"])
        self.assertFalse(report["serving_allowed"])
        self.assertEqual([call[1].rsplit("/", 1)[-1] for call in self.pool.calls],
                         ["DescribeTable", "StartLocalTransaction", "AbortTransaction"])

    def test_preflight_rejects_automatic_expiry_before_transaction(self):
        self.pool.bad_table = True
        with self.assertRaisesRegex(ClientError, "^preflight_invalid_table$"):
            self.client.preflight()
        self.assertEqual(len(self.pool.calls), 1)

    def test_start_encodes_only_approved_table_partition_and_validates_response(self):
        from tablestore.protobuf import table_store_pb2 as pb
        self.assertEqual(self.client.start(), "offline-tx")
        method, url, options = self.pool.calls[-1]
        request = pb.StartLocalTransactionRequest()
        request.ParseFromString(options["body"])
        self.assertEqual(method, "POST")
        self.assertEqual(url, config().endpoint + "/StartLocalTransaction")
        self.assertEqual(request.table_name, "journal")
        self.assertIn(JOURNAL.encode("ascii"), request.key)
        self.assertIs(options["retries"], False)
        self.assertIs(options["redirect"], False)
        self.assertEqual(options["pool_timeout"], 1.0)
        self.assertEqual(options["timeout"].connect_timeout, 2.0)
        self.assertEqual(options["timeout"].read_timeout, 3.0)
        self.assertTrue(self.pool.responses[-1].released)

    def test_transaction_id_is_attached_to_row_and_range_reads(self):
        from tablestore.protobuf import table_store_pb2 as pb
        tx = self.client.start()
        self.client.get(tx, "head", "current")
        read = pb.GetRowRequest()
        read.ParseFromString(self.pool.calls[-1][2]["body"])
        self.assertEqual(read.table_name, "journal")
        self.assertEqual(read.transaction_id, tx)
        self.assertEqual(read.max_versions, 1)
        self.assertIn(JOURNAL.encode(), read.primary_key)
        self.client.scan(tx, "operation", limit=3)
        scan = pb.GetRangeRequest()
        scan.ParseFromString(self.pool.calls[-1][2]["body"])
        self.assertEqual(scan.transaction_id, tx)
        self.assertEqual(scan.limit, 3)
        self.assertIn(JOURNAL.encode(), scan.inclusive_start_primary_key)
        self.assertIn(JOURNAL.encode(), scan.exclusive_end_primary_key)

    def test_insert_probe_uses_existence_condition_and_same_transaction(self):
        from tablestore.protobuf import table_store_pb2 as pb
        tx = self.client.start()
        self.client.insert(tx, "operation", "synthetic-op", {"payload": "synthetic"})
        request = pb.PutRowRequest()
        request.ParseFromString(self.pool.calls[-1][2]["body"])
        self.assertEqual(request.transaction_id, tx)
        self.assertEqual(request.table_name, "journal")
        self.assertIn(JOURNAL.encode(), request.row)
        self.assertEqual(request.condition.row_existence, pb.EXPECT_NOT_EXIST)

    def test_commit_and_abort_are_distinct_and_cannot_be_blindly_repeated(self):
        tx = self.client.start()
        self.client.finish(tx, commit=True)
        self.assertTrue(self.pool.calls[-1][1].endswith("/CommitTransaction"))
        calls = len(self.pool.calls)
        with self.assertRaises(ClientError):
            self.client.finish(tx, commit=False)
        self.assertEqual(len(self.pool.calls), calls)
        tx = self.client.start()
        self.client.finish(tx)
        self.assertTrue(self.pool.calls[-1][1].endswith("/AbortTransaction"))

    def test_one_active_transaction_per_facade_stops_ambiguous_reuse(self):
        self.client.start()
        with self.assertRaisesRegex(ClientError, "^transaction_already_active$"):
            self.client.start()
        self.assertEqual(len(self.pool.calls), 1)

    def test_head_update_encodes_full_predecessor_condition_and_transaction(self):
        from tablestore.protobuf import table_store_pb2 as pb
        from tools.deletion_journal_publication import zero_head
        from tools.deletion_journal import canonical
        before = zero_head(JOURNAL)
        after = dict(before, batch_seq=1, event_seq=2, batch_head="a" * 64, event_head="b" * 64)
        tx = self.client.start()
        self.client.advance_head(tx, before, after)
        request = pb.UpdateRowRequest()
        request.ParseFromString(self.pool.calls[-1][2]["body"])
        self.assertEqual(request.table_name, "journal")
        self.assertEqual(request.transaction_id, tx)
        self.assertEqual(request.condition.row_existence, pb.EXPECT_EXIST)
        self.assertIn(canonical(before), request.condition.column_condition)
        self.assertIn(canonical(after), request.row_change)
        calls = len(self.pool.calls)
        for invalid in (dict(after, batch_seq=0), dict(after, journal_id="foreign"), dict(after, event_seq=0), dict(after, version=True)):
            with self.assertRaises(ClientError):
                self.client.advance_head(tx, before, invalid)
        self.assertEqual(len(self.pool.calls), calls)

    def test_operation_terminal_update_encodes_binding_condition(self):
        from tablestore.protobuf import table_store_pb2 as pb
        from tools.deletion_journal import canonical
        operation_id = "00000000-0000-0000-0000-000000000042"
        binding = {"operation_id": operation_id, "action": "delete"}
        previous = canonical({"operation": binding, "phase": "intent"}).decode("ascii")
        following = canonical({"operation": binding, "phase": "committed"}).decode("ascii")
        tx = self.client.start()
        self.client.advance_operation(tx, operation_id, previous, following)
        request = pb.UpdateRowRequest()
        request.ParseFromString(self.pool.calls[-1][2]["body"])
        self.assertEqual(request.transaction_id, tx)
        self.assertIn(following.encode("ascii"), request.row_change)
        self.assertIn(previous.encode("ascii"), request.condition.column_condition)
        self.assertEqual(request.condition.row_existence, pb.EXPECT_EXIST)
        self.client.finish(tx)

    def test_metadata_adapter_decodes_real_sdk_rows_and_transactions(self):
        from tablestore.plainbuffer.plain_buffer_builder import PlainBufferBuilder
        from tablestore.protobuf import table_store_pb2 as pb
        from tools import deletion_journal_publication as publication
        from tools.deletion_journal import canonical
        from tools.j4c_metadata import Metadata
        head = publication.zero_head(JOURNAL)
        def reply(api, body, message):
            if api == "GetRow":
                request = pb.GetRowRequest()
                request.ParseFromString(body)
                self.assertEqual(request.transaction_id, "offline-tx")
                message.row = bytes(PlainBufferBuilder.serialize_for_put_row(
                    [("journal_id", JOURNAL), ("row_type", "head"), ("row_id", "current")],
                    [("payload", canonical(head).decode("ascii"), 1)]))
            elif api == "GetRange":
                request = pb.GetRangeRequest()
                request.ParseFromString(body)
                self.assertEqual(request.transaction_id, "offline-tx")
        self.pool.reply_hook = reply
        # read() must not access object storage at all.
        snapshot = Metadata(self.client, None).read(JOURNAL)
        self.assertEqual(snapshot, dict(checkpoint=head, keys=[], operations={}, cycles=[]))
        self.assertEqual([call[1].rsplit("/", 1)[-1] for call in self.pool.calls],
                         ["StartLocalTransaction", "GetRow", "GetRange", "GetRange", "GetRange",
                          "GetRow", "AbortTransaction"])

    def test_operation_update_refuses_rebinding_or_terminal_reversal(self):
        from tools.deletion_journal import canonical
        operation_id = "00000000-0000-0000-0000-000000000042"
        old = {"operation": {"operation_id": operation_id}, "phase": "intent"}
        tx = self.client.start()
        before = len(self.pool.calls)
        for new in ({"operation": {"operation_id": JOURNAL}, "phase": "committed"},
                    {"operation": old["operation"], "phase": "intent"},
                    {"operation": old["operation"], "phase": "unknown"}):
            with self.assertRaises(ClientError):
                self.client.advance_operation(tx, operation_id, canonical(old).decode("ascii"),
                                              canonical(new).decode("ascii"))
        self.assertEqual(len(self.pool.calls), before)
        self.client.finish(tx)

    def test_errors_not_retried_and_unknown_commit_never_aborts(self):
        tx = self.client.start()
        self.pool.status = 500
        before = len(self.pool.calls)
        with self.assertRaisesRegex(ClientError, "^sdk_call_unknown$"):
            self.client.finish(tx, commit=True)
        self.assertEqual(len(self.pool.calls) - before, 1)
        self.assertFalse(any(call[1].endswith("/AbortTransaction") for call in self.pool.calls))
        with self.assertRaises(ClientError):
            self.client.finish(tx)
        self.assertEqual(len(self.pool.calls) - before, 1)

    def test_signature_and_checksum_errors_are_refused_with_fixed_messages(self):
        for field in ("bad_signature", "bad_md5"):
            setattr(self.pool, field, True)
            with self.assertRaisesRegex(ClientError, "^sdk_call_unknown$"):
                self.client.start()
            setattr(self.pool, field, False)

    def test_response_limit_closes_connection_and_never_parses_partial_success(self):
        self.client._sdk.connection.max_reply = 16
        self.pool.oversize = True
        with self.assertRaisesRegex(ClientError, "^sdk_call_unknown$"):
            self.client.start()
        self.assertTrue(self.pool.responses[-1].closed)
        self.assertFalse(self.pool.responses[-1].released)

    def test_unsupported_operations_and_bad_scope_stop_before_transport(self):
        tx = self.client.start()
        calls = len(self.pool.calls)
        for work in (lambda: self.client.get("foreign", "head", "current"),
                     lambda: self.client.get(tx, "other", "current"),
                     lambda: self.client.scan(tx, "head", limit=101),
                     lambda: self.client.insert(tx, "head", "current", {"title": "private"}),
                     lambda: self.client._sdk.list_table(), lambda: self.client._sdk.delete_table("journal")):
            with self.assertRaises(ClientError):
                work()
        self.assertEqual(len(self.pool.calls), calls)

    def test_sdk_error_body_secret_and_headers_are_not_logged_or_returned(self):
        self.pool.status = 500
        stream = io.StringIO()
        handler = logging.StreamHandler(stream)
        logger = logging.getLogger("usapp.j4c.sdk.offline")
        logger.addHandler(handler)
        try:
            with redirect_stdout(stream), redirect_stderr(stream):
                with self.assertRaises(ClientError) as error:
                    self.client.start()
            output = stream.getvalue() + str(error.exception)
            self.assertNotIn("private-synthetic-error", output)
            self.assertNotIn("offline-synthetic-secret", output)
            self.assertNotIn("authorization", output.lower())
        finally:
            logger.removeHandler(handler)

    def test_transport_exception_is_one_call_not_rollback_proof(self):
        self.pool.error = TimeoutError("private-timeout")
        with self.assertRaisesRegex(ClientError, "^sdk_call_unknown$"):
            self.client.start()
        self.assertEqual(len(self.pool.calls), 1)


if __name__ == "__main__":
    unittest.main()
