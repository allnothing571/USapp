"""OSS V4 and immutable objects, synthetic in-memory transport only."""
import hashlib
import hmac
import os
import socket
import unittest
from unittest.mock import patch

from tools import deletion_journal as j1
from tools import deletion_journal_publication as p
from tools.j4c_oss_objects import Config, Objects, canonical_request, signing_key, string_to_sign


JOURNAL = "00000000-0000-0000-0000-000000000041"
STAMP = "20250411T064124Z"


def candidate():
    from test_j4c_metadata import operation
    publisher = p.Publisher(JOURNAL, None, None)
    frame, _ = publisher._candidate(publisher._operation(operation()), "intent", p.zero_head(JOURNAL))
    return p.object_key(JOURNAL, frame["hash"]), j1.canonical(frame) + b"\n"


class SigningTests(unittest.TestCase):
    def test_vendor_worked_values_and_separate_key_vector(self):
        headers = {"content-disposition": "attachment", "content-length": "3",
                   "content-md5": "ICy5YqxZB1uWSwcVLSNLcA==", "content-type": "text/plain",
                   "x-oss-content-sha256": "UNSIGNED-PAYLOAD", "x-oss-date": STAMP}
        canonical = canonical_request("PUT", "/examplebucket/exampleobject", headers,
                                      "content-disposition;content-length")
        text = string_to_sign(STAMP, "cn-hangzhou", canonical)
        self.assertEqual(text.splitlines()[-1], "c46d96390bdbc2d739ac9363293ae9d710b14e48081fcb22cd8ad54b63136eca")
        # Vendor final signature uses its published derived key, NOT the placeholder secret.
        key = bytes.fromhex("3543b7686e65eda71e5e5ca19d548d78423c37e8ddba4dc9d83f90228b457c76")
        self.assertEqual(hmac.new(key, text.encode("ascii"), hashlib.sha256).hexdigest(),
                         "053edbf550ebd239b32a9cdfd93b0b2b3f2d223083aa61f75e9ac16856d61f23")
        # Existing C++ signing regression independently pins placeholder-secret derivation.
        self.assertEqual(signing_key("yourAccessKeySecret", "20250411", "cn-hangzhou").hex(),
                         "8a01ff4efcc65ca2cbc75375045c61ab5f3fa8b9a2d84f0add27ef16a25feb3c")

    def test_config_restricts_region_bucket_and_journal(self):
        Config("usapp-j4c-log-1234abcd", JOURNAL).validate()
        for values in (("existing-bucket", JOURNAL, "cn-shanghai"),
                       ("usapp-j4c-log-1234abcd", JOURNAL, "cn-hangzhou"),
                       ("usapp-j4c-log-1234abcd", "bad", "cn-shanghai")):
            with self.assertRaises(ValueError):
                Config(*values).validate()


class Response:
    def __init__(self, status, data, headers=None):
        self.status, self.data, self.headers = status, data, headers or {}
        self.closed = self.released = False

    def read(self, amount, decode_content=False):
        if decode_content:
            raise AssertionError("decompression_forbidden")
        return self.data[:amount]

    def release_conn(self):
        self.released = True

    def close(self):
        self.closed = True


class Pool:
    def __init__(self, **options):
        self.options, self.calls, self.data, self.responses = options, [], {}, []
        self.status = None
        self.lose_put = self.error = self.corrupt = self.encoded = self.oversize = False
        self.active = lambda: False

    def urlopen(self, method, url, **kwargs):
        if self.active():
            raise AssertionError("object_call_under_transaction")
        self.calls.append((method, url, kwargs))
        if self.error:
            raise RuntimeError("private-synthetic-secret-and-URL")
        if method == "PUT":
            existed = url in self.data
            if not existed:
                self.data[url] = kwargs["body"]
            if self.lose_put:
                self.lose_put = False
                raise TimeoutError("private-lost-reply")
            status, body = (409 if existed else 200), b""
        else:
            status, body = (200, self.data[url]) if url in self.data else (404, b"missing-private-error")
        if self.status is not None:
            status = self.status
        if self.corrupt:
            body = b"corrupt"
        if self.oversize:
            body = b"x" * (1024 * 1024 + 1)
        response = Response(status, body, {"Content-Encoding": "gzip"} if self.encoded else {})
        self.responses.append(response)
        return response


@unittest.skipUnless(os.environ.get("USAPP_J4C_OFFLINE_SDK") == "1", "run isolated SDK verification runner")
class ObjectTests(unittest.TestCase):
    def setUp(self):
        for name in ("socket.socket.connect", "socket.create_connection", "socket.getaddrinfo"):
            patcher = patch(name, side_effect=AssertionError("outbound_network_forbidden"))
            patcher.start()
            self.addCleanup(patcher.stop)
        self.pool = None
        def factory(**kwargs):
            self.pool = Pool(**kwargs)
            return self.pool
        self.config = Config("usapp-j4c-log-1234abcd", JOURNAL)
        self.objects = Objects(self.config, "offline-id", "offline-secret", pool_factory=factory, clock=lambda: STAMP)
        self.key, self.data = candidate()

    def test_success_requires_readback_and_repeated_put_cannot_replace_bytes(self):
        self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.CONFIRMED)
        self.assertEqual([c[0] for c in self.pool.calls], ["PUT", "GET"])
        self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.CONFIRMED)
        self.assertEqual(len(self.pool.data), 1)

    def test_fixed_transport_tls_no_retry_and_signed_overwrite_prevention(self):
        self.objects.put_once(self.key, self.data)
        self.assertEqual(self.pool.options["cert_reqs"], "CERT_REQUIRED")
        self.assertTrue(os.path.isfile(self.pool.options["ca_certs"]))
        self.assertIs(self.pool.options["retries"], False)
        method, url, kw = self.pool.calls[0]
        self.assertEqual(url, self.config.endpoint + "/" + self.key)
        self.assertEqual(method, "PUT")
        self.assertEqual(kw["headers"]["x-oss-forbid-overwrite"], "true")
        self.assertEqual(kw["headers"]["accept-encoding"], "identity")
        self.assertFalse(kw["redirect"])
        self.assertFalse(kw["retries"])
        self.assertFalse(kw["preload_content"])
        self.assertEqual(kw["timeout"].connect_timeout, 2.0)
        self.assertEqual(kw["timeout"].read_timeout, 3.0)
        self.assertIn("20250411/cn-shanghai/oss/aliyun_v4_request", kw["headers"]["authorization"])

    def test_lost_put_reply_is_not_retried_and_later_read_can_find_bytes(self):
        self.pool.lose_put = True
        self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.UNKNOWN)
        self.assertEqual([c[0] for c in self.pool.calls], ["PUT"])
        self.assertEqual(self.objects.get(self.key), self.data)

    def test_success_status_or_etag_cannot_substitute_for_integrity(self):
        self.pool.corrupt = True
        self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.UNKNOWN)
        with self.assertRaisesRegex(p.Unavailable, "^object_read_unknown$"):
            self.objects.get(self.key)

    def test_wrong_scope_and_modified_bytes_stop_before_transport(self):
        for key, data in ((self.key + "?acl", self.data), (self.key.replace(JOURNAL, "other"), self.data),
                          (self.key, self.data + b" "), (self.key, b"x" * (1024 * 1024 + 1))):
            self.assertEqual(self.objects.put_once(key, data), p.Receipt.CONFLICT)
        self.assertEqual(self.pool.calls, [])

    def test_redirect_error_and_missing_are_never_success(self):
        for status in (301, 302, 307, 403, 404, 500, 503):
            self.pool.status = status
            before = len(self.pool.calls)
            self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.UNKNOWN)
            self.assertEqual(len(self.pool.calls), before + 1)
            with self.assertRaises(p.Unavailable):
                self.objects.get(self.key)

    def test_encoded_or_oversize_response_is_closed_not_pooled(self):
        for flag in ("encoded", "oversize"):
            setattr(self.pool, flag, True)
            self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.UNKNOWN)
            self.assertTrue(self.pool.responses[-1].closed)
            self.assertFalse(self.pool.responses[-1].released)
            setattr(self.pool, flag, False)

    def test_errors_have_fixed_categories_and_default_network_is_blocked(self):
        self.pool.error = True
        self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.UNKNOWN)
        with self.assertRaisesRegex(p.Unavailable, "^object_read_unknown$"):
            self.objects.get(self.key)
        blocked = Objects(self.config, "offline-id", "offline-secret", clock=lambda: STAMP)
        self.assertEqual(blocked.put_once(self.key, self.data), p.Receipt.UNKNOWN)

    def test_invalid_clock_and_header_injection_stop_before_network(self):
        for timestamp in ("bad", "20251311T000000Z", STAMP + "\r\nheader:x"):
            self.objects._clock = lambda: timestamp
            self.assertEqual(self.objects.put_once(self.key, self.data), p.Receipt.UNKNOWN)
        self.assertEqual(self.pool.calls, [])
        with self.assertRaises(ValueError):
            Objects(self.config, "id\r\nx:y", "secret")

    def test_objects_and_metadata_adapter_publish_end_to_end_with_fake_sdk(self):
        from test_j4c_metadata import Client, operation
        from tools.j4c_metadata import Metadata
        client = Client()
        self.pool.active = lambda: client.active
        publisher = p.Publisher(JOURNAL, self.objects, Metadata(client, self.objects))
        op = operation()
        self.assertEqual(publisher.publish(op), p.State.READY)
        client.lose_commit = True
        self.assertEqual(publisher.publish(op, "committed"), p.State.UNKNOWN)
        self.assertEqual(publisher.publish(op), p.State.COMMITTED)
        self.assertFalse(publisher.inspect()["serving_allowed"])


if __name__ == "__main__":
    unittest.main()
